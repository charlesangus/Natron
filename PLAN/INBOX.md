## Pending

### 2026-09-18T01:31:24-04:00 — change-request
- refs: M24
- Append a new trailing task to M24's Phase 24.3 (after M24.P3.T5), covering
  the backlog item "node text should flip to white if the node colour is dark
  so text remains readable." Insert into
  `PLAN/MILESTONES/M24-node-graph-category-colour.md`:

  ```
  - [ ] M24.P3.T6 — Flip node label text colour to white when the node body
        colour is dark
    - files: `Gui/NodeGui.cpp`, `Gui/NodeGui.h`
    - approach: Node labels are drawn in a fixed colour today. Compute the
      relative luminance of the node's current body (category) colour
      wherever the label is painted/updated, and switch between a light and a
      dark text colour at a fixed luminance threshold, so labels stay
      readable regardless of category or user body colour. Re-evaluate when
      the body colour changes at runtime (the M24.P3.T5 re-colour path), so
      an open graph's label colours stay correct after a Preferences change.
    - verify: Xvfb GUI check — a node with a light category colour keeps dark
      text, a node with a dark category colour shows white text, and
      changing a category's colour in Preferences updates already-placed
      nodes' label colour along with their body.
    - size: S
  ```

### 2026-09-18T01:31:24-04:00 — change-request
- refs: none (new milestone M33)
- New milestone. Suggested file: `PLAN/MILESTONES/M33-input-pipe-visibility.md`.
  Board row: `| M33 | Node graph: input pipes not always shown | todo | ... |`

  ```
  # Milestone 33: Node graph — input pipes not always shown

  > Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

  Users report that a connected input sometimes fails to show its pipe/edge in
  the node graph. Investigate root cause (a repaint/refresh gap, z-order issue,
  or a specific node type/action that triggers it) and fix so every connected
  input reliably draws its pipe.

  Blocked on: not yet reproduced — needs a concrete repro (which node types or
  actions trigger the missing pipe) before this can be elaborated.

  Acceptance sketch:
  - A repro case is identified and documented.
  - Every connected input reliably shows its pipe across the reproduced
    scenario and a general graph smoke-test.
  ```

### 2026-09-18T01:31:24-04:00 — change-request
- refs: none (new milestone M34)
- New milestone. Suggested file: `PLAN/MILESTONES/M34-new-native-shuffle-node.md`.
  Board row: `| M34 | New native Shuffle node | todo | ... |`

  ```
  # Milestone 34: New native Shuffle node

  > Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

  The existing Shuffle node's UI/semantics are counter-intuitive and reportedly
  broken. Design and ship a new native Shuffle node with clearer
  channel-mapping semantics, replacing (or living alongside, then replacing)
  the current one.

  Blocked on: not yet prioritized — captured as backlog (user request
  2026-09-18); needs a design pass on the new node's UI/semantics before
  elaboration.

  Acceptance sketch:
  - A native Shuffle node ships with clear, testable channel-mapping
    semantics.
  - Existing projects using the old Shuffle node still load (migration or
    compatibility path defined).
  ```

### 2026-09-18T01:31:24-04:00 — change-request
- refs: none (new milestone M35)
- New milestone. Suggested file:
  `PLAN/MILESTONES/M35-remove-implicit-shuffle-elsewhere.md`. Board row:
  `| M35 | Remove implicit output-plane shuffling from non-Shuffle nodes | todo | ... |`

  ```
  # Milestone 35: Remove implicit output-plane shuffling from non-Shuffle nodes

  > Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

  Nodes other than Shuffle currently expose an "output plane" selector that
  effectively lets them shuffle channels. Drop that from all non-Shuffle
  nodes so they process channels in-place, and point users to the Shuffle
  node for actual shuffling.

  Blocked on: M34 — needs the new native Shuffle node shipped as the
  supported replacement path before removing the escape hatch elsewhere.

  Acceptance sketch:
  - Non-Shuffle nodes no longer expose an output-plane/channel-shuffle
    selector.
  - Channel remapping is only available via the Shuffle node.
  ```

