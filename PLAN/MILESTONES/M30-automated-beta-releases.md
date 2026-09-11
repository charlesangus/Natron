# Milestone 30: Full release + AppImage on every merge, auto-versioned betas from 0.1.0-beta1

`.github/workflows/release.yml` (name `Release`) already builds the full
release pipeline — build, ctest, smoke test, `stage-bundle.sh`,
`make-tarball.sh`, `make-appimage.sh`, `gh release create` — but only on a
manual `v*` tag push or `workflow_dispatch`. This milestone makes that same
pipeline fire automatically on every merge to `main`, tagged as a
sequentially-numbered beta pre-release starting at `v0.1.0-beta1`, without
duplicating any of the build/package logic.

Design: a new job computes the next beta tag and **dispatches the existing
`release.yml`** (via `gh workflow run`) rather than re-implementing packaging.
`workflow_dispatch` triggered through the API (`gh workflow run`) is not
subject to the "events from `GITHUB_TOKEN` don't trigger workflows"
restriction — that only applies to git push/PR events — so this works with
the default token plus `actions: write` permission on the job.

Gating on "successful merge": `main`'s branch protection already requires
`format`, `lint-ci`, and `build-and-test` (M10) before a merge can land, so by
the time the post-merge `Tests` workflow run on `main` completes, the merge
was already gated on all three. The new trigger keys off `Tests` alone rather
than re-deriving a cross-workflow AND of `Checks` + `Tests` — see Decisions.

## Phase 30.1: Let `release.yml` build a dispatched tag, and mark pre-releases

- [ ] M30.P1.T1 — Add `tools/release/next-beta-tag.sh`
  - files: `tools/release/next-beta-tag.sh` (new)
  - approach: pure, side-effect-free script. Takes the existing tag list (`git tag -l 'v0.1.0-beta*'`), extracts the numeric suffix after `beta` from each, finds the max, and prints `v0.1.0-beta$((max+1))` to stdout. Prints `v0.1.0-beta1` when no matching tag exists yet. Does not create or push anything — that's the caller's job (P2.T1), so this script stays independently testable.
  - verify: with no matching tags, prints `v0.1.0-beta1`; given a tag set containing `v0.1.0-beta1` and `v0.1.0-beta2` (e.g. via a scratch repo or a `git tag` in a tmp dir), prints `v0.1.0-beta3`; a gap (`beta1`, `beta3`) still yields `beta4` (max+1, not fill-the-gap).
  - size: S

- [ ] M30.P1.T2 — Let `release.yml` build a dispatched tag and mark pre-releases
  - files: `.github/workflows/release.yml`
  - approach: add a required `tag` string input to the `workflow_dispatch` trigger. Add an early step, gated on `github.event_name == 'workflow_dispatch'`, that creates an annotated tag at `inputs.tag` and pushes it to `origin` before the build runs — so the rest of the job (and `gh release create`) has a real tag to work from, same as the existing `push: tags: v*` path. Replace `github.ref_name` throughout with a `TAG` computed as `${{ inputs.tag || github.ref_name }}`. In the `gh release create` step, add `--prerelease` whenever `TAG` contains a `-` (pre-release semver convention — distinguishes `v0.1.0-beta1` from a stable `vX.Y.Z`).
  - verify: `gh workflow run release.yml --ref main -f tag=v0.1.0-beta1` (or a throwaway tag) completes and produces a GitHub Release marked "Pre-release" carrying both the tarball and the AppImage; a plain `git push origin vX.Y.Z` (no dispatch input) still works exactly as before and is **not** marked pre-release.
  - size: M

## Phase 30.2: Fire it automatically on merge

- [ ] M30.P2.T1 — Add `.github/workflows/beta-release.yml`
  - files: `.github/workflows/beta-release.yml` (new)
  - approach: trigger on `workflow_run` for the `Tests` workflow (`ci.yml`'s `name: Tests`, the one running `build-and-test`), `types: [completed]`. Guard the job with `if: github.event.workflow_run.conclusion == 'success' && github.event.workflow_run.head_branch == 'main'`. Steps: checkout with `fetch-depth: 0` and tags fetched, run `tools/release/next-beta-tag.sh` to compute `TAG`, then `gh workflow run release.yml --ref main -f tag="$TAG"` using the default `GITHUB_TOKEN` (declare `permissions: actions: write` on the job). Comment the file with the branch-protection gating rationale from this milestone's header so a future reader doesn't "fix" it into a redundant cross-workflow check.
  - verify: merge a trivial change to `main`; `beta-release.yml` fires once `Tests` goes green on the resulting push; it dispatches `release.yml` with the next sequential `v0.1.0-betaN` tag; the resulting GitHub Release is a pre-release carrying the tarball and the AppImage.
  - size: M

## Decisions

- 2026-09-11 — betas are auto-versioned `v0.1.0-beta1`, `v0.1.0-beta2`, ...
  (max-existing + 1, per `next-beta-tag.sh`), reusing `release.yml`'s existing
  build/package/publish pipeline via `workflow_dispatch` rather than
  duplicating it in a second workflow. Existing manual stable-tag releases
  (`vX.Y.Z` pushed by hand) are untouched and remain non-prerelease.
- 2026-09-11 — the auto-beta trigger keys off the `Tests` workflow's
  post-merge completion alone, not an explicit AND of `Checks` + `Tests`.
  Rationale: `main`'s branch protection (M10) already requires both before a
  merge lands, so re-checking `Checks`' conclusion here would be redundant
  cross-workflow polling for no behavioural difference under normal
  operation. Revisit only if branch protection on `main` is ever relaxed.

**Verification gate:** `release.yml` can be manually dispatched with an
explicit tag and produces a correctly-marked (pre-)release with both
artifacts attached; merging any change to `main` automatically produces a new
`v0.1.0-betaN` pre-release (N sequential from 1) through the same pipeline
with no manual step; the pre-existing manual `vX.Y.Z` tag-push release flow is
unchanged.
