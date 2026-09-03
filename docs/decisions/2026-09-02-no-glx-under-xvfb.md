# No GLX under xvfb-run on ci-vfxall; the two OpenGL tests are disabled, not fixed

`OSGLContext.Basic` and `GPUContextPool.Basic` (`Tests/OSGLContext_Test.cpp`)
both open with `if (!appPTR->isOpenGLLoaded()) { ...; return; }`. Under
`xvfb-run` on `aswf/ci-vfxall:2027-clang21.1` that branch is always taken --
the ctest log shows `Error while loading OpenGL: GLX: No GLXFBConfigs
returned` followed by `OpenGL rendering is disabled` -- but a bare `return`
inside a `TEST()` body is scored by gtest as a **pass**. The suite has been
reporting `[ PASSED ] 30 tests` while only 28 ever executed their bodies.

## What was measured

Directly against the image, not inferred: `Xvfb` there produces no GLX
visual with `+extension GLX`, with `+iglx` (indirect GLX), or with
`LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe` forcing software
rasterization -- even though `swrast_dri.so` and `libGLX_mesa.so.0` are both
present in the image. This isn't a missing flag or a missing package; the
combination of this Xvfb build and this Mesa build does not produce a usable
GLX config, full stop.

## Decision

Rename both cases to `DISABLED_Basic` so gtest's own `YOU HAVE 2 DISABLED
TESTS` notice makes the gap visible instead of the suite silently claiming
28 tests' worth of coverage it doesn't have. The vendored gtest in
`Tests/google-test/` predates `GTEST_SKIP` (already flagged by a `TODO` in
the test file), so `DISABLED_` is the mechanism available. The
`isOpenGLLoaded()` guard and its stderr line stay as-is: if someone runs
these explicitly with `--gtest_also_run_disabled_tests` on a GL-less host,
they should still bail cleanly rather than crash.

This is scoped as a correctness-reporting fix, not a GL-coverage fix. It
does not attempt to restore real execution of these tests in CI.

## What restoring real GL coverage would take

Either an EGL-surfaceless context path in `Engine/OSGLContext_x11.cpp`
(bypassing GLX/Xvfb entirely), or swapping the CI display server for one
that actually backs GLX with a real or virtual GPU. Both are a project, not
a task: the EGL path touches context-creation code that's currently GLX-only
end to end, and a different X server changes what the CI container needs to
provide. Neither is undertaken here.

## Verification

`tools/ci/local/test.sh ctest debug` after the rename: ctest's discovery
picks up the renamed cases and the run reports 28 executed / 2 disabled
(gtest's `YOU HAVE 2 DISABLED TESTS` line), where it previously reported 30
passed.

No `/dev/dri`-capable host was available to this task, so the disabled
tests' real (GL-enabled) code path was not exercised -- only confirmed that
`--gtest_also_run_disabled_tests` still finds and runs them by name, taking
the `isOpenGLLoaded()` bail-out on this GL-less machine as expected.
