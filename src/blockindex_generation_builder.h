#ifndef INNOVA_BLOCKINDEX_GENERATION_BUILDER_H
#define INNOVA_BLOCKINDEX_GENERATION_BUILDER_H

#include "fixed_blockindex_store.h"
#include "blockindex_hashindex.h"
#include "blockindex_activeindex.h"
#include "blockindex_derived_state.h"

#include <stdint.h>
#include <string>
#include <vector>

// Phase A.5 + A.10.1b-fix1: offline real-chain Block Index V2 generation builder.
//
// This builds a COMPLETE, AUTHORITATIVE-CAPABLE generation (records.dat +
// active.dat + hashindex + derived.dat + MANIFEST) from the EXISTING local
// legacy blockchain, using the verified production store APIs.
//
// Safety contract:
//  - operates ONLY on a caller-supplied static snapshot directory of the legacy
//    block-index LevelDB (never the live datadir), and writes ONLY to a
//    caller-supplied isolated output generation directory
//  - never touches wallet.dat, txleveldb of the live node, blk*.dat, or any
//    network/wallet/P2P/RPC/staking state
//  - deterministic RecordId assignment ordered by block hash (reproducible,
//    independent of process/pointer identity)

// A single legacy source record: the authoritative block hash (the legacy DB
// key) plus the fully reconstructed BlockIndexRecord V1.
struct BlockIndexGenerationSourceRecord
{
    uint256 hash;
    BlockIndexRecord record;

    BlockIndexGenerationSourceRecord()
        : hash(0)
    {
    }
};

// The populated source for building a generation.
struct BlockIndexGenerationSource
{
    std::vector<BlockIndexGenerationSourceRecord> records; // all known legacy index records
    uint256 hashBestChain;                                 // authoritative active-chain tip hash
    bool foundBestChain;                                   // true when hashBestChain was read

    // DAG links from LevelDB (optional, for post-DAG trust computation)
    std::map<uint256, std::vector<uint256> > dagLinks;     // hash -> parent hashes
    bool foundDAGLinks;

    // A.10.1b-fix2: DAG scores from LevelDB (for canonical post-DAG trust)
    std::map<uint256, uint256> dagScores;                  // hash -> nDAGScore

    // A.10.1b-fix2: path to directory containing blk*.dat files for exact nSize.
    // If empty, nSize will be marked unavailable (blocks not accessible).
    std::string blockDataDir;

    BlockIndexGenerationSource()
        : foundBestChain(false), foundDAGLinks(false)
    {
    }
};

// Result statistics of a build.
struct BlockIndexGenerationStats
{
    uint64_t totalRecords;
    uint64_t activeRecords;
    uint64_t sideChainRecords;
    int32_t activeTipHeight;
    uint256 activeTipHash;
    bool hasDerived;

    BlockIndexGenerationStats()
        : totalRecords(0),
          activeRecords(0),
          sideChainRecords(0),
          activeTipHeight(-1),
          activeTipHash(0),
          hasDerived(false)
    {
    }
};

// Reads every legacy "blockindex" record from a static LevelDB snapshot
// directory (NOT the live datadir) and reconstructs the full source required to
// build a generation. Uses the exact legacy CDiskBlockIndex serialization.
// Also reads DAG links if present.
bool ReadLegacyBlockIndexSource(const std::string& snapshotLevelDbDir,
                                BlockIndexGenerationSource* out,
                                std::string* error);

// A.10.1b-fix2: Read DAG links and scores from a static LevelDB snapshot.
// Used to populate source.dagLinks and source.dagScores for canonical
// post-DAG trust computation.
bool ReadDAGLinksFromSnapshot(const std::string& snapshotLevelDbDir,
                              std::map<uint256, std::vector<uint256> >* dagLinks,
                              std::map<uint256, uint256>* dagScores,
                              std::string* error);

// A.10.1b-fix3 C3: Reconstruct canonical DAG scores using production ColorBlock
// semantics. Populates canonicalScores with the nDAGScore that production
// RebuildDAGOrder would compute for each post-DAG PoW block.
bool ReconstructCanonicalDAGScores(
    const std::vector<std::pair<int32_t, uint256>>& heightSorted,
    const std::map<uint256, const BlockIndexRecord*>& recordByHash,
    const std::map<uint256, std::vector<uint256>>& dagLinks,
    std::map<uint256, uint256>* canonicalScores,
    std::string* error);

class BlockIndexGenerationBuilder
{
public:
    BlockIndexGenerationBuilder();

    // Build a COMPLETE generation at generationDir for the given generation id,
    // from the given legacy source. On success the MANIFEST is COMPLETE and
    // `stats` is populated. Also produces derived.dat if source data permits.
    // Never writes outside generationDir.
    bool Build(const BlockIndexGenerationSource& source,
               const std::string& generationDir,
               uint64_t generation,
               BlockIndexGenerationStats* stats,
               std::string* error);

    // Free 1:1 object lifetime (deterministic close ordering).
    void Close();

private:
    FixedBlockIndexStore store;
    BlockIndexHashIndex hashIndex;
    BlockIndexActiveIndex activeIndex;
    BlockIndexDerivedStateStore derivedStore;
    bool built;
};

// Collects the results of a massive differential of a COMPLETE generation
// against its legacy source. All mismatch/corruption/not-found counters must be
// zero for a successful verdict.
struct BlockIndexDifferentialResult
{
    // hash lookups
    uint64_t hashQueries;
    uint64_t hashMismatches;
    uint64_t hashCorruptions;
    uint64_t hashNotFound;
    // active height lookups
    uint64_t heightQueries;
    uint64_t heightMismatches;
    uint64_t heightCorruptions;
    uint64_t heightNotFound;
    // active parent continuity
    uint64_t parentChecks;
    uint64_t parentMismatches;
    // side-chain differential
    uint64_t sideChainSamples;
    uint64_t sideChainMismatches;
    // deterministic random samples
    uint64_t randomHashChecks;
    uint64_t randomHeightChecks;
    uint64_t randomMismatches;
    // tip coherence
    bool tipCoherent;
    std::string tipDetail;

    BlockIndexDifferentialResult()
        : hashQueries(0), hashMismatches(0), hashCorruptions(0), hashNotFound(0),
          heightQueries(0), heightMismatches(0), heightCorruptions(0), heightNotFound(0),
          parentChecks(0), parentMismatches(0),
          sideChainSamples(0), sideChainMismatches(0),
          randomHashChecks(0), randomHeightChecks(0), randomMismatches(0),
          tipCoherent(false)
    {
    }
};

// Exhaustively (or on a large deterministic sample when exceedSamples > 0)
// reopens the COMPLETE generation from disk and compares it field-by-field
// against the legacy source. Treats "no surprises": every hash must resolve,
// every active height must resolve, parent continuity must hold, and the tip
// identities must all agree. Never relies on builder in-memory maps.
bool VerifyGenerationAgainstSource(const BlockIndexGenerationSource& source,
                                   const std::string& generationDir,
                                   uint64_t generation,
                                   BlockIndexDifferentialResult* out,
                                   std::string* error);

#endif
