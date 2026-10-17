// Stage 3A - incremental (O(delta)) chainstate store: paged CoW B+tree + durable
// MANIFEST/backup/ACK commit protocol (carried from the audited Stage-2B design).
#pragma once
#include "chainstate.h"
#include "btree.h"
#include <string>
#include <vector>
#include <deque>

namespace cs { namespace store3a {

struct Manifest {
    uint32_t formatVersion = 2;
    uint64_t generation = 0;
    uint64_t undoSize = 0;
    uint32_t rootPageId = 0;
    Hash rootHash{};
    Hash blockHash{};
    int32_t tipHeight = -1;
    std::vector<uint8_t> encode() const;
    static bool decode(const uint8_t* d, size_t n, Manifest* m, ChainError* e);
};

struct UndoRecord {
    Hash blockHash{};
    int32_t height = 0;
    std::vector<Op> ops;
    std::vector<uint8_t> encode() const;           // includes trailing crc32 (S2B-1)
    static bool decode(const uint8_t* d, size_t n, size_t* off, UndoRecord* r, ChainError* e);
};

struct UndoMeta { Hash blockHash{}; int32_t height = 0; uint64_t offset = 0; uint32_t len = 0; };

// S1.3: the in-RAM undo metadata is no longer one entry per record. A CommittedSegment records the byte
// range of one CLOSED undo segment (fixed recordCount == segmentSize); the recent-record cache holds at
// most UNDO_RECENT_CACHE_SEGMENTS x segmentSize entries so both disconnect (tip peek/pop) and segment
// publication (last segmentSize metas) are O(1) in the common case without retaining every record.
// S1.1 C++11 note: aggregate brace-init is unavailable once members carry default initializers, so this
// carries an explicit ctor; the directory is rebuilt from a validated index (or the authoritative scan) on
// every load, so CRCs are not retained in RAM (re-derived by encode() when the index is (re)written).
struct CommittedSegment {
    uint64_t startOffset; uint64_t endOffset;
    CommittedSegment() = default;
    CommittedSegment(uint64_t s, uint64_t e) : startOffset(s), endOffset(e) {}
};

// ---------------------------------------------------------------------------------------------
// S1.1 -- DURABLE SEGMENTED UNDO INDEX (`UNDOINDEX`), DERIVED METADATA ONLY.
//
// Authority: MANIFEST.undoSize remains the SOLE authority for the committed undo length. `UNDOINDEX`
// is a rebuildable cache of the boundaries of CLOSED segments of the committed undo stream; it is never
// consulted for authority. Since S1.3, lookup/disconnect use the bounded recent cache and, past it,
// on-demand parsing of the committed stream (see DESIGN 17).
//
// Publication ordering (the reconciliation with the MANIFEST publication -- S1.1 section 4):
//   the index is published INSIDE `commitBarriers`, AFTER the durable undo write and BEFORE
//   `MANIFEST.tmp`/the MANIFEST rename, and ONLY when the append closes a segment (every
//   `segmentSize` records). Consequences:
//     * crash before the MANIFEST rename  -> the index describes bytes the manifest does not yet
//       commit: AHEAD (benign; the extra records are dropped, never authoritative);
//     * crash after  the MANIFEST rename  -> index and manifest agree: CURRENT;
//     * power-loss reordering that loses the index rename but keeps the manifest rename -> the
//       committed range has crossed a segment boundary the index does not describe: STALE
//       (rebuilt loudly from the committed stream; never silently trusted);
//     * absent -> rebuilt once from the committed stream;
//     * structurally invalid (magic/version/crc/bounds/contiguity) -> FAIL CLOSED (never rebuilt
//       silently, never accepted).
//   No new authority mechanism and no new durability barrier is introduced: the index rename lands
//   in the same directory as the MANIFEST rename, so the existing `MANIFEST` directory fsync
//   (commitBarriers step 6) already makes it durable. If a crash leaves the index ahead, nothing is
//   authoritative until the manifest says so; if it leaves it stale, open() rebuilds it. The
//   existing crash protocol therefore supports this ordering -- no conflict had to be reported.
//
// Cost discipline (S1.1 section 5): the index is rewritten once per CLOSED segment (not per block),
// so the per-block amortised cost is O(segmentSize^-1) bytes plus one file fsync + one rename per
// segment -- explicitly NOT a full-index rewrite per block. Measured cost + the model are in the
// S1.1 report; the alternatives considered (per-segment files; append-only file with an in-place
// header update) are documented there and in DESIGN.md.
// ---------------------------------------------------------------------------------------------
static const uint32_t UNDOINDEX_FORMAT_VERSION       = 1;
static const uint32_t UNDOINDEX_DEFAULT_SEGMENT_SIZE = 4096;   // records per closed segment (documented default)
static const size_t   UNDOINDEX_HEADER_SIZE          = 48;
static const size_t   UNDOINDEX_SEGMENT_SIZE         = 24;     // {startOffset u64, endOffset u64, recordCount u32, crc32 u32}

struct UndoSegment {
    uint64_t startOffset = 0;   // byte offset of the segment's first record (inclusive)
    uint64_t endOffset   = 0;   // byte offset just past the segment's last record (exclusive)
    uint32_t recordCount = 0;   // == segmentSize for a closed segment
    uint32_t crc32       = 0;   // crc32 over {startOffset, endOffset, recordCount}
};

struct UndoIndex {
    uint32_t formatVersion = UNDOINDEX_FORMAT_VERSION;
    uint32_t segmentSize   = UNDOINDEX_DEFAULT_SEGMENT_SIZE;
    uint64_t committedBytes = 0;   // endOffset of the LAST CLOSED segment described by this index
    uint64_t baseOffset    = 0;    // S1.1: always 0 (head pruning / retention floor is a later slice)
    uint64_t baseGeneration = 0;   // S1.1: always 0 (not recoverable from the current on-disk format)
    std::vector<UndoSegment> segments;
    std::vector<uint8_t> encode() const;
    static bool decode(const uint8_t* d, size_t n, UndoIndex* ix, ChainError* e);
};

// S1.1: how the on-disk index relates to the committed state. Classification is recorded for
// diagnostics and tests; None = not evaluated (fresh-open path or index publication disabled).
enum class UndoIndexState { None, Current, Absent, Stale, Ahead, Corrupt };

// S1.1-F1 (finding S1.1-1): explicit, operator-authorized recovery of the DERIVED undo index.
// The default path stays fail closed: nothing is quarantined, rebuilt or written because a caller
// merely opened the store. Recovery is a deliberate, one-shot authorization supplied by the operator
// through openWithOptions(); it never changes what is committed (MANIFEST.undoSize stays the only
// authority) and it rebuilds exclusively from the committed stream already validated by the existing
// MANIFEST/undo recovery path. There is no datadir flag, no RPC, no CLI option and no production wiring.
struct OpenOptions {
    bool authorizeUndoIndexRecovery = false;   // one-shot: consumed by the open() call it authorizes
};
// What an authorized recovery actually did (also populated (authorized=false) for every other open, so
// a caller can see whether the index was present/current without asking for recovery).
struct UndoIndexRecoveryReport {
    bool     authorized = false;        // the caller requested recovery on this open
    bool     indexPresent = false;      // an UNDOINDEX artifact existed on disk
    bool     indexReadError = false;    // the artifact could not be read at all (I/O error, not corruption)
    bool     indexCorrupt = false;      // the artifact was read but failed structural validation
    std::string quarantinePath;         // where the corrupt artifact was preserved ("" => none)
    uint64_t quarantinedBytes = 0;
    bool     rebuilt = false;           // the index was rebuilt from the committed stream
    bool     nothingToIndex = false;    // the rebuild found no closed segment (no index file written)
    size_t   rebuiltSegments = 0;
    uint64_t rebuiltCommittedBytes = 0;
    std::string resolution;             // "none" | "already-current" | "rebuilt" | "quarantined+rebuilt" | "refused"
};

// ---------------------------------------------------------------------------------------------
// S1.2 -- CONSUMING THE DERIVED INDEX AT STARTUP ("fast startup"), COUNTERS INCLUDED.
//
// What the S1.1 index can and cannot establish (the integrity question, answered explicitly):
//   * segment descriptors carry {startOffset, endOffset, recordCount} and a crc32 OVER THOSE FIELDS
//     ONLY -- the checksum never covers the undo bytes the descriptor points at;
//   * therefore a valid, CURRENT index proves the BYTE BOUNDARIES of the closed segments and nothing
//     about the record layout inside them: `UndoMeta` needs each record's offset, len, blockHash and
//     height, which only exist in the record headers interleaved with payloads in `undo.dat`;
//   * headers are NOT contiguous (records are variable length), so per-record metadata cannot be read
//     without either seeking per record (the legacy scan) or reading whole segment spans (this slice).
// The fast path below therefore CONSUMES the index only to replace one syscall per record with one
// syscall per closed segment, and it re-parses every record it reads, with two extra cross-checks the
// legacy scan does not have: the parse must land EXACTLY on each descriptor's endOffset and must yield
// EXACTLY its recordCount. It is a pure optimisation layer: on ANY anomaly it declines and the
// authoritative record scan below runs unchanged, after which the S1.1 classification repairs or
// refuses the artifact -- nothing is ever accepted on trust, and no validation is relaxed.
// A validation-free "trust the descriptors" startup is NOT possible with this format and is NOT done
// here; see the S1.2 report for the smallest format/representation correction that would allow it.
// ---------------------------------------------------------------------------------------------
// Counters are diagnostic only: they never influence behaviour and exist so a startup profile can be
// reported (records parsed, reads issued, bytes touched) instead of asserted.
struct UndoLoadStats {
    int      fastPath = 0;      // 1 = the derived index was consumed; 0 = the authoritative record scan
    std::string fallback;       // non-empty => why the index was not consumed (diagnostic only)
    uint64_t descriptors = 0;   // segment descriptors processed from the index
    uint64_t segmentReads = 0;  // bulk reads issued for closed segments
    uint64_t headerReads = 0;   // per-record header reads (legacy scan and/or the uncovered tail)
    uint64_t records = 0;       // records parsed
    uint64_t bytesRead = 0;     // bytes returned by those reads
};

// S1.1-F1 (finding S1.1-2): deterministic, one-shot test-only faults at the UNDOINDEX publication
// barrier. The default is None, so no injection exists in a production run; the value is set only by
// injectUndoIndexFault() from focused tests and is consumed (reset) by the first publication it affects.
enum class UndoIndexFault { None, TmpCreate, TmpShortWrite, TmpFsync, Rename, DirFsync };

enum class CrashPoint { None, BeforeBakReplace, BakPrepared, AfterBakReplace, AfterUndoWrite, BeforeRename, AfterRename, AfterDirSync };
enum class InjectStage { None, PageWrite, UndoAppend, ManifestTmp, BakWrite, PrimaryRename, DirFsync, AckWrite };
// R1 (F0-2): creation policy for the first undo write after a fresh open. It closes the TOCTOU window
// between the open-time classification of an orphan undo.dat and the first create/write of the log:
//   Any             - create if missing (ordinary publish on an established store).
//   RequireAbsent   - open O_CREAT|O_EXCL: fail closed if the path exists at all, i.e. if anything
//                     appeared between classification and creation (kernel-atomic, no lstat race).
//   RequireExisting - never create (no O_CREAT): fail closed if the file disappeared after it was
//                     classified as an accepted empty residue.
enum class UndoCreate { Any, RequireAbsent, RequireExisting };

// R2 (F0-2-R1): the accepted EMPTY undo.dat is pinned by an OPEN DESCRIPTOR, not by a pathname record.
// The descriptor is opened and re-validated at fresh-open classification (O_RDWR|O_NOFOLLOW|O_NONBLOCK;
// fstat must show a regular, still-empty file) and is the object the first record is written to, so:
//   - no second pathname lookup can redirect the write: the target is fixed by the descriptor itself,
//     which is why there is no window between "identity validated" and "first write";
//   - while the descriptor is open the classified inode cannot be unlinked-then-recreated into the same
//     dev/ino on the same filesystem, and an unlinked original is detected anyway through st_nlink==0;
//   - the pathname must denote that same descriptor BEFORE the write and again AFTER the fsync, so a
//     record that becomes durable is a record reachable under the committed name (durability binding).
// (A pure dev/ino/ctime fingerprint is NOT sufficient on its own: this platform reuses an inode NUMBER
// after unlink+create and its ctime granularity can alias within the same millisecond.)
// Ownership: the descriptor belongs to the store and is closed either by its first writer or by close().
//
// R3 (F0-2-R2-1): the fresh-open policy -- and, for an accepted empty residue, the pinned descriptor -- is a
// ONE-SHOT resource, spent by the first publication that enters the undo write phase. R2 left that
// consumption invisible: a first publication that failed BEFORE the MANIFEST rename (renamePassed_ false)
// left the instance retryable, and the retry then fell back to UndoCreate::Any, re-resolving undo.dat by
// pathname -- re-exposing exactly the substitution TOCTOU the pin had just closed.
// The instance therefore fails closed: when a publication that spent the one-shot does not complete, the
// store becomes REOPEN_REQUIRED and refuses every later publish/disconnect before touching persistent state
// (it never silently degrades to UndoCreate::Any). Reopening reclassifies the actual on-disk evidence and
// never trusts this instance's in-memory descriptor state, so an ambiguous residue is refused rather than
// re-initialized. A failure that happens BEFORE the undo write phase spends nothing, keeps the pin, and
// remains safely retryable on the same instance; established-store retry semantics are unchanged.

class CommitStore3A {
public:
    ~CommitStore3A();
    bool open(const std::string& dir, ChainError* e);
    // S1.1-F1: the only way to authorize recovery of a corrupt derived index. open() above is exactly
    // openWithOptions(dir, OpenOptions(), e) -- i.e. fail closed -- and remains the production entry point.
    bool openWithOptions(const std::string& dir, const OpenOptions& o, ChainError* e);
    const UndoIndexRecoveryReport& lastUndoIndexRecovery() const { return ixRecovery_; }
    const UndoLoadStats& undoLoadStats() const { return undoLoadStats_; }   // S1.2: last open's startup profile
    void close();

