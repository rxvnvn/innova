// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

// PM1-P0-07b (C2) - daemon-native Block Index V2 bootstrap / legacy migration.
//
// OWNER-ACCEPTED MIXED-STORE BOUNDARY:
//   The legacy persistence is the SHARED <datadir>/txleveldb LevelDB. It is
//   NOT renamed, NOT split, NOT deleted and NOT turned into a second database.
//   After durable logical CURRENT it remains an ACTIVE runtime database for the
//   non-block-index namespaces that still require it; its legacy block-index rows
//   survive only as NON-AUTHORITATIVE RESIDUE / R3 compatibility mirror.
//   Retiring legacy AUTHORITY is a C4 (semantic) concern, never a whole-store
//   rename.
//
// A temporary, bounded, read-only copy of ONLY <datadir>/txleveldb may be used
// as the migration SOURCE (migration scratch; removed by the caller). It never
// becomes a second production database and is never archived.

#ifndef INNOVA_BLOCKINDEX_NATIVE_BOOTSTRAP_H
#define INNOVA_BLOCKINDEX_NATIVE_BOOTSTRAP_H

#include <stdint.h>
#include <string>

#include "blockindex_startup_ownership.h"

struct BlockIndexGenerationSource;

// C2-C: snapshot the migration source. Copies ONLY <dataDir>/txleveldb into
// snapshotDir (no blk*.dat, wallet.dat, peers.dat or any other datadir content).
// The source must be quiescent: this fails closed when the process-global legacy
// LevelDB handle is open (a copy taken under an open handle is not guaranteed
// consistent). Returns false + error otherwise.
bool SnapshotLegacyBlockIndexDb(const std::string& dataDir,
                                const std::string& snapshotDir,
                                std::string* error);

// C2-A: build the MINIMUM canonical EMPTY_NEW genesis source using the SINGLE
// shared genesis primitive (CreateBlockIndexGenesisBlock) plus its REAL on-disk
// binding, mirroring exactly the field set the legacy AddToBlockIndex genesis
// materialisation persists. Writes the genesis block to the node block file so
// the shared builder can source nSize. Sets source->blockDataDir = dataDir.
// Never invents a field the legacy genesis record does not carry.
bool CreateGenesisBlockIndexSource(const std::string& dataDir,
                                   BlockIndexGenerationSource* source,
                                   std::string* error);

enum BlockIndexNativePrepareStatus
{
    BLOCK_INDEX_NATIVE_PREPARE_OK = 0,
    BLOCK_INDEX_NATIVE_PREPARE_FAILED = 1,
};

// C2-B/D/E: prepare the durable V2 generation for a NON-V2 startup state by
// building from the appropriate source, validating, publishing and selecting
// logical CURRENT. Fail-closed: never writes CURRENT on any failure, so a crash
// before SelectGeneration leaves no authority transfer (CURRENT is the Rubicon).
//   EMPTY_NEW                 -> canonical genesis source
//   LEGACY_MIGRATION_REQUIRED -> bounded txleveldb migration source
// Any other state is rejected.
BlockIndexNativePrepareStatus PrepareNativeBlockIndexGeneration(
    const std::string& v2Root,
    const std::string& dataDir,
    BlockIndexStartupOwnershipState state,
    std::string* error);

#endif // INNOVA_BLOCKINDEX_NATIVE_BOOTSTRAP_H
