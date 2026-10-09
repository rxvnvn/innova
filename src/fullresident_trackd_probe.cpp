// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// TRACK D plugin seam (UNTRACKED SCRATCH ONLY; not part of production or the
// test suite) and TrackDTailCountInternal's Tail() accessor. Empty bodies are
// all that the Probe() function needs from the private internals of Impl.
// A scoped link-time access (the only mechanism that works against the PIMPL's
// incomplete-type barrier without touching the production header) is used by
// the standalone driver executables built from the combined TU
// fullresident_trackd_main.cpp. When this file is OUT of the build (it stays a
// loose untracked .cpp under scratch), samples of its Probe() calls inside the
// driver must fall back to the -1 stub in that driver's TrackDTailCountInternal
// static (returns -1 when the live-tail count is not available in that TU).
#include "blockindex_authoritative_live.h"
#include "blockindex_live_tail.h"

// Return the live-tail logical-resident count (informational only; the former operation-resident map (now removed)
// is the growth target this TRACK D driver measures, not the tail).
static long TrackDTailCountInternal(const BlockIndexAuthoritativeLive& live)
{
    const BlockIndexLiveTail& t = live.Tail();
    if (&t == NULL)
        return -1;
    // The live tail is a composite (base reader + mutable tip) by-value
    // materializer; its residency is governed by the AnchorPolicy in
    // blockindex_hot_owner.h, separate from the former operation-resident map (now removed). Report the
    // error-resilient placeholder: not measured in this driver (-1).
    return -1;
}
