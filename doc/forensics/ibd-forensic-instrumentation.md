# IBD forensic instrumentation and runbook

Reference documentation for the IBD block-request forensic instrumentation in
Innova Core: the flags, the emitted streams, the CSV schemas, the safety
properties, the Experiment A/B configurations, and the operational runbook.
This is the canonical instrumentation guide; `doc/forensics/ibd-root-cause-investigation.md`
holds the older root-cause write-up and `doc/forensics/frontier-admission-exemption.md`
documents the frontier admission exemption.

The document is written against the current `src/` tree.  Every section is
marked with the kind of evidence behind it:

* `Code fact` — verified directly in `src/`.
* `Runtime observation` — observed in a real capture run (see section 14).
* `Working hypothesis` — plausible explanation, not yet proven.
* `Known limitation` — a caveat or missing feature.

## 1. Purpose and scope

`Code fact`.  The instrumentation answers, for a real IBD run, questions such
as:

* does block-request latency grow with the position of a hash inside a getdata
  batch?
* is the in-flight-timeout tail located at the end of the batch?
* how many blocks arrive after their in-flight request already timed out, and
  how many of those had been re-requested (possibly to another peer)?
* how much duplicate traffic does the timeout tail generate?
* does a timed-out hashContinue block delay advancement of the download batch?
* how many outstanding getblocks are dropped without a response or suppressed
  by rate limiting?
* what is the end-to-end per-block decomposition from askfor enqueue to
  connect, per peer and per batch position?

Everything in this document is observation tooling.  None of the modules
change a scheduler decision (see section 5).

## 2. Core concepts

`Code fact`.  The modules share a small vocabulary:

* **Batch** — one `getdata` message that carries at least one block request.
  A batch has an id (`batch_id`), a peer, an enqueue time, a wire size, a
  sequence of block hashes in wire order, and (optionally) the first socket
  send time.
* **Canonical entry** — per block hash, the first request plus the whole
  lifecycle (mark, timeout, receipt, re-request).  A re-request after release
  updates the canonical entry; it never creates a second one.
* **Generation** — one request/ownership lifecycle of a single hash, from
  `MarkBlockInFlight` (generation start) to ownership release (generation
  end).  A hash can have several generations when it is released (for example
  by a timeout) and requested again.
* **Ownership** — a hash is owned by exactly one peer in one of two states,
  `queued` or `inflight` (the single-owner invariant, section 7).
* **T0..T7** — the per-block timeline stamps used by `ibdblocklatency`
  (section 11):
  * T0 — askfor enqueue (`RecordAskForEnqueue`).
  * T1 — getdata pushed to the socket (`RecordGetDataSent`).
  * T2 — full block message finished framing (`ReceiveMsgBytes`,
    `msg.nTime = GetTimeMicros()`); this is the *end* of the receive, not the
    first byte.
  * T3 — `ProcessBlock` begin.
  * T4 — `AcceptBlock` begin.
  * T5 — `AddToBlockIndex` begin.
  * T6 — `SetBestChain` begin.
  * T7 — block connected at `nBestHeight` (end of a successful `SetBestChain`).
* **IBD state** — the `IsInitialBlockDownload()` on/off state plus its reason
  and the number of transitions (`ibd_state_current`,
  `ibd_state_transitions`).

## 3. Module inventory

`Code fact`.

| Module | Files | What it does |
|---|---|---|
| `ibdmetrics` | `src/ibdmetrics.{h,cpp}` | Pure aggregate counters (relaxed atomics only); pressure mirrors; exposed via the `getinfo` RPC as the `ibdmetrics` object. |
| `ibdforensic` | `src/ibdforensic.{h,cpp}` | Passive per-getdata-batch request ledger, per-hash canonical entries, generation ledger; writes a CSV dump + summary at shutdown. |
| `ibdblocklatency` | `src/ibdblocklatency.{h,cpp}` | Per-block GETDATA-to-CONNECT decomposition (T0..T7); streams a CSV and emits `IBD_BLOCKLAT_1S`. |
| `ibdactivepath` | `src/ibdactivepath.{h,cpp}` | IBD active-path throughput: `IBD_ACTIVE_1S`, `IBD_SLOW_BLOCK`, `IBD_STATE_TRACE`. |
| `ibdefficiency` | `src/ibdefficiency.{h,cpp}` | IBD efficiency counters (`IBDEFFICIENCY` summaries every 60 s and at shutdown). |
| `blockrequesttrace` | `src/blockrequesttrace.h` | `BLOCKREQTRACE` and `SYNC_EVENT` stream declarations; source/result enums; `ProcessBlockRejectReason`. |
| `ibdforensic` hooks | `src/net.{h,cpp}`, `src/main.cpp` | The call sites that feed the modules (MarkBlockInFlight, ReleaseBlockRequestOwner, ProcessBlock, getdata push, block receive, expiry). |
| analyzer | `contrib/forensics/analyze_forensic_csv.py` | Offline analyzer for the `ibdforensic` CSV dump (tracked in the repo). |

There is no index file in `doc/forensics/`; this document is the index for the
instrumentation.

## 4. Runtime flags

`Code fact`.  The flags are declared in the help text at `src/init.cpp`
(help section lines 589-609) and read in `AppInit2` (`src/init.cpp:971-987`).

