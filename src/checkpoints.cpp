// Copyright (c) 2009-2012 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/assign/list_of.hpp> // for 'map_list_of()'
#include <boost/foreach.hpp>

#include "checkpoints.h"

#include "blockindex_authoritative_startup.h"   // V2-R2B: authoritative by-value resolver
#include "chainstate/observe_hook.h"             // S2.0: V2 store legacy-index guard
#include "chainstate/store3a/incstore.h"          // P2(S4): csReadFile/csWriteFile/ChainError (V2 scalar home)
#include "txdb.h"
#include "main.h"
#include "uint256.h"


static const int nCheckpointSpan = nCoinbaseMaturity * 2; // twice of coin maturity is a long enough chain

namespace Checkpoints
{
    // ------------------------------------------------------------------
    // P2 (S4): V2-native synchronized-checkpoint scalar home.
    //
    // The persisted hashSyncCheckpoint is ONE tiny scalar, consensus-critical
    // (CheckSync' descendant rule needs it across restarts; its ABSENT value is
    // the canonical ppcoin "no synchronized checkpoint"). It historically lived
    // in the legacy LevelDB (CTxDB "hashSyncCheckpoint" key). In V2 store mode
    // we must NOT create/open/read/write the legacy txleveldb at all, so the
    // scalar gets a dedicated V2 home under the approved chainstate/ directory
    // (a sibling of Store3A + TX-LOC). Format: a small self-describing record
    // written atomically (temp file + fsync + rename), crash-safe:
    //     [4-byte magic "V2CK"] [1 byte version=1] [32-byte uint256 hash]
    // Absent file / bad magic / bad version / short read all fail CLOSED to
    // SYNC_CHECKPOINT_ABSENT or CORRUPT (never a silent genesis substitution).
    // ------------------------------------------------------------------
    const std::string V2SyncCheckpointScalarPath()
    {
        return (GetDataDir() / "chainstate" / "sync-checkpoint").string();
    }

    // Returns true+LOADED on a vaild persisted scalar; true+ABSENT on a missing
    // file (fresh/empty chainstate) or a wrong-record; true+CORRUPT on an
    // unreadable/undecodable present file; false on filesystem error (fan to
    // caller, which fails startup fail-closed).
    SyncCheckpointLoadStatus ReadV2SyncCheckpointScalar(uint256& hashOut, std::string* error)
    {
        hashOut = uint256(0);
        cs::ChainError e;
        std::vector<uint8_t> buf; bool exists = false;
        if (!cs::store3a::csReadFile(V2SyncCheckpointScalarPath(), &buf, &exists, &e))
        {
            if (error) *error = "V2 sync-checkpoint scalar read failed: " + e.str();
            return SYNC_CHECKPOINT_CORRUPT;   // present-but-unreadable => corrupt/fail-closed
        }
        if (!exists)
        {
            if (error) error->clear();
            return SYNC_CHECKPOINT_ABSENT;    // canonical, not an error
        }
        const unsigned char magic[4] = { 'V', '2', 'C', 'K' };
        if (buf.size() != 1 + 4 + 32 || buf[4] != 1 ||
            !std::equal(buf.begin(), buf.begin() + 4, magic))
        {
            if (error) *error = "V2 sync-checkpoint scalar record invalid (wrong size/magic/version)";
            return SYNC_CHECKPOINT_CORRUPT;
        }
        memcpy(hashOut.begin(), &buf[5], 32);
        if (error) error->clear();
        return SYNC_CHECKPOINT_LOADED;
    }

    bool WriteV2SyncCheckpointScalar(const uint256& hashCheckpoint)
    {
        // Ensure the chainstate/ home exists before the first genesis-establishment
        // write (the authoritative Store3A opens later, so the scalar must create it).
        try
        {
            boost::filesystem::path p = V2SyncCheckpointScalarPath();
            if (!p.parent_path().empty() && !boost::filesystem::exists(p.parent_path()))
                boost::filesystem::create_directories(p.parent_path());
        }
        catch (const std::exception& ex)
        {
            printf("V2 sync-checkpoint scalar mkdir failed: %s\n", ex.what());
            return false;
        }
        std::vector<uint8_t> rec;
        rec.reserve(1 + 4 + 32);
        const unsigned char magic[4] = { 'V', '2', 'C', 'K' };
        rec.insert(rec.end(), magic, magic + 4);   // 0..3
        rec.push_back(1);                           // 4: version
        rec.insert(rec.end(), hashCheckpoint.begin(), hashCheckpoint.end()); // 5..36
        cs::ChainError e;
        if (!cs::store3a::csWriteFile(V2SyncCheckpointScalarPath(), rec.data(), rec.size(), false, false, &e))
        {
            printf("V2 sync-checkpoint scalar write failed at %s: %s\n", V2SyncCheckpointScalarPath().c_str(), e.str().c_str());
            return false;
        }
        return true;
    }
    //typedef std::map<int, uint256> MapCheckpoints;

    // How many times we expect transactions after the last checkpoint to
    // be slower. This number is a compromise, as it can't be accurate for
    // every system. When reindexing from a fast disk with a slow CPU, it
    // can be up to 20, while when downloading from a slow network with a
    // fast multicore CPU, it won't be much higher than 1.
    static const double SIGCHECK_VERIFICATION_FACTOR = 5.0;

    struct CCheckpointData {
        const MapCheckpoints *mapCheckpoints;
        int64_t nTimeLastCheckpoint;
        int64_t nTransactionsLastCheckpoint;
        double fTransactionsPerDay;
    };

    //
    // What makes a good checkpoint block?
    // + Is surrounded by blocks with reasonable timestamps
    //   (no blocks before with a timestamp after, none after with
    //    timestamp before)
    // + Contains no strange transactions
    //
    MapCheckpoints mapCheckpoints =
        boost::assign::map_list_of
        ( 0,       uint256("0x000009bd42d259eb7031ae4f634aede1a690da795e5529786a72c3cd6d989995") )
        ( 2000,    uint256("0x000000007e4fbd38a6072a2725273901d2eafdadbe9ce2e28f22882db6e76817") )
        ( 5000,    uint256("0x0000000090efedc86969fcd821ee8fdde179796547a4bd07c643800d75c1ddbd") )
        ( 10000,   uint256("0x00000000026e6c5aca75f2c3b288f5f68907dbf7fe4525cc39bffc3d40234984") )
        ( 20000,   uint256("0x00000000078317bdc2930e6918491529b42dc399e7d291b5cf1add64afcdce20") )
        ( 30000,   uint256("0x05f89e228fbf708ca70539f8725835c70dbe726cb400a6417ddd1e456008fe96") )
        ( 50000,   uint256("0x00000000062440c503446206ea2b49472b5096dfdcbfa94fd2d60ac2babe7f8d") )
        ( 75000,   uint256("0xa18904ad0c2997b421e1e71375f5d0195d40a0bf5672ce63e387d8d0a794134d") )
        ( 90000,   uint256("0xb173dd04c9a979196bf2eb35764214464ef418013c7cb9306bf1a421865c8d7a") )
        ( 100000,  uint256("0xf20e568304e357a1b47edb451ddeaaa60c41ce16cedc7b3b203cd0b29052955d") )
        ( 150000,  uint256("0xa6396410d82ddb43420d61e8aaa6e99549cc0309517d72b09d26eccda40ac440") )
        ( 200000,  uint256("0x83aff3e1e000dd4429fe6e43fba3d6a18fc1fcdb82088c347851457a00164779") )
        ( 300000,  uint256("0x035f2afd77f628d533e0445bc615deeff181d0618a1857b59328a9828614d1e5") )
        ( 400000,  uint256("0xbc36f854e8a329704dee481edd1ffeb1bcc7fe00b0dcd0e2273ff59510fcff5d") )
        ( 500000,  uint256("0x597cb4a8b906c4a68c91d68b7315c2d22a3374cbbe1f8dd51f7fc1d26ffa39f8") )
        ( 600000,  uint256("0x2bafd6a6b6013062d2e13ec848b31cc051f6b77ef6894689110929a52fde4743") )
        ( 700000,  uint256("0x1a6e5dbcb406c34f93ec89d7dd26cbc97b89b9cb97312d2c08d23fdc92d42a0e") )
        ( 800000,  uint256("0x000dee38aaf0072e3ef0813e8ee55b7621d04f2ffe30198e715fceef47d2accf") )
        ( 900000,  uint256("0x4f71ab6407626c619e5d2dfcd528da4a8fc8fec35f08ff151059ac27ccc87f78") )
        ( 1000000, uint256("0x4e9e1d81e2eac234189d293608b6873b1f341158278e8a3ecaebfa9cf5b04a54") )
        ( 1100000, uint256("0x4dd971be6ab9087558f9870b46361a0a846270525b3af49dedcacc8ae16fbca7") )
        ( 1110000, uint256("0x5368ef0d8c9c931137b129ac729df887de41be6ee1e2d3cb37a214baf5ea1203") )
        ( 1120000, uint256("0x803877c47a918f136c04f2cd3e2e06ec892e4e125a999ce8cdb35ad7f8b8ff50") )
        ( 1130000, uint256("0x077eae09473d73956e0da6452b569d0cabbddeea7657351b115551df44b4b34c") )
        ( 1140000, uint256("0x36676d18d540a72a0c3a94e408405a2a4dcc53fbeeba17809189568a8be08cb3") )
        ( 1150000, uint256("0xf86cc40fb6ff191e577e9386f7d9dba56fd34c5183ac372647b93cc7428a2553") )
        ( 1160000, uint256("0x06b9dac23681f4eb66cd2269259c449b1cd8b938bae8f6a73dbf57d154d2492b") )
        ( 1170000, uint256("0x1b5e51d5b548e7e47d2f70b866bcbfc9558420b82fd016f97d5663a60aabbe94") )
        ( 1108000, uint256("0xc94f1deedee3413ea301b78d15b46e53a6ff1eaee110fceb1b0fec51f202a307") )
        ( 1190000, uint256("0x18dd50254ef59d8ac1987e39af097c24be959abefd12d43027860e9f944ba677") )
        ( 1200000, uint256("0x879fe371bbe58f9b333b99ebf6403d438fe3be4a8f19e599e5aebd87674be54a") )
        ( 1210000, uint256("0x8bd87d74bc13df35504c4b8d5d091a9ff25111e0b78f5f694b0b66b591a7de9b") )
        ( 1500000, uint256("0x188abe5b815d27f3d02ba59efc8b600ff21607a27d29bed5c2e5cc996b702b04") )
        ( 1750000, uint256("0xf702251416b21ac460830b469367d90aa493007b3c1f2ae5bd6cd605e80c2403") )
        ( 1840000, uint256("0x69c472731a4d4815f1689b571c0943068be7e074d71083d280a0d83538c40b2e") )
        ( 1848418, uint256("0x5c4effd70e19cbe733119a21578121f74f88013ea7843d0a1bc77d31f82062da") )
        ( 1848420, uint256("0x414eedde45efae3607abd5daa800b16a5a8e4e0da6349c3cd50273bbed4aed90") )
        ( 2000000, uint256("0x9ecac25841b459effd32198d8b315a90266cafd80d2e137b7d212df4b955afd5") )
        ( 2250000, uint256("0x00000000020bd81f16b6735604e1cfcfdc48e33452fb1efbe12f0b57d291b1f6") )
        ( 2500000, uint256("0x000000000789135978f2518fffe7f7e1c514713ffffa4b6377097a74b41d85fe") )
        ( 2750000, uint256("0x00000000487bc235e814fa7449a4ddd4fe8e3a952be2e85e224cbf6e0dba7801") )
        ( 3000000, uint256("0x000000000371241dd6d41b824653a2bcfcf245cef68103e2627c4db900d3a057") )
        ( 4000000, uint256("0x0000000017616bc0c07d126eee9148634ba408772e06a0423f8d08da3458c7dd") )
        ( 4250000, uint256("0x00000000049b6e69fc658a8031fb6f56b87c46dd06680ed979898cf002be7d03") )
        ( 4500000, uint256("0x00000000090633f2e4c2c69ee88c441b44e4c4e93ebbe82e2804e49b06dd6bb8") )
        ( 4750000, uint256("0x00000007975cd663b5fe9e846a6da1ce1a82fb38962a2c81fe1c2dcf5217d10a") )
        ( 5000000, uint256("0x00000000fe96fc559e716b99d3ba4fafbb961154b3f9a73d5ad30db22a0d860a") )
        ( 5250000, uint256("0xc733eb696b8444fb8a9bdb59aca0e6b233fd9e20450b152de574590b0343934a") )
        ( 5500000, uint256("0x00000000ae6f26383b0cd2dc97599249cddb0fc660817ab6a267d1cd739dab57") )
        ( 5750000, uint256("0x00000000c4b7a28e0fb2b69a23a886a13841d107461373c63f7d90e56fad2864") )
        ( 6000000, uint256("0x00000003da828e507f76dfdcd210ec60c1badf55c9320119631328eec3a9302c") )
        ( 6250000, uint256("0x000000d85b4e0798609a31c9e9272e0e1f776cc9ce61d2bc516eaba2068685f0") )
        ( 6500000, uint256("0x0000000072362d64a180a6f36407b0b0ba014d9146d7e67899a6b6a42643d38f") )
        ( 6750000, uint256("0x000000a86c88b161a6c013963911d4bcd5e6a65fd7ba81c3474f5cfa1a384c64") )
	;

