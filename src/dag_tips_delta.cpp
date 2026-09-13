// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.
#include "dag_tips_delta.h"

#include <boost/filesystem.hpp>
#include <cstdio>
#include <cstring>
#include <vector>

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
    std::vector<DagTipDeltaRecord> ram;
    boost::filesystem::path spillPath;

    Recorder() : observer(NULL), context(NULL), active(false), invalid(false),
                 spilled(false), capacity(256), logical(0), depth(0),
                 origin(DAG_TIP_DELTA_ADD_TO_BLOCK_INDEX) {}
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
        fclose(f);
        g.spilled = true;
        return true;
    } catch (...) { g.invalid = true; return false; }
}

bool WriteRecord(FILE* f, const DagTipDeltaRecord& r)
{
    const unsigned char op = (unsigned char)r.op;
    return fwrite(&op, 1, 1, f) == 1 && fwrite(r.hash.begin(), 1, 32, f) == 32;
}

bool ReadRecord(FILE* f, DagTipDeltaRecord* out)
{
    unsigned char op = 0;
    if (fread(&op, 1, 1, f) == 0) return false;
    unsigned char bytes[32];
    if (fread(bytes, 1, 32, f) != 32) { g.invalid = true; return false; }
    out->op = (op == DagTipDeltaRecord::TIP_REMOVE) ? DagTipDeltaRecord::TIP_REMOVE : DagTipDeltaRecord::TIP_ADD;
    memcpy(out->hash.begin(), bytes, 32);
    return true;
}

void ResetPending()
{
    g.ram.clear();
    g.logical = 0;
    g.depth = 0;
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
    if (g.ram.size() < g.capacity) { g.ram.push_back(r); return; }
    if (!SpillOpen()) return;
    FILE* f = fopen(g.spillPath.string().c_str(), "ab");
    if (!f) { g.invalid = true; return; }
    // Preserve exact operation order: flush the oldest bounded in-RAM prefix
    // before accepting the next record into the next bounded window.
    for (size_t i = 0; i < g.ram.size(); ++i)
        if (!WriteRecord(f, g.ram[i])) { g.invalid = true; break; }
    fclose(f);
    if (g.invalid) return;
    g.ram.clear();
    g.ram.push_back(r);
}

void CommitDagTipDeltaTransaction()
{
    if (!g.active) return;
    if (!g.invalid && g.observer) {
        Deliver(DagTipCommittedDeltaEvent(DagTipCommittedDeltaEvent::BEGIN, g.origin));
        if (!g.spillPath.empty()) {
            FILE* f = fopen(g.spillPath.string().c_str(), "rb");
            if (!f) g.invalid = true;
            else {
                DagTipDeltaRecord r;
                while (!g.invalid && ReadRecord(f, &r))
                    Deliver(DagTipCommittedDeltaEvent(g.origin, r));
                fclose(f);
            }
        }
        for (size_t i = 0; !g.invalid && i < g.ram.size(); ++i)
            Deliver(DagTipCommittedDeltaEvent(g.origin, g.ram[i]));
        if (!g.invalid)
            Deliver(DagTipCommittedDeltaEvent(DagTipCommittedDeltaEvent::END, g.origin));
    }
    ResetPending();
}

void DiscardDagTipDeltaTransaction()
{
    ResetPending();
}
