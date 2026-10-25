# M205 - Object Mask To Roto

A click-to-segment tool for Roto and RotoPaint, and for any curve host M146 - Roto Hosting For Any Node enables. It is ported from darktable 5.6's AI object mask: `src/develop/masks/object.c` and `src/common/ai/segmentation.c`, PR #20378, with the architecture generalised in #21492.

The user picks "Object select" in the curve toolbar and works on the current frame of the host's Source input:
- A click adds a positive point. A Shift-click adds a negative point. A drag adds a box prompt.
- After each prompt, the mask's outline is drawn live in the viewer.
- Enter commits. The mask is traced into editable Roto Beziers, keyed at the current frame in the selected layer.

Animating the shapes afterwards is ordinary Roto work, by hand or with the tracker (user choice, 2026-10-25). There is no per-frame segmentation and no video propagation.

The encoder runs once per frame and input, and its embedding is cached. Each prompt runs only the light decoder, on the CPU, as darktable does. The default model is SegNext ViT-B SAx2 HQ (MIT). SAM 2.1 tiny, small and base-plus are optional.

Deps: M203 - ONNX Inference Backend, M204 - AI Model Manager, M146 - Roto Hosting For Any Node (curve interact, toolbar builder). See `DECISIONS/2026-10-25-darktable-ai-and-image-tool-ports.md`.

## Scope

**In:**
- **`Engine/Ai/Segmentation`:** prepares the encoder input, runs the encoder, caches the embedding, and runs the decoder for a set of points and boxes. It returns a float mask at source resolution, and covers both the SegNext and SAM 2.1 manifests.
- **Display-referred encoder input:**
  - The models expect sRGB uint8. The input goes through the project's OCIO default display/view (M50 - Project OCIO Support) to sRGB and is resized so its longest side is 1024, as darktable does.
  - Using the view transform is correct here: only the *prompt image* is display-referred. The output is geometry.
- **Contour tracing and Bezier fitting:**
  - Marching squares at a threshold, then hole detection.
  - Then Douglas–Peucker simplification and Schneider cubic fitting into Roto control points.
  - Holes become separate Beziers using the subtracting operator.
- **The "Object select" tool in the curve toolbar**, with tool knobs:
  - `aiModel` (via `ModelKnob`), `threshold` (0.5), `simplify` (px tolerance, 1.5) and `minArea` (px², 64)
  - `holes` (on), `smooth` (on), `feather` (0) and `clearPrompts`
- **A live outline preview** drawn with the existing host overlay line loops (`Gui/HostOverlay.cpp`). The prompt points are drawn as handles.
- **One undo step per commit.**

**Out:**
- Per-frame AI masks, mask propagation through SAM2's video memory, and darktable master's raster `.dtdata` storage: none (user, 2026-10-25).
- A pixel-mask preview overlay: M165 - Rich Viewer Overlays. The outline is the preview here.
- A matte output node: none. The output is Roto shapes.
- Brush strokes as output: none.

**Edge cases:**
- **No Source connected:** the tool is disabled, with a tooltip.
- **Model missing or AI disabled:** the tool shows `ModelKnob`'s message in the viewer, and prompts are ignored.
- **Proxy or render scale:** points are kept in canonical coordinates, the encoder always sees full-resolution input, and the Beziers are emitted in canonical coordinates.
- **Changing frame with uncommitted prompts:** the prompts are discarded, with a viewer message.
- **The Source changes while prompting** (detected by its hash): the embedding is re-encoded on the next prompt.
- **Encoder latency:** the encoder runs on a worker thread with a busy cursor. Prompts that arrive during encoding are queued.
- **Pixel aspect ratio ≠ 1:** the resize to the encoder input follows pixel aspect ratio, and the contour is mapped back.
- **An empty mask:** nothing is committed, and the viewer shows a message.

**Assumptions:**
- darktable-ai's SegNext and SAM 2.1 `config.json` name the encoder and decoder graphs and their prompt tensors in the way `segmentation.c` reads them.
- M146's toolbar builder lets a host append a tool with its own knobs, as its Scope says.

## Phase 205.1: Segmentation core