  static const CCheckpointData data = {
        &mapCheckpoints,
        1737676800, // * UNIX timestamp of last checkpoint block (approx Jan 24, 2025)
        8500000,    // * total number of transactions between genesis and last checkpoint
                    //   (the tx=... number in the SetBestChain debug.log lines)
        5000.0      // * estimated number of transactions per day after checkpoint
    };

    // TestNet has no checkpoints
    MapCheckpoints mapCheckpointsTestnet =
        boost::assign::map_list_of
        ( 0, hashGenesisBlockTestNet )
    ;

    static const CCheckpointData dataTestnet = {
        &mapCheckpointsTestnet,
        1779753600,
        0,
        0
    };

    const CCheckpointData &Checkpoints() {
        if (fTestNet)
            return dataTestnet;
        else
            return data;
    }

    bool CheckHardened(int nHeight, const uint256& hash)
    {
        if (fRegTest) return true; // Regtest has no checkpoints
        MapCheckpoints& checkpoints = (fTestNet ? mapCheckpointsTestnet : mapCheckpoints);

        MapCheckpoints::const_iterator i = checkpoints.find(nHeight);
        if (i == checkpoints.end()) return true;
        return hash == i->second;
    }

    int GetTotalBlocksEstimate()
    {
        MapCheckpoints& checkpoints = (fTestNet ? mapCheckpointsTestnet : mapCheckpoints);

		//if (checkpoints.empty())
            //return 0;
        return checkpoints.rbegin()->first;
    }

    CBlockIndex* GetLastCheckpoint(const std::map<uint256, CBlockIndex*>& mapBlockIndex)
    {
        MapCheckpoints& checkpoints = (fTestNet ? mapCheckpointsTestnet : mapCheckpoints);

        BOOST_REVERSE_FOREACH(const MapCheckpoints::value_type& i, checkpoints)
        {
            const uint256& hash = i.second;
            std::map<uint256, CBlockIndex*>::const_iterator t = mapBlockIndex.find(hash);
            if (t != mapBlockIndex.end())
                return t->second;
        }
        return NULL;
    }

    // ppcoin: synchronized checkpoint (centrally broadcasted)
    uint256 hashSyncCheckpoint = 0;
    uint256 hashPendingCheckpoint = 0;
    CSyncCheckpoint checkpointMessage;
    CSyncCheckpoint checkpointMessagePending;
    uint256 hashInvalidCheckpoint = 0;
    CCriticalSection cs_hashSyncCheckpoint;

    // ppcoin: get last synchronized checkpoint
    CBlockIndex* GetLastSyncCheckpoint()
    {
        LOCK(cs_hashSyncCheckpoint);
        if (!mapBlockIndex.count(hashSyncCheckpoint))
            error("GetSyncCheckpoint: block index missing for current sync-checkpoint %s", hashSyncCheckpoint.ToString().c_str());
        else
            return mapBlockIndex[hashSyncCheckpoint];
        return NULL;
    }

    // ------------------------------------------------------------------
    // V2-R2B (FIX A + FIX B): synchronized checkpoint under
    // BY_VALUE_AUTHORITATIVE startup.
    //
    // ROOT CAUSE (production incident): the V2 authoritative startup bypasses
    // CTxDB::LoadBlockIndex (which is where the legacy boot read the persisted
    // sync-checkpoint), so Checkpoints::hashSyncCheckpoint stayed 0; once
    // IsInitialBlockDownload() became false, CheckSync()'s non-IBD branch did
    // mapBlockIndex.count(hashSyncCheckpoint) -> absent -> "CheckSync:
    // sync-checkpoint not in block index" -> EVERY candidate block was
    // rejected, the tip froze and every miner thread went idle.
    //
    // FIX A loads the persisted value. FIX B makes the checkpoint predicates
    // resolve through the authoritative by-value resolver instead of
    // mapBlockIndex residency (only a bounded live tail survives
    // RetireBlockIndexBelowFloor). Semantics are preserved exactly:
    //   * only a descendant of the current sync-checkpoint may pass;
    //   * the same height requires hash equality;
    //   * a lower height requires the block to be a KNOWN index record.
    // The legacy resident path is used whenever authoritative startup is NOT
    // active and is otherwise byte-for-byte unchanged.
    // ------------------------------------------------------------------

    // FIX A: typed load of the persisted synchronized checkpoint.
    SyncCheckpointLoadStatus LoadPersistedSyncCheckpoint(uint256& hashOut, std::string* error)
    {
        if (error) error->clear();
        hashOut = uint256(0);
        // Read-only handle on the legacy tx-leveldb (still an ACTIVE runtime
        // database for its non-block-index namespaces). A fresh datadir has no
        // such key -> SYNC_CHECKPOINT_ABSENT, which is the canonical ppcoin
        // "no synchronized checkpoint" value and NOT a genesis substitution.
        // ROUND-3 P3b: this is a READ-ONLY boot probe. The CTxDB constructor
        // (init_blockindex) throws runtime_error on a leveldb open error and,
        // inside the version-mismatch dance, can RENAME + RECREATE the whole
        // legacy store -- neither is acceptable on this read-only path, where
        // an uncaught exception aborted the process in the previous code.
        // Catch ALL exceptions from the handle construction, do not trigger the
        // backup/reset dance (no version key was written), and report the
        // failure gracefully: the caller (init.cpp) turns it into an InitError
        // and startup fails loudly but gracefully.
        try
        {
            // S2.0 BI-V2 authoritative startup: the persisted synced-checkpoint
            // scalar is consensus-critical and must survive restart. P2 (S4):
            // in V2 store mode it is served from the V2-native scalar home
            // (chainstate/sync-checkpoint), so this boot probe NEVER opens the
            // legacy LevelDB — a clean V2 datadir gets NO txleveldb created. In
            // legacy (non-V2) mode keep the old behavior.
            if (g_fAuthoritativeStartup)
            {
                const SyncCheckpointLoadStatus vs = ReadV2SyncCheckpointScalar(hashOut, error);
                if (vs == SYNC_CHECKPOINT_CORRUPT && error)
                    *error = "V2 persisted hashSyncCheckpoint scalar is present but not decodable";
                return vs;
            }
            s16::V2LegacyIndexProbeScope probe;
            CTxDB txdb("r");
            const SyncCheckpointLoadStatus st = txdb.LoadSyncCheckpoint(hashOut);
            if (st == SYNC_CHECKPOINT_CORRUPT && error)
                *error = "persisted hashSyncCheckpoint entry is present but not decodable";
            return st;
        }
        catch (const std::exception& e)
        {
            hashOut = uint256(0);
            if (error)
                *error = std::string("legacy tx-leveldb open/read failed: ") + e.what();
            return SYNC_CHECKPOINT_CORRUPT;
        }
        catch (...)
        {
            hashOut = uint256(0);
            if (error)
                *error = "legacy tx-leveldb open/read failed (unknown exception)";
            return SYNC_CHECKPOINT_CORRUPT;
        }
    }

    // ------------------------------------------------------------------
    // V2-R3 (ROUND-2 P0): one-time chain-CREATION checkpoint establishment.
    //
    // LEGACY PARITY, NOT A FALLBACK. Legacy LoadBlockIndex established the
    // synchronized checkpoint exactly ONCE, in its mapBlockIndex.empty()
    // branch, right after the genesis was accepted:
    //     WriteSyncCheckpoint(hashGenesis);   // main.cpp:10120-10121
    // It was an explicit, DURABLE, persisted write gated on chain creation;
    // every later boot LOADED the persisted value instead. The V2
    // authoritative path performs the same creation work through the EMPTY_NEW
    // native bootstrap but historically never performed the checkpoint half,
    // so a fresh datadir booted with hashSyncCheckpoint == 0 forever (the
    // fresh-boot freeze).
    //
    // EstablishGenesisCheckpoint reproduces the legacy CREATION event with
    // strict, independently-verified preconditions. It can only WRITE when
    // (a) the just-loaded persisted value is ABSENT (a real persisted
    // checkpoint can never be overwritten), (b) the immutable base tip is the
    // genesis anchor at height 0, and (c) the published active best is that
    // SAME genesis anchor. Any other combination is a no-op: LOADED -> the
    // persisted authority stands; a non-empty chain -> the RESIDUAL state is
    // reported as RESIDUAL_ABSENT_ON_NONEMPTY and the caller boots with the
    // canonical 0 under a LOUD warning (documented pass-through choice: see
    // init.cpp block comment and the establishment commit message).
    // ------------------------------------------------------------------
    SyncCheckpointEstablishmentResult EstablishGenesisCheckpoint(
        const SyncCheckpointLoadStatus loadStatus,
        const uint256& hashGenesis,
        const int32_t baseTipHeight,
        const uint256& currentBestHash,
        std::string* error)
    {
        if (error) error->clear();
        if (hashGenesis == uint256(0))
        {
            if (error)
                *error = "genesis checkpoint establishment: caller supplied a zero genesis hash";
            return SYNC_CHECKPOINT_ESTABLISHMENT_REJECTED_PRECONDITION;
        }
        if (loadStatus != SYNC_CHECKPOINT_ABSENT)
            // LOADED (or CORRUPT, which the caller fails on before calling):
            // a persisted synchronized checkpoint exists; the creation event
            // is structurally impossible and must not fire.
            return SYNC_CHECKPOINT_ESTABLISHMENT_NOT_APPLICABLE;
        if (baseTipHeight < 0)
            // Not authoritative / base not bound: out of scope (defensive;
            // the caller only invokes this on the V2 authoritative path).
            return SYNC_CHECKPOINT_ESTABLISHMENT_NOT_APPLICABLE;

        if (baseTipHeight > 0 || currentBestHash != hashGenesis)
        {
            // RESIDUAL STATE: a chain exists but no persisted checkpoint.
            // Legacy created the persisted genesis checkpoint in the same
            // step as chain creation, so this pairing is unreachable through
            // either legacy or the round-2 creation path (it can only arise
            // from an exotic out-of-band store loss). The DOCUMENTED choice
            // is a loud-warning pass-through: hashSyncCheckpoint stays at the
            // canonical 0 ("no synchronized checkpoint"), no genesis
            // substitution, no re-creation. NEVER silently.
            printf("ROUND2_P0 WARNING: chain at best height %d is non-empty but no persisted "
                   "synchronized checkpoint exists; hashSyncCheckpoint stays 0 "
                   "(canonical no synchronized checkpoint; NO genesis substitution, "
                   "NO re-creation). This state must be repaired operationally -- "
                   "it is not recoverable by this code path.\n",
                   baseTipHeight);
            fflush(stdout);
            return SYNC_CHECKPOINT_ESTABLISHMENT_RESIDUAL_ABSENT_ON_NONEMPTY;
        }

        // GENUINELY NEW CHAIN: the immutable base tip is the genesis anchor
        // (height 0) and the published active best is that same anchor. This
        // is the exact legacy mapBlockIndex.empty() creation moment. Overlay
        // the legacy creation semantics: an explicit, transactional, PERSISTED
        // WriteSyncCheckpoint(hashGenesis) plus the in-process publication.
        if (!WriteSyncCheckpoint(hashGenesis))
        {
            if (error)
                *error = "genesis checkpoint establishment: failed to persist hashSyncCheckpoint";
            return SYNC_CHECKPOINT_ESTABLISHMENT_ERROR;
        }
        printf("ROUND2_P0: fresh chain created; genesis synchronized checkpoint established "
               "and persisted (legacy creation parity): %s\n",
               hashGenesis.ToString().c_str());
        fflush(stdout);
        return SYNC_CHECKPOINT_ESTABLISHMENT_CREATED;
    }

