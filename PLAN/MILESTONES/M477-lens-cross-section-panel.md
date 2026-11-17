# M477 - Lens Cross-Section Panel

A live cross-section of the selected traced lens at the top of the tab that holds the lens knobs: the Bokeh tab on ZDefocus, the Lens tab on Defocus (user, 2026-11-16). It draws the elements, the iris, the sensor plane at the current focus, and ray fans for the centre and the corner of the frame, with blocked rays in red. It updates as focus, f-stop, film format, coverage and the creative knobs change (Higx Optics parity). It reuses M463 - Bundled Lens Database's browser diagram, moved into a shared widget, and is a custom knob type made through `KnobGuiFactory`, as `Gui/KnobGuiColorWheelGrid.{h,cpp}` is.

Deps: M292 - Traced Lens Bokeh, M473 - Lens Camera And Focus Controls (format, Filmback Offset, coverage), M474 - Traced Lens Creative Aberrations (perturbed ray landings), M476 - Traced Anamorphic Bokeh (the meridian toggle).

## Scope

**In:**
- `Gui/LensDiagramWidget.{h,cpp}`: the browser's QPainter diagram moved into a widget, plus the sensor plane, ray fans for given field heights, rays coloured by fate (passed or blocked, the blocked part in red), and an optional aperture and front-filter position.
- An engine-side `LensDiagramData` (prescription, stop semi-diameter from f-stop, image-plane position from the focus, corner field from the film format and coverage, `PupilTerms`) that the nodes provide, so the GUI never reads knobs itself.
- A knob type for the diagram (no value, not animatable, not saved) with a `KnobGui` through `KnobGuiFactory`. It is collapsible, shown only while a traced lens is selected, and sized to fit 1920x1080 with the panel open.
- Ray tracing for the drawing on a worker thread, cancelled and restarted when an input changes.

**Out:**
- The diagram in the lens browser stays as M463 built it, now using the shared widget.
- Drawing in the viewer: none.

**Edge cases:**
- Procedural lens, or a lens error: the knob is hidden.
- Rapid knob drags: only the latest request draws; stale results are dropped.
- Anamorphic lenses (M476 - Traced Anamorphic Bokeh): the y–z meridian is drawn, with a toggle for x–z.

**Assumptions:**
- Unconfirmed: 15 rays per fan at 3 field heights (centre, half, corner) read clearly at the panel's width.

## Phase 477.1: Diagram

- [ ] M477.P1.T1 — Shared lens diagram widget
  - files: `Gui/LensDiagramWidget.{h,cpp}` (new), `Gui/LensBrowser.{h,cpp}`, `Gui/CMakeLists.txt`
  - approach: Move the browser's cross-section painting into `LensDiagramWidget`. Add the sensor plane, ray fans at given field heights traced with `Optics::traceSequential` (with optional `PupilTerms`), and rays drawn grey up to their last valid surface and red from where they are blocked. The browser uses the widget unchanged in look.
  - verify: `Tests/gui/m463_lens_browser.py` still passes and its screenshot is unchanged in layout; a new widget test draws Petzval with a corner fan and checks that some red pixels exist at f/1.4 and none on axis.
  - size: M

- [ ] M477.P1.T2 — Lens diagram knob type
  - files: `Engine/KnobTypes.{h,cpp}` (a `KnobLensDiagram` type), `Engine/Optics/LensDiagramData.h` (new), `Gui/KnobGuiLensDiagram.{h,cpp}` (new), `Gui/KnobGuiFactory.cpp`, `Gui/CMakeLists.txt`
  - approach: Follow `KnobGuiColorWheelGrid`'s pattern for a custom knob GUI. The knob holds a `std::function<LensDiagramData(TimeValue)>` the node sets, and signals a refresh. The GUI wraps `LensDiagramWidget`, has a collapse arrow, and recomputes on a worker thread on refresh, cancelling the previous run. The knob has no value, isn't animatable, and is never serialised.
  - verify: a native GUI test (under Xvfb) creates a test node with the knob, sets data, and checks the widget renders and a refresh replaces the drawing; a project save and load round-trips with no diagram data in the file.
  - size: L

## Phase 477.2: Nodes

- [ ] M477.P2.T1 — Wire the diagram into ZDefocus and Defocus
  - files: `Engine/Nodes/Filter/ZDefocus.{h,cpp}`, `Engine/Nodes/Filter/Defocus.{h,cpp}`, `Tests/gui/m477_lens_diagram.py` (new)
  - approach: Add the knob at the top of the tab holding the lens knobs, visible only for a traced lens. Provide `LensDiagramData` from the knobs at the current time, and refresh it on changes to the lens, f-stop, focus, format, coverage or creative knobs.
  - verify: the GUI script under Xvfb selects Petzval on ZDefocus, screenshots the diagram at f/1.8 and f/8 and at two focus distances, and checks the panel fits 1920x1080; screenshots are collected for F13 - ZDefocus Rework's UAT.
  - size: M

**Verification gate:** the full build and `ctest` pass; the lens browser's GUI script still passes; the diagram GUI script passes, and its screenshots are collected at F13 - ZDefocus Rework's UAT.

## Decisions

- 2026-11-16 — **At the top of the lens tab in the panel (user, /cat-plan grill).**
- 2026-11-16 — **A knob type through KnobGuiFactory (planner, code finding):** `KnobGuiColorWheelGrid` is the precedent for a custom knob widget; native nodes have no other panel-widget hook.
