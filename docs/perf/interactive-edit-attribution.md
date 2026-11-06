# Interactive edit attribution

Why an edit at the end of a RAW chain re-runs the unchanged nodes above it. This is the attribution for
M276 - Upstream Reuse On Edits. It is not the baseline: that comes from the user's host (M276.P2.T2).

## Result

- **All 720 upstream executions on the software host have a cause, and none is unclassified.**
  400 are not admitted to the cache. The other 320 run behind a cache hit.
- **With the panel closed**, `Node::shouldCacheOutput` refuses all five raw nodes, so every edit re-runs them.
- **With the panel open**, the rule does admit the edited node's input, `RawInputColor`, and that node hits on
  every edit. The four raw nodes above it run anyway, because the task graph runs a task's dependencies
  without asking whether the consumer is already cached. Opening the panel saves only `RawInputColor` itself,
  about 15 ms or less of a multi-second edit.
- **The behind-a-hit cause was not in the brief's list.** It came out of the data, and no milestone has a task
  for it. See "Behind a hit" below.
- **The GPU causes are unmeasured.** No configuration here formed a GPU region, not even under lavapipe.
- **Draft is unmeasured.** Programmatic edits never switch to draft.

## Causes

The classifier is `classify_task` in `tools/bench/edit_latency_bench.py`. Run
`python3 -I tools/bench/edit_latency_bench.py --classify <out-dir>`; it exits 1 if anything is unclassified.
A task counts as upstream if another task of the frame depends on it and it is not the edited node. A task
whose state is `hit` did not execute.

Checks run in this order, and the first match wins.

| Class | Evidence in the records | Code path | Owner | Software host | Lavapipe |
|---|---|---|---|---|---|
| `hash_change` | the node's content hash differs across the edit | `Effect::getContentHash` (`Engine/PyNode.cpp:575`) | none: the render is required | 0 | 0 |
| `behind_hit` | every consumer in the same frame is a `hit` or itself behind one | every dependency runs: `RenderScheduler::executeTask` (`Engine/RenderScheduler.cpp:1223`); the only cache probe outside `renderRoI` is placement's | M300 - Unified Tiered Cache, by its gate; no task yet | 320 | 48 |
| `placement_iscached_miss` | on the device, with a host image stored earlier at this mip | `isCached` (`Engine/GpuPlacement.cpp:1024`, draft key fixed by M276.P2.T1) | M301 - GPU Cache Tier | unmeasured | unmeasured |
| `gpu_region_member` | on the device, or in a region, with no host image | `storeOutputs` (`Engine/GpuRegionTask.cpp:2978`) writes only the per-frame store, and skips interior members (`:2985`) | M301 - GPU Cache Tier | unmeasured | unmeasured |
| `not_admitted` | CPU, state `bypass`, real work done | `Node::shouldCacheOutput` (`Engine/Node.cpp:8652`), called at `Engine/EffectInstanceRenderRoI.cpp:933`; state set at `:1366` | M300 - Unified Tiered Cache | 400 | 60 |
| `partial_roi` | CPU, state `partial` | `Engine/EffectInstanceRenderRoI.cpp:1372` | M300 - Unified Tiered Cache | 0 | 0 |
| `first_store` | CPU, state `miss`, no earlier cached image of the node | as above; nothing to reuse yet | none | 0 | 0 |
| `eviction` | CPU, state `miss`, an image stored earlier at this mip or finer, hash unchanged | blind LRU past 90% (`Engine/Cache.h:67`) | M300 - Unified Tiered Cache | 0 | 0 |
| `mip_or_draft_key` | CPU, state `miss`, earlier images only at coarser mips | draft in `ImageKey` (`Engine/EffectInstanceRenderRoI.cpp:~940`, `Engine/ImageKey.cpp:89`), draft mip (`Engine/ViewerInstance.cpp:1769`) | M303 - Interactive Edit Latency | 0, drag unmeasured | 0, drag unmeasured |
| `unclassified` | a node's hash is missing, or an unknown state | - | - | 0 | 0 |