    bool opened() const { return opened_; }
    bool needsReopen() const { return needsReopen_; }
    uint64_t generation() const { return generation_; }
    uint64_t acknowledgedGeneration() const { return ackGen_; }
    const std::string& dir() const { return dir_; }
    const Hash& tipHash() const { return blockHash_; }
    int32_t tipHeight() const { return tipHeight_; }
    size_t undoCount() const { return (size_t)undoRecordCount_; }
    // S1.3: number of CLOSED segments currently in memory (diagnostic; memory scales with this, not
    // undoCount()). Clamped to true closed bounds by trimUncommittedSegments() on every mutation.
    size_t undoSegmentCount() const { return undoSegments_.size(); }
    // S1.3 diagnostics (memory-scaling evidence): the recent cache is bounded independent of undoCount();
    // the refill counter rises only when a disconnect had to re-read the stream past the cache.
    size_t undoRecentCount() const { return undoRecent_.size(); }
    uint64_t undoRecentRefills() const { return undoRecentRefills_; }
    uint64_t undoCommittedBytes() const { return undoCommittedBytes_; }   // S1.2: committed undo length loaded
    bool hadRecoveryIssue() const { return hadRecoveryIssue_; }
    // S1.1: derived undo index -- classification at the last open/publication + cost counters.
    UndoIndexState undoIndexState() const { return undoIndexState_; }
    uint32_t undoIndexSegmentSize() const { return ixSegmentSize_; }
    uint64_t undoIndexPublications() const { return ixPublishes_; }
    uint64_t undoIndexBytesWritten() const { return ixBytesWritten_; }
    uint64_t undoIndexFsyncs() const { return ixFsyncs_; }
    uint64_t undoIndexNanos() const { return ixNanos_; }
    bool readUndoIndexForTest(UndoIndex* ix, ChainError* e);   // structural read (no mutation)
    const BTree& tree() const { return tree_; }
    BTree& tree() { return tree_; }
    uint32_t rootPageId() const { return tree_.rootId(); }
    const Hash& rootHash() const { return tree_.rootHash(); }
    bool truncatedTreeTail() const { return tree_.truncatedTail(); }

