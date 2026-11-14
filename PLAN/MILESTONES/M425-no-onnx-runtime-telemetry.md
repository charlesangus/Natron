# M425 - No ONNX Runtime Telemetry

Natron bundles Microsoft's prebuilt ONNX Runtime 1.31.1 CPU release (`build/assets/onnxruntime`,
pinned by `ONNXRUNTIME_VERSION` in `tools/ci/local/fetch-assets.sh`). That library contains
Microsoft's 1DS telemetry client (`onnxruntime::OneDsTelemetry`,
`Microsoft::Applications::Events::TelemetrySystem`) with the collector URL
`https://mobile.events.data.microsoft.com/OneCollector/1.0/` compiled in. When AI is enabled,
`OrtLoader` calls `api->CreateEnv(...)` (`Engine/Ai/OrtLoader.cpp`, ~line 215 on `main`), and the
user saw Natron resolve that host at launch. Natron never calls `DisableTelemetryEvents`.

The fix is ORT's runtime opt-out, which covers whichever runtime the loader picks up (the
Preferences path, a user-installed GPU build, or the bundled CPU build). A test proves the leak
before the fix and its absence after it. Building ORT ourselves without telemetry is a fallback,
used only if the opt-out doesn't stop the traffic.

## Scope

**In:**
- `DisableTelemetryEvents` on every environment `OrtLoader` creates.
- An in-process regression test, plus a reusable launch-level "no outbound network" check script.
- Confirming the `--rocm` source build in `tools/ai/install-ort-gpu.sh` has no telemetry.
- Phase 425.2 (our own telemetry-free ORT build) only if Phase 425.1 shows that traffic remains.

**Out:**
- Other phone-homes in the project. A separate read-only audit runs first, and its findings may
  be added to this milestone as new tasks later.
- Building ORT for CUDA ourselves. The `--cuda` path keeps Microsoft's release, covered by the
  runtime opt-out.
- macOS/Windows. The fork is Linux x86-64 only.

**Edge cases:**
- `DisableTelemetryEvents` returns a non-null status: release it, log it at warning level, and
  keep the environment. Telemetry failing to switch off doesn't make AI unavailable, but the
  test must then fail.
- AI disabled in Preferences: `OrtLoader` loads nothing, so there's nothing to disable. The
  check script must still show no outbound traffic in this mode.
- The upload may be asynchronous or deferred. The tests must wait long enough, or force the env
  through a session run and release, before declaring "no traffic".

**Assumptions:**
- ORT 1.31's Linux 1DS client honours `DisableTelemetryEvents` (unverified; that is what the red
  and green test runs settle).
- ORT's `build.sh` compiles telemetry only with `--use_telemetry`, so the `--rocm` build has none
  (P1.T3 checks this).

## Phase 425.1: Runtime opt-out and proof

- [ ] M425.P1.T1 — Disable ORT telemetry after `CreateEnv`, with a regression test that fails without it
  - files: `Engine/Ai/OrtLoader.cpp`, `Tests/Ai/OrtTelemetry_Test.cpp` (new), `Tests/CMakeLists.txt`, `Engine/Ai/README.md`
  - approach: Right after a successful `api->CreateEnv(...)` in `OrtLoader`, call `api->DisableTelemetryEvents(env)`. A non-null status is released with `api->ReleaseStatus`, logged once at warning level, and is not a load failure. For the test, define `getaddrinfo` and `connect` in the test translation unit so they interpose libc for the dlopen()ed `libonnxruntime.so`. Each records the host name, or a non-loopback, non-`AF_UNIX` address, then forwards through `dlsym(RTLD_NEXT, ...)`. The test loads the build-tree ORT through a fresh `OrtLoader`, as `Tests/Ai/AiTestSupport.h` does. It opens and runs a trivial session (any small fixture model the Ai tests already use), then waits a bounded few seconds, since 1DS may upload asynchronously. It asserts that nothing resolved `*.data.microsoft.com` and that no outbound connect happened. If interposition proves blind to ORT's resolver (for example a statically linked resolver), switch to an `strace -f -e trace=connect,sendto,sendmsg` child-process check, and record that as a milestone decision. Add the test to `Tests/CMakeLists.txt` next to `Ai/OrtLoader_Test.cpp`, and add one line on telemetry to `Engine/Ai/README.md`'s "Loading the library". Follow the comment policy.
  - verify: With the `DisableTelemetryEvents` call temporarily removed, the new test fails and names the 1DS host. This red run proves the test catches the leak; record its output in the milestone's `## Decisions`. With the call in place it passes, and the existing `AiOrtLoader*` tests still pass. If it still fails with the call in place, the opt-out doesn't work: record that, leave the call in, and Phase 425.2 becomes mandatory.
  - size: M

