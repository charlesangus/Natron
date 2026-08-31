# Small fixes to NatronGitHub repos: fork, fix on the fork, consume the fork

2026-08-31. Standing pattern, not a one-off.

When this project needs a small change to a repository owned by
`NatronGitHub` — a build-system fix, a compatibility shim, a one-line
correction — the change goes on **our own fork under `charlesangus/`**, and
this repo pins the fork. We do not carry the change as a local patch, a `sed`
in a build script, or a command-line workaround, and we do not block on
upstream review before using it.

## Why

The alternative we had been doing was worse in three specific ways, all of
them visible in the first instance of this (below):

1. **Workarounds hide the bug.** Passing both spellings of a CMake variable
   on the command line made the build work while leaving the actual defect in
   place, undocumented, in a repo other people also build. A fork commit is a
   fix with a message, a diff and a history; a `-D` flag in our script is a
   fact about *our* script that reads like superstition to the next person.
2. **Workarounds accumulate silently.** Each one is individually small and
   individually justified. Ten of them is an unmaintainable build script that
   nobody can safely simplify, because no one can tell which flags still
   matter.
3. **Upstream review is not on our critical path, and shouldn't be.** These
   repos move slowly. Waiting for a merge to unblock our CI couples our
   schedule to someone else's. Forking decouples them without forking the
   *project* — we stay two commits ahead, not a divergent branch.

## The rules

- **Fork under `charlesangus/`**, keep the fork's default branch in sync with
  upstream, and land the fix there (via PR on the fork, so it has a review
  trail).
- **Pin the fork by exact commit SHA** in whatever consumes it. Not a branch,
  not a tag — same reasoning as any other test fixture pin.
- **Keep the delta minimal and upstreamable.** The fork exists to hold fixes
  that *should* go upstream, in a form that can be sent upstream unchanged.
  This is the mechanism for
  `PLAN/DECISIONS/2026-08-29-upstream-agnostic-fixes.md`, not a departure from
  it: platform-agnostic fixes still get offered upstream, they just don't
  block us first.
- **Record the delta where it is consumed.** The comment at the pin says what
  the fork changes and why, so a reader can evaluate whether it is still
  needed without cloning anything.
- **This is for small changes.** A fix that needs real design discussion, or
  that would leave the fork substantially divergent, is a different decision
  and should be taken as one.

## First instance

`charlesangus/openfx-io`, pinned at `59318530ed1c6b78e6a85dc7c4cf60366520ba7f`
in `tools/ci/local/fetch-assets.sh`. Two commits ahead of
`NatronGitHub/openfx-io`, zero behind; the delta is one hunk in
`CMakeLists.txt`.

Upstream's `CMakeLists.txt` read `${SEEXPR2_INCLUDES}` / `${SEEXPR2_LIBRARIES}`
while its own `cmake/Modules/FindSeExpr2.cmake` only ever set
`SEEXPR2_INCLUDE_DIR` / `SEEXPR2_LIBRARY`. The mismatched names expanded to
empty, so the SeExpr sources compiled but `IO.ofx` was never linked against
`libSeExpr` — and because undefined symbols in a shared library do not fail a
link by default, nothing complained until load time, with
`undefined symbol: _ZTI11SeExprFuncX`. It is very likely why openfx-io's own
CI builds via its Makefile rather than CMake.

`fetch-assets.sh` previously papered over this by passing all four variable
spellings. Those two extra `-D` flags are now gone; see
`PLAN/DECISIONS/2026-08-31-restore-vendored-ofx-plugin-tests.md` for the
surrounding change.

## Not in scope

`wdas/SeExpr` is pinned directly at `wdas/SeExpr` — it is not a NatronGitHub
repo and we carry no changes to it. The `sed` that strips SeExprEditor/demos/
tests/doc from its `CMakeLists.txt` before configuring is a *build option*, not
a fix: it selects which subprojects to build, exactly as openfx-io's own CI
does. If we ever need to actually change SeExpr, that is when it gets forked.

The existing submodules (`libs/OpenFX` → `NatronGitHub/openfx`, `google-test`,
`google-mock`, `SequenceParsing`) are **not** being migrated as part of this
decision. This pattern applies from now on, when a change is actually needed;
it is not a mandate to fork everything pre-emptively.

**Landed** in `3b4d09f8b` on `milestone/m2-qt6-migration` (the openfx-io pin).