    bool get(const OutPoint& k, Coin* c, bool* found, ChainError* e) const;
    bool existsUnspentTxid(const Hash& txid, bool* found, ChainError* e) const;

    bool publish(const Delta& d, const Hash& blockHash, int32_t height, ChainError* e);
    bool disconnect(const Hash& blockHash, const Hash& parentTipHash, ChainError* e);
    bool disconnect(const Hash& blockHash, ChainError* e) {
        return disconnect(blockHash, Hash(), e);
    }

    // test hooks
    void setCrashPoint(CrashPoint c) { crashPoint_ = c; }
    // R3 test hook: bounded pause at a named point of the publication path, so a test can perform an
    // external substitution inside one specific window (Phase F). Default None/0 is a no-op; production
    // callers never set it. Same test-instrumentation contract as setCrashPoint / inject* / injectUndoShortOnce.
    enum class PausePoint { None, BeforeUndoNameRecheck, BeforeAck };
    void setPause(PausePoint p, int ms) { pausePoint_ = p; pauseMs_ = ms; }
    void setInjectStage(InjectStage s) { injectStage_ = s; }
    void injectNoSpace(bool v) { injectNoSpace_ = v; }
    void injectFsyncFail(bool v) { injectFsyncFail_ = v; }
    void clearInjection() { injectStage_ = InjectStage::None; injectNoSpace_ = false; injectFsyncFail_ = false; }
    // R1 test hook: force the next tree page append to be a short/interrupted write.
    void injectPartialAppendOnce(size_t bytes) { tree_.injectPartialAppendOnce(bytes); }
    // R2 test hook: fail the n-th page append (earlier appends succeed and stage pages).
    void injectPartialAppendOn(size_t n) { tree_.injectPartialAppendOn(n); }
    // F0 test hook: write only the first `bytes` bytes of the next undo record, then fail (torn write).
    void injectUndoShortOnce(size_t bytes) { injectUndoShortOnce_ = bytes; }
    // S1.1 test hooks: segment size override (the documented production default stays 4096), a
    // simulated lost index publication (the power-loss shape that produces a stale index), and a
    // control switch that disables the derived index entirely so the other artifacts can be compared.
    void setUndoIndexSegmentSizeForTest(uint32_t s) { ixSegmentSize_ = (s ? s : UNDOINDEX_DEFAULT_SEGMENT_SIZE); }
    void injectUndoIndexSkipNextPublish() { ixSkipNextPublish_ = true; }
    void injectUndoIndexDisabled(bool v) { ixDisabledForTest_ = v; }
    void injectUndoIndexFault(UndoIndexFault f) { ixFault_ = (int)f; }   // S1.1-F1: one-shot
    void clearUndoIndexFaults() { ixFault_ = (int)UndoIndexFault::None; }
    bool undoIndexFaultPending() const { return ixFault_ != (int)UndoIndexFault::None; }
    // S1.1-F2: one-shot test-only errno injection for the quarantine hard link (0 = off; never set outside
    // tests). An injected failure is consumed by the first link() attempt, so a later retry uses the real path.
    void injectQuarantineLinkErrnoForTest(int e){ ixQuarantineLinkErrno_ = e; }

private:
    bool ns(InjectStage s) const { return injectStage_ == s && injectNoSpace_; }
    bool ff(InjectStage s) const { return injectStage_ == s && injectFsyncFail_; }
    bool inj(InjectStage s) const { return injectStage_ == s && (injectNoSpace_ || injectFsyncFail_); }
    bool injErr(ChainError* e, const std::string& what) {
        if (injectNoSpace_) { setErr(e, ErrCode::NO_SPACE, "injected ENOSPC at " + what); return true; }
        if (injectFsyncFail_) { setErr(e, ErrCode::FS, "injected fsync failure at " + what); return true; }
        return false;
    }