| Flag | Default | Meaning |
|---|---|---|
| `-blockrequesttrace=<n>` | `0` | Trace anomalous block request lifecycles (`BLOCKREQTRACE` + `SYNC_EVENT` streams). |
| `-blockrequesttracehash=<hash>` | empty | Limit block request tracing to one block hash (requires `-blockrequesttrace`). A malformed hash is an `InitError` at startup. |
| `-continuitybreakms=<n>` | `60000` | Minimum no-connect gap (ms) before the one-shot `FIRST_CONTINUITY_BREAK` event fires (requires `-blockrequesttrace`). |
| `-ibdefficiencytrace=<0\|1>` | `0` | IBD efficiency counters and `IBDEFFICIENCY` summaries. |
| `-ibdactivepathtrace=<0\|1>` | `0` | Emit `IBD_ACTIVE_1S`, `IBD_SLOW_BLOCK`, `IBD_STATE_TRACE`. |
| `-ibdactiveslowthresholdms=<n>` | `50` | Slow-phase threshold for `IBD_SLOW_BLOCK` (ms); the effective floor is 1 ms. |
| `-ibdblocklatency=<0\|1>` | `0` | Per-block GETDATA-to-CONNECT latency decomposition. |
| `-ibdblocklatencycsv=<file>` | empty | CSV dump path for `ibdblocklatency` (written at shutdown). Empty = summary only. |
| `-ibdforensic=<0\|1>` | `0` | Per-getdata-batch request instrumentation. |
| `-ibdforensicpath=<file>` | empty | `ibdforensic` CSV dump + summary path (written at shutdown). Empty = summary only. |
| `-ibdmaxactiveperpeer=<n>` | `128` | IBD per-peer active request window; clamped to `[1,512]`; zero/negative/non-numeric falls back to the default; only consulted during IBD. |
| `-ibddivfuture=<0\|1>` | `0` | Experiment A: future-supply diversification (see section 8). |
| `-ibddivfrac=<d>` | `0.15` | Max fraction of deferred future candidates that may leave the announcer lane when the announcer still has window capacity; parsed as permille, clamped to `[0,1]`. |
| `-processblockrejecttrace` | off | Not in the help text; toggles the `pb_reason=` field on `REJECT_RETRY_RECORDED`/`REJECT_RETRY_SUPPRESSED` `SYNC_EVENT` lines. Read at `src/init.cpp:979`. |
| `-getinfosyncprobe=<n>` | `0` | Log cached P2P state before, during, and after `getinfo`. |
| `-rpcperftrace=<n>` | `0` | Trace `getinfo`/`getmininginfo` RPC performance. |
| `-synclockdiagnostics=<n>` | `0` | Log long sync-related lock waits and holds. |
| `-synclockthresholdms=<n>` | `250` | Lock diagnostic threshold. |
| `-syncstalltimeout=<n>` | `15` | Seconds without chain progress before sync recovery. |
| `-syncstallcooldown=<n>` | `15` | Initial recovery cooldown; failures back off. |
| `-logtimestamps` | off | Prepend a timestamp to debug output. |
| `-shrinkdebugfile` | on (when no `-debug`) | Shrink `debug.log` on startup. |
| `-maxorphanblocks=<n>` | `2500` | Orphan in-memory cap (`DEFAULT_MAX_ORPHAN_BLOCKS`, `src/main.h`). |

Init order (`Code fact`): `InitBlockRequestTrace` first (an invalid
`-blockrequesttracehash` aborts startup), then `InitIBDEfficiencyTrace`,
`InitProcessBlockRejectTrace`, `ibdactivepath::InitIBDActivePathTrace`, then
`ibdblocklatency::SetEnabled` and `ibdforensic::SetEnabled`
(`src/init.cpp:971-987`).  The shutdown dumps run in `Shutdown()`
(`src/init.cpp:154-157`): `ibdblocklatency::Dump()` then `ibdforensic::Dump()`.

## 5. Flag parsing and clamp semantics

`Code fact`.

* `-ibdmaxactiveperpeer`: parsed with `ParseInt64`; non-numeric, zero and
  negative values fall back to `128` (`MAX_DEFERRED_INV_ACTIVE_PER_PEER`);
  values above `512` (`MAX_DEFERRED_INV_ACTIVE_GLOBAL`) are clamped
  (`src/net.cpp:1004-1020`).  `GetMaxActiveBlockRequestsPerPeer()`
  (`src/net.cpp:1025-1035`) returns the configured value only while
  `IsInitialBlockDownload()` is true and the default `128` otherwise.
* `-ibddivfrac`: parsed with `strtod`; non-numeric, trailing-garbage and
  out-of-range values fall back to `0.15`; valid values are converted to
  permille (`0.15` → `150`); `"0"` → `0`, `"1"` → `1000`
  (`src/net.cpp:1057-1071`).
* Both configs are loaded lazily on first use (once per process) so unit tests
  can reload them via the `...ForTesting()` resets.
* Unknown flags (for example `-firstorderbreaktrace`, see section 19) are
  accepted silently by `ParseParameters` (`src/util.cpp:658-701`): they are
  stored in `mapArgs` but never read, and no warning is printed.

## 6. Safety properties and invariants

`Code fact`.