`partial_roi` and `first_store` are additions, needed so the list is exhaustive. Neither occurred in a recorded
edit. A `bypass` task under 0.5 ms counts as no work, because `renderRoI` returns for an identity
node or an empty RoI before it sets a state; none occurred upstream in a recorded edit.

### Limits of the classifier

- `not_admitted` is partly by elimination. `bypass` also covers `byPassCache`, OpenGL renders, loop variables and
  `allowsStoreUnder` refusals (`Engine/EffectInstanceRenderRoI.cpp:930, 963`). The benchmark forces no refresh,
  the tasks ran on the CPU and upstream hashes did not move, but the profile does not record the reason.
- The profile has no draft flag. A draft key change at an unchanged mip would read as `eviction`. That cannot
  happen in this benchmark, and it must be revisited when a drag is added.
- The two GPU classes separate only by history: a host image stored earlier at this mip makes a device task
  `placement_iscached_miss`, none makes it `gpu_region_member`. That is an inference like `not_admitted`, and
  neither row is established until a GPU run supplies data. A region member that fell back to the CPU still counts
  as a device task, and now carries its real cache state; the region record's `executed` list names the members
  that rendered.
- A timed-out edit has no frame. Frames after the last framed edit before it, up to the next framed edit,
  cannot be attributed, so the classifier counts their tasks as `unclassified` and exits 1.
- A task whose cache state is `partial` or `miss` is read after the fact: a non-tiled effect with part of its RoI
  cached is widened to the whole RoI, and the state is taken before that, so it reads `partial`. A frame whose
  rectangles are all identity reads `bypass`.
- Only `not_admitted` and `behind_hit` occurred in real data. The other branches were checked on 11 synthetic
  records, not on real renders.
- The content hash is read on the main thread around the edit, not recorded per frame.

## Behind a hit

In every panel-open frame the profile shows `RawInputColor1` as `hit` while `RawDemosaic1`, `RawHighlights1`,
`RawTemperature1` and `RawPrepare1` executed in the same frame, as dependencies of that hit. The rule did what
its comment says. The work was wasted anyway, because the image it was needed for was already cached.

M300 - Unified Tiered Cache's gate requires no raw-chain execution after the first render. Admitting everything
would meet that gate, since each task would then hit. Its other policy, admitting above a priority floor, would
not: a node below the floor re-runs behind a hit, and so does an evicted one. Either M300 prunes the graph behind
a hit, or the admit-only policy cannot pass its gate. The legacy scheduler was not measured.

## Software-host results

Everything in this section comes from a software GL host:

- 4 cores, 15 GB RAM, no GPU, in a throwaway `ghcr.io/charlesangus/natron-dev:4c9cb243f4823d04` container
  (`--memory 12g`), under Xvfb with llvmpipe, 1920x1080.
- The AppImage is `build/remote/release/Natron-2.6.0-x86_64.AppImage`, as handed over. That it matches
  ca6e532a7 was assumed, not checked.
- `NATRON_GPU=off`, 10 recorded edits after 3 warm-up edits, Canon 5D Mk III fixture.

### Attribution

| case | zoom | panel | proxy | edits | upstream executions | not admitted | behind a hit | every other cause | unclassified |
|---|---|---|---|---|---|---|---|---|---|
| grade | fit | closed | off | 10 | 50 | 50 | 0 | 0 | 0 |
| grade | fit | closed | on | 10 | 50 | 50 | 0 | 0 | 0 |
| grade | fit | open | off | 10 | 40 | 0 | 40 | 0 | 0 |
| grade | fit | open | on | 10 | 40 | 0 | 40 | 0 | 0 |
| grade | 100 | closed | off | 10 | 50 | 50 | 0 | 0 | 0 |
| grade | 100 | closed | on | 10 | 50 | 50 | 0 | 0 | 0 |
| grade | 100 | open | off | 10 | 40 | 0 | 40 | 0 | 0 |
| grade | 100 | open | on | 10 | 40 | 0 | 40 | 0 | 0 |
| hsv | fit | closed | off | 10 | 50 | 50 | 0 | 0 | 0 |
| hsv | fit | closed | on | 10 | 50 | 50 | 0 | 0 | 0 |
| hsv | fit | open | off | 10 | 40 | 0 | 40 | 0 | 0 |
| hsv | fit | open | on | 10 | 40 | 0 | 40 | 0 | 0 |
| hsv | 100 | closed | off | 10 | 50 | 50 | 0 | 0 | 0 |
| hsv | 100 | closed | on | 10 | 50 | 50 | 0 | 0 | 0 |
| hsv | 100 | open | off | 10 | 40 | 0 | 40 | 0 | 0 |
| hsv | 100 | open | on | 10 | 40 | 0 | 40 | 0 | 0 |