    namespace {

    // ---------------------------------------------------------------------
    // ROUND-3 P2a: bounded, memoized checkpoint ancestry support.
    //
    // CheckSync runs for EVERY accepted candidate. Previously, a NOT_ACTIVE
    // parent made CheckpointAncestorAtHeight issue one authoritative resolver
    // call per height step down to the checkpoint height while holding
    // cs_hashSyncCheckpoint -- O(depth) reads per candidate. Two memo tables
    // bound that cost:
    //   * SyncCheckpointMemoEntry: per-checkpoint-hash+height resolution of the
    //     sync-checkpoint snapshot for the CURRENT hashSyncCheckpoint epoch
    //     (invalidated when hashSyncCheckpoint itself changes);
    //   * mapDescendantVerdict: per-parent-hash descendant-of-checkpoint
    //     verdicts (VALID/DEFINITIVE). A failed ancestry walk returns
    //     fail-closed BEFORE MemoizeDescendantVerdict, so it is deliberately
    //     NOT memoized and is re-probed on the next attempt (a transient
    //     authority failure must not be frozen into a verdict).
    // Both structures are only touched under cs_hashSyncCheckpoint. The cache
    // is plausibly bounded per epoch by the candidate frontier; the epoch tag
    // ensures a checkpoint advance cannot ever serve stale verdicts.
    // ---------------------------------------------------------------------
    // ROUND-5 TASK B: the ancestry walk is bounded by the START HEIGHT, never
    // by an arbitrary maximum distance. A block's predecessor sits at height-1
    // in the same authoritative record stream, so a VALID walk from `start`
    // down to any target takes at most start.height steps; a step count above
    // start.height can only come from a corrupt/looping structure. The walk
    // keeps one snapshot at a time (O(1) auxiliary memory), so it needs no
    // depth cap to bound memory. The previous fixed budget of 4096 could REJECT
    // a valid descendant/checkpoint farther than 4096 blocks -- a divergence
    // from legacy, whose pprev walk was unbounded (source: original legacy
    // ValidateSyncCheckpoint / CheckSync pprev loops).

    struct SyncCheckpointMemoEntry
    {
        uint256 hashCheckpoint;   // epoch tag: hashSyncCheckpoint at fill time
        BlockIndexSnapshot snapshot; // resolved checkpoint record
        int syncHeight;           // resolved checkpoint height (-1 = memoized failure)
        uint256 syncHash;
        std::string failureDetail;
        bool resolved;
        bool transientFailure;    // failed lookup was TRANSIENT (retryable)

        SyncCheckpointMemoEntry()
            : hashCheckpoint(0), syncHeight(-1), syncHash(0), resolved(false),
              transientFailure(false) {}
    };

    SyncCheckpointMemoEntry g_syncCheckpointMemo;
    std::map<uint256, bool> mapDescendantVerdict;
    uint256 g_descendantMemoEpoch; // hashSyncCheckpoint when verdicts were valid
    int g_descendantMemoSteps = 0; // total resolver steps served from the walk (diagnostics)

    // ROUND-5 TASK C: bound the per-epoch descendant-verdict cache. A verdict is
    // a pure function of (epoch checkpoint, parent hash) and is always
    // recomputable, so eviction is a PERFORMANCE-only event and can never change
    // a consensus result. At the cap the map is cleared wholesale (no partial
    // eviction ordering to reason about): memory is bounded to
    // CHECKPOINT_DESCENDANT_VERDICT_CAP entries and the recompute cost is at
    // most one re-probe per cleared verdict. The epoch tag still clears the map
    // on every hashSyncCheckpoint change, so no stale verdict survives an epoch
    // change. Worst-case memory: std::map<uint256,bool> node ~= 80-96 B
    // (32-byte key + bool + RB-tree overhead) => 8192 entries ~= 0.8 MiB.
    static const size_t CHECKPOINT_DESCENDANT_VERDICT_CAP = 8192;
    size_t g_descendantVerdictCap = CHECKPOINT_DESCENDANT_VERDICT_CAP; // test-overridable

    // The single insert point for the per-epoch descendant-verdict cache; it
    // enforces the bounded-memory policy. Called only under cs_hashSyncCheckpoint.
    void MemoizeDescendantVerdict(const uint256& parentHash, bool verdict)
    {
        if (g_descendantVerdictCap != 0 &&
            mapDescendantVerdict.size() >= g_descendantVerdictCap)
            mapDescendantVerdict.clear(); // performance-only; verdicts are recomputable
        mapDescendantVerdict.insert(std::make_pair(parentHash, verdict));
    }

    // Per (checkpoint epoch, best-tip identity) verdict of "the active best
    // chain extends the synchronized checkpoint". Paid once per tip advance
    // with a single O(1) resolver probe (active block at the sync height);
    // obviously NOT a per-candidate walk. Keyed by the best-tip HASH because
    // the active chain can fork within one checkpoint epoch, and a forked tip
    // may have a different anchor at the sync height.
    struct BestExtendsCheckpointMemo
    {
        bool fValid;
        uint256 epochCheckpoint;
        uint256 bestTip;
        bool bestExtends;
        BestExtendsCheckpointMemo()
            : fValid(false), epochCheckpoint(0), bestTip(0), bestExtends(false) {}
    };

    BestExtendsCheckpointMemo g_bestExtendsMemo;

    // Memoized per-epoch authoritative resolution of the CURRENT synchronized
    // checkpoint. Returns the resolver result; on FOUND/NOT_ACTIVE fills *out.
    // Epoch tag = hashSyncCheckpoint, so a checkpoint advance never serves a
    // stale snapshot. All callers hold cs_hashSyncCheckpoint.
    //
    // ROUND-4 P2 (sticky-memo discipline): a resolver result is memoized ONLY
    // when it is definitive — FOUND and NOT_ACTIVE (a valid snapshot), or
    // NOT_FOUND where the by-hash resolver itself reports a genuine immutable
    // miss. AUTHORITATIVE_BLOCK_AUTHORITY_FAILURE is TRANSIENT (authority /
    // storage hiccup): it fails this call closed but is NOT cached, so the
    // next caller retries instead of inheriting a frozen negative verdict for
    // the whole epoch.
    AuthoritativeBlockResolutionResult ResolveSyncCheckpointSnapMemoized(
        BlockIndexSnapshot* out, std::string* err)
    {
        // ROUND-5 TASK A: a memoized TRANSIENT failure must NOT be served as a
        // verdict. The previous implementation returned the cached
        // AUTHORITATIVE_BLOCK_AUTHORITY_FAILURE from here (resolved=true,
        // transientFailure=true, syncHeight<0), so after ONE transient resolver
        // hiccup the SAME checkpoint never re-probed the resolver until
        // hashSyncCheckpoint changed: a stale fail-closed FREEZE at an unchanged
        // checkpoint/tip (the audit's sticky-transient defect on the BY-HASH
        // path). A memoized transient failure is therefore treated as NOT
        // memoized: the memo is cleared so the block below RETRIES the resolver
        // on this very call. A DEFINITIVE outcome (FOUND / NOT_ACTIVE, or
        // NOT_FOUND from a COMPLETED by-hash lookup) is still cached and is never
        // re-probed; a repeated transient failure fails this call closed again
        // (and remains retryable).
        if (g_syncCheckpointMemo.resolved &&
            g_syncCheckpointMemo.hashCheckpoint == hashSyncCheckpoint &&
            g_syncCheckpointMemo.syncHeight < 0 &&
            g_syncCheckpointMemo.transientFailure)
        {
            g_syncCheckpointMemo = SyncCheckpointMemoEntry(); // force a fresh resolve
        }
        if (!g_syncCheckpointMemo.resolved ||
            g_syncCheckpointMemo.hashCheckpoint != hashSyncCheckpoint)
        {
            g_syncCheckpointMemo = SyncCheckpointMemoEntry();
            std::string resolveErr;
            const AuthoritativeBlockResolutionResult r =
                ResolveAuthoritativeBlockSnapshotR(hashSyncCheckpoint,
                                                   &g_syncCheckpointMemo.snapshot,
                                                   &resolveErr);
            g_syncCheckpointMemo.hashCheckpoint = hashSyncCheckpoint;
            if (r == AUTHORITATIVE_BLOCK_FOUND || r == AUTHORITATIVE_BLOCK_NOT_ACTIVE)
            {
                g_syncCheckpointMemo.syncHeight = g_syncCheckpointMemo.snapshot.height;
                g_syncCheckpointMemo.transientFailure = false;
            }
            else if (r == AUTHORITATIVE_BLOCK_AUTHORITY_FAILURE)
            {
                // Transient: record it so the CALLER can fail closed, but do
                // not persist a definitive negative verdict for the epoch.
                g_syncCheckpointMemo.syncHeight = -1;
                g_syncCheckpointMemo.failureDetail = resolveErr;
                g_syncCheckpointMemo.transientFailure = true;
            }
            else
            {
                g_syncCheckpointMemo.syncHeight = -1;
                g_syncCheckpointMemo.failureDetail = resolveErr;
                g_syncCheckpointMemo.transientFailure = false;
            }
            g_syncCheckpointMemo.resolved = true;
        }
        if (g_syncCheckpointMemo.syncHeight < 0)
        {
            if (err)
                *err = g_syncCheckpointMemo.failureDetail;
            return g_syncCheckpointMemo.transientFailure
                ? AUTHORITATIVE_BLOCK_AUTHORITY_FAILURE
                : AUTHORITATIVE_BLOCK_NOT_FOUND;
        }
        *out = g_syncCheckpointMemo.snapshot;
        return AUTHORITATIVE_BLOCK_FOUND;
    }

    // True when the hash is a KNOWN authoritative index record. Mirrors the
    // legacy mapBlockIndex.count() membership test, which counts ACTIVE and
    // non-active (side) records alike; FOUND and NOT_ACTIVE are both "known".
    bool IsAuthoritativeIndexedRecord(const uint256& hash)
    {
        BlockIndexSnapshot snap;
        std::string err;
        const AuthoritativeBlockResolutionResult r =
            ResolveAuthoritativeBlockSnapshotR(hash, &snap, &err);
        return r == AUTHORITATIVE_BLOCK_FOUND || r == AUTHORITATIVE_BLOCK_NOT_ACTIVE;
    }

    // True when the hash is the authoritative ACTIVE record at its height (the
    // checkpoint is accepted on this node's authoritative active chain).
    bool IsAuthoritativeActiveRecord(const uint256& hash)
    {
        BlockIndexSnapshot snap;
        std::string err;
        return ResolveAuthoritativeBlockSnapshotR(hash, &snap, &err) ==
               AUTHORITATIVE_BLOCK_FOUND;
    }

