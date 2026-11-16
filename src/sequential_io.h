// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

#ifndef INNOVA_SEQUENTIAL_IO_H
#define INNOVA_SEQUENTIAL_IO_H

// BOOT-002 / startup I/O: buffering for known full-file SEQUENTIAL scans.
//
// Every startup pass over an immutable block-index component (records.dat
// 1.87 GB, derived.dat 0.46 GB at 8.24M records) reads fixed-size records with
// fread() through a glibc stdio stream. glibc sizes that buffer from
// st_blksize (4 KiB on this filesystem), so ONE full pass becomes ~470k read()
// syscalls on records.dat alone (~2.3M for the whole startup). The syscall count,
// not the byte volume, is what makes a cold boot IOPS-bound: the measured
// baseline was 2.34M read syscalls averaging ~3 KB.
//
// This helper raises the stream buffer for a pass that is known to be a
// straight sequential traversal. It changes ONLY how many bytes each read()
// requests; the decoded bytes, the committed-region bound, the per-record CRC
// and the fail-closed behaviour are untouched.
//
// Size rationale: 1 MiB is large enough to make syscalls irrelevant (>=512
// records per read at 228 B/record, ~1830 reads for a full 8M-record pass)
// while staying far below the fixed residency already budgeted for this phase
// (the pass itself holds one record buffer) and small enough that it does not
// perturb the cgroup footprint. It matches the kernel's maximum readahead
// window, so the request does not outrun what the block layer prefetches.
//
// Usage contract: construct it immediately after the stream is opened and
// before the first read/seek on that stream; the object must outlive every use
// of the stream (glibc keeps the pointer).
#include <cstddef>
#include <cstdio>
#include <vector>

static const std::size_t INNOVA_SEQUENTIAL_IO_BUFFER_BYTES = 1u << 20; // 1 MiB

class SequentialIoBuffer
{
public:
    explicit SequentialIoBuffer(FILE* file,
                                std::size_t bytes = INNOVA_SEQUENTIAL_IO_BUFFER_BYTES)
        : buf_(bytes), engaged_(false)
    {
        if (file && !buf_.empty())
            engaged_ = (setvbuf(file, &buf_[0], _IOFBF, buf_.size()) == 0);
    }
    bool Engaged() const { return engaged_; }

private:
    std::vector<char> buf_;
    bool engaged_;
    SequentialIoBuffer(const SequentialIoBuffer&);
    SequentialIoBuffer& operator=(const SequentialIoBuffer&);
};

#endif // INNOVA_SEQUENTIAL_IO_H