* **Observation-only.**  `ibdforensic` explicitly guarantees that no function
  influences the scheduler: it takes no state from it, returns nothing to it,
  and the in-flight/timeout/cap decisions are untouched (`src/ibdforensic.h`
  INVARIANT block).  `ibdmetrics` uses relaxed atomics only, no new hot lock,
  no per-event strings or printf (`src/ibdmetrics.h` header comment).
* **Disabled is a no-op.**  With the module disabled every record function
  returns before touching shared state, so normal (non-forensic) runs are
  bit-identical in behaviour (`src/ibdforensic.h`).
* **Leaf locks.**  `ibdforensic::g_mutex` and `ibdblocklatency::cs` are leaf
  locks: they are never held while acquiring any other lock, so they can be
  taken from inside `cs_main` / `cs_vSend` / `cs_vNodes` without a cycle.
* **No reverse-order trace locking.**  `blockrequesttrace` passes event state
  by value so tracing never acquires `cs_main`, `cs_vNodes`, or a peer lock in
  reverse order (`src/blockrequesttrace.h`).
* **Bounded state.**  `ibdblocklatency` keeps `g_samples` (fixed 16384-entry
  ring), `g_records` (16384 incomplete lifecycles, stale after 300 s), and a
  1 MiB fully-buffered stdio CSV (`src/ibdblocklatency.cpp`).  `ibdforensic`
  keeps a per-hash canonical entry and per-batch records; the trace registry in
  `net.cpp` is bounded to 16384 hashes and 16 peers per hash
  (`BLOCK_REQUEST_TRACE_MAX_HASHES` / `..._PEERS_PER_HASH`).
* **Single-owner invariant.**  A hash has at most one owner peer, in state
  `queued` or `inflight`; the scheduler enforces this at every admission point
  and the trace counts conflicts (`duplicate_assign_after_receive_release`,
  `duplicate_send_after_receive_release` in the `BLOCKREQTRACE SUMMARY`).
* **One-shot continuity gate.**  `FIRST_CONTINUITY_BREAK` fires at most once
  per process; re-arming is only by re-enabling the trace
  (`BlockRequestTraceContinuityBreakReset`).

## 7. Scheduling context

`Code fact`.  The instrumentation is easiest to interpret with the scheduler
model in mind:

* **Pressure.**  Per peer, active pressure = queued block requests + in-flight
  block requests.  `peerLiveActivePressure` mirrors
  `setAskForBlocks.size() + setBlocksInFlight.size()` and is updated at the
  same six loci that maintain the counted metrics: add askfor, erase askfor,
  `MarkBlockInFlight`, `ClearBlockInFlight`/receive, `ExpireBlockInFlight`,
  and disconnect reset (`src/net.h:1010-1018`, update sites in `src/net.cpp`).
* **Budget.**  `GetDeferredBlockRequestBudget` returns the number of block
  requests that may be admitted; when it is zero the block inv is deferred.
  Global pressure uses `MAX_DEFERRED_INV_ACTIVE_GLOBAL` (512) with a
  fail-closed value `MAX_DEFERRED_INV_ACTIVE_GLOBAL + 1` when the
  `cs_vNodes` lock cannot be taken (`src/main.cpp`, `src/ibdmetrics.h`).
  Orphan storage is *not* subtracted from the budget (it is bounded
  separately by the storage caps).
* **In-flight timeout.**  `BLOCK_IN_FLIGHT_TIMEOUT = 5` seconds
  (`src/net.h:1843`).  `ExpireBlockInFlight` (`src/net.h:1841`) decrements
  pressure, bumps `inflight_timeouts`, requests a pipeline wake, consumes the
  diversification attribution ledger if any, and — when `ibdforensic` is
  enabled — computes the head age (age of the oldest in-flight hash at the
  moment of expiry).
* **Orphan limits.**  `MAX_ORPHAN_BLOCKS_PER_PEER` = 750,
  `DEFAULT_MAX_ORPHAN_BLOCKS` = 2500, orphan-limit reject cooldowns bounded by
  `MAX_ORPHAN_LIMIT_REJECTED_PER_PEER` = 750 and
  `MAX_ORPHAN_LIMIT_REJECTED_GLOBAL` = 50000 (`src/net.h`, `src/main.h`).
* **Frontier admission.**  The block directly after the local active tip may be
  admitted even at zero budget — see
  `doc/forensics/frontier-admission-exemption.md`.  The related counters
  (`frontier_response_armed/consumed/pending`, `frontier_reject_*`,
  `FRONTIER_ADMIT` / `FRONTIER_DENY` / `FRONTIER_CLEAR` trace events) expose
  this path.
* **Stalled-sync recovery.**  With no chain progress for `-syncstalltimeout`
  seconds, a recovery peer is assigned (`MaybeQueueStalledSyncRecovery`,
  `STALL_RECOVERY`/`STALL_RECOVERY_OWNER`/`STALL_TOUCH` events).

## 8. Experiment A: future-supply diversification

`Code fact`.  `-ibddivfuture=1` enables a bounded re-routing of deferred
*future* candidates (hashes announced ahead of the current window) to other
eligible peers instead of always re-requesting the announcer.

Mechanism:

* `CollectEligibleFutureSupplyLanes` (`src/net.cpp:1166-1189`) builds the lane
  set from the `vNodesCopy` snapshot: peers that are connected full nodes, not
  disconnecting, block-sync capable, able to advance beyond `nBestHeight`, and
  with a free window slot (`peerLiveActivePressure < nWindow`).
