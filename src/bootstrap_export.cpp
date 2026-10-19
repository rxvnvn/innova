// Copyright (c) 2026 Innova developers
// Distributed under the MIT software license.
//
// bootstrap_export - build a strictly continuous bootstrap.dat from blk*.dat
//
// Why a C++ tool and not a script: on this chain CBlockHeader::GetHash()
// returns GetPoWHash(), and the PoW hash is Tribus (JH-512 -> Keccak-512 ->
// ECHO-512, src/hashblock.h).  A block's identity therefore cannot be
// recomputed with plain SHA-256d, and any external script that tries silently
// fails to resolve hashPrevBlock links.  This tool reuses the consensus hash
// implementation, so the links it verifies are the links the node will verify.
//
// Record layout written by CBlock::WriteToDisk (src/main.h):
//     magic 4 | size 4 (LE, fixed width) | block[size]
// bootstrap.dat has exactly the same layout and is consumed by
//     innovad -loadblock=<file>   ->   LoadExternalBlockFile
//
// The exporter writes a record only when its hashPrevBlock equals the hash of
// the last written block, so the output is continuous by construction.  Blocks
// that cannot be linked yet are buffered; blocks that never link are reported
// and *not* written, so the file never claims completeness it does not have.

#include "hashblock.h"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <deque>
#include <algorithm>
#include <memory>

#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

static const unsigned char kMagic[4] = {0xfa, 0xf4, 0x3f, 0xb7};
static const size_t kHeaderSize = 80;
static const uint64_t kMaxPendingBytes = 512ull << 20;  // 512 MiB buffer bound

struct Record {
    std::vector<unsigned char> raw;  // 8-byte record header + block body
    uint256 hash;
};

static bool ReadExact(FILE* f, void* buf, size_t n)
{
    return n == 0 || fread(buf, 1, n, f) == n;
}

// Parse one record. Returns false at clean EOF, throws away the caller on error.
enum class ReadResult { Ok, Eof, Desync, Truncated, BadSize };

static ReadResult ReadRecord(FILE* f, Record& rec, std::string& why)
{
    unsigned char hdr[8];
    size_t got = fread(hdr, 1, 8, f);
    if (got == 0) return ReadResult::Eof;
    if (got != 8) { why = "short record header"; return ReadResult::Truncated; }
    if (memcmp(hdr, kMagic, 4) != 0) { why = "bad magic"; return ReadResult::Desync; }

    uint32_t size = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8) |
                    ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 24);
    if (size < kHeaderSize || size > (16u << 20)) { why = "implausible record size"; return ReadResult::BadSize; }

    rec.raw.resize(8 + size);
    memcpy(&rec.raw[0], hdr, 8);
    if (!ReadExact(f, &rec.raw[8], size)) { why = "truncated body"; return ReadResult::Truncated; }

    const unsigned char* body = &rec.raw[8];
    rec.hash = Tribus(body, body + kHeaderSize);
    return ReadResult::Ok;
}

static bool IsZero(const uint256& h)
{
    for (int i = 0; i < 32; ++i)
        if (h.begin()[i] != 0) return false;
    return true;
}

struct BlkFile { std::string path; unsigned int index; };

static std::vector<BlkFile> ListBlkFiles(const std::string& datadir)
{
    std::vector<BlkFile> out;
    DIR* d = opendir(datadir.c_str());
    if (!d) return out;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        std::string name(e->d_name);
        if (name.size() < 8 || name.compare(0, 3, "blk") != 0) continue;
        if (name.compare(name.size() - 4, 4, ".dat") != 0) continue;
        std::string digits = name.substr(3, name.size() - 7);
        if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) continue;
        out.push_back(BlkFile{datadir + "/" + name, (unsigned int)strtoul(digits.c_str(), nullptr, 10)});
    }
    closedir(d);
    std::sort(out.begin(), out.end(), [](const BlkFile& a, const BlkFile& b) { return a.index < b.index; });
    return out;
}

struct Stats {
    uint64_t records = 0, written = 0, duplicates = 0, unlinked = 0;
    uint64_t resync_events = 0, resync_bytes = 0;
    uint64_t bytes_written = 0;
    std::string first_hash, last_hash, prev_of_first;
    std::string stop_reason = "end-of-archive";
    double elapsed = 0;
};