### 2026-09-18T01:31:24-04:00 — change-request
- refs: none (new milestone M36)
- New milestone. Suggested file: `PLAN/MILESTONES/M36-new-channel-affordance.md`.
  Board row: `| M36 | Add "new channel/layer" affordance wherever a node outputs channels | todo | ... |`

  ```
  # Milestone 36: Add "new channel/layer" affordance wherever a node outputs channels

  > Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

  Anywhere a node has a channel/layer output setting, add a "new" option to
  create a channel/layer on the fly, rather than requiring it to already
  exist.

  Blocked on: not yet prioritized — captured as backlog (user request
  2026-09-18); needs a survey of which knob types need the "new channel"
  affordance.

  Acceptance sketch:
  - Channel/layer output selectors offer a "new..." option that creates and
    selects a new channel/layer.
  ```

### 2026-09-18T01:31:24-04:00 — change-request
- refs: none (new milestone M37)
- New milestone. Suggested file: `PLAN/MILESTONES/M37-channel-management-nodes.md`.
  Board row: `| M37 | Channel/layer management nodes | todo | ... |`

  ```
  # Milestone 37: Channel/layer management nodes

  > Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

  Add nodes for adding/removing channels/layers, with wildcard and/or regex
  support for selecting which channels/layers to affect.

  Blocked on: not yet prioritized — captured as backlog (user request
  2026-09-18).

  Acceptance sketch:
  - A node exists to add channels/layers.
  - A node exists to remove channels/layers, selectable via wildcard or
    regex pattern.
  ```

### 2026-09-18T01:31:24-04:00 — change-request
- refs: none (new milestone M38)
- New milestone. Suggested file:
  `PLAN/MILESTONES/M38-channel-layer-ui-organization.md`. Board row:
  `| M38 | Improve channel/layer information organization in the node UI | todo | ... |`

  ```
  # Milestone 38: Improve channel/layer information organization in the node UI

  > Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

  How nodes organize and display channel/layer information is awkward today.
  Nuke's approach is a reference point, not a model to copy outright. Redesign
  the layout for clarity.

  Blocked on: not yet prioritized — captured as backlog (user request
  2026-09-18); benefits from M39's terminology decision (layers vs. planes)
  landing first.

  Acceptance sketch:
  - Channel/layer selection UI is redesigned and demonstrably clearer than
    the current layout.
  ```

### 2026-09-18T01:31:24-04:00 — change-request
- refs: none (new milestone M39)
- New milestone. Suggested file: `PLAN/MILESTONES/M39-layers-not-planes.md`.
  Board row: `| M39 | Adopt "layer" terminology instead of "planes" | todo | ... |`

  ```
  # Milestone 39: Adopt "layer" terminology instead of "planes"

  > Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

  Replace the app's "planes" terminology with the more standard "layers" (per
  common compositing/EXR usage — confirm exact EXR nomenclature during
  elaboration) across UI strings, docs, and, where safe, internal naming.

  Blocked on: not yet prioritized — captured as backlog (user request
  2026-09-18); needs a scoping pass on how far the rename reaches (UI-only
  vs. internal APIs/serialization) before elaboration.

  Acceptance sketch:
  - User-facing UI uses "layer" instead of "plane" consistently.
  - Project files and serialization compatibility are unaffected.
  ```

### 2026-09-18T01:31:24-04:00 — change-request
- refs: none (new milestone M40)
- New milestone. Suggested file: `PLAN/MILESTONES/M40-node-text-wrap.md`.
  Board row: `| M40 | Node text layout: grow to max width, then wrap vertically | todo | ... |`

  ```
  # Milestone 40: Node text layout — grow to max width, then wrap vertically

  > Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

  Node labels currently can hang off the node's sides. Nodes should grow
  horizontally up to a maximum width, then wrap text onto additional lines
  (grow vertically) instead of overflowing.

  Blocked on: not yet prioritized — captured as backlog (user request
  2026-09-18).

  Acceptance sketch:
  - A long node name wraps onto multiple lines once the node reaches its max
    width, instead of overflowing the node's silhouette.
  ```

### 2026-09-18T01:31:24-04:00 — change-request
- refs: none (new milestone M41)
- New milestone. Suggested file: `PLAN/MILESTONES/M41-icon-replacement-pass.md`.
  Board row: `| M41 | Icon replacement pass | todo | ... |`

  ```
  # Milestone 41: Icon replacement pass

  > Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

  Many of the app's icons are hard to read. Do a thorough pass
  replacing/redesigning icons for legibility.

  Blocked on: not yet prioritized — captured as backlog (user request
  2026-09-18); needs an icon audit and a design direction before elaboration.

  Acceptance sketch:
  - A defined set of hard-to-read icons is identified and replaced with
    clearer versions.
  ```