    // Walk the persisted authoritative ancestry (hashPrev links) of *start*
    // down to targetHeight. Fail closed on a missing / inconsistent /
    // unavailable step, exactly like the legacy NULL-pprev exhaustion. On
    // success *out is the ancestry record at targetHeight.
    bool CheckpointAncestorAtHeight(const BlockIndexSnapshot& start, int targetHeight,
                                    BlockIndexSnapshot* out, std::string* error,
                                    bool* pTransientFailure = NULL)
    {
        if (pTransientFailure) *pTransientFailure = false;
        // ROUND-5 TASK B: memory-bounded (one snapshot at a time) and
        // distance-UNBOUNDED ancestry walk with an INVARIANT guard. Each step
        // strictly decreases the height by one, so a valid walk can never take
        // more than start.height steps; a larger count means the persisted
        // structure is corrupt rather than a legitimate long-distance
        // checkpoint. Missing / corrupt / transiently-unavailable / inconsistent
        // steps still fail closed below, exactly as before.
        BlockIndexSnapshot cur = start;
        const int64_t maxSteps = (int64_t)start.height;
        int64_t steps = 0;
        while (cur.height > targetHeight)
        {
            if (++steps > maxSteps)
            {
                if (error) *error = "ancestry walk exceeded the maximum possible depth " +
                                    itostr(maxSteps) +
                                    " from " + start.hash.GetHex() + " (corrupt ancestry)";
                return false;
            }
            if (!cur.hasParent || cur.height <= 0)
            {
                if (error) *error = "pprev null - block index structure failure";
                return false;
            }
            BlockIndexSnapshot parent;
            std::string perr;
            const AuthoritativeBlockResolutionResult r =
                ResolveAuthoritativeBlockSnapshotR(cur.hashPrev, &parent, &perr);
            if (r == AUTHORITATIVE_BLOCK_AUTHORITY_FAILURE)
            {
                // ROUND-6 TASK 3: a TRANSIENT authority/storage failure is not a
                // verdict about the ancestry; report it so a caller can RETAIN a
                // pending checkpoint instead of clearing it.
                if (pTransientFailure) *pTransientFailure = true;
                if (error) *error = "ancestry step transiently unavailable at " + cur.hash.GetHex() +
                                    ": " + (perr.empty() ? std::string("(no detail)") : perr);
                return false;
            }
            if (r != AUTHORITATIVE_BLOCK_FOUND && r != AUTHORITATIVE_BLOCK_NOT_ACTIVE)
            {
                if (error) *error = "ancestry step unavailable at " + cur.hash.GetHex() +
                                    ": " + (perr.empty() ? std::string("(no detail)") : perr);
                return false;
            }
            if (parent.height != cur.height - 1)
            {
                if (error) *error = "ancestry height inconsistency at " + cur.hash.GetHex();
                return false;
            }
            cur = parent;
        }
        *out = cur;
        return true;
    }

    } // namespace

    // ROUND-4 P2: test-only memo invalidation seam (see checkpoints.h; used by
    // checkpoint_v2_authoritative_tests.cpp). Clears the per-tip extends-memo,
    // the per-parent descendant-verdict map and the per-epoch checkpoint
    // resolution memo WITHOUT touching hashSyncCheckpoint, so a test can
    // replay the memo's RETRY path from a known-EMPTY state. All three live in
    // the anonymous namespace above and are only ever mutated under
    // cs_hashSyncCheckpoint; this debug/test-only writer honours the same
    // locking discipline. Production startup never calls it.
    void ResetCheckpointMemoForTest(const uint256& tipHash, const uint256& epochCheckpoint)
    {
        LOCK(cs_hashSyncCheckpoint);
        g_bestExtendsMemo = BestExtendsCheckpointMemo();
        mapDescendantVerdict.clear();
        g_descendantMemoEpoch = epochCheckpoint;
        g_descendantMemoSteps = 0;
        g_syncCheckpointMemo = SyncCheckpointMemoEntry();
        (void)tipHash; // tip hash documents the intended (epoch, tip) replay target
    }

    // ROUND-5 TASK C test seams (production never calls them): observe and
    // override the bounded descendant-verdict cache so a test can exercise
    // saturation / eviction / recomputation / epoch invalidation cheaply.
    void SetCheckpointDescendantVerdictCapForTest(size_t cap)
    {
        LOCK(cs_hashSyncCheckpoint);
        g_descendantVerdictCap = cap;
    }
    size_t GetCheckpointDescendantVerdictCapForTest()
    {
        LOCK(cs_hashSyncCheckpoint);
        return g_descendantVerdictCap;
    }
    size_t DescendantVerdictCacheSizeForTest()
    {
        LOCK(cs_hashSyncCheckpoint);
        return mapDescendantVerdict.size();
    }

    static int g_pendingValidationTransientFailForTest = 0;
    void SetPendingValidationTransientFailForTest(int count)
    {
        g_pendingValidationTransientFailForTest = (count > 0) ? count : 0;
    }
    // ROUND-6 TASK 3: tri-state by-value descendant/conflict validation
    // (legacy ValidateSyncCheckpoint). A DEFINITIVE verdict (unknown /
    // conflicting / non-descendant / older) is distinguished from a TRANSIENT
    // authority/storage failure, so the pending-checkpoint consumer never clears
    // a known pending checkpoint because of a transient hiccup. The bool wrapper
    // below preserves the original acceptance semantics for existing callers.
    SyncCheckpointValidationOutcome ValidateSyncCheckpointDetailed(uint256 hashCheckpoint)
    {
        LOCK(cs_hashSyncCheckpoint);
        // The canonical zero checkpoint is never an index record: legacy did
        // mapBlockIndex.count(0)==0 -> error -> false, which the pending-checkpoint
        // consumer treats as DEFINITIVE (clear). Never classify it as transient.
        if (hashSyncCheckpoint == uint256(0))
        {
            error("ValidateSyncCheckpoint: block index missing for current sync-checkpoint %s",
                  hashSyncCheckpoint.ToString().c_str());
            return SYNC_CHECKPOINT_DEFINITIVE_REJECT;
        }
        if (g_pendingValidationTransientFailForTest > 0)
        {
            --g_pendingValidationTransientFailForTest;              // ROUND-6 test seam
            error("ValidateSyncCheckpoint: transient failure resolving current sync-checkpoint %s",
                  hashSyncCheckpoint.ToString().c_str());
            return SYNC_CHECKPOINT_TRANSIENT_FAILURE;
        }
        // Current synchronized checkpoint (may be a non-resident historical record).
        BlockIndexSnapshot syncSnap;
        std::string syncErr;
        const AuthoritativeBlockResolutionResult syncRes =
            ResolveAuthoritativeBlockSnapshotR(hashSyncCheckpoint, &syncSnap, &syncErr);
        if (syncRes == AUTHORITATIVE_BLOCK_AUTHORITY_FAILURE)
        {
            error("ValidateSyncCheckpoint: transient failure resolving current sync-checkpoint %s",
                  hashSyncCheckpoint.ToString().c_str());
            return SYNC_CHECKPOINT_TRANSIENT_FAILURE;
        }
        if (syncRes == AUTHORITATIVE_BLOCK_NOT_FOUND)
        {
            error("ValidateSyncCheckpoint: block index missing for current sync-checkpoint %s",
                  hashSyncCheckpoint.ToString().c_str());
            return SYNC_CHECKPOINT_DEFINITIVE_REJECT;
        }

        // Received synchronized checkpoint.
        BlockIndexSnapshot recvSnap;
        std::string recvErr;
        const AuthoritativeBlockResolutionResult recvRes =
            ResolveAuthoritativeBlockSnapshotR(hashCheckpoint, &recvSnap, &recvErr);
        if (recvRes == AUTHORITATIVE_BLOCK_AUTHORITY_FAILURE)
        {
            error("ValidateSyncCheckpoint: transient failure resolving received sync-checkpoint %s",
                  hashCheckpoint.ToString().c_str());
            return SYNC_CHECKPOINT_TRANSIENT_FAILURE;
        }
        if (recvRes == AUTHORITATIVE_BLOCK_NOT_FOUND)
        {
            error("ValidateSyncCheckpoint: block index missing for received sync-checkpoint %s",
                  hashCheckpoint.ToString().c_str());
            return SYNC_CHECKPOINT_DEFINITIVE_REJECT;
        }

        if (recvSnap.height <= syncSnap.height)
        {
            // Received an older checkpoint, trace back from current checkpoint
            // to the same height of the received checkpoint to verify that the
            // current checkpoint is a descendant block.
            BlockIndexSnapshot anc;
            std::string aerr;
            bool aTransient = false;
            if (!CheckpointAncestorAtHeight(syncSnap, recvSnap.height, &anc, &aerr, &aTransient))
            {
                error("ValidateSyncCheckpoint: pprev null - block index structure failure");
                return aTransient ? SYNC_CHECKPOINT_TRANSIENT_FAILURE : SYNC_CHECKPOINT_DEFINITIVE_REJECT;
            }
            if (anc.hash != hashCheckpoint)
            {
                hashInvalidCheckpoint = hashCheckpoint;
                error("ValidateSyncCheckpoint: new sync-checkpoint %s is conflicting with current sync-checkpoint %s",
                      hashCheckpoint.ToString().c_str(), hashSyncCheckpoint.ToString().c_str());
                return SYNC_CHECKPOINT_DEFINITIVE_REJECT;
            }
            return SYNC_CHECKPOINT_DEFINITIVE_REJECT; // older checkpoint (legacy clears the pending)
        }

        // Received checkpoint should be a descendant block of the current
        // checkpoint. Trace back to the same height of current checkpoint.
        BlockIndexSnapshot anc;
        std::string aerr;
        bool aTransient = false;
        if (!CheckpointAncestorAtHeight(recvSnap, syncSnap.height, &anc, &aerr, &aTransient))
        {
            error("ValidateSyncCheckpoint: pprev2 null - block index structure failure");
            return aTransient ? SYNC_CHECKPOINT_TRANSIENT_FAILURE : SYNC_CHECKPOINT_DEFINITIVE_REJECT;
        }
        if (anc.hash != hashSyncCheckpoint)
        {
            hashInvalidCheckpoint = hashCheckpoint;
            error("ValidateSyncCheckpoint: new sync-checkpoint %s is not a descendant of current sync-checkpoint %s",
                  hashCheckpoint.ToString().c_str(), hashSyncCheckpoint.ToString().c_str());
            return SYNC_CHECKPOINT_DEFINITIVE_REJECT;
        }
        return SYNC_CHECKPOINT_VALID;
    }

    bool ValidateSyncCheckpointByValue(uint256 hashCheckpoint)
    {
        return ValidateSyncCheckpointDetailed(hashCheckpoint) == SYNC_CHECKPOINT_VALID;
    }