int main(int argc, char** argv)
{
    std::string datadir, out_path, json_path;
    uint64_t stop_after = 0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) out_path = argv[++i];
        else if (a == "--stop-after" && i + 1 < argc) stop_after = strtoull(argv[++i], nullptr, 10);
        else if (a == "--json" && i + 1 < argc) json_path = argv[++i];
        else if (a.rfind("--", 0) == 0) { fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
        else datadir = a;
    }
    if (datadir.empty() || out_path.empty()) {
        fprintf(stderr, "usage: %s <datadir> --out <bootstrap.dat> [--stop-after N] [--json file]\n", argv[0]);
        return 2;
    }

    auto files = ListBlkFiles(datadir);
    if (files.empty()) { fprintf(stderr, "no blk*.dat in %s\n", datadir.c_str()); return 2; }

    FILE* out = fopen(out_path.c_str(), "wb");
    if (!out) { fprintf(stderr, "cannot open %s for writing\n", out_path.c_str()); return 2; }

    Stats st;
    uint256 tip;
    bool have_tip = false;
    uint64_t pending_bytes = 0;
    std::map<uint256, std::deque<Record>> pending;  // hashPrevBlock -> waiting children
    std::deque<uint256> pending_order;              // FIFO of parents, for reporting
    std::vector<Record> buffer;
    int rc = 0;

    auto write_record = [&](const Record& r) {
        fwrite(&r.raw[0], 1, r.raw.size(), out);
        st.written++;
        st.bytes_written += r.raw.size();
        tip = r.hash;
        have_tip = true;
        st.last_hash = r.hash.ToString();
        if (st.first_hash.empty()) st.first_hash = st.last_hash;
    };

    // Emit a block and then any buffered descendants, depth-first.
    std::function<void(const Record&)> emit = [&](const Record& r) {
        write_record(r);
        if (st.written >= stop_after && stop_after != 0) return;
        std::vector<uint256> stack{r.hash};
        while (!stack.empty()) {
            uint256 parent = stack.back();
            stack.pop_back();
            auto it = pending.find(parent);
            if (it == pending.end()) continue;
            std::deque<Record>& kids = it->second;
            while (!kids.empty()) {
                Record kid = kids.front();
                kids.pop_front();
                pending_bytes -= kid.raw.size();
                write_record(kid);
                if (stop_after != 0 && st.written >= stop_after) return;
                stack.push_back(kid.hash);
            }
            if (kids.empty()) pending.erase(it);
        }
    };

    for (const auto& bf : files) {
        FILE* f = fopen(bf.path.c_str(), "rb");
        if (!f) { st.stop_reason = "cannot open " + bf.path; rc = 1; break; }
        for (;;) {
            if (stop_after != 0 && st.written >= stop_after) {
                st.stop_reason = "stop-after"; break;
            }
            Record rec;
            std::string why;
            long rec_pos = ftell(f);
            ReadResult rr = ReadRecord(f, rec, why);
            if (rr == ReadResult::Eof) break;
            if (rr == ReadResult::Desync) {
                // A torn tail (power loss mid-append) or padding between
                // sections leaves bytes that are not a record.  LoadExternalBlockFile
                // resynchronises by advancing one byte at a time; do the same
                // instead of abandoning the rest of the archive.
                st.resync_events++;
                st.resync_bytes++;
                if (rec_pos < 0 || fseek(f, rec_pos + 1, SEEK_SET) != 0) {
                    st.stop_reason = bf.path + ": cannot resynchronise"; rc = 1; break;
                }
                continue;
            }
            if (rr != ReadResult::Ok) {
                st.stop_reason = bf.path + ": " + why; rc = 1; break;
            }
            st.records++;
            uint256 prev;
            memcpy(prev.begin(), &rec.raw[8 + 4], 32);
            if (!have_tip) {
                if (!IsZero(prev)) {
                    st.stop_reason = "archive does not start at genesis";
                    rc = 1;
                    break;
                }
                st.prev_of_first = prev.ToString();
                emit(rec);
                if (rc) break;
                continue;
            }
            if (prev == tip) {
                emit(rec);
                continue;
            }
            if (!pending.count(prev)) pending_order.push_back(prev);
            pending[prev].push_back(std::move(rec));
            pending_bytes += pending[prev].back().raw.size();
            if (pending_bytes > kMaxPendingBytes) {
                st.stop_reason = "pending buffer bound exceeded";
                rc = 1;
                break;
            }
        }
        fclose(f);
        if (rc) break;
    }

    for (auto& kv : pending) st.unlinked += kv.second.size();
    st.duplicates = 0;
    fflush(out);
    fsync(fileno(out));
    fclose(out);

    printf("{\n");
    printf("  \"output\": \"%s\",\n", out_path.c_str());
    printf("  \"records_scanned\": %llu,\n", (unsigned long long)st.records);
    printf("  \"blocks_written\": %llu,\n", (unsigned long long)st.written);
    printf("  \"bytes_written\": %llu,\n", (unsigned long long)st.bytes_written);
    printf("  \"first_hash\": \"%s\",\n", st.first_hash.c_str());
    printf("  \"last_hash\": \"%s\",\n", st.last_hash.c_str());
    printf("  \"prev_of_first\": \"%s\",\n", st.prev_of_first.c_str());
    printf("  \"unlinked_records\": %llu,\n", (unsigned long long)st.unlinked);
    printf("  \"resync_events\": %llu,\n", (unsigned long long)st.resync_events);
    printf("  \"resync_bytes\": %llu,\n", (unsigned long long)st.resync_bytes);
    printf("  \"pending_missing_parents\": %zu,\n", pending_order.size());
    printf("  \"stop_reason\": \"%s\",\n", st.stop_reason.c_str());
    printf("  \"continuous\": %s\n", (st.unlinked == 0 && st.written > 0 && rc == 0) ? "true" : "false");
    printf("}\n");

    if (!json_path.empty()) {
        FILE* j = fopen(json_path.c_str(), "w");
        if (j) {
            fprintf(j, "{\"records_scanned\": %llu, \"blocks_written\": %llu, \"bytes_written\": %llu, "
                       "\"first_hash\": \"%s\", \"last_hash\": \"%s\", \"prev_of_first\": \"%s\", "
                       "\"unlinked_records\": %llu, \"stop_reason\": \"%s\"}\n",
                    (unsigned long long)st.records, (unsigned long long)st.written,
                    (unsigned long long)st.bytes_written, st.first_hash.c_str(), st.last_hash.c_str(),
                    st.prev_of_first.c_str(), (unsigned long long)st.unlinked, st.stop_reason.c_str());
            fclose(j);
        }
    }
    return (st.unlinked == 0 && st.written > 0 && rc == 0) ? 0 : 1;
}