### 2026-09-18T01:31:24-04:00 — change-request
- refs: none (new milestone M42)
- New milestone. Suggested file: `PLAN/MILESTONES/M42-stylesheet-overhaul.md`.
  Board row: `| M42 | Stylesheet / look-and-feel overhaul | todo | ... |`

  ```
  # Milestone 42: Stylesheet / look-and-feel overhaul

  > Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

  The general stylesheet and layout feel cramped and cluttered. Revisit
  spacing, density, and visual hierarchy across the app.

  Blocked on: not yet prioritized — captured as backlog (user request
  2026-09-18); needs a design direction/spec before elaboration.

  Acceptance sketch:
  - Key panels (node graph, properties, viewer) read as less cramped, judged
    against before/after screenshots.
  ```

### 2026-09-18T01:31:24-04:00 — change-request
- refs: none (new milestone M43)
- New milestone. Suggested file: `PLAN/MILESTONES/M43-drop-premult-concept.md`.
  Board row: `| M43 | Drop the premultiplied/unpremultiplied concept | todo | ... |`

  ```
  # Milestone 43: Drop the premultiplied/unpremultiplied concept

  > Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

  Remove the app's built-in premultiplied/unpremultiplied tracking and
  handling; treat that as the user's responsibility to manage/track, as in
  other professional compositing software.

  Blocked on: not yet prioritized — captured as backlog (user request
  2026-09-18); needs a scoping pass on everywhere premult state is read or
  written before elaboration, since it likely touches many nodes' metadata
  handling.

  Acceptance sketch:
  - The app no longer tracks or exposes a premultiplied/unpremultiplied
    concept; existing projects still load sensibly.
  ```

### 2026-09-18T01:31:24-04:00 — change-request
- refs: M24 (new milestone M44)
- New milestone. Suggested file: `PLAN/MILESTONES/M44-trackball-colour-editing.md`.
  Board row: `| M44 | Trackball-style colour editing | todo | ... |`

  ```
  # Milestone 44: Trackball-style colour editing

  > Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

  Add a "trackball" interaction to colour controls — holding modifier keys
  and dragging adjusts hue/saturation/value/temperature directly on the
  swatch, rather than only via sliders/dialogs.

  Blocked on: not yet prioritized — captured as backlog (user request
  2026-09-18); pairs naturally with M24's colour-panel work landing first.

  Acceptance sketch:
  - Dragging on a colour control with the documented modifier held adjusts
    hue, saturation, value, or temperature respectively.
  ```

### 2026-09-18T01:31:24-04:00 — change-request
- refs: none (new milestone M45)
- New milestone. Suggested file: `PLAN/MILESTONES/M45-tabtabtab-native-tab-menu.md`.
  Board row: `| M45 | Port tabtabtab-nuke as the native tab menu | todo | ... |`

  ```
  # Milestone 45: Port tabtabtab-nuke as the native tab menu

  > Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

  Port github.com/charlesangus/tabtabtab-nuke as Natron's native tab/create-node
  menu (fuzzy search + smart insertion).

  Blocked on: not yet prioritized — captured as backlog (user request
  2026-09-18); needs a look at tabtabtab-nuke's behavior/license and how it
  maps onto Natron's existing node-creation menu before elaboration.

  Acceptance sketch:
  - Pressing the tab-menu shortcut in the node graph opens a fuzzy-searchable
    create-node menu with tabtabtab-style smart insertion onto the selected
    node/pipe.
  ```

### 2026-09-18T01:31:24-04:00 — change-request
- refs: none (new milestone M46)
- New milestone. Suggested file: `PLAN/MILESTONES/M46-labelmaker-annotations.md`.
  Board row: `| M46 | Port Labelmaker as a native node graph annotation feature | todo | ... |`

  ```
  # Milestone 46: Port Labelmaker as a native node graph annotation feature

  > Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

  Port github.com/charlesangus/Labelmaker as a standard part of the app, to
  display rich information/annotations on the node graph.

  Blocked on: not yet prioritized — captured as backlog (user request
  2026-09-18); needs a look at Labelmaker's feature set and license before
  elaboration.

  Acceptance sketch:
  - Nodes can display rich, Labelmaker-style annotation text/data on the node
    graph.
  ```