* `ChooseDeferredDispatchLane` (`src/net.cpp:1191-1246`) picks the
  lowest-pressure eligible lane, tie-broken by the round-robin seq tick
  (`peerDiversifySeq`, lower first).  Diversification is forced when the
  announcer is saturated (`nFromPressure >= nWindow`); otherwise it happens
  with probability `-ibddivfrac` (default 0.15).
* The single deferred *frontier* candidate keeps the announcer path verbatim
  (frontier-exemption semantics unchanged); orphan-source retries never pass
  through the deferred refill, so the orphan lane is inherently untouched
  (`src/main.cpp:572-604`).
* Attribution: when a dispatch leaves the announcer lane, the announcing peer
  is recorded in `mapDiversifyAnnounce` (`RecordDiversifyDispatch`,
  `src/net.cpp:1110-1114`) and consumed on receive, timeout, or pre-dispatch
  removal (`TakeDiversifyAnnounce`, `src/net.cpp:1128-1139`).  This feeds the
  `announce_peer`/`diversified` generation columns and
  `diversify_other_lane_timeout`.
* Counters: `diversify_candidates`, `diversify_picked_other_lane`,
  `diversify_picked_announcer`, `diversify_snapshot_skip_lock`,
  `diversify_no_other_lane`, `diversify_other_lane_timeout`
  (`src/ibdmetrics.h`).

## 9. Output streams and line formats

All trace lines go through `printf`, which is redefined to
`OutputDebugStringF` (`src/util.h:274`).  Daemon output lands in
`debug.log` (append, unbuffered); with `-printtoconsole` it goes to stdout
instead.  `OutputDebugStringF` prepends a `%x %H:%M:%S` timestamp when
`-logtimestamps` is on (`src/util.cpp:380-381`).

> `Runtime observation` / `Known limitation`: with `-logtimestamps=1` a line
> appears as `08/03/26 11:54:41 IBD_ACTIVE_1S time_us=...`.  A collector must
> grep `IBD_ACTIVE_1S` anywhere in the line, not `^IBD_ACTIVE_1S`.

### 9.1 `BLOCKREQTRACE`

`Code fact`.  Prefix `BLOCKREQTRACE time_us=<us> event=<EVENT> ...`, emitted
when `-blockrequesttrace=1`.  Events:

`START`, `SUMMARY`, `OWNER_ASSIGN`, `OWNER_RELEASE`, `OWNER_TRANSITION`,
`ASK_SCHEDULE`, `ASK_SKIP` (2 forms), `GETDATA_SEND`, `GETDATA_SKIP`,
`ASK_REMOVE`, `INFLIGHT_CLEAR` (2 forms), `INFLIGHT_EXPIRE`, `BLOCK_RECEIVE`,
`BLOCK_RESULT`, `ALREADY_ASKED_PRUNE`, `REGISTRY_DROP`, `GETBLOCKS_QUEUE`,
`GETBLOCKS_TRIGGER`, `STALL_RECOVERY`, `STALL_TOUCH`, `FRONTIER_ADMIT`,
`FRONTIER_DENY` (3 forms), `FRONTIER_CLEAR`, `ORPHAN_WATERMARK`,
`DEFERRED_WATERMARK` (`src/net.cpp:2340-4817`).  The `SYNC_EVENT`-style
events (`SETBESTCHAIN_COMMIT`, `REORG`, `FIRST_CONTINUITY_BREAK`,
`MISSING_PARENT_*`, `REJECT_RETRY_*`, `STALL_RECOVERY_OWNER`) are described in
section 9.2.

The one-shot gate:

```
BLOCKREQTRACE time_us=<us> event=START enabled=1 filter=<hash|none> hash_cap=16384 peer_cap_per_hash=16 normal_retention_s=60 anomaly_retention_s=1800
```

Sources and results used as `source=` / `result=` values
(`src/blockrequesttrace.h`): sources `inv`, `orphan`, `checkpoint`,
`headers-direct`, `reject-recovery`, `orphan-limit-retry`, `askfor`; results
`accepted_active`, `accepted_indexed`, `orphan_new`, `already_known`,
`orphan_duplicate`, `rejected`, `orphan_limit_ibd`, `accept_failed`,
`true_unindexed`.

### 9.2 `SYNC_EVENT`

`Code fact`.  The continuity/divergence diagnostics emit on the `SYNC_EVENT`
stream, enabled whenever `-blockrequesttrace=1`:

`SETBESTCHAIN_COMMIT`, `REORG`, `FIRST_CONTINUITY_BREAK`,
`MISSING_PARENT_REQUEST`, `MISSING_PARENT_RESOLVED`,
`REJECT_RETRY_RECORDED`/`REJECT_RETRY_SUPPRESSED` (with `pb_reason=` when
`-processblockrejecttrace` is on), `REJECT_RETRY_SCHEDULED`,
`STALL_RECOVERY_OWNER`, plus `CHECKBLOCK_POS_PAYEE_REJECT`,
`CHECKBLOCK_POW_PAYEE_REJECT`, `ACCEPT_BLOCK_BEGIN`, `PROCESS_BLOCK_BEGIN`
(from `src/main.cpp`).

