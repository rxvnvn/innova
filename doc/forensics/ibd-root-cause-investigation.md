# Engineering note

One of the most important outcomes of this investigation is methodological rather than code-related.

The production fix itself is intentionally minimal. However, arriving at that fix required several weeks of forensic work.

The investigation included:

* runtime instrumentation;
* request ownership tracing;
* inflight lifecycle analysis;
* orphan classification;
* ProcessBlock result taxonomy;
* block-request timeline reconstruction;
* first-order receive tracing;
* static audit of request scheduling.

Only after these independent lines of evidence converged was it possible to identify a single scheduling mechanism that could explain the observed behavior.

As a result, the production change is small, but the confidence in that change is high.

This investigation demonstrates an important engineering principle:

> Small production patches often require large investigations.
> The size of a correct fix is not proportional to the effort required to prove that it is the correct fix.

Without the preceding forensic work, applying the same code modification would have been a speculative change with unknown side effects.

The investigation transformed a hypothesis into an evidence-backed production fix.