    bool tryLoadManifest(const std::string& path, bool* exists, uint32_t* rootId, Hash* rootHash, ChainError* e);
    bool loadUndoFor(const Manifest& m, ChainError* e);
    // S1.2: does the on-disk index describe exactly the CLOSED segments of this committed stream, and is
    // it structurally sound? (`usable` stays false when the artifact is absent, invalid, not current, or
    // disabled for a control run -- the caller then simply runs the authoritative scan.)
    bool tryLoadCurrentUndoIndex(const Manifest& m, UndoIndex* ix, bool* usable, ChainError* err);
    bool loadUndoForIndexed(const Manifest& m, const UndoIndex& ix, ChainError* e);
    // S1.1: derived undo index (all of these are read-only w.r.t. undo.dat / tree.dat / MANIFEST / ACK)
    bool buildUndoIndexFromSegments(UndoIndex* out) const;
    void trimUncommittedSegments();                 // drop map entries uncovered by rollback (S1.3)
    bool reloadRecentFromStream(ChainError* e);     // on-demand reparse of the most recent segment region
    uint64_t undoRecentRefills_ = 0;                     // reloadRecentFromStream() invocations (on-demand reads)
    void recentPush(const UndoMeta& m);             // bounded append to undoRecent_
    static const size_t UNDO_RECENT_CACHE_SEGMENTS = 3;  // undoRecent_ holds <= ~3 segments of metas
    bool loadUndoIndexFromDisk(bool* exists, UndoIndex* ix, ChainError* e);
    bool classifyAndNormalizeUndoIndex(ChainError* e);
    bool publishUndoIndex(const UndoIndex& ix, ChainError* e);
    bool publishUndoIndexOnSegmentClose(uint64_t appendEndOffset, ChainError* e);
    // S1.1-F1: preserve a corrupt derived artifact under a collision-safe name, never overwriting an
    // existing file; returns the quarantine path. Only ever called under explicit operator authorization.
    // S1.1-F2 (finding F1-1): link()+unlink() is the ONLY move primitive here -- there is deliberately no
    // lstat+rename fallback (rename can atomically REPLACE an existing quarantine artifact on Linux).
    bool quarantineCorruptUndoIndex(std::string* quarantinePath, uint64_t* bytes, ChainError* e);
    bool commitBarriers(const Manifest& m, uint64_t undoOffset, const uint8_t* undoAppend, size_t undoAppendLen, ChainError* e);
    bool writeAck(uint64_t gen, ChainError* e);
    bool readAck(bool* present, uint64_t* gen, ChainError* e);
    void crashIf(CrashPoint at);
    void cleanupTmp();
    // R3 (F0-2-R2-1): mark this instance REOPEN_REQUIRED with the exact reason that closed it (fail-closed).
    void requireReopen(const std::string& why);
    void pauseAt(PausePoint p) const;