Autoproxy on and off give the same counts and the same mip, as expected with no drag.

### Timing

**These timings are not a baseline, and configurations cannot be compared with each other.** The host was shared
with other builds and the load average swung between about 5 and 22 during the run. The clearest sign is the same
raw chain taking about 12.5 s in `grade fit closed` at load 20 and 3.5 s in `hsv fit closed` at load 6, so the
Grade and HSVKeyer difference here is contention. Compare counts, not milliseconds.

| case | zoom | panel | proxy | median ms | p95 ms | load average, min-max | RawDemosaic median wall ms | RawInputColor median wall ms |
|---|---|---|---|---|---|---|---|---|
| grade | fit | closed | off | 12509 | 15934 | 19.9-21.9 | 8237 | 15.4 |
| grade | fit | closed | on | 12409 | 14997 | 16.9-19.1 | 7654 | 15.6 |
| grade | fit | open | off | 4742 | 7307 | 8.1-11.8 | 3123 | 0.6 |
| grade | fit | open | on | 4040 | 4443 | 6.2-7.9 | 2700 | 0.6 |
| grade | 100 | closed | off | 333 | 398 | 6.0-6.5 | 48 | 12.7 |
| grade | 100 | closed | on | 338 | 407 | 6.5-7.2 | 46 | 9.8 |
| grade | 100 | open | off | 323 | 362 | 6.9-7.6 | 50 | 0.9 |
| grade | 100 | open | on | 215 | 236 | 5.8-6.7 | 26 | 0.6 |
| hsv | fit | closed | off | 3467 | 4083 | 5.5-6.5 | 2385 | 4.5 |
| hsv | fit | closed | on | 4340 | 5129 | 6.2-6.9 | 2809 | 6.8 |
| hsv | fit | open | off | 4621 | 5381 | 5.5-6.1 | 3046 | 0.6 |
| hsv | fit | open | on | 10706 | 15529 | 6.0-13.5 | 6863 | 0.6 |
| hsv | 100 | closed | off | 1501 | 1806 | 15.8-19.8 | 236 | 44.0 |
| hsv | 100 | closed | on | 1243 | 1497 | 16.3-18.3 | 198 | 44.4 |
| hsv | 100 | open | off | 1229 | 1600 | 14.5-15.2 | 182 | 0.9 |
| hsv | 100 | open | on | 1207 | 1630 | 13.5-14.4 | 124 | 1.0 |

The latency ends when the scheduler flushes the profile at the end of the frame. Texture upload and the display of
the image are not in it, and llvmpipe drawing is not timed.

What the numbers can still say:

- Fit is much slower than 100%, because the raw nodes cannot scale. At fit (mip 3) `RawDemosaic` processes the whole
  image; at 100% only the viewer's crop reaches it, while `RawPrepare` and `RawTemperature` stay full frame. Both
  zooms ran on the CPU here.
- Fit is mip 3 only because the viewer pane is about 320 px tall in the default layout, which is 7% zoom.
- A cache hit is not free at full size: `Read1` hits cost tens of milliseconds at 100% (23 to 103 ms medians, under
  this load), which is already a large share of M303 - Interactive Edit Latency's 33 ms.

## Lavapipe run

