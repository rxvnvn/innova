// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.
#include "dag_tips_delta.h"

#include <boost/filesystem.hpp>
#include <cstdio>
#include <cstring>
#include <vector>
#include <openssl/sha.h>

bool g_testFailDagTipDeltaSpillWrite = false;
bool g_testFailDagTipDeltaSpillClose = false;

namespace {

struct Recorder
{
    DagTipCommittedDeltaObserver observer;
    void* context;
    bool active;
    bool invalid;
    bool spilled;
    size_t capacity;
    uint64_t logical;
    unsigned int depth;
    DagTipDeltaOrigin origin;
    bool hasFinalSourceState;
    uint256 finalSourceState;
    bool hasInitialSourceState;
    uint256 initialSourceState;
    SHA256_CTX digest;
    std::vector<DagTipDeltaRecord> ram;
    boost::filesystem::path spillPath;

    Recorder() : observer(NULL), context(NULL), active(false), invalid(false),
                 spilled(false), capacity(256), logical(0), depth(0),
                 origin(DAG_TIP_DELTA_ADD_TO_BLOCK_INDEX), hasFinalSourceState(false),
                 finalSourceState(0), hasInitialSourceState(false), initialSourceState(0) { SHA256_Init(&digest); }
};

Recorder g;

void RemoveSpill()
{
    if (!g.spillPath.empty()) {
        boost::system::error_code ec;
        boost::filesystem::remove(g.spillPath, ec);
        g.spillPath.clear();
    }
    g.spilled = false;
}

bool SpillOpen()
{
    if (!g.spillPath.empty()) return true;
    try {
        g.spillPath = boost::filesystem::temp_directory_path() /
            boost::filesystem::unique_path("innova-dag-tip-delta-%%%%-%%%%.bin");
        FILE* f = fopen(g.spillPath.string().c_str(), "wb");
        if (!f) { g.invalid = true; return false; }
        const int closeResult = fclose(f);
        if (closeResult != 0) { g.invalid = true; return false; }
        g.spilled = true;
        return true;
    } catch (...) { g.invalid = true; return false; }
}

bool WriteRecord(FILE* f, const DagTipDeltaRecord& r)
{
    const unsigned char op = (unsigned char)r.op;
    return !g_testFailDagTipDeltaSpillWrite &&
        fwrite(&op, 1, 1, f) == 1 && fwrite(r.hash.begin(), 1, 32, f) == 32;
}

bool ReadRecord(FILE* f, DagTipDeltaRecord* out)
{
    unsigned char op = 0;
    if (fread(&op, 1, 1, f) == 0) { if (ferror(f)) g.invalid = true; return false; }
    unsigned char bytes[32];
    if (fread(bytes, 1, 32, f) != 32) { g.invalid = true; return false; }
    if (op != DagTipDeltaRecord::TIP_REMOVE && op != DagTipDeltaRecord::TIP_ADD) { g.invalid = true; return false; }
    out->op = static_cast<DagTipDeltaRecord::Op>(op);
    memcpy(out->hash.begin(), bytes, 32);
    return true;
}

void ResetPending()
{
    g.ram.clear();
    SHA256_Init(&g.digest);
    g.logical = 0;
    g.depth = 0;
    g.hasFinalSourceState = false;
    g.finalSourceState = uint256(0);
    g.hasInitialSourceState = false;
    g.initialSourceState = uint256(0);
    g.active = false;
    g.invalid = false;
    RemoveSpill();
}

void Deliver(const DagTipCommittedDeltaEvent& e)
{
    if (!g.observer) return;
    try { g.observer(e, g.context); }
    catch (...) { g.invalid = true; }
}

} // namespace

void SetDagTipCommittedDeltaObserver(DagTipCommittedDeltaObserver observer, void* context)
{
    DiscardDagTipDeltaTransaction();
    g.observer = observer;
    g.context = context;
}

bool CorruptDagTipDeltaSpillForTest(int mode)
{
    if (g.spillPath.empty()) return false;
    if (mode == 0) {
        boost::filesystem::resize_file(g.spillPath, 33); // aligned truncation
        return true;
    }
    FILE* file = fopen(g.spillPath.string().c_str(), "r+b");
    if (!file) return false;
    if (mode == 2) fseek(file, 1, SEEK_SET); // hash byte, valid framing/op
    const unsigned char bad = 0xff;
    const bool ok = fwrite(&bad, 1, 1, file) == 1;
    return fclose(file) == 0 && ok;
}

void SetDagTipDeltaRamCapacityForTest(size_t capacity)
{
    g.capacity = capacity;
}