### 9.3 `IBD_ACTIVE_1S`

`Code fact` (exact format, `src/ibdactivepath.cpp:590-603`).  Emitted once per
second by `EmitIBDActive1s`, which is called from the message-handler loop
(`src/net.cpp:7666`) on a ~100 ms tick and rate-limited to 1 s with a
compare-exchange cadence:

```
IBD_ACTIVE_1S time_us=<us> uptime_s=<s> local_height=<h> peers=<n> blocks_1s=<n> getblocks_sent_1s=<n> getdata_sent_1s=<n> zero_pass_due_cap_1s=<n> inv_msgs_1s=<n> askfor_depth=<n> askfor_due=<n> deferred_peer=<n> inflight_peer=<n> free_capacity_peer=<n> getblocks_queued_peer=<n> getblocks_outstanding_peer=<n> passes_sent=<n> stop_empty=<n> stop_no_due=<n> stop_inflight_cap=<n> askfor_to_getdata_avg_us=<n> askfor_to_getdata_max_us=<n> pass_interval_avg_us=<n> pass_interval_max_us=<n> sleep_us=<n> dispatch_delay_avg_us=<n> dispatch_delay_max_us=<n> cs_main_wait_avg_us=<n> cs_main_wait_max_us=<n> processblock_avg_us=<n> acceptblock_avg_us=<n> addtoblockindex_avg_us=<n> setbestchain_avg_us=<n> connectblock_avg_us=<n> raw_write_avg_us=<n> filecommit_avg_us=<n> blockindex_commit_avg_us=<n> chainstate_commit_avg_us=<n> dag_commit_avg_us=<n> wallet_cb_avg_us=<n> peers_inflight_gt0=<n> peers_queued_gt0=<n> inflight_peer_max=<n> queued_peer_max=<n> dominant_peer_inflight_share_pct=<n> global_free_active_slots=<n> global_free_slots_with_deferred=<n> samples_single_peer_over_75pct=<n> samples_global_below_half_with_deferred=<n> wire_latency_avg_us=<n> wire_latency_max_us=<n>
```

The `event=START` line is emitted when the trace is (re)armed
(`src/ibdactivepath.cpp:104`).

### 9.4 `IBD_SLOW_BLOCK`

`Code fact`.  Emitted from `ActivePathTimer` when a phase exceeds the
threshold (`max(1000, -ibdactiveslowthresholdms) * 1000` µs, default 50 ms),
rate-limited to at most one per 5 s:

```
IBD_SLOW_BLOCK time_us=<us> phase=<phase> us=<us> height=<h> threshold_us=<us>
```

### 9.5 `IBD_STATE_TRACE`

`Code fact`.  Emitted on IBD-state change or at most every 5 s
(`src/ibdactivepath.cpp:427-474`):

```
IBD_STATE_TRACE time_us=<us> ibd=<0|1> reason=<name> local_height=<h> fresh_peer_height=<h> peer_median=<n> gui_estimate=<n> estimate_ahead=<n> fresh_height_age_s=<n> local_last_recv_age_s=<n> deferred=<n> queued=<n> inflight=<n> getblocks_outstanding=<n> eligible_ahead_peers=<n> ibd_false_while_estimate_ahead=<0|1>
```

### 9.6 `IBD_BLOCKLAT_1S`

`Code fact`.  Emitted once per second from the same message-handler pass as
`IBD_ACTIVE_1S` (`src/net.cpp:7667`).  Funnel line (always):

```
IBD_BLOCKLAT_1S time_us=<us> samples=<n> lifetime=<n> funnel_received=<n> funnel_unsolicited=<n> funnel_processed=<n> funnel_connected=<n> funnel_orphaned=<n> funnel_incomplete_dropped=<n> outcome_connected_active=<n> outcome_accepted_side=<n> outcome_already_have=<n> outcome_rejected=<n> outcome_timeout=<n> outcome_disconnect=<n> outcome_incomplete_evicted=<n>
```

followed (when samples exist) by one aggregate per interval:
`askfor_to_getdata={mean=<us>,med=<us>,p95=<us>,max=<us>,n=<n>}` and so on for
`getdata_to_recv`, `recv_to_process`, `process_to_accept`, `accept_to_index`,
`index_to_best`, `best_to_connect`, `total` (`src/ibdblocklatency.cpp`).

### 9.7 `IBDEFFICIENCY`

`Code fact` (`src/ibdefficiency.cpp`).  Periodic summary every 60 s
(`IBDEfficiencyMaybeSummary`) plus `IBDEFFICIENCY TOTAL ...` at shutdown:
origin (`blocks_requested`, `blocks_unsolicited`, `requested_ratio`,
`bytes_*`), novelty (`blocks_unique`, `blocks_duplicate_indexed`,
`blocks_duplicate_orphan`, `unique_ratio`, `bytes_*`), outcome
(`blocks_accepted_active`, `blocks_accepted_side`, `blocks_orphan_new`,
`blocks_rejected`, `blocks_retry_recorded`, `active_ratio`, `efficiency`,
`blocks_per_height`, `bytes_*`).

## 10. `ibdforensic` CSV schema

`Code fact` (`src/ibdforensic.cpp Dump`, lines 731-865).

The dump file is written **at shutdown only** (section 19).  Layout:

1. Human-readable summary block starting with `IBDFORENSIC SUMMARY` (also
   printed to `debug.log`/stdout), containing `batches=... canonical_entries=...
   unsolicited_receipts=... clean_arrivals=...`, a per-position latency table
   (`bucket=[0=head..9=tail] count mean_us p50_us p95_us max_us
   timeouts_in_bucket`), `latency_slope_us_per_seq` (least squares over clean
   arrivals), `timeouts_total`/`never_received_total`, generation closure
   counts (`closed_receive/timeout/queue_removal/clear/disconnect`),
   `received_after_timeout` breakdowns, hashContinue statistics, and
   getblocks rate-limit counters.
2. Per-hash header (column names, parsed verbatim):

```
# peer,batch_id,seq,n_hashes,hash,was_hashcontinue,mark_time_us,recv_time_us,timeout_time_us,received_after_timeout,rerequested,rerequested_other_peer,rerequest_peer,rerequest_time_us,send_buffer_bytes,generation_id,generation_mark_us,generation_release_us,generation_release_reason,enqueue_time_us,first_socket_send_us,nsend_first_send,progress_last_us,head_age_at_expiry_us,recv_framing_complete_us
```

   followed by one comma-separated row per canonical hash entry.  All
   timestamps are wall-clock microseconds (`GetTimeMicros()`); `0` means
   "never".
3. `#generations` marker, a generation header
   `# generation_id,batch_id,hash,peer,mark_us,release_us,reason` (plus
   `,announce_peer,diversified` when `-ibddivfuture` attribution is active),
   and one row per generation.  Reasons: `receive|timeout|queue-removal|clear|disconnect`.

Field semantics (`src/ibdforensic.h`):

* `mark_time_us` — getdata push time (when in flight was marked).
* `recv_time_us` — block dispatch time (message-handler processing of the
  block); `recv_framing_complete_us` — when the block message finished
  framing in `ReceiveMsgBytes`.  Their delta is the handler dispatch backlog.
* `timeout_time_us` — when the in-flight request expired; `received_after_timeout`
  — receipt happened after the timeout fired.
* `rerequested` / `rerequested_other_peer` / `rerequest_peer` /
  `rerequest_time_us` — first re-request after release (only possible after the
  scheduler released ownership, so a hash appearing twice is always a
  re-request signal).
* `was_hashcontinue` — this hash was the peer's `hashContinue` at send time.
* `progress_last_us` — requesting peer's last genuine receive at the moment the
  entry's generation was released.
* `head_age_at_expiry_us` — age of the oldest in-flight hash of the peer at the
  moment this entry expired (0 = never expired).
* `generation_*` — the delivering generation (the one whose `[mark,release]`
  window contains the dispatch), falling back to the most recent.

## 11. `ibdblocklatency` CSV schema

`Code fact` (`src/ibdblocklatency.cpp`, header at lines 181-190, rows at
157-171, footer at 647-662).

The CSV is **streamed**: opened in `SetEnabled` during `AppInit2`, fully
buffered (1 MiB), one row per terminal lifecycle, no per-row flush, closed at
shutdown.  Header (two comment lines):

```
#ibdblocklatency: per-block GETDATA->CONNECT decomposition
# request_peer,receive_peer,height,size,ping_ms,req_peer_pressure,global_inflight,global_queued,global_deferred,outcome,orphaned,askfor_to_getdata_us,getdata_to_recv_us,recv_to_process_us,process_to_accept_us,accept_to_index_us,index_to_best_us,best_to_connect_us,total_us,hash
```

Data row column order matches the header.  Notes:

* `outcome` — `connected_active | accepted_side | already_have_duplicate |
  rejected | timeout | disconnect | incomplete_evicted`
  (`OutcomeName`, `src/ibdblocklatency.cpp:561-574`).
* `orphaned` is 1 when the block was recorded as an orphan during its
  lifecycle (`RecordBlockOrphaned`).  `Known limitation`: for an orphaned
  block the `process_to_accept` interval spans the whole orphan residency
  (first `ProcessBlock` that inserted it as an orphan, later `AcceptBlock` in
  descendant processing), so it is *not* CPU self-time.
* `ping_ms`, `req_peer_pressure`, `global_inflight/queued/deferred` are
  snapshots taken at receive time.
* A missing stamp yields `-1` for that interval; `total_us` falls back to the
  terminal event time when T7 was never reached.
* Terminal paths: non-`receive` releases map to `timeout` or
  `incomplete_evicted` (`src/net.cpp:2396-2401`); disconnect releases map to
  `disconnect` (`src/net.cpp:2442-2447`); already-have / rejected / accepted
  side outcomes are recorded at their `ProcessBlock`/`AcceptBlock` sites.
* Footer at shutdown:

```
# OUTCOME_SUMMARY connected_active=<n> accepted_side=<n> already_have_duplicate=<n> orphaned=<n> rejected=<n> timeout=<n> disconnect=<n> incomplete_evicted=<n> unsolicited=<n>
```

## 12. Shutdown summaries and dump lifecycle

`Code fact`.  `Shutdown()` runs, in order (`src/init.cpp:154-157`):
`FlushIBDBatch()`, `IBDEfficiencyShutdownSummary()`, `ibdblocklatency::Dump()`,
`ibdforensic::Dump()`.