    // FIX B: by-value non-IBD CheckSync (legacy CheckSync else-branch).
    // ROUND-3 P2a: O(1) on the common path where the candidate's parent is the
    // published active best tip: that chain extends the sync-checkpoint iff the
    // ACTIVE authoritative block at the checkpoint height IS the checkpoint --
    // one bounded resolver probe memoized per (checkpoint epoch, tip hash).
    // Descendant verdicts (including rejection) are memoized per parent hash
    // for the epoch. The bounded-fail-closed ancestry walk runs only for a
    // new/side parent.
    bool CheckSyncByValue(const uint256& hashBlock, const CBlockIndex* pindexPrev)
    {
        LOCK(cs_hashSyncCheckpoint);
        if (pindexPrev == NULL)
            return error("CheckSync: no previous block to evaluate against sync-checkpoint");
        const int nHeight = pindexPrev->nHeight + 1;

        // Memoized per-epoch authoritative resolution of the CURRENT synchronized
        // checkpoint.
        BlockIndexSnapshot syncSnap;
        std::string syncErr;
        const AuthoritativeBlockResolutionResult syncRes =
            ResolveSyncCheckpointSnapMemoized(&syncSnap, &syncErr);
        if (syncRes != AUTHORITATIVE_BLOCK_FOUND && syncRes != AUTHORITATIVE_BLOCK_NOT_ACTIVE)
            return error("CheckSync: sync-checkpoint not in block index (%s)",
                         hashSyncCheckpoint.ToString().c_str());
        const int nSyncHeight = syncSnap.height;

        // Checkpoint epoch for both verdict memos: invalidated whenever
        // hashSyncCheckpoint changes.
        if (g_descendantMemoEpoch != hashSyncCheckpoint)
        {
            mapDescendantVerdict.clear();
            g_bestExtendsMemo = BestExtendsCheckpointMemo();
            g_descendantMemoEpoch = hashSyncCheckpoint;
            g_descendantMemoSteps = 0;
        }

        if (nHeight < nSyncHeight)
            // lower height than sync-checkpoint: only a KNOWN index record passes
            return IsAuthoritativeIndexedRecord(hashBlock);

        if (nHeight == nSyncHeight)
            return hashBlock == hashSyncCheckpoint;

        // nHeight > nSyncHeight: only a DESCENDANT of the sync-checkpoint may
        // pass.

        // O(1) memo hit: a parent already proven to be (or rejected as NOT) a
        // descendant keeps its verdict for this checkpoint epoch.
        const uint256 hashPrev = pindexPrev->GetBlockHash();
        std::map<uint256, bool>::iterator mv = mapDescendantVerdict.find(hashPrev);
        if (mv != mapDescendantVerdict.end())
            return mv->second;

        // O(1) common path: the parent IS the published active best tip. That
        // chain extends the sync-checkpoint iff the ACTIVE authoritative block
        // at nSyncHeight is the checkpoint itself -- one bounded resolver
        // probe, memoized per (checkpoint epoch, tip identity): within one
        // epoch the verdict only changes when the best chain forks (a different
        // tip may have a different anchor at the sync height), so the tip hash
        // is part of the memo key.
        //
        // ROUND-4 P2 (sticky-memo discipline): the probe result is memoized
        // ONLY when it is DEFINITIVE. AuthoritativeGetActiveSnapshotByHeightR
        // distinguishes a real miss (NOT_FOUND: the probed active chain does
        // not anchor at the checkpoint) from a TRANSIENT authority failure
        // (AUTHORITY_FAILURE: storage/resolver hiccup). Both fail closed for
        // THIS call (enforcement is never weakened), but a transient failure
        // leaves this memo invalid AND mapDescendantVerdict untouched, so the
        // next caller for the same tip retries the probe instead of inheriting
        // a frozen false verdict for the whole epoch.
        if (hashPrev == hashBestChain)
        {
            if (!g_bestExtendsMemo.fValid ||
                g_bestExtendsMemo.epochCheckpoint != hashSyncCheckpoint ||
                g_bestExtendsMemo.bestTip != hashPrev)
            {
                BlockIndexSnapshot anchor;
                std::string anchorErr;
                const AuthoritativeBlockResolutionResult anchorRes =
                    AuthoritativeGetActiveSnapshotByHeightR(nSyncHeight, &anchor, &anchorErr);
                if (anchorRes == AUTHORITATIVE_BLOCK_FOUND &&
                    anchor.height == nSyncHeight && anchor.hash == hashSyncCheckpoint)
                {
                    // DEFINITIVE: the active chain extends the checkpoint.
                    g_bestExtendsMemo.bestExtends = true;
                }
                else if (anchorRes == AUTHORITATIVE_BLOCK_NOT_FOUND ||
                         (anchorRes == AUTHORITATIVE_BLOCK_FOUND &&
                          (anchor.height != nSyncHeight || anchor.hash != hashSyncCheckpoint)))
                {
                    // DEFINITIVE: not a descendant of the sync-checkpoint.
                    g_bestExtendsMemo.bestExtends = false;
                }
                else
                {
                    // TRANSIENT authority failure: fail closed for THIS call
                    // only. Do NOT memoize a verdict (neither in
                    // g_bestExtendsMemo nor mapDescendantVerdict) so the same
                    // tip's next candidate retries the probe.
                    return error("CheckSync: active-chain anchor probe at height %d "
                                 "unavailable (transient; not memoized): %s",
                                 nSyncHeight,
                                 anchorErr.empty() ? "(no detail)" : anchorErr.c_str());
                }
                g_bestExtendsMemo.epochCheckpoint = hashSyncCheckpoint;
                g_bestExtendsMemo.bestTip = hashPrev;
                g_bestExtendsMemo.fValid = true;
            }
            MemoizeDescendantVerdict(hashPrev, g_bestExtendsMemo.bestExtends);
            return g_bestExtendsMemo.bestExtends;
        }

        BlockIndexSnapshot prevSnap;
        std::string prevErr;
        const AuthoritativeBlockResolutionResult prevRes =
            ResolveAuthoritativeBlockSnapshotR(pindexPrev->GetBlockHash(), &prevSnap, &prevErr);
        if (prevRes != AUTHORITATIVE_BLOCK_FOUND && prevRes != AUTHORITATIVE_BLOCK_NOT_ACTIVE)
            return error("CheckSync: previous block %s not resolvable: %s",
                         pindexPrev->GetBlockHash().ToString().c_str(),
                         prevErr.empty() ? "(no detail)" : prevErr.c_str());

        BlockIndexSnapshot anc;
        bool fDescendant;
        if (prevRes == AUTHORITATIVE_BLOCK_FOUND)
        {
            // The parent is the authoritative ACTIVE block at its height, so its
            // ancestor at nSyncHeight IS the active-chain block at that height:
            // resolve it directly (bounded, no per-step ancestry walk).
            if (prevSnap.height == nSyncHeight)
            {
                anc = prevSnap;
            }
            else
            {
                std::string aerr;
                if (!AuthoritativeGetActiveSnapshotByHeight(nSyncHeight, &anc))
                    return error("CheckSync: active ancestor at height %d unavailable", nSyncHeight);
            }
            fDescendant = (anc.height == nSyncHeight && anc.hash == hashSyncCheckpoint);
        }
        else
        {
            // Side-branch parent: walk the persisted ancestry down to the
            // sync-checkpoint height (depth-bounded, fail-closed).
            std::string aerr;
            if (!CheckpointAncestorAtHeight(prevSnap, nSyncHeight, &anc, &aerr))
                return error("CheckSync: pprev null - block index structure failure");
            fDescendant = (anc.height == nSyncHeight && anc.hash == hashSyncCheckpoint);
        }
        // Memoize the verdict (including rejection) for this checkpoint epoch.
        MemoizeDescendantVerdict(pindexPrev->GetBlockHash(), fDescendant);
        return fDescendant;
    }

    // ROUND-6 test seams (production never calls them).
    static int g_workGateAnchorTransientFailForTest = 0;
    void SetWorkGateAnchorTransientFailForTest(int count)
    {
        g_workGateAnchorTransientFailForTest = (count > 0) ? count : 0;
    }
    int WorkGateAnchorTransientFailCountForTest()
    {
        return g_workGateAnchorTransientFailForTest;
    }
    void SetPendingCheckpointMessageForTest(const uint256& hashCheckpoint)
    {
        checkpointMessagePending = CSyncCheckpoint();
        checkpointMessagePending.hashCheckpoint = hashCheckpoint;
        checkpointMessagePending.vchMsg.assign(1, 0x42);
        checkpointMessagePending.vchSig.assign(1, 0x42);
    }
    bool PendingCheckpointMessageIsNullForTest()
    {
        LOCK(cs_hashSyncCheckpoint);
        return checkpointMessagePending.IsNull();
    }
    uint256 PendingCheckpointMessageHashForTest()
    {
        LOCK(cs_hashSyncCheckpoint);
        return checkpointMessagePending.hashCheckpoint;
    }
    void ClearPendingCheckpointMessageForTest()
    {
        LOCK(cs_hashSyncCheckpoint);
        checkpointMessagePending.SetNull();
    }

    // ROUND-7 TASK 3: bounded memo for RESOLVED work-gate anchors.
    // The anchor is a pure function of the sync-checkpoint EPOCH
    // (hashSyncCheckpoint) and the requested proof type: the walk follows
    // hash-fixed ancestry records (hashPrev links and each record's proof type
    // are immutable properties of the block hash), so for a given
    // (epoch, type) the resolved anchor height / nBits / checkpoint time are
    // immutable facts. ONLY a VALID outcome is ever cached -- a TRANSIENT
    // authority/storage failure and a definitive ABSENCE are never memoized, so
    // a storage hiccup can never be frozen into a verdict (the round-5/6 rule).
    // The epoch is part of the key, so a checkpoint advance can never serve a
    // stale anchor; the map is additionally cleared when the epoch changes and
    // capped (CHECKPOINT_WORKGATE_ANCHOR_CAP) so auxiliary memory is constant.
    // Eviction/invalidation is performance-only and can never change a
    // consensus outcome.
    struct WorkGateAnchorMemoKey
    {
        uint256 hashCheckpoint;
        bool fProofOfStake;
        bool operator<(const WorkGateAnchorMemoKey& o) const
        {
            if (hashCheckpoint != o.hashCheckpoint)
                return hashCheckpoint < o.hashCheckpoint;
            return (fProofOfStake ? 1 : 0) < (o.fProofOfStake ? 1 : 0);
        }
    };
    struct WorkGateAnchorMemoEntry
    {
        int64_t nCheckpointTime;
        unsigned int nBits;
        int nHeight;
        int nAnchorHeight;
    };
    static const size_t CHECKPOINT_WORKGATE_ANCHOR_CAP = 16;
    static std::map<WorkGateAnchorMemoKey, WorkGateAnchorMemoEntry> mapWorkGateAnchorMemo;
    static uint256 hashWorkGateAnchorMemoEpoch = uint256(0);
    // ROUND-7 test seams (production never calls them; the counters are
    // observational only and cannot alter a consensus result).
    static int g_workGateAnchorResolveCountForTest = 0;
    // ROUND-7 TASK 1 test seam: lets a test drive CSyncCheckpoint::
    // ProcessSyncCheckpoint past signature verification (a test cannot
    // produce a signature for the hard-coded checkpoint keys). DELIBERATELY
    // inert by default: with the flag clear (production, always) the
    // original CheckSignature() gate is the first thing the function does.
    static bool fProcessSyncCheckpointSkipSignatureForTest = false;
    static int64_t g_workGateAnchorWalkStepsForTest = 0;
    int GetWorkGateAnchorResolveCountForTest()
    {
        LOCK(cs_hashSyncCheckpoint);
        return g_workGateAnchorResolveCountForTest;
    }
    int64_t GetWorkGateAnchorWalkStepsForTest()
    {
        LOCK(cs_hashSyncCheckpoint);
        return g_workGateAnchorWalkStepsForTest;
    }
    void SetProcessSyncCheckpointSkipSignatureForTest(bool f)
    {
        LOCK(cs_hashSyncCheckpoint);
        fProcessSyncCheckpointSkipSignatureForTest = f;
    }
    bool ProcessSyncCheckpointSkipSignatureForTest()
    {
        LOCK(cs_hashSyncCheckpoint);
        return fProcessSyncCheckpointSkipSignatureForTest;
    }
    void ClearWorkGateAnchorMemoForTest()
    {
        LOCK(cs_hashSyncCheckpoint);
        mapWorkGateAnchorMemo.clear();
    }
    void ResetWorkGateAnchorCountersForTest()
    {
        LOCK(cs_hashSyncCheckpoint);
        g_workGateAnchorResolveCountForTest = 0;
        g_workGateAnchorWalkStepsForTest = 0;
    }
    size_t WorkGateAnchorMemoSizeForTest()
    {
        LOCK(cs_hashSyncCheckpoint);
        return mapWorkGateAnchorMemo.size();
    }