DagTipDeltaState GetDagTipDeltaState()
{
    DagTipDeltaState s;
    s.enabled = g.observer != NULL;
    s.active = g.active;
    s.failureLatched = g.invalid;
    s.spilled = g.spilled;
    s.ramCapacity = g.capacity;
    s.ramRecords = g.ram.size();
    s.logicalRecords = g.logical;
    s.hasIntendedFinalSourceStateId = g.hasFinalSourceState;
    s.intendedFinalSourceStateId = g.finalSourceState;
    return s;
}

bool BeginDagTipDeltaTransaction(DagTipDeltaOrigin origin)
{
    if (!g.observer) return false;
    // Joined scope: root owns origin, journal, and publication.
    if (g.active) { ++g.depth; return false; }
    ResetPending();
    g.origin = origin;
    g.depth = 1;
    g.active = true;
    return true;
}

void LeaveDagTipDeltaTransaction()
{
    if (g.active && g.depth > 1)
        --g.depth;
}

void AppendDagTipDelta(const DagTipDeltaRecord& r)
{
    if (!g.active || g.invalid) return;
    ++g.logical;
    const unsigned char op = static_cast<unsigned char>(r.op);
    SHA256_Update(&g.digest, &op, 1);
    SHA256_Update(&g.digest, r.hash.begin(), 32);
    if (g.ram.size() < g.capacity) { g.ram.push_back(r); return; }
    if (!SpillOpen()) return;
    FILE* f = fopen(g.spillPath.string().c_str(), "ab");
    if (!f) { g.invalid = true; return; }
    // Preserve exact operation order: flush the oldest bounded in-RAM prefix
    // before accepting the next record into the next bounded window.
    for (size_t i = 0; i < g.ram.size(); ++i)
        if (!WriteRecord(f, g.ram[i])) { g.invalid = true; break; }
    const int closeResult = fclose(f);
    if (closeResult != 0 || g_testFailDagTipDeltaSpillClose) g.invalid = true;
    if (g.invalid) return;
    g.ram.clear();
    g.ram.push_back(r);
}

void SetDagTipDeltaInitialSourceStateId(const uint256& id)
{
    if (!g.active || g.invalid || g.hasInitialSourceState) return;
    g.initialSourceState = id;
    g.hasInitialSourceState = true;
}

void InvalidateDagTipDeltaTransaction() { if (g.active) g.invalid = true; }

void SetDagTipDeltaFinalSourceStateId(const uint256& id)
{
    if (!g.active || g.invalid) return;
    g.finalSourceState = id;
    g.hasFinalSourceState = true;
}

bool GetDagTipDeltaFinalSourceStateId(uint256* out)
{
    if (!out || !g.active || !g.hasFinalSourceState) return false;
    *out = g.finalSourceState;
    return true;
}

void CommitDagTipDeltaTransaction()
{
    if (!g.active) return;
    if (!g.invalid && g.observer) {
        DagTipCommittedDeltaEvent begin(DagTipCommittedDeltaEvent::BEGIN, g.origin);
        begin.hasInitialSourceStateId = g.hasInitialSourceState;
        begin.initialSourceStateId = g.initialSourceState;
        begin.expectedRecordCount = g.logical;
        Deliver(begin);
        uint64_t delivered = 0;
        SHA256_CTX replay; SHA256_Init(&replay);
        if (!g.spillPath.empty()) {
            FILE* f = fopen(g.spillPath.string().c_str(), "rb");
            if (!f) g.invalid = true;
            else {
                DagTipDeltaRecord r;
                while (!g.invalid && ReadRecord(f, &r))
                {
                    ++delivered;
                    const unsigned char op = static_cast<unsigned char>(r.op);
                    SHA256_Update(&replay, &op, 1); SHA256_Update(&replay, r.hash.begin(), 32);
                    Deliver(DagTipCommittedDeltaEvent(g.origin, r));
                }
                if (fclose(f) != 0) g.invalid = true;
            }
        }
        for (size_t i = 0; !g.invalid && i < g.ram.size(); ++i) {
            ++delivered;
            const unsigned char op = static_cast<unsigned char>(g.ram[i].op);
            SHA256_Update(&replay, &op, 1); SHA256_Update(&replay, g.ram[i].hash.begin(), 32);
            Deliver(DagTipCommittedDeltaEvent(g.origin, g.ram[i]));
        }
        unsigned char expected[32], actual[32];
        SHA256_Final(expected, &g.digest); SHA256_Final(actual, &replay);
        if (delivered != g.logical || memcmp(expected, actual, 32) != 0) g.invalid = true;
        if (!g.invalid) {
            DagTipCommittedDeltaEvent end(DagTipCommittedDeltaEvent::END, g.origin);
            end.expectedRecordCount = g.logical;
            end.hasFinalSourceStateId = g.hasFinalSourceState;
            end.finalSourceStateId = g.finalSourceState;
            Deliver(end);
        }
    }
    ResetPending();
}

void DiscardDagTipDeltaTransaction()
{
    ResetPending();
}