### 2026-09-18T01:31:24-04:00 — change-request
- refs: none (new milestone M47)
- New milestone. Suggested file: `PLAN/MILESTONES/M47-render-dialog-rework.md`.
  Board row: `| M47 | Render dialog rework: framerange prompt and foreground-by-default | todo | ... |`

  ```
  # Milestone 47: Render dialog rework — framerange prompt and foreground-by-default

  > Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

  Render commands should prompt for the desired frame range instead of
  assuming the project range, and renders should happen in the foreground by
  default, with background rendering as an explicit option in the render
  dialog.

  Blocked on: not yet prioritized — captured as backlog (user request
  2026-09-18).

  Acceptance sketch:
  - Triggering a render prompts for a frame range (defaulting sensibly, e.g.
    to the project range).
  - Renders run in the foreground unless the user opts into background
    rendering via the dialog.
  ```

### 2026-09-18T01:31:24-04:00 — change-request
- refs: none (new milestone M48)
- New milestone. Suggested file: `PLAN/MILESTONES/M48-splice-node-into-pipe.md`.
  Board row: `| M48 | Node graph: splice a node/group into an existing pipe by drop | todo | ... |`

  ```
  # Milestone 48: Node graph — splice a node/group into an existing pipe by drop

  > Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

  Dropping a node or group of nodes onto an existing pipe should pipe it into
  that connection, with a highlight during drag showing how it will be
  spliced in.

  Blocked on: not yet prioritized — captured as backlog (user request
  2026-09-18).

  Acceptance sketch:
  - Dragging a node over an existing pipe highlights the pipe to preview the
    splice.
  - Dropping inserts the node between the two previously-connected nodes.
  ```

### 2026-09-18T01:31:24-04:00 — change-request
- refs: none (new milestone M49)
- New milestone. Suggested file: `PLAN/MILESTONES/M49-roto-feather-multi-select.md`.
  Board row: `| M49 | Roto: feather-handle drag affects all selected points | todo | ... |`

  ```
  # Milestone 49: Roto — feather-handle drag affects all selected points

  > Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

  On the Roto node, dragging a feather handle while multiple points are
  selected should adjust the feather for all selected points, not just the
  one being dragged.

  Blocked on: not yet prioritized — captured as backlog (user request
  2026-09-18).

  Acceptance sketch:
  - With multiple roto points selected, dragging any one's feather handle
    adjusts feather uniformly (or proportionally, per design) across the
    selection.
  ```

### 2026-09-18T01:31:24-04:00 — change-request
- refs: none (new milestone M50)
- New milestone. Suggested file: `PLAN/MILESTONES/M50-proper-ocio-support.md`.
  Board row: `| M50 | Proper OCIO support as a project property | todo | ... |`

  ```
  # Milestone 50: Proper OCIO support as a project property

  > Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

  Make OCIO configuration a first-class property of the project, and have the
  Viewer use OCIO for display transforms, rather than today's ad hoc
  handling.

  Blocked on: not yet prioritized — captured as backlog (user request
  2026-09-18); needs a scoping pass on current OCIO usage (M3 already made
  ACES 2.0 Studio the default OCIO config) before elaboration.

  Acceptance sketch:
  - The project has an OCIO config property, persisted with the project.
  - The Viewer's display transform is driven by that OCIO config.
  ```

