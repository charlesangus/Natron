# 2026-08-30 — Merge M7 with CI red; RB-2.6 has been red since M1

M7's PR (#4) was merged with a failing `Test Ubuntu Python 3.10` check, by
explicit decision. Two facts justify it:

1. **The red is pre-existing.** `RB-2.6` has failed CI on every run since M1
   — including the run for "chore(plan): close out M4 (CI/CD rebuild)". M7's
   diff adds only new files under `tools/ci/local/` plus plan updates, none of
   which participate in the build, so no content of M7 could have made CI green
   or red.
2. **The failure is exactly what M2 exists to fix.** CI failed with
   `moc_qhttprequest.cpp:227:62: error: conversion from
   'QHttpRequest::HttpMethod' to 'QChar' is ambiguous` — byte-for-byte the
   error M7's own local loop reproduced in 4 seconds, versus 2m46s plus queue
   time on Actions. The PR's red is therefore also the cleanest available
   evidence that the local loop faithfully reproduces CI, which is the one
   thing it had to prove.

**On M4's status:** M4 remains `done`. Its deliverable was a working CI
workflow, and the workflow does work — it builds the right container, runs the
right steps, and correctly reports failure. What is red is the *code* it
compiles, which is M2's scope and already tracked. Recording this so a later
reader does not mistake a green M4 row for a green default branch: it has not
been green since M0.
