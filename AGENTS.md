# Innova Core Agent Rules

## Mission

This repository is Innova Core.

Engineering work must optimize for correctness, reproducibility,
measurable evidence, forward progress, and reviewability rather than
fast patch generation.

Do not treat model confidence as evidence.

## Authoritative coordination state

The authoritative operational coordination ledger is:

/home/user/data/innova-logs/coordination_ledger.md

The coordination ledger is a long-lived project source of truth, but it must
NOT be read wholesale by harness agents by default.

Harness agents must start from:

/home/user/data/innova-logs/agent-harness/current_task.md

and:

/home/user/data/innova-logs/agent-harness/task_context.md

The task context is a curated working set for the current task.

Consult /home/user/data/innova-logs/coordination_ledger.md only when:

- current_task.md names a specific ledger section, fact, task, or search term;
- task_context.md explicitly requests ledger verification; or
- a concrete contradiction requires checking historical evidence.

When consulting the ledger, use targeted search or bounded excerpts.
Do not read the complete ledger merely to acquire general project context.

Do not replace confirmed facts with assumptions.

## Evidence discipline

Explicitly distinguish between:

- FACT
- OBSERVATION
- HYPOTHESIS
- INFERENCE
- DECISION
- PROPOSED CHANGE
- UNKNOWN

A hypothesis must not silently become a fact.

Runtime observations must include enough provenance to make them
independently checkable.

When evidence conflicts, preserve the contradiction instead of choosing
the more convenient interpretation.

## Scope discipline

Do not silently expand the assigned task.

If the task cannot be completed inside its declared scope, stop and report
why.

Do not opportunistically refactor unrelated code.

Do not modify production behavior during a forensic-only task unless the
task explicitly authorizes implementation.

## Git discipline

Inspect Git state before editing:

    git status --short
    git rev-parse HEAD
    git log -1 --oneline

The working tree may already contain unrelated user changes.

Never overwrite, restore, stage, commit, or otherwise absorb unrelated
pre-existing changes.

Work in the current branch unless explicitly instructed otherwise.

Do not create a new branch without explicit approval.

Do not commit unless the task explicitly authorizes a commit.

If a commit is authorized, keep it narrowly scoped.

## Innova synchronization engineering rules

Pipeline occupancy is not equivalent to useful forward progress.

High request volume is not equivalent to useful forward progress.

Do not assume a requested block is lost merely because delivery exceeds a
fixed wall-clock threshold.

Late delivery, request ownership, reissue behavior, ordered frontier
progress, peer diversity, orphan pressure, and actual connect rate must be
reasoned about together.

A scheduler hot path must not perform work proportional to data structures
that grow with chain history unless the cost is explicitly justified and
measured.

Prefer semantic progress metrics over queue-size proxies.

When changing synchronization behavior, preserve ordered forward progress
as the primary invariant.

## Investigation before implementation

Before implementing a non-trivial change:

1. read the active task and task context;
2. consult the coordination ledger only as required by the task
   (targeted/bounded, per the rules above);
3. read the approved research artifact when present;
4. read the approved plan;
5. inspect HEAD and working-tree state;
6. inspect the relevant production code;
7. verify that assumptions in the plan still match the current source.

If the plan is contradicted by current code or evidence, do not improvise
a new architecture during implementation.

Stop and report:

    PLAN_INVALID

with the exact contradiction.

## Verification

Agent-written code is not correct merely because it compiles.

Use the narrowest relevant verification first, then broader verification
when justified.

Daemon build:

    cd ~/innova/src
    make -f makefile.unix -j"$(nproc)" innovad

Qt build:

    cd ~/innova
    qmake ./innova-qt.pro USE_UPNP=- USE_NATIVETOR=-
    make -j"$(nproc)"

Do not invent generic build commands for this repository.

When tests already exist for the affected subsystem, run them.

When runtime behavior is the subject of the change, static reasoning alone
is insufficient unless the task explicitly limits work to design or code
review.

## Harness artifacts

Operational harness artifacts live outside the repository:

    /home/user/data/innova-logs/agent-harness/

Important files:

    current_task.md
    research.md
    plan.md
    implementation.md
    review.md

Do not put raw multi-megabyte logs into these Markdown files.

Store or reference large evidence separately and record exact paths.

## Reporting

Every substantial task report must state:

- repository HEAD inspected;
- working-tree state observed;
- files inspected;
- files changed, if any;
- facts established;
- hypotheses remaining;
- contradictions found;
- verification performed;
- exact failures;
- unresolved questions;
- recommended next action.

Never hide uncertainty.