    // FIX B: by-value anti-spam work-gate anchors (ProcessBlock weak-competitor
    // gate). pnBits is the nBits of the last block of the requested type at or
    // below the sync-checkpoint; pnCheckpointTime is the checkpoint's time.
    // ROUND-6 TASKS 1/2: legacy GetLastBlockIndex(pcheckpoint, fProofOfStake)
    // semantics BY VALUE: walk back until the proof type MATCHES or the walk
    // reaches genesis (no parent); the anchor may then be genesis even when its
    // proof type differs -- byte-for-byte the legacy behaviour. There is NO
    // arbitrary step cutoff: the sole structural bound is the chain height
    // itself (each step strictly decreases the height by one), so a valid
    // long-distance anchor can never be rejected, and a block of the WRONG proof
    // type is never returned as the anchor (the pre-round-6 fixed 10000-step
    // cutoff did exactly that, silently anchoring a PoS candidate on a PoW
    // block's nBits -- or vice versa -- past the cutoff).
    SyncCheckpointWorkGateAnchor GetSyncCheckpointWorkGateAnchor(bool fProofOfStake, int64_t* pnCheckpointTime,
                                                                 unsigned int* pnBits, int* pnHeight,
                                                                 int* pnAnchorHeight)
    {
        LOCK(cs_hashSyncCheckpoint);
        if (hashSyncCheckpoint == uint256(0))
            return SYNC_CHECKPOINT_WORKGATE_ANCHOR_ABSENT; // no established checkpoint (legacy skips too)
        // ROUND-6 test seam, checked FIRST so an armed seam always forces a
        // TRANSIENT verdict regardless of any memoized anchor (test-only).
        if (g_workGateAnchorTransientFailForTest > 0)
        {
            --g_workGateAnchorTransientFailForTest;
            return SYNC_CHECKPOINT_WORKGATE_ANCHOR_TRANSIENT;
        }
        // ROUND-7 TASK 3: serve a memoized VALID anchor for this (epoch, type)
        // instead of repeating the ancestry walk.
        if (hashWorkGateAnchorMemoEpoch != hashSyncCheckpoint)
        {
            mapWorkGateAnchorMemo.clear();
            hashWorkGateAnchorMemoEpoch = hashSyncCheckpoint;
        }
        WorkGateAnchorMemoKey memoKey;
        memoKey.hashCheckpoint = hashSyncCheckpoint;
        memoKey.fProofOfStake = fProofOfStake;
        std::map<WorkGateAnchorMemoKey, WorkGateAnchorMemoEntry>::const_iterator memoIt =
            mapWorkGateAnchorMemo.find(memoKey);
        // A TEST-injected authority failure must stay observable even for an
        // already-memoized epoch; production never arms injections (the count is
        // always 0), so this guard never changes production behaviour.
        const bool fMemoUsable = (AuthoritativeActiveHashFailCountForTest() == 0);
        if (fMemoUsable && memoIt != mapWorkGateAnchorMemo.end())
        {
            const WorkGateAnchorMemoEntry& e = memoIt->second;
            if (pnCheckpointTime) *pnCheckpointTime = e.nCheckpointTime;
            if (pnBits) *pnBits = e.nBits;
            if (pnHeight) *pnHeight = e.nHeight;
            if (pnAnchorHeight) *pnAnchorHeight = e.nAnchorHeight;
            return SYNC_CHECKPOINT_WORKGATE_ANCHOR_VALID;
        }
        ++g_workGateAnchorResolveCountForTest;   // ROUND-7 seam: a real (uncached) resolution
        BlockIndexSnapshot snap;
        std::string err;
        AuthoritativeBlockResolutionResult r =
            ResolveAuthoritativeBlockSnapshotR(hashSyncCheckpoint, &snap, &err);
        if (r == AUTHORITATIVE_BLOCK_AUTHORITY_FAILURE)
            return SYNC_CHECKPOINT_WORKGATE_ANCHOR_TRANSIENT;
        if (r != AUTHORITATIVE_BLOCK_FOUND && r != AUTHORITATIVE_BLOCK_NOT_ACTIVE)
            return SYNC_CHECKPOINT_WORKGATE_ANCHOR_ABSENT; // legacy: count()==0 -> no anchor
        if (pnCheckpointTime) *pnCheckpointTime = snap.nTime;
        if (pnHeight) *pnHeight = snap.height;
        // Find the last block at or below the checkpoint of the requested type
        // (mirrors CBlockIndex::GetLastBlockIndex). The invariant bound below can
        // never reject a valid anchor: it only detects a corrupt/looping record.
        BlockIndexSnapshot cur = snap;
        const int64_t maxSteps = (int64_t)snap.height;
        int64_t steps = 0;
        while (cur.fProofOfStake != fProofOfStake && cur.hasParent)
        {
            if (cur.height <= 0 || ++steps > maxSteps)
                return SYNC_CHECKPOINT_WORKGATE_ANCHOR_ABSENT; // corrupt ancestry: no valid anchor
            BlockIndexSnapshot parent;
            std::string perr;
            const AuthoritativeBlockResolutionResult pr =
                ResolveAuthoritativeBlockSnapshotR(cur.hashPrev, &parent, &perr);
            if (pr == AUTHORITATIVE_BLOCK_AUTHORITY_FAILURE)
                return SYNC_CHECKPOINT_WORKGATE_ANCHOR_TRANSIENT; // transient: not a verdict
            if (pr != AUTHORITATIVE_BLOCK_FOUND && pr != AUTHORITATIVE_BLOCK_NOT_ACTIVE)
                return SYNC_CHECKPOINT_WORKGATE_ANCHOR_ABSENT; // definitive: no valid anchor
            if (parent.height != cur.height - 1)
                return SYNC_CHECKPOINT_WORKGATE_ANCHOR_ABSENT; // corrupt ancestry
            cur = parent;
        }
        if (pnBits) *pnBits = cur.nBits;
        if (pnAnchorHeight) *pnAnchorHeight = cur.height;
        // ROUND-7 TASK 3: cache ONLY this proven-stable VALID outcome. Bounded:
        // evict wholesale at the cap (performance-only, never consensus).
        g_workGateAnchorWalkStepsForTest += steps;
        if (mapWorkGateAnchorMemo.size() >= CHECKPOINT_WORKGATE_ANCHOR_CAP)
            mapWorkGateAnchorMemo.clear();
        WorkGateAnchorMemoEntry e;
        e.nCheckpointTime = snap.nTime;
        e.nBits = cur.nBits;
        e.nHeight = snap.height;
        e.nAnchorHeight = cur.height;
        mapWorkGateAnchorMemo.insert(std::make_pair(memoKey, e));
        return SYNC_CHECKPOINT_WORKGATE_ANCHOR_VALID;
    }

    // Back-compatible bool form (true iff a VALID anchor was resolved: the
    // requested proof type, or the genesis fallback). Retained for callers and
    // tests; ProcessBlock uses the tri-state form so a transient failure cannot
    // silently disable the gate.
    bool GetSyncCheckpointWorkGateByValue(bool fProofOfStake, int64_t* pnCheckpointTime,
                                          unsigned int* pnBits, int* pnHeight)
    {
        return GetSyncCheckpointWorkGateAnchor(fProofOfStake, pnCheckpointTime, pnBits, pnHeight)
               == SYNC_CHECKPOINT_WORKGATE_ANCHOR_VALID;
    }

    // ppcoin: only descendant of current sync-checkpoint is allowed
    bool ValidateSyncCheckpoint(uint256 hashCheckpoint)
    {
        // V2-R2B (FIX B): under BY_VALUE_AUTHORITATIVE startup the historical
        // CBlockIndex objects that this predicate walks (mapBlockIndex / pprev)
        // are not resident, so delegate to the by-value authoritative validator
        // with IDENTICAL acceptance / conflict-rejection semantics.
        if (g_fAuthoritativeStartup)
            return ValidateSyncCheckpointByValue(hashCheckpoint);

        if (!mapBlockIndex.count(hashSyncCheckpoint))
            return error("ValidateSyncCheckpoint: block index missing for current sync-checkpoint %s", hashSyncCheckpoint.ToString().c_str());
        if (!mapBlockIndex.count(hashCheckpoint))
            return error("ValidateSyncCheckpoint: block index missing for received sync-checkpoint %s", hashCheckpoint.ToString().c_str());

        CBlockIndex* pindexSyncCheckpoint = mapBlockIndex[hashSyncCheckpoint];
        CBlockIndex* pindexCheckpointRecv = mapBlockIndex[hashCheckpoint];

        if (pindexCheckpointRecv->nHeight <= pindexSyncCheckpoint->nHeight)
        {
            // Received an older checkpoint, trace back from current checkpoint
            // to the same height of the received checkpoint to verify
            // that current checkpoint should be a descendant block
            CBlockIndex* pindex = pindexSyncCheckpoint;
            while (pindex->nHeight > pindexCheckpointRecv->nHeight)
                if (!(pindex = pindex->pprev))
                    return error("ValidateSyncCheckpoint: pprev null - block index structure failure");
            if (pindex->GetBlockHash() != hashCheckpoint)
            {
                hashInvalidCheckpoint = hashCheckpoint;
                return error("ValidateSyncCheckpoint: new sync-checkpoint %s is conflicting with current sync-checkpoint %s", hashCheckpoint.ToString().c_str(), hashSyncCheckpoint.ToString().c_str());
            }
            return false; // ignore older checkpoint
        }

        // Received checkpoint should be a descendant block of the current
        // checkpoint. Trace back to the same height of current checkpoint
        // to verify.
        CBlockIndex* pindex = pindexCheckpointRecv;
        while (pindex->nHeight > pindexSyncCheckpoint->nHeight)
            if (!(pindex = pindex->pprev))
                return error("ValidateSyncCheckpoint: pprev2 null - block index structure failure");
        if (pindex->GetBlockHash() != hashSyncCheckpoint)
        {
            hashInvalidCheckpoint = hashCheckpoint;
            return error("ValidateSyncCheckpoint: new sync-checkpoint %s is not a descendant of current sync-checkpoint %s", hashCheckpoint.ToString().c_str(), hashSyncCheckpoint.ToString().c_str());
        };

        return true;
    }

    bool WriteSyncCheckpoint(const uint256& hashCheckpoint)
    {
        // P2 (S4): in V2 store mode persist to the V2-native scalar home
        // (chainstate/sync-checkpoint) instead of the legacy LevelDB — no
        // txleveldb write on a V2 datadir. Legacy (non-V2) mode keeps the old
        // transactional store write.
        if (g_fAuthoritativeStartup)
        {
            if (!WriteV2SyncCheckpointScalar(hashCheckpoint))
                return error("WriteSyncCheckpoint(): failed to persist V2 sync-checkpoint scalar %s", hashCheckpoint.ToString().c_str());
            Checkpoints::hashSyncCheckpoint = hashCheckpoint;
            printf("WriteSyncCheckpoint(): V2 scalar persisted %s\n", hashCheckpoint.ToString().c_str());
            return true;
        }
        CTxDB txdb;
        txdb.TxnBegin();
        if (!txdb.WriteSyncCheckpoint(hashCheckpoint))
        {
            txdb.TxnAbort();
            return error("WriteSyncCheckpoint(): failed to write to db sync checkpoint %s", hashCheckpoint.ToString().c_str());
        }
        if (!txdb.TxnCommit())
            return error("WriteSyncCheckpoint(): failed to commit to db sync checkpoint %s", hashCheckpoint.ToString().c_str());

        Checkpoints::hashSyncCheckpoint = hashCheckpoint;
        return true;
    }

