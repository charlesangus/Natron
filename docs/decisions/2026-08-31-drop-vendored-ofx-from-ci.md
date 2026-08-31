# Vendored OFX plugins leave the build and test path

> **SUPERSEDED, 2026-08-31, same day** — see
> `PLAN/DECISIONS/2026-08-31-restore-vendored-ofx-plugin-tests.md`. Nothing
> below was acted on; `BaseTest.cpp`, the plugin fetch and `OFX_PLUGIN_PATH`
> all remain. Two of the three reasons here are wrong or obsolete:
>
> - **Reason 2 ("cannot work in the target container") was about the image,
>   not the tests.** Correct for `ci-baseqt:2027.0`; the move to
>   `aswf/ci-vfxall:2027-clang21.1` supplies OIIO/OCIO/OpenEXR/OpenFX, and
>   building the plugin from source there gives 28/28 green.
> - **Reason 3 is factually wrong.** `SeNoise` does not live in openfx-misc.
>   It is `openfx-io/SeExpr/SeNoise.cpp`, built into the *same* `IO.ofx`
>   bundle as ReadOIIO/WriteOIIO — the bundle CI was already downloading
>   exports it. All three required plugins were being fetched all along; the
>   bundle simply failed to `dlopen`.
> - **Reason 1 stands as a judgement call** and was overridden deliberately;
>   the superseding decision says why.
>
> Kept unedited below as the record of what was believed at the time.

CI no longer fetches or tests against prebuilt third-party OFX plugin bundles.
`Tests/BaseTest.cpp`, the openfx-io asset fetch, and the `OFX_PLUGIN_PATH`
plumbing are all removed in M9. Plugin loading returns as a pre-release
integration test (M11), not as a unit test on the merge gate.

Three reasons, in order of weight:

1. **It tests someone else's binaries.** `BaseTest` asserted that a downloaded
   openfx-io bundle exports ReadOIIO/WriteOIIO and that openfx-misc exports
   SeNoise. A failure there says nothing about this repository.
2. **It cannot work in the target container.** The only published asset is an
   Ubuntu 22.04 build needing `GLIBCXX_3.4.30`; `aswf/ci-baseqt:2027.0` is
   Rocky 9 and provides at most `3.4.29`. No EL9 build exists upstream, and
   `ci-baseqt` ships no OIIO/OCIO to build one in place.
3. **One of the three required plugins was never fetched by anything.** SeNoise
   lives in openfx-misc, which no CI here or upstream has ever downloaded, and
   which publishes no testing build. `ASSERT_TRUE` aborted on it first, so the
   suite has been failing on a dependency that was never satisfiable.

This makes the ctest suite green (25/25) and honest about its scope: it tests
Natron. Whether Natron drives real plugins correctly is a real property, and
M11 owns proving it — with pinned, EL9-compatible bundles, before a release
rather than on every push.