    std::string p(const std::string& name) const { return dir_ + "/" + name; }

    std::string dir_;
    int lockFd_ = -1;
    bool opened_ = false;
    bool needsReopen_ = false;
    BTree tree_;
    uint64_t generation_ = 0;
    uint64_t undoCommittedBytes_ = 0;
    uint64_t ackGen_ = 0;
    bool ackPresent_ = false;
    Hash blockHash_{};
    int32_t tipHeight_ = -1;
    bool hadRecoveryIssue_ = false;
    bool renamePassed_ = false;
    std::vector<CommittedSegment> undoSegments_;   // CLOSED undo segments in commit order (16 B/segment)
    std::deque<UndoMeta> undoRecent_;              // most recent committed records, bounded (S1.3)
    uint64_t undoRecordCount_ = 0;                 // total committed records (== undoCount()); O(1) count
    // S1.1: derived undo index state/hooks/counters (production defaults: 4096 records/segment).
    uint32_t ixSegmentSize_ = UNDOINDEX_DEFAULT_SEGMENT_SIZE;
    UndoIndexState undoIndexState_ = UndoIndexState::None;
    uint64_t ixPublishes_ = 0, ixBytesWritten_ = 0, ixFsyncs_ = 0, ixNanos_ = 0;
    UndoLoadStats undoLoadStats_;       // S1.2: startup profile of the last loadUndoFor()
    bool ixFreshPath_ = false;          // set when open() took a MANIFEST-absent (fresh/prior-store) path
    bool ixSkipNextPublish_ = false;    // test hook: simulate one lost index publication (power-loss shape)
    bool ixDisabledForTest_ = false;    // test hook: control run -- never touch UNDOINDEX at all
    int ixFault_ = 0;                   // S1.1-F1: one-shot injected publication-barrier fault (None by default)
    int ixQuarantineLinkErrno_ = 0;     // S1.1-F2: one-shot injected errno for the quarantine link (0 = off)
    bool ixAuthorizeRecovery_ = false;  // S1.1-F1: set only from openWithOptions(authorize=...)
    UndoIndexRecoveryReport ixRecovery_;// S1.1-F1: what the last open did about the index artifact
    CrashPoint crashPoint_ = CrashPoint::None;
    InjectStage injectStage_ = InjectStage::None;
    bool injectNoSpace_ = false;
    bool injectFsyncFail_ = false;
    size_t injectUndoShortOnce_ = 0;
    // R1 (F0-2): the creation policy for the next undo write after a fresh open (consumed by it).
    UndoCreate freshUndoCreate_ = UndoCreate::Any;
    // R2 (F0-2-R1): descriptor pinning the EMPTY undo.dat accepted at fresh open (until its first write).
    int freshUndoFd_ = -1;
    // R3 (F0-2-R2-1): true when the publication currently being attempted spent the fresh-open one-shot
    // policy. A failure of such a publication must not be retried on this instance (see requireReopen).
    bool freshOneShotTaken_ = false;
    // R3: why this instance requires a reopen (surfaced by the publish/disconnect guards).
    std::string reopenWhy_;
    // R3 (F0-2-R2-2): descriptor of the undo log written by the in-flight commit, held OPEN until the
    // generation is acknowledged (or the commit fails). Holding it is what makes the pre-ACK pathname
    // recheck sound: the inode cannot be unlinked-and-number-reused while a descriptor refers to it.
    int heldLogFd_ = -1;
    // R3 test hook state (see setPause).
    PausePoint pausePoint_ = PausePoint::None;
    int pauseMs_ = 0;
};

// R1 (F0-2): the create policy is documented on the UndoCreate enum above.
bool csWriteFile(const std::string& path, const uint8_t* d, size_t n, bool injNs, bool injFs, ChainError* e);
bool csAppendFile(const std::string& path, const uint8_t* d, size_t n, bool injNs, bool injFs, ChainError* e);
// F0: write at an explicit offset. Never truncates; never moves the offset implicitly (no O_APPEND);
// refuses to write past a hole (file shorter than off). Bytes < off are left byte-identical.
// R1: the trailing `create` policy decides whether the file may be created, and fails closed on any
// classification/creation race (see UndoCreate). Every failure path here is fail-closed: the caller
// must not treat a false return as "nothing was written" without checking its own recovery contract.
// R3 (F0-2-R2-2): holdFd (optional) receives the descriptor the record was written through and keeps it
// OPEN (ownership transfers to the caller). The caller holds it until the generation is acknowledged, which
// is what makes the pre-ACK pathname->object recheck sound: while a descriptor is open the inode cannot be
// unlinked-and-number-reused, so a dev/ino mismatch is real evidence of substitution.
bool csWriteAtFile(const std::string& path, uint64_t off, const uint8_t* d, size_t n, bool injNs, bool injFs, ChainError* e,
                   UndoCreate create = UndoCreate::Any, int pinnedFd = -1, int* holdFd = nullptr);
bool csReadFile(const std::string& path, std::vector<uint8_t>* out, bool* exists, ChainError* e);
bool csReadRange(const std::string& path, uint64_t off, uint64_t len, std::vector<uint8_t>* out, ChainError* e);
bool csFsyncDir(const std::string& dir, ChainError* e);
uint32_t csCrc32(const uint8_t* d, size_t n);

}} // namespace cs::store3a