    bool AcceptPendingSyncCheckpoint()
    {
        LOCK(cs_hashSyncCheckpoint);
        // V2-R3 (ROUND-2 P1): RESTORED legacy "known" semantics under
        // authoritative startup. Legacy treated a pending checkpoint as KNOWN
        // when mapBlockIndex.count(hashPendingCheckpoint) — counting ACTIVE
        // and non-active records alike — and then FORCED the active chain to
        // the checkpoint via CBlock::SetBestChain when it was not already on
        // the main chain. Fix B had narrowed this gate to ACTIVE-only rows,
        // which left a NOT_ACTIVE checkpoint pending forever with no reorg
        // ever attempted (a consensus-visible divergence). The by-value
        // resolver reproduces the legacy count() exactly (FOUND = active,
        // NOT_ACTIVE = known side record); the promotion then dispatches
        // through the EXISTING CBlock::SetBestChain consensus path
        // (AuthoritativeSyncCheckpointSwitchChainByValue, main.cpp) — no
        // parallel reorg. Acceptance/rejection parity is preserved: a
        // conflicting checkpoint is still rejected/invalidated first and a
        // switch refusal fails the acceptance WITHOUT invalidating (matching
        // legacy SetBestChain-failure behaviour).
        bool fPendingKnown = false;
        if (hashPendingCheckpoint != 0)
        {
            fPendingKnown = g_fAuthoritativeStartup
                ? IsAuthoritativeIndexedRecord(hashPendingCheckpoint)
                : (mapBlockIndex.count(hashPendingCheckpoint) != 0);
        }
        if (fPendingKnown)
        {
            // ROUND-6 TASK 3: distinguish a TRANSIENT authority/storage failure
            // from a definitive verdict. On a transient failure the pending
            // checkpoint AND its pending message are RETAINED, so a later attempt
            // can still accept, persist and relay it; only a definitive
            // unknown / invalid / conflicting / older verdict clears them (legacy
            // parity for every state the legacy resident index could represent).
            SyncCheckpointValidationOutcome pendingVerdict = SYNC_CHECKPOINT_VALID;
            if (g_fAuthoritativeStartup)
                pendingVerdict = ValidateSyncCheckpointDetailed(hashPendingCheckpoint);
            else if (!ValidateSyncCheckpoint(hashPendingCheckpoint))
                pendingVerdict = SYNC_CHECKPOINT_DEFINITIVE_REJECT;
            if (pendingVerdict == SYNC_CHECKPOINT_TRANSIENT_FAILURE)
            {
                printf("AcceptPendingSyncCheckpoint: transient authority failure validating pending sync-checkpoint %s; kept pending\n",
                       hashPendingCheckpoint.ToString().c_str());
                return false;
            }
            if (pendingVerdict != SYNC_CHECKPOINT_VALID)
            {
                hashPendingCheckpoint = 0;
                checkpointMessagePending.SetNull();
                return false;
            }

            if (!g_fAuthoritativeStartup)
            {
                CTxDB txdb;
                CBlockIndex* pindexCheckpoint = mapBlockIndex[hashPendingCheckpoint];
                if (!pindexCheckpoint->IsInMainChain())
                {
                    CBlock block;
                    if (!block.ReadFromDisk(pindexCheckpoint))
                        return error("AcceptPendingSyncCheckpoint: ReadFromDisk failed for sync checkpoint %s", hashPendingCheckpoint.ToString().c_str());

                    if (!block.SetBestChain(txdb, pindexCheckpoint))
                    {
                        hashInvalidCheckpoint = hashPendingCheckpoint;
                        return error("AcceptPendingSyncCheckpoint: SetBestChain failed for sync checkpoint %s", hashPendingCheckpoint.ToString().c_str());
                    };
                };
            }
            else
            {
                // ROUND-2 P1: KNOWN-but-NOT-ACTIVE checkpoint -> legacy forced
                // promotion through the existing consensus engine. Guarded (the
                // switch is all-or-nothing): on refusal the checkpoint stays
                // pending (retained for a later attempt) and the pending
                // message is NOT consumed, so acceptance is never lost — the
                // checkpoint neither silently vanishes nor bypasses validation.
                if (!IsAuthoritativeActiveRecord(hashPendingCheckpoint))
                {
                    std::string switchErr;
                    {
                        LOCK(cs_main);
                        if (!AuthoritativeSyncCheckpointSwitchChainByValue(hashPendingCheckpoint, &switchErr))
                        {
                            printf("AcceptPendingSyncCheckpoint: authoritative forced promotion not completed for %s (%s); kept pending\n",
                                   hashPendingCheckpoint.ToString().c_str(),
                                   switchErr.c_str());
                            return false; // parity with legacy SetBestChain failure path
                        }
                    }
                }
            }

            if (!WriteSyncCheckpoint(hashPendingCheckpoint))
                return error("AcceptPendingSyncCheckpoint(): failed to write sync checkpoint %s", hashPendingCheckpoint.ToString().c_str());
            hashPendingCheckpoint = 0;
            checkpointMessage = checkpointMessagePending;
            checkpointMessagePending.SetNull();
            printf("AcceptPendingSyncCheckpoint : sync-checkpoint at %s\n", hashSyncCheckpoint.ToString().c_str());
            // relay the checkpoint
            if (!checkpointMessage.IsNull())
            {
                LOCK(cs_vNodes);
                BOOST_FOREACH(CNode* pnode, vNodes)
                    checkpointMessage.RelayTo(pnode);
            }
            return true;
        }
        return false;
    }


    // Automatically select a suitable sync-checkpoint
    const CBlockIndex*  AutoSelectSyncCheckpoint()
    {
        const CBlockIndex *pindex = pindexBest;
        if (!pindex)
            return pindexGenesisBlock;
        // Search backward for a block within max span and maturity window
        while (pindex->pprev && (pindex->GetBlockTime() + nCheckpointSpan * nTargetSpacing > pindexBest->GetBlockTime() || pindex->nHeight + nCheckpointSpan > pindexBest->nHeight))
            pindex = pindex->pprev;
        return pindex;
    }

    // Check the chain this block is going to attach to is valid past maturity
    bool CheckSync(const uint256& hashBlock, const CBlockIndex* pindexPrev)
    {
        if (fTestNet || fRegTest) return true; // Testnet/Regtest have no checkpoints
        int nHeight = pindexPrev->nHeight + 1;
        if (IsInitialBlockDownload()) {
            // Innova: no height-based rejection while catching up. The former
            // IBD branch rejected any block with height <= the auto-selected
            // boundary (AutoSelectSyncCheckpoint(), derived from the active
            // branch = active tip - nCheckpointSpan). That boundary followed
            // the active branch: once a competing branch became active, the
            // true higher-trust chain below the boundary was permanently
            // rejected (fork trap), because rejected blocks never enter the
            // block index and never accumulate nChainTrust. Chain selection by
            // nChainTrust already guarantees the strongest chain wins; the
            // consensus-critical hardened checkpoints remain enforced via
            // CheckHardened(). Synced protection below stays intact.
            return true;
        } else { // do a more thorough check when we are already synced
            // V2-R2B (FIX B): authoritative mode resolves the sync-checkpoint and
            // the candidate's ancestry by value (mapBlockIndex is trimmed).
            if (g_fAuthoritativeStartup)
                return CheckSyncByValue(hashBlock, pindexPrev);
            LOCK(cs_hashSyncCheckpoint);
            // sync-checkpoint should always be accepted block
            if (!mapBlockIndex.count(hashSyncCheckpoint))
                return error("CheckSync: sync-checkpoint not in block index");
            const CBlockIndex* pindexSync = mapBlockIndex[hashSyncCheckpoint];

            if (nHeight > pindexSync->nHeight)
            {
                // trace back to same height as sync-checkpoint
                const CBlockIndex* pindex = pindexPrev;
                while (pindex->nHeight > pindexSync->nHeight)
                    if (!(pindex = pindex->pprev))
                        return error("CheckSync: pprev null - block index structure failure");
                if (pindex->nHeight < pindexSync->nHeight || pindex->GetBlockHash() != hashSyncCheckpoint)
                    return false; // only descendant of sync-checkpoint can pass check
            };

            if (nHeight == pindexSync->nHeight && hashBlock != hashSyncCheckpoint)
                return false; // same height with sync-checkpoint
            if (nHeight < pindexSync->nHeight && !mapBlockIndex.count(hashBlock))
                return false; // lower height than sync-checkpoint
            return true;
        }
    }

    bool WantedByPendingSyncCheckpoint(uint256 hashBlock)
    {
        LOCK(cs_hashSyncCheckpoint);
        if (hashPendingCheckpoint == 0)
            return false;
        if (hashBlock == hashPendingCheckpoint)
            return true;
        if (mapOrphanBlocks.count(hashPendingCheckpoint)
            && hashBlock == WantedByOrphan(mapOrphanBlocks[hashPendingCheckpoint]))
            return true;
        return false;
    }

    // ppcoin: reset synchronized checkpoint to last hardened checkpoint
    bool ResetSyncCheckpoint()
    {
        LOCK(cs_hashSyncCheckpoint);
        const uint256& hash = mapCheckpoints.rbegin()->second;
        std::map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hash);
        if (mi != mapBlockIndex.end() && !mi->second->IsInMainChain())
        {
            // checkpoint block accepted but not yet in main chain
            printf("ResetSyncCheckpoint: SetBestChain to hardened checkpoint %s\n", hash.ToString().c_str());
            CTxDB txdb;
            CBlock block;
            if (!block.ReadFromDisk(mi->second))
                return error("ResetSyncCheckpoint: ReadFromDisk failed for hardened checkpoint %s", hash.ToString().c_str());
            if (!block.SetBestChain(txdb, mi->second))
            {
                return error("ResetSyncCheckpoint: SetBestChain failed for hardened checkpoint %s", hash.ToString().c_str());
            };
        } else
        if (mi == mapBlockIndex.end())
        {
            // checkpoint block not yet accepted
            hashPendingCheckpoint = hash;
            checkpointMessagePending.SetNull();
            printf("ResetSyncCheckpoint: pending for sync-checkpoint %s\n", hashPendingCheckpoint.ToString().c_str());
        };

        BOOST_REVERSE_FOREACH(const MapCheckpoints::value_type& i, mapCheckpoints)
        {
            const uint256& hash = i.second;
            std::map<uint256, CBlockIndex*>::iterator it = mapBlockIndex.find(hash);
            if (it != mapBlockIndex.end() && it->second->IsInMainChain())
            {
                if (!WriteSyncCheckpoint(hash))
                    return error("ResetSyncCheckpoint: failed to write sync checkpoint %s", hash.ToString().c_str());
                printf("ResetSyncCheckpoint: sync-checkpoint reset to %s\n", hashSyncCheckpoint.ToString().c_str());
                return true;
            };
        };

