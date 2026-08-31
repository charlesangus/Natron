# 2026-08-30 — CI invokes the local loop's scripts, and gates on a stable check

Two decisions about CI, taken together after `M2.P3.T1b` broke branch
protection by renaming a job.

**CI calls `tools/ci/local/build.sh` and `test.sh` instead of restating their
steps in YAML.** M7 built a local environment that reproduces CI, and proved it
by reproducing a CI failure byte-for-byte. But it left two definitions of "how
this project builds" — one in the scripts, one in `ci.yml` — and keeping them in
agreement was documentation and diligence. Making CI call the same scripts
removes the second definition. Drift stops being a thing that can happen rather
than a thing we watch for.

**Branch protection gates on one stable aggregator check, not on a job's display
name.** `RB-2.6`'s required check was the literal string
`Test Ubuntu Python 3.10`. Renaming that job — correct, since it ran neither
Ubuntu nor Python 3.10 — meant the required check would never report and no PR
could merge. A required check is a contract with branch protection; it must not
be a label that an accurate rename can invalidate. A final job that `needs:` the
real jobs and reports their aggregate result gives a name that never changes,
and frees the inner jobs to be renamed, split, or reordered.

**PR gate scope:** configure + debug build + `ctest` only, with ccache restored
from `actions/cache`. Release build, smoke test and log artifacts move to
post-merge and nightly. Today every PR pays for two sequential cold builds with
no caching — the same cost that made M7 necessary.
