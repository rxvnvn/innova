# B3-v2 Reproducible Impaired Synchronization Harness

This harness is a new tracked stress test. It is **not** the lost original B3
implementation and must not be used to rewrite the historical B3 result.

## Frozen profile

The initial profile is derived from the historical B3 evidence:

| Field | B3-v2 default |
| --- | --- |
| Source | fresh moving-tip regtest node |
| Topology | one client peer through one loopback relay |
| One-way delay | 6950 ms |
| Data drop | 10% whole-message probability |
| Seed | 1337, independently seeded per direction |
| Liveness exemptions | `version`, `verack`, `ping`, `pong`, `addr` |
| Header treatment | `getheaders`/`headers` delayed, never dropped |
| Active capacity | production defaults: 128 per peer / 512 global |
| Client orphan cap | production default: 750 per peer |

The explicit header rule is B3-v2's two-plane contract. It prevents a random
header drop from making the ordered experiment invalid while applying the same
impaired block/announcement path to CONTROL and EXPERIMENT. Header messages
still pay the configured one-way delay. Any calibration change must be made to
the frozen profile before comparing schedulers and recorded in the run output.

## Validity gates

A run is valid only when:

1. The fresh source is mining and its height increases during readiness.
2. The relay logs `READY` and its counters show the requested delay/drop profile.
3. CONTROL completes productive `getblocks -> INV -> AskFor -> getdata -> block` discovery.
4. EXPERIMENT completes at least two `getheaders -> headers` rounds and has a
   non-empty ordered window.
5. Both runs use fresh source/client lifecycles, identical profile values, and
   the same fixed duration.

The initial CONTROL calibration target is structural, not numerical: a growing
source/client gap, a busy pipeline, receive/connect divergence, orphan
pressure, and a prolonged flat or near-flat frontier. If that does not occur,
the profile is not yet a B3-v2 gate and must be calibrated before EXPERIMENT.

## Usage

The runner requires an already-built `innovad` and RPC credentials accepted by
the local regtest daemon. It performs one fresh lifecycle per invocation:

```bash
contrib/forensics/b3v2/run_b3v2.sh \
  --mode control \
  --innovad /home/user/innova/src/innovad \
  --run-dir /tmp/b3v2-control
```

Use `--mode experiment` only after CONTROL satisfies the calibration gate.
The runner records exact commands, profile, readiness heights, duration, and
relay log under the run directory. It does not tune production parameters.

## Lightweight checks

```bash
python3 contrib/forensics/b3v2/smoke_test.py
bash -n contrib/forensics/b3v2/run_b3v2.sh
```

The smoke test proves frame extraction and policy classification without
starting a daemon. A live contract check requires the fresh source/client
runner and is intentionally separate from this harness-definition task.