Valid for attribution counts only; its timings mean nothing. `NATRON_GPU=force`, proxy off, 3 edits after 2 warm-ups.

| case | zoom | panel | proxy | edits | upstream executions | not admitted | behind a hit | every other cause | unclassified |
|---|---|---|---|---|---|---|---|---|---|
| grade | fit | closed | off | 3 | 15 | 15 | 0 | 0 | 0 |
| grade | fit | open | off | 3 | 12 | 0 | 12 | 0 | 0 |
| grade | 100 | closed | off | 3 | 15 | 15 | 0 | 0 | 0 |
| grade | 100 | open | off | 3 | 12 | 0 | 12 | 0 | 0 |
| hsv | fit | closed | off | 3 | 15 | 15 | 0 | 0 | 0 |
| hsv | fit | open | off | 3 | 12 | 0 | 12 | 0 | 0 |
| hsv | 100 | closed | off | 3 | 15 | 15 | 0 | 0 | 0 |
| hsv | 100 | open | off | 3 | 12 | 0 | 12 | 0 | 0 |

GPU compute came up (`GPU compute ready on llvmpipe`), yet no task in any profile ran on the device and no region
formed. So this run confirms the CPU attribution with compute available, and does not exercise `gpu_region_member` or
`placement_iscached_miss`. At fit the refusal is the known one for non-scaling nodes at mip != 0
(`Engine/GpuPlacement.cpp:675`). **Why no region formed at 100% was not determined:** `Engine/GpuPlacement.cpp` logs
nothing about its refusals, and neither `natron.log` nor the profile says which one applied. A real GPU may place differently, so this says nothing about the GPU path on a GPU host.

## Not measured

- **GPU on a real device.** All 16 configurations with `NATRON_GPU=auto` remain, and with them the reported "upstream
  lights up" on the GPU path. On a GPU host's display:

  ```
  GUI_TEST_HOST_DISPLAY=:0 XAUTHORITY=$XAUTHORITY BENCH_GPUS=auto BENCH_EDITS=10 \
      tools/bench/edit_latency.sh <Natron.AppImage> build/bench/edit-latency-gpu
  ```

  Under Xvfb the GPU goes unused, so the host's display is required, and a fresh out-dir per run.
- **A drag.** Edits are set programmatically, so `Gui/KnobGuiValue.cpp` and `Gui/ColorKnobEditSession.cpp` never turn
  on draft. The auto-proxy mip switch and the draft key are unmeasured, and the `off` and `on` proxy rows do not show
  what auto-proxy costs. The `mip_or_draft_key` count of 0 means only that.
- **Eviction under pressure.** The count of 0 comes from a run that stores only `Read`, the edited node's result of each
  edit and, with the panel open, `RawInputColor`. It does not show that eviction would stay away once more is admitted.
- **The user's host and file**, and any latency fit for the gate. That is M276.P2.T2 and M303 - Interactive Edit
  Latency.

## Fixture and harness checks

- The Canon fixture's LFS object is present: 25,965,009 bytes, and its SHA-256 matches `git lfs ls-files`.
- `lumaRange` has five dimensions: Outer Low, Inner Low, Centre, Inner High, Outer High
  (`Engine/Nodes/Keyer/docs/HSVKeyer.md`). The benchmark now moves Centre, dimension 2.
- Viewer zoom is confirmed: every recorded edit checks the root's mip (3 at fit, 0 at 100%), and each configuration
  saves a screenshot. After `zoomSlot` the zoom box keeps its old text (7%), though the viewer has zoomed.

To reproduce, run in a throwaway container with the repository mounted at the same path:

```
docker run --rm --memory 12g --user "$(id -u):$(id -g)" --init -e HOME=/tmp -e BENCH_GPUS=off -e BENCH_EDITS=10 \
    -v <repo>:<repo> -w <worktree> ghcr.io/charlesangus/natron-dev:<hash> \
    tools/bench/edit_latency.sh <Natron.AppImage> build/bench/edit-latency-matrix
```