* `ibdblocklatency::Dump` reclaims still-open records as `incomplete_evicted`,
  writes the footer, closes the CSV, prints `IBD_BLOCKLAT_DUMP wrote=<path>`,
  then a summary (`IBD_BLOCKLAT_SUMMARY ...`) and per-outcome
  (`IBD_BLOCKLAT_OUTCOME <name>=<n>`) and per-interval
  (`IBD_BLOCKLAT_INTERVAL <name> lifetime_mean_us=... lifetime_n=... ring_*...`)
  lines.
* `ibdforensic::Dump` prints the summary to `debug.log`/stdout and rewrites
  the configured dump file (re-opened `"w"` at shutdown; the file is created
  empty at startup as a fail-fast check).
* `ibdefficiency` prints the `IBDEFFICIENCY TOTAL` summary when the trace was
  enabled.

## 13. Experiment A/B runbook

The experiments were run on the `forensics/ibd-block-latency-audit` branch.
`Runtime observation`: the capture runs used the following forensic-relevant
settings (RPC bindings and credentials are deliberately not reproduced here;
the RPC endpoint must be bound to loopback with strong auth):

```
logtimestamps=1
shrinkdebugfile=1
debug=0
debugnet=0
debugchain=0

ibdefficiencytrace=0
blockrequesttrace=0
processblockrejecttrace=0
ibdactivepathtrace=1
ibdforensic=1
ibdforensicpath=/home/user/ibd-forensic-cap256.log
ibdmaxactiveperpeer=256
```

`Known limitation`: the captured `innova.conf` also contained
`firstorderbreaktrace=0`.  No such flag exists in the current `src/` (it is
referenced only by the untracked local analysis script
`contrib/forensics/analyze_first_order_break.py`).  `ParseParameters` accepts
it silently and no code reads it, so the line is inert.

Suggested experiment matrix:

