# 2026-08-30 — Trunk-based development on `main`

The fork inherited upstream's release-branch layout: the default branch is
`RB-2.6`, alongside ~25 `RB-*` and experiment branches mostly dead since
2021-2024. What this project actually does is trunk-based — short-lived
`milestone/*` branches, PR, squash merge — against a branch named as though it
were a maintenance line for Natron 2.6.

The default branch becomes `main`. `RB-2.6` is kept as a frozen pointer at the
rename commit rather than deleted, so existing links resolve and the upstream
bridge recorded in `2026-08-29-upstream-agnostic-fixes.md` still works. The
legacy `RB-*` branches are kept untouched and are explicitly not merge targets:
they cost nothing, they are the historical record, and pruning them would break
that bridge for no gain.

Branch naming going forward: `milestone/<id>-<slug>` for planned milestone work,
`fix/<slug>` for everything else. Ad-hoc names like `ci-smoke-test-m2p3t1a`,
created mid-M2, are what this convention exists to prevent.