- [ ] M425.P1.T2 — Add a launch-level "no outbound network" check script
  - files: `tools/ci/local/check-no-network.sh` (new)
  - approach: `check-no-network.sh [--seconds N] -- <command...>` runs the command under `strace -f -e trace=connect,sendto,sendmsg -o <log>` for N seconds (default 20), then stops it with SIGTERM. It fails, printing the offending lines, on any `connect`/`sendto` to an `AF_INET`/`AF_INET6` address other than loopback, or to port 53 on any address. `AF_UNIX` (X11, D-Bus) is allowed. It runs from a fresh temporary `HOME` with `checkForUpdates` pre-seeded off (see the first-launch update dialog), and works for `NatronRenderer` headless and for `Natron` under the container's Xvfb recipe (`build/deeprepro/run-gui.sh`). It is general on purpose, so the phone-home audit's findings can reuse it.
  - verify: Run it against an AppImage built before this milestone (for example `build/appimages/Natron-m399-node-creation-keys-and-speed.AppImage`, extracted) with AI enabled. It fails and shows the lookup of `mobile.events.data.microsoft.com`, or a port-53 send for it. Run it against this milestone's release build: it passes in both AI-enabled and AI-disabled modes.
  - size: M

- [ ] M425.P1.T3 — Confirm the `--rocm` source build of ORT has no telemetry
  - files: `tools/ai/install-ort-gpu.sh` (read; edit only if needed)
  - approach: Check, in the ONNX Runtime source at the tags the script builds (`ORT_VERSION_COMMIT` and the `ROCM_TABLE` tags), that `tools/ci_build/build.py` enables `onnxruntime_USE_TELEMETRY` only with `--use_telemetry`, and that the script doesn't pass it. If it is on by default, add the flag that turns it off to the script's `./build.sh` line. Report the evidence (file:line in ORT source).
  - verify: The cited ORT source shows telemetry off by default, or the script now passes the opt-out. `bash -n tools/ai/install-ort-gpu.sh` is clean.
  - size: S

## Phase 425.2: Fallback — our own telemetry-free ORT build

Run this phase only if M425.P1.T1's green run or M425.P1.T2's check still shows 1DS traffic with
the opt-out in place. Otherwise strike its tasks through as not needed, and record why in
`## Decisions`.

- [ ] M425.P2.T1 — Build ONNX Runtime 1.31.1 CPU without telemetry and bundle it instead of Microsoft's release
  - files: `tools/ai/build-ort-cpu.sh` (new), `tools/ci/local/fetch-assets.sh`, `.github/workflows/*.yml` that cache `build/assets/onnxruntime`
  - approach: A script builds the `v1.31.1` tag, checked against its pinned commit like `install-ort-gpu.sh` does, with `./build.sh --config Release --build_shared_lib --parallel --skip_tests` and no `--use_telemetry`. It runs in the published natron-dev image, so glibc and the toolchain match the release build. It packages the result in the same layout as Microsoft's tarball (`include/`, `lib/`, `LICENSE`, `ThirdPartyNotices.txt`). The user uploads the tarball to a fork release, which is the user's action, so raise it under `# Open questions` with the file path. Then point `ONNXRUNTIME_URL`/`ONNXRUNTIME_SHA256` in `fetch-assets.sh` at it, and bump the stamp so caches refresh.
  - verify: `strings -a` on the built `libonnxruntime.so` finds no `OneCollector` or `data.microsoft.com`. The full Ai test set passes against it, and `check-no-network.sh` passes on the AppImage built with it.
  - size: L

**Verification gate:** `OrtTelemetry_Test` and every `Ai*` test pass in PR CI. The red run (opt-out
removed) is recorded in `## Decisions`. `tools/ci/local/check-no-network.sh` fails on the
pre-milestone AppImage and passes on this milestone's release AppImage, with AI both enabled and
disabled. M425.P1.T3's ROCm finding is recorded. Phase 425.2 is either done, with its checks
green, or struck through with the reason recorded.
