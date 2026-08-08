# Stage 1c — provisional header graph performance

Status: profiling/design evidence; Stage 2 not implemented.

## Finding

The original per-header insertion path scanned the complete node map while
linking children and rebuilt active-path state for every header. A 2000-header
response therefore incurred quadratic work. Stage 1c adds `InsertBatch`, which
links the batch once, derives heights in forward order, and updates the active
path once.

## Synthetic scaling

The deterministic graph test reports batch insertion times (milliseconds):

| headers | time |
|---:|---:|
| 100 | 0.235 |
| 200 | 0.474 |
| 400 | 1.025 |
| 800 | 2.194 |
| 1600 | 4.715 |
| 3200 | 10.003 |
| 6400 | 21.213 |

This is approximately linear (the small super-linear component is ordered-map
lookup), not quadratic. Batch and single insertion retain equivalent graph
semantics, including quarantine and re-anchor behavior.

## Runtime interpretation

The Stage 1b capture's 10.4 s graph insertion was consistent with the former
quadratic path. The same capture also recorded ProcessBlock calls up to 12.4 s;
cs_main wait telemetry was negligible, so this was execution time rather than
lock contention. It occurred on the legacy INV-driven data path, where orphan
and disconnected work can accumulate; it is not evidence that ordinary single
block validation is intrinsically 12 s. A Stage 2 capture must measure orphan
cascade counts and ConnectBlock/SetBestChain components directly.

The observer had 16,000 headers while the connected tip was 7. With the
observation window of 512, this provides substantial lookahead. At 500–800
connected blocks/s, 512 blocks are consumed in roughly 0.64–1.02 seconds;
continuation responses of 2000 headers replenish several seconds of healthy
consumption. The requirement is therefore that lookahead not exhaust, rather
than an unconditional millisecond dispatch deadline.

## Concurrency decision

No additional concurrency mechanism is selected at Stage 1c. The existing
single message-handler thread and non-preemptible ProcessBlock remain. The
headers graph now has proportional ingestion cost; Stage 2 must verify that
legacy orphan cascades disappear when requests are ordered and that lookahead
remains above the active window. A separate consensus/control thread is not
introduced.