| Experiment | Change vs. baseline | Purpose |
|---|---|---|
| A (baseline) | `-ibdmaxactiveperpeer=128` (default), no `-ibddivfuture` | Reference latency/duplicate profile. |
| B | `-ibdmaxactiveperpeer=256` (captured run) | Wider per-peer window. |
| C | `-ibdmaxactiveperpeer=384` (the analyzer's default cap size) | Wider still; the analyzer refuses a cap comparison when the input has no `n_hashes=384` batches. |
| D | add `-ibddivfuture=1 -ibddivfrac=0.15` | Future-supply diversification vs. the same cap. |

For every run capture: `debug.log` (forensic streams), the `ibdforensic`
dump, the `ibdblocklatency` CSV, and periodic `getinfo` snapshots (section 14)
in one timestamped run directory, and keep the `innovad` binary hash and git
commit recorded with the run.

## 14. Capture run layout and tooling

`Runtime observation`.  The capture manager is **external operator tooling**:
it is not shipped in this repository, and this document does not claim it is.
It produced a run directory such as
`/home/user/innova-logs/ibd-forensic-runs/generation-ledger-cap256-20260803T115440Z`
with a `latest-cap256` symlink, laid out as:

```
meta/        innova.conf, innovad.sha256, innovad-cmdline.txt, df/free/ip-link/proc-net-dev snapshots, MANIFEST.sha256/.verify.txt, peer-window-collector.pid
telemetry/   IBD_ACTIVE_1S.log, getinfo-5s.jsonl, ibd-5s.csv, peer-window-5s.csv, collector-errors.log, ibd-forensic-cap256.log (empty during the run)
snapshots/   minute-snapshots.log, manual-* and post-* snapshots
logs/        debug.final.log, collector / launch logs
final/       getinfo-before-stop.json, getpeerinfo-before-stop.json, shutdown-result.txt, end-of-run system snapshots
scripts/     collector.py
```

The collector (`scripts/collector.py`) samples every 5 s: it writes a
flattened `getinfo` snapshot to `getinfo-5s.jsonl` and a CSV row to
`ibd-5s.csv`, appends to `minute-snapshots.log` every 60 s, tails
`IBD_ACTIVE_1S` into `IBD_ACTIVE_1S.log`, and merges per-process
(`pgrep -xo innovad`) and per-interface counters.  The collector must grep for
`IBD_ACTIVE_1S` without an anchor at the start of line because `-logtimestamps`
prefixes every line (section 9).

## 15. `getinfo` RPC and probe side effects

`Code fact`.  `getinfo` returns an `ibdmetrics` object
(`src/rpcwallet.cpp:341-560`) built by `ibdmetrics::SnapshotAll`, including:
`start_time_unix`, `now_unix`, `uptime_seconds`, `height`; the deferred-budget
group; INV admission (`block_inv_unknown_total/admitted/deferred/...`);
pressure high-water marks; refill counters; pipeline-usefulness counters; the
`active_decrement_*` cause counters; zero-active-period counters; getblocks
decision/queue/wire-sent counters by source; recovery outcomes; frontier
counters; `ibd_state_current/transitions`; the `pipeline_wake` object;
`orphan_limit_*` counters; and the Experiment A `diversify_*` counters.

`Known limitation`: the snapshot is a relaxed-load read, not a consistent
snapshot (documented at `src/ibdmetrics.h:546-547`).

Probe side effects: `getinfo` (and `getmininginfo`) go through a probe path in
the RPC dispatcher (`src/innovarpc.cpp:1447+`).  With `-rpcperftrace=1` the
RPC handler records lock wait / handler micros; with `-getinfosyncprobe=1` the
cached P2P state is logged before, during, and after the call.  Repeated
`getinfo` polling (as the collector does every 5 s) exercises this path and is
documented in `doc/getinfo-sync-side-effects.md`.

## 16. Analyzing the dumps

`Code fact`.  The tracked analyzer
`contrib/forensics/analyze_forensic_csv.py` reads the `ibdforensic` dump and
reports, by declared batch size and by peer: first-block delay, stream
duration, total receive duration (p50/p90/p95/p99), timeout-before-first rate,
fully-late rate, and rerequest / cross-peer rates.  It is column-name driven so
it tolerates the additive generation columns.

```
python3 contrib/forensics/analyze_forensic_csv.py ibd-forensic-cap256.log [--json out.json]
python3 contrib/forensics/analyze_forensic_csv.py --baseline A.log --compare B.log [--json out.json]
```

Defaults: `--min-samples 30`, `--cap-sizes 255,256,384`.  Singleton batches
(`n_hashes == 1`) are excluded from the multi-block stream-duration quantiles
and reported in a dedicated section.  `Runtime observation`: the `ibd-5s.csv`
schema used by the collector is the collector's own CSV (section 14), distinct
from the on-disk `ibdforensic`/`ibdblocklatency` schemas in sections 10-11.

## 17. Tests

`Code fact`.  Unit tests live in `src/test/` and are built into the
`test_innova` binary.  Run a suite with:

```
./test_innova --run_test=p2p_sync_tests
./test_innova --run_test=ibdforensic_tests
./test_innova --run_test=ibdblocklatency_tests
```

or via the makefile phony targets (`src/makefile.unix`): `check-ibdforensic`
and `check-ibdblocklatency`.  Coverage highlights: `ibdblocklatency_tests`
exercises the T0..T7 lifecycle, interval integrity, funnel counters, the
unsolicited path, disabled no-op, and CSV dump; `ibdforensic_tests` covers
batch records, generation lifecycle, timeout attribution, CSV dump, and
send-meta alignment; `p2p_sync_tests` covers diversification (round-robin,
frontier exemption, no-ownership-stealing, config parse/clamp, the
`peerLiveActivePressure` mirror loci), continuity break, orphan-limit cooldown,
frontier admission, pipeline wake, and the config helpers.

## 18. Build

`Code fact`.  Innova Core builds with the classic unix makefile:

```
cd src
make -f makefile.unix -j"$(nproc)" innovad      # daemon
make -f makefile.unix -j"$(nproc)" test_innova   # unit-test binary
```

## 19. Known limitations

* `ibdforensic` writes its dump file only at shutdown; during the run the file
  exists but is empty (`Runtime observation`: the captured
  `ibd-forensic-cap256.log` stayed 0 bytes until shutdown).  The
  `IBD_BLOCKLAT_1S` / `IBD_ACTIVE_1S` streams in `debug.log` are the
  live view.
* `-logtimestamps` prefixes trace lines, so line-anchored greps fail; grep for
  the marker substring instead (section 9).
* T2 is full-message framing complete, not first byte, so the
  `getdata_to_recv` interval includes message framing at the remote and any
  socket buffering.
* For orphaned blocks, `process_to_accept` includes orphan residency (not CPU
  self-time); filter on `orphaned=1` when interpreting it.
* `-firstorderbreaktrace` is not a flag in `src/`; only the untracked local
  script references it.  Any config line for it is inert (section 13).
* `-processblockrejecttrace` is functional but absent from the help text.
* The `getinfo` `ibdmetrics` snapshot is not a consistent read.
* The 1 s emitters are driven by the message-handler loop (~100 ms tick), so
  emission times jitter when the handler is stalled; the cumulative `sleep_us`
  and the `pass_interval_avg/max_us` fields in `IBD_ACTIVE_1S` quantify the
  handler idle/stall.
* No root cause is asserted in this document.  Peer `23.118.242.131:60016`
  appearing in degraded captures is a correlation, not a proven cause; treat
  any attribution as a `Working hypothesis` until reproduced.

## 20. Related documents and provenance

* `doc/forensics/ibd-root-cause-investigation.md` — earlier root-cause notes.
* `doc/forensics/frontier-admission-exemption.md` — frontier admission
  exemption invariant and mechanism.
* `doc/forensics/artifacts/` — experiment artifacts.
* `doc/getinfo-sync-side-effects.md` — `getinfo` probe side effects and the
  `-blockrequesttrace` cross-reference.
* `contrib/forensics/analyze_forensic_csv.py` — the tracked offline analyzer.
* `contrib/devtools/analyze_block_request_trace.py` and
  `contrib/devtools/analyze_innova_debug_log.py` — older dev-tool log
  analyzers.
* Untracked local tooling (not shipped): the capture manager and
  `contrib/forensics/analyze_first_order_break.py`.

All facts in this document were verified against the current `src/` tree on
branch `forensics/ibd-block-latency-audit`.  Where this document disagrees
with any earlier note (for example the `BLOCKREQTRACE` prefix rather than
`BLOCK_REQ`, and the `SYNC_EVENT` stream), the code wins.
