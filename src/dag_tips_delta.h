// Copyright (c) 2019-2026 The Innova developers
// Passive transactional, value-only DAG-tip delta seam.
#ifndef INNOVA_DAG_TIPS_DELTA_H
#define INNOVA_DAG_TIPS_DELTA_H
#include "uint256.h"
#include <cstddef>
#include <stdint.h>

struct DagTipDeltaRecord {
    enum Op { TIP_ADD = 1, TIP_REMOVE = 2 } op;
    uint256 hash;
    DagTipDeltaRecord() : op(TIP_ADD), hash(0) {}
    DagTipDeltaRecord(Op o, const uint256& h) : op(o), hash(h) {}
};

enum DagTipDeltaOrigin { DAG_TIP_DELTA_ADD_TO_BLOCK_INDEX = 1, DAG_TIP_DELTA_REORGANIZE = 2 };

struct DagTipCommittedDeltaEvent {
    enum Kind { BEGIN = 1, RECORD = 2, END = 3 } kind;
    DagTipDeltaOrigin origin;
    DagTipDeltaRecord record;
    DagTipCommittedDeltaEvent(Kind k, DagTipDeltaOrigin o) : kind(k), origin(o), record() {}
    DagTipCommittedDeltaEvent(DagTipDeltaOrigin o, const DagTipDeltaRecord& r) : kind(RECORD), origin(o), record(r) {}
};

typedef void (*DagTipCommittedDeltaObserver)(const DagTipCommittedDeltaEvent&, void*);

struct DagTipDeltaState {
    bool enabled, active, failureLatched, spilled;
    size_t ramCapacity, ramRecords;
    uint64_t logicalRecords;
    DagTipDeltaState() : enabled(false), active(false), failureLatched(false), spilled(false), ramCapacity(0), ramRecords(0), logicalRecords(0) {}
};

void SetDagTipCommittedDeltaObserver(DagTipCommittedDeltaObserver observer, void* context);
void SetDagTipDeltaRamCapacityForTest(size_t capacity);
DagTipDeltaState GetDagTipDeltaState();
bool BeginDagTipDeltaTransaction(DagTipDeltaOrigin origin);
// Close a joined nested scope without publication. Root callers use Commit/Discard.
void LeaveDagTipDeltaTransaction();
void AppendDagTipDelta(const DagTipDeltaRecord& record);
void CommitDagTipDeltaTransaction();
void DiscardDagTipDeltaTransaction();
#endif
