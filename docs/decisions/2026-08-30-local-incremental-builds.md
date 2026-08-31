# 2026-08-30 — Local incremental builds before further feature work

Stand up a local build/test environment mirroring `aswf/ci-baseqt:2027.0` — a
long-lived container with a persistent build tree, ccache, and Ninja — and use
it as the primary iteration loop from now on. GitHub Actions remains the
merge gate, not the debugging tool.

**Rationale.** Every test-suite run cost a full cold build on GitHub Actions.
Debugging `M2.P3.T1a` (the PySide6/Shiboken6 smoke test) degenerated into
pushing five consecutive `ci(temp)` commits whose only purpose was to make a
stack trace readable. The fixed cost of building the local loop is smaller
than the cost of continuing that pattern across M2, M3, and M5.

**Scope.** Local only — CI's own build is deliberately left alone; adding
ccache and dependency caching to the workflow is a separate, later change,
and touching the gating pipeline while M2's failure is under diagnosis would
confound it. The tooling is committed under `tools/ci/local/` rather than kept
as untracked scaffolding, so it is reproducible for anyone cloning the fork and
sits visibly adjacent to the CI steps it must mirror. The local image pins
`2027.0` to match `.github/workflows/ci.yml` exactly, which conflicts with the
`2027.1` recorded in `2026-08-29-pin-exact-aswf-tag.md`; that mismatch is
tracked under the board's `# Open questions` and resolved after M2 closes.

Planned as **M7**, which runs ahead of the rest of the board. M2 is parked at
`blocked` until M7's gate passes.
