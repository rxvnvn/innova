# Tribus CPU mining

Innova Core includes an optional built-in CPU miner for the Tribus
proof-of-work algorithm. CPU mining is disabled by default and never starts
automatically.

## RPC commands

Start with one worker:

```text
setgenerate true
```

Start, or safely restart, with a specified number of workers:

```text
setgenerate true <threads>
```

Stop mining and wait for all workers to exit:

```text
setgenerate false
```

Query the actual worker state:

```text
getgenerate
getmininginfo
```

`getgenerate` is true only while CPU mining workers are running.
`getmininginfo` reports the same state in `cpumining` and reports the actual
number of live workers in `cputhreads`; `requestedcputhreads` reports the
configured pool size. Network hash-rate fields are not local CPU measurements.

When `threads` is omitted, one worker is used. `-1` selects the number of
logical CPUs reported by the operating system, capped at 16 workers. `0`,
values below `-1`, and values above 16 are rejected. Supplying the same worker
count while mining is already active does not create duplicate workers;
supplying a different count safely restarts the worker pool.

`gethashespersec` estimates the proof-of-work hash rate of the network. It does
not report the local CPU miner's performance.

## Resource use and rewards

CPU mining can create sustained high processor load and significant heat.
Tribus mining efficiency on a general-purpose CPU may be low, and finding a
block is never guaranteed. Do not treat built-in CPU mining as a promise of
profitability.

The generated coinbase follows the active Collateral Node consensus rules.
Currently, 65% of the total proof-of-work reward is paid to the selected
Collateral Node and the miner receives 35%.