### 2026-09-18T01:31:32-04:00 — change-request
- refs: none (new milestone M51)
- New milestone. Suggested file: `PLAN/MILESTONES/M51-deep-filtering-opendcx.md`.
  Board row: `| M51 | Deep filtering nodes (OpenDCX integration) | todo | [M51-deep-filtering-opendcx.md](PLAN/MILESTONES/M51-deep-filtering-opendcx.md) |`

  ```
  # Milestone 51: Deep filtering nodes (OpenDCX integration)

  Integrate OpenDCX (DreamWorks' open-source deep compositing extensions
  library) to add production-grade filtering, transformation, and manipulation
  nodes for deep pixel data. Extends M18's core deep compositing foundation
  with filtering operations on multi-sample deep pixels, subpixel masking,
  and efficient deep-data transformations.

  ## Phase 51.1: OpenDCX integration and DeepBlur

  - [ ] M51.P1.T1 — Vendorize OpenDCX and integrate as a CMake dependency
    - files: `tools/cmake/FindOpenDCX.cmake`, `CMakeLists.txt`, build config
    - approach: Add OpenDCX (github.com/dreamworksanimation/opendcx) as a
      pinned vendored dependency (fetch on build). Ensure it links cleanly
      against the existing OpenEXR and Python bindings. Target glibc 2.34+
      compliance on EL9.
    - verify: Build succeeds with OpenDCX statically linked; a minimal test
      that calls OpenDCX's deep API without errors completes.
    - size: M
  - [ ] M51.P1.T2 — Design DeepBlur node: blur on deep pixel data
    - files: `Engine/Nodes/Deep/DeepBlur.cpp`, `.h`, Python bindings
    - approach: Blur (box, Gaussian, or both) applied per-sample on deep
      pixels, preserving sample depth and coverage. Each sample at each pixel
      location is blurred independently. Knobs: blur method (box/Gaussian),
      blur size (XY radii), edge handling (pad/wrap/clamp). Start with a
      single blur method; add others if time/QA headroom permits.
    - verify: GUI test — blur radius knob changes output interactively. CLI
      render: deep blur on a deep image with varying depth/coverage produces
      smooth inter-sample blurring and maintains coverage correctness. Compare
      against OpenDCX reference behavior where applicable.
    - size: L
  - [ ] M51.P1.T3 — Add DeepBlur to the node registry and OFX plugin set
    - files: `HostModel/EffectPluginLoader.cpp`, deep node registry
    - approach: Register DeepBlur as a native node, add category tagging for
      deep nodes, and vendor it in the release AppImage.
    - verify: DeepBlur appears in the Create Node menu under Deep category.
      A project using it loads and renders without errors.
    - size: S

  ## Phase 51.2: DeepTransform (affine transforms on deep pixels)

  - [ ] M51.P2.T1 — Design DeepTransform: scale, rotate, skew deep pixels
    - files: `Engine/Nodes/Deep/DeepTransform.cpp`, `.h`
    - approach: Affine transformation (scale, rotation, skew) applied per-sample
      on deep pixels, preserving sample depth and coverage. Resampling strategy:
      decide between bilinear, bicubic, or OpenDCX's built-in resampling. Knobs:
      translate (XY), scale (XY or uniform), rotate (degrees), and optional
      skew (shear). Reference OpenDCX's DeepTransform class for efficient
      subpixel-accurate implementation.
    - verify: GUI interactive transform. CLI render: a rotated deep image shows
      correct depth ordering and no sample loss at boundaries. Verify subpixel
      accuracy via a test with fractional transforms.
    - size: L
  - [ ] M51.P2.T2 — Ship DeepTransform in the deep node registry
    - files: same registry as M51.P1.T3
    - approach: As M51.P1.T3 — register and vendor.
    - verify: Menu presence, load/render verification as above.
    - size: S

  ## Phase 51.3: Deep utility nodes (QA and Polish)

  - [ ] M51.P3.T1 — Add DeepSamples and DeepInfo inspection nodes
    - files: `Engine/Nodes/Deep/DeepInfo.cpp`, `.h`
    - approach: Read-only nodes that output deep-pixel statistics/visualization.
      DeepInfo outputs depth range, sample count per-pixel, coverage, and
      channel list as readable metadata overlays or log output. No knobs,
      pure inspection.
    - verify: Xvfb GUI: connect DeepInfo to a deep image, viewer shows per-pixel
      statistics correctly. CLI: log output matches expected format.
    - size: M
  - [ ] M51.P3.T2 — Polish deep node UI and docs
    - files: doc strings, Help menu entries, example projects
    - approach: Write brief help text for DeepBlur and DeepTransform. Add
      example .ntp projects demonstrating each node. Ensure knob tooltips
      explain subpixel precision, sample depth semantics, and coverage
      preservation.
    - verify: Help text is searchable and explains each knob. Example projects
      load and render without errors.
    - size: S

  **Verification gate:** DeepBlur and DeepTransform nodes ship, load existing
  deep projects without errors, and render deep images with correct per-sample
  filtering/transformation. Example projects demonstrating both exist. OpenDCX
  library is vendored and statically linked; no external runtime dependency.

  ## Decisions

  - 2026-09-18 — **OpenDCX chosen**: DreamWorks' production-grade deep
    compositing library. Rationale: widely used in film VFX, C++ API integrates
    cleanly with OpenEXR, provides battle-tested filtering and transform
    algorithms on deep pixels. Alternative (DIY implementation) ruled out due to
    algorithmic complexity and risk of sample-loss bugs.
  - 2026-09-18 — **Phase sequencing**: DeepBlur first (simpler, high immediate
    value), DeepTransform second (more complex, useful for deep manipulation
    workflows), then utility/polish. Blocks on M18 shipping; pairs naturally
    with M21 (deep tier-2 nodes).
  ```
