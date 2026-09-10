# Fix the shared test-fixture teardown flake before finishing M18

The `QThread: Destroyed while thread is still running` abort first recorded as an
M17 watch item is no longer intermittent enough to live with. Measured on
2026-09-10 against M18's new deep tests: three consecutive `ctest -R
DeepRenderPipelineTest` runs gave 2/4, 0/4 and 2/4 failures — roughly a 50% flake
rate, and a full-suite run failed 5 of 121 (three `BaseTest` cases plus two
`DeepRenderPipelineTest` cases). In every instance the gtest body printed
`[  OK  ]` and `[  PASSED  ]` first and the process then aborted during teardown,
which places the fault in the shared `BaseTest` fixture's teardown rather than in
any one case.

The original decision was to raise it as its own milestone **at the M18 gate**.
That premise no longer holds: it is not confined to `BaseTest` any more but hits
anything built on the app fixture, so every remaining M18 task — all six of which
verify through ctest — would be verified against a suite that fails half the time
for reasons unrelated to the change under test. An intermittently red
`build-and-test` job is worse than a failing one precisely because it stops
telling you anything.

Decided: fix it now, as its own milestone (M26), merged to the default branch
before M18 continues, with M18 then rebased onto a green base. A separate
milestone rather than an M18 task because the defect is inherited M17 fixture
code and has nothing to do with deep compositing — folding it into M18 would put
an unrelated fixture fix in the deep-compositing PR diff. This supersedes the
"raise it at the M18 gate" note in `MILESTONES/M18-deep-compositing-v1.md`.
