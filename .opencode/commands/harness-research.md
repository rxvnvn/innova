---
description: Run Innova harness research phase
---

Execute the RESEARCH phase of Innova Agent Harness v0.1.

This is a primary orchestration command. You act as the orchestration layer;
the actual investigation is delegated to the read-only researcher subagent.
You hold the write path for the durable artifact.

## 1. Read task contract and context

Read, in order:

- /home/user/data/innova-logs/agent-harness/current_task.md
- /home/user/data/innova-logs/agent-harness/task_context.md

Do not read the coordination ledger except on the targeted conditions stated
in AGENTS.md.

## 2. Validate the active task

If current_task.md is empty, whitespace-only, or has no concrete Objective:

- write to /home/user/data/innova-logs/agent-harness/research.md a stub that
  states NO_ACTIVE_TASK and explicitly records that no research was performed;
- print NO_ACTIVE_TASK;
- STOP. Do not fabricate research.

## 3. Launch the researcher subagent

Invoke the Task tool with subagent_type "researcher".

Pass the exact task contract: the researcher must start from current_task.md
and task_context.md, inspect only the git diffs and source named there, stay
inside the declared scope, and return its complete structured report plus
exactly one RESEARCH VERDICT as its final message.

The researcher is read-only (edit: deny) and must not modify any files,
including harness artifacts. It returns the report; you persist it.

## 4. Persist the durable report

Write the researcher's returned report verbatim to:

/home/user/data/innova-logs/agent-harness/research.md

Overwrite only that harness artifact.

If the researcher stopped with SCOPE_EXPANSION_REQUIRED, persist its
stop-report verbatim. The durable artifact must record the stop result and
must not contain invented research.

## 5. Report and STOP

Print a concise summary and the RESEARCH VERDICT.

Then STOP. This is a HUMAN GATE.

Do NOT automatically start the planner (/harness-plan) or any other phase.
The pipeline pauses here for human review.