        return false;
    }

    void AskForPendingSyncCheckpoint(CNode* pfrom)
    {
        LOCK(cs_hashSyncCheckpoint);
        // V2-R2B (FIX B): "we do not have the pending checkpoint record" is
        // decided by the authoritative resolver under authoritative startup.
        const bool fPendingMissing =
            g_fAuthoritativeStartup
                ? !IsAuthoritativeIndexedRecord(hashPendingCheckpoint)
                : (mapBlockIndex.count(hashPendingCheckpoint) == 0);
        if (pfrom && hashPendingCheckpoint != 0 && fPendingMissing && (!mapOrphanBlocks.count(hashPendingCheckpoint)))
            pfrom->AskFor(
                CInv(MSG_BLOCK, hashPendingCheckpoint),
                BLOCKREQ_SOURCE_CHECKPOINT);
    }

    bool SetCheckpointPrivKey(std::string strPrivKey)
    {
        if (fDebug)
            printf("SetCheckpointPrivKey()\n");

        // Test signing a sync-checkpoint with genesis block
        CSyncCheckpoint checkpoint;
        checkpoint.hashCheckpoint = GetGenesisBlockHash();
        CDataStream sMsg(SER_NETWORK, PROTOCOL_VERSION);
        sMsg << (CUnsignedSyncCheckpoint)checkpoint;
        checkpoint.vchMsg = std::vector<unsigned char>(sMsg.begin(), sMsg.end());

        std::vector<unsigned char> vchPrivKey = ParseHex(strPrivKey);
        CKey key;
        key.SetPrivKey(CPrivKey(vchPrivKey.begin(), vchPrivKey.end()),false); // if key is not correct openssl may crash
        if (!key.Sign(Hash(checkpoint.vchMsg.begin(), checkpoint.vchMsg.end()), checkpoint.vchSig))
            return false;

        // Test signing successful, proceed
        CSyncCheckpoint::strMasterPrivKey = strPrivKey;
        return true;
    }

    bool SendSyncCheckpoint(uint256 hashCheckpoint)
    {
        if (fDebug)
            printf("SendSyncCheckpoint()\n");

        CSyncCheckpoint checkpoint;
        checkpoint.hashCheckpoint = hashCheckpoint;
        CDataStream sMsg(SER_NETWORK, PROTOCOL_VERSION);
        sMsg << (CUnsignedSyncCheckpoint)checkpoint;
        checkpoint.vchMsg = std::vector<unsigned char>(sMsg.begin(), sMsg.end());

        if (CSyncCheckpoint::strMasterPrivKey.empty())
            return error("SendSyncCheckpoint: Checkpoint master key unavailable.");
        std::vector<unsigned char> vchPrivKey = ParseHex(CSyncCheckpoint::strMasterPrivKey);
        CKey key;
        key.SetPrivKey(CPrivKey(vchPrivKey.begin(), vchPrivKey.end()), false); // if key is not correct openssl may crash
        if (!key.Sign(Hash(checkpoint.vchMsg.begin(), checkpoint.vchMsg.end()), checkpoint.vchSig))
            return error("SendSyncCheckpoint: Unable to sign checkpoint, check private key?");

        if (!checkpoint.ProcessSyncCheckpoint(NULL))
        {
            printf("WARNING: SendSyncCheckpoint: Failed to process checkpoint.\n");
            return false;
        };

        // Relay checkpoint
        {
            LOCK(cs_vNodes);
            BOOST_FOREACH(CNode* pnode, vNodes)
                checkpoint.RelayTo(pnode);
        }
        return true;
    }

    // Is the sync-checkpoint outside maturity window?
    bool IsMatureSyncCheckpoint()
    {
        LOCK(cs_hashSyncCheckpoint);
        // V2-R2B (FIX B): resolve the checkpoint by value under authoritative
        // startup (a non-resident record previously fell into the "not in block
        // index -> mature" default). The predicate itself is unchanged.
        if (g_fAuthoritativeStartup)
        {
            BlockIndexSnapshot snap;
            std::string err;
            const AuthoritativeBlockResolutionResult r =
                ResolveAuthoritativeBlockSnapshotR(hashSyncCheckpoint, &snap, &err);
            if (r != AUTHORITATIVE_BLOCK_FOUND && r != AUTHORITATIVE_BLOCK_NOT_ACTIVE)
                return true; // legacy "sync-checkpoint not in block index" -> true
            return (nBestHeight >= snap.height + nCoinbaseMaturity ||
                    (int64_t)snap.nTime + nStakeMinAge < GetAdjustedTime());
        }
        // sync-checkpoint should always be accepted block
        if (!mapBlockIndex.count(hashSyncCheckpoint))
            return true;
        const CBlockIndex* pindexSync = mapBlockIndex[hashSyncCheckpoint];
        return (nBestHeight >= pindexSync->nHeight + nCoinbaseMaturity ||
                pindexSync->GetBlockTime() + nStakeMinAge < GetAdjustedTime());
    }
}

// ppcoin: sync-checkpoint master key
const std::string CSyncCheckpoint::strMasterPubKey = "02334512c0d7a9a69289c4b7ad1c76e5330ef9dacd9da912c91a853986bddf0434";

std::string CSyncCheckpoint::strMasterPrivKey = "";

// ppcoin: verify signature of sync-checkpoint message
bool CSyncCheckpoint::CheckSignature()
{
    const std::string& strPubKey = CSyncCheckpoint::strMasterPubKey;

    if (strPubKey.size() != 66 && strPubKey.size() != 130)
        return error("CSyncCheckpoint::CheckSignature() : invalid pubkey hex length %u", (unsigned int)strPubKey.size());

    for (size_t i = 0; i < strPubKey.size(); i++)
    {
        char c = strPubKey[i];
        bool isValidHex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!isValidHex)
            return error("CSyncCheckpoint::CheckSignature() : invalid hex char '%c' at position %u", c, (unsigned int)i);
    }

    CPubKey key(ParseHex(strPubKey));
    if (!key.IsValid())
        return error("CSyncCheckpoint::CheckSignature() : invalid checkpoint pubkey");

    if (!key.Verify(Hash(vchMsg.begin(), vchMsg.end()), vchSig))
        return error("CSyncCheckpoint::CheckSignature() : verify signature failed");

    // Now unserialize the data
    CDataStream sMsg(vchMsg, SER_NETWORK, PROTOCOL_VERSION);
    sMsg >> *(CUnsignedSyncCheckpoint*)this;
    return true;
}

// ppcoin: process synchronized checkpoint
bool CSyncCheckpoint::ProcessSyncCheckpoint(CNode* pfrom)
{
    // ROUND-7 TASK 1: the test seam is checked ONLY to bypass signature
    // verification for a test; when the flag is clear (production, always) the
    // original gate below is byte-for-byte in force.
    if (!Checkpoints::ProcessSyncCheckpointSkipSignatureForTest())
    {
        if (!CheckSignature())
            return false;
    }

    LOCK(Checkpoints::cs_hashSyncCheckpoint);
    // V2-R3 (ROUND-2 P1): RESTORED legacy "known" semantics under
    // authoritative startup. Legacy defined fCheckpointKnown as
    // mapBlockIndex.count(hashCheckpoint) (ACTIVE and non-active records
    // alike) and then FORCED the main chain to the checkpoint through
    // SetBestChain when it was not on the main chain. Fix B had narrowed both
    // gates to ACTIVE-only, so a NOT_ACTIVE checkpoint stayed pending forever
    // (consensus-visible divergence from legacy). The by-value resolver
    // reproduces the legacy count() exactly; the promotion dispatches through
    // the EXISTING CBlock::SetBestChain consensus path
    // (AuthoritativeSyncCheckpointSwitchChainByValue, main.cpp) — no parallel
    // reorg. Acceptance/rejection parity: conflicting checkpoints are still
    // rejected by ValidateSyncCheckpoint, and a switch refusal fails WITHOUT
    // WriteSyncCheckpoint (the checkpoint stays pending, exactly like a legacy
    // SetBestChain failure).
    const bool fCheckpointKnown =
        g_fAuthoritativeStartup
            ? Checkpoints::IsAuthoritativeIndexedRecord(hashCheckpoint)
            : (mapBlockIndex.count(hashCheckpoint) != 0);
    if (!fCheckpointKnown)
    {
        // We haven't received the checkpoint chain, keep the checkpoint as pending
        Checkpoints::hashPendingCheckpoint = hashCheckpoint;
        Checkpoints::checkpointMessagePending = *this;
        printf("ProcessSyncCheckpoint: pending for sync-checkpoint %s\n", hashCheckpoint.ToString().c_str());
        // Ask this guy to fill in what we're missing
        if (pfrom)
        {
            pfrom->PushGetBlocks(pindexBest, hashCheckpoint, ibdmetrics::GETBLOCKS_SOURCE_CHECKPOINT);
            //PushGetBlocks(pfrom, pindexBest, hashCheckpoint);
            // ask directly as well in case rejected earlier by duplicate
            // proof-of-stake because getblocks may not get it this time
            std::map<uint256, CBlock*>::iterator orphanIt = mapOrphanBlocks.find(hashCheckpoint);
            pfrom->AskFor(
                CInv(MSG_BLOCK, orphanIt != mapOrphanBlocks.end()
                    ? WantedByOrphan(orphanIt->second)
                    : hashCheckpoint),
                BLOCKREQ_SOURCE_CHECKPOINT);
        };
        return false;
    };

    // ROUND-7 TASK 1: a KNOWN incoming checkpoint whose validation fails
    // TRANSIENTLY (authoritative storage/authority unavailable) must NOT lose the
    // incoming message. Legacy could not represent this state (its block index
    // was resident), so the message was simply dropped and recovery of storage
    // did not guarantee a retry. Retain the incoming message as PENDING exactly
    // as the UNKNOWN branch above does, so the next processing opportunity --
    // AcceptPendingSyncCheckpoint(), driven from AcceptBlock -- retries it. A
    // DEFINITIVE verdict (invalid / conflicting / older) keeps legacy behaviour
    // and records nothing. An already-pending DIFFERENT candidate is never
    // overwritten (do not silently replace a more appropriate pending
    // checkpoint), and no queue is added: the pending state stays a single
    // (hash, message) pair. Nothing is promoted, persisted or relayed here.
    if (g_fAuthoritativeStartup)
    {
        const Checkpoints::SyncCheckpointValidationOutcome recvVerdict =
            Checkpoints::ValidateSyncCheckpointDetailed(hashCheckpoint);
        if (recvVerdict == Checkpoints::SYNC_CHECKPOINT_TRANSIENT_FAILURE)
        {
            if (Checkpoints::hashPendingCheckpoint == uint256(0) ||
                Checkpoints::hashPendingCheckpoint == hashCheckpoint)
            {
                Checkpoints::hashPendingCheckpoint = hashCheckpoint;
                Checkpoints::checkpointMessagePending = *this;
                printf("ProcessSyncCheckpoint: transient validation failure for known sync-checkpoint %s; kept pending\n",
                       hashCheckpoint.ToString().c_str());
            }
            else
            {
                printf("ProcessSyncCheckpoint: transient validation failure for known sync-checkpoint %s; a different pending checkpoint %s is retained\n",
                       hashCheckpoint.ToString().c_str(),
                       Checkpoints::hashPendingCheckpoint.ToString().c_str());
            }
            return false;
        }
        if (recvVerdict != Checkpoints::SYNC_CHECKPOINT_VALID)
            return false;                       // definitive: legacy consume-and-drop
    }
    else if (!Checkpoints::ValidateSyncCheckpoint(hashCheckpoint))
    {
        return false;
    }

    if (!g_fAuthoritativeStartup)
    {
        CTxDB txdb;
        CBlockIndex* pindexCheckpoint = mapBlockIndex[hashCheckpoint];
        if (!pindexCheckpoint->IsInMainChain())
        {
            // checkpoint chain received but not yet main chain
            CBlock block;
            if (!block.ReadFromDisk(pindexCheckpoint))
                return error("ProcessSyncCheckpoint: ReadFromDisk failed for sync checkpoint %s", hashCheckpoint.ToString().c_str());
            if (!block.SetBestChain(txdb, pindexCheckpoint))
            {
                Checkpoints::hashInvalidCheckpoint = hashCheckpoint;
                return error("ProcessSyncCheckpoint: SetBestChain failed for sync checkpoint %s", hashCheckpoint.ToString().c_str());
            };
        };
    }
    else
    {
        // ROUND-2 P1: KNOWN-but-NOT-ACTIVE checkpoint -> legacy forced
        // promotion through the existing consensus engine. On refusal the
        // checkpoint is neither written (hashSyncCheckpoint unchanged) nor
        // broadcast — the message is NOT consumed and stays valid for a re-send.
        if (!Checkpoints::IsAuthoritativeActiveRecord(hashCheckpoint))
        {
            std::string switchErr;
            {
                LOCK(cs_main);
                if (!AuthoritativeSyncCheckpointSwitchChainByValue(hashCheckpoint, &switchErr))
                    return error("ProcessSyncCheckpoint: authoritative forced promotion failed for sync checkpoint %s: %s",
                                 hashCheckpoint.ToString().c_str(), switchErr.c_str());
            }
        }
    }

    if (!Checkpoints::WriteSyncCheckpoint(hashCheckpoint))
        return error("ProcessSyncCheckpoint(): failed to write sync checkpoint %s", hashCheckpoint.ToString().c_str());
    Checkpoints::checkpointMessage = *this;
    Checkpoints::hashPendingCheckpoint = 0;
    Checkpoints::checkpointMessagePending.SetNull();
    printf("ProcessSyncCheckpoint: sync-checkpoint at %s\n", hashCheckpoint.ToString().c_str());
    return true;
}