- [ ] M205.P1.T1 — Segmentation API: encoder, embedding cache and decoder
  - files: new `Engine/Ai/Segmentation.h`, `Engine/Ai/Segmentation.cpp`, `Engine/CMakeLists.txt`
  - approach:
    - Port darktable `src/common/ai/segmentation.c` at the `DARKTABLE_REF` in `tools/ci/local/fetch-assets.sh`. If the 5.6.2 pin lacks #21492's generalised form, take the file from the darktable master SHA recorded in `## Decisions`.
    - `Segmenter::encode(rgb8, w, h, key)` caches the embedding in memory by key: node hash, frame and model id, LRU of 4.
    - `Segmenter::decode(points[], labels[], boxes[]) -> mask` scales the mask back to source resolution. The decoder session is forced to the CPU EP.
    - Encoder and decoder sessions come from M203's `SessionCache`, and model files from M204's `ModelConfig`.
  - verify: P1.T3's tests pass.
  - size: L

- [ ] M205.P1.T2 — Mask tracing to Bezier curves
  - files: new `Engine/Ai/MaskTrace.h`, `Engine/Ai/MaskTrace.cpp`
  - approach:
    - Marching squares on the thresholded mask gives closed contours with an outer/hole classification by signed area. Drop contours under `minArea`.
    - Douglas–Peucker at `simplify`, then a Schneider cubic fit with corner detection. The result is control points plus left and right tangents.
    - If darktable master's vector trace (#22611) is present at the pinned SHA, compare against it and note any difference in `## Decisions`.
  - verify: P1.T3's tests pass.
  - size: L

- [ ] M205.P1.T3 — Test segmentation and tracing
  - files: new `Tests/Ai/Segmentation_Test.cpp`, new `Tests/Ai/MaskTrace_Test.cpp`, `Tests/fixtures/ai/` (README)
  - approach:
    - **MaskTrace** uses synthetic masks: a disc traces to one Bezier within 0.5 px Hausdorff, a ring gives one outer and one hole, and a blob below `minArea` is dropped.
    - **Segmentation** skips with a message when SegNext isn't installed. Otherwise, a positive click on the centre of a synthetic bright square on a dark ground gives an IoU above 0.95 with the square.
    - Embedding cache hits are counted.
  - verify: `ctest -R Ai` passes, with the model-gated test run on a host that has the model.
  - size: M

## Phase 205.2: Roto tool

- [ ] M205.P2.T1 — "Object select" tool and its knobs
  - files: `Engine/RotoCurveInteract.h`, `Engine/RotoCurveInteract.cpp` (from M146), `Engine/RotoPaint.cpp`, new `Engine/RotoObjectSelect.h`, `Engine/RotoObjectSelect.cpp`
  - approach:
    - Register the tool through M146's toolbar builder, with the Scope's knobs.
    - Pen handling: click for a positive point, Shift-click for a negative point, drag for a box, Backspace removes the last prompt, Esc clears, Enter commits. Each prompt calls `decode` and traces for the preview.
    - Encoding is triggered on the tool's first prompt per frame. It renders the Source at the current frame at full scale, then applies the OCIO display/view transform to sRGB uint8 via the M50 helper.
  - verify: P2.T3's GUI script passes.
  - size: L

- [ ] M205.P2.T2 — Preview overlay and commit to Beziers
  - files: `Engine/RotoObjectSelect.cpp`, `Engine/RotoUndoCommand.h`, `Engine/RotoUndoCommand.cpp`
  - approach:
    - Draw the preview contours as line loops and the prompts as handles: green for positive, red for negative.
    - A commit builds one Bezier per outer contour in the selected layer, keyframed at the current time. Holes become Beziers with the subtracting operator. The shape is named `Object<N>`. `feather` is applied.
    - Add a `CommitObjectMaskUndoCommand` that removes the shapes on undo.
  - verify: P2.T3's GUI script passes, and undo removes the shapes.
  - size: M

- [ ] M205.P2.T3 — GUI test and docs
  - files: new `Tests/gui/test_roto_object_select.py` (follow the existing Xvfb GUI test layout), `Engine/Nodes/README.md` or the Roto doc page, `Engine/Ai/README.md`
  - approach:
    - Under Xvfb (recipe in `build/deeprepro/run-gui.sh`), the script builds a Read of a fixture still and a Roto node, picks Object select, and synthesises a click.
    - It commits, and asserts that a Bezier exists keyed at the frame and that its rendered matte has an IoU above 0.9 with the fixture's object. It also checks undo.
    - Docs explain the workflow and the model licence notes.
  - verify: the script passes on a host with SegNext installed, and screenshots of the preview and the committed shapes are shared.
  - size: M

**Verification gate:**
- The full build and ctest pass.
- The Xvfb object-select script passes with SegNext installed.
- Screenshots of the prompt preview and the committed Beziers on a real plate are shared with the user before sign-off.
- An AppImage built with `package.sh` is handed over for UAT.
