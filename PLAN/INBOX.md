## Pending

- **Shared geometry group selection** (user, 2026-11-25, raised during M600 - Native Geometry UAT Round 2): extract the Group / Group Type logic into one shared component used by every native geometry node, so no node reinvents it or differs from the others in how it presents or applies the rule. GeoUVProject only got a Group Type knob in M600. The component covers:
  - the knobs, labels, tooltips and Guess-from-Group resolution;
  - the rule that blank means every element of the class;
  - mapping a point or primitive selection onto points, vertices or primitives;
  - leaving unselected elements untouched.

  Audit every Geo* node that takes a Group (GeoTransform, GeoUVProject, GeoDisplace, GeoWrangle, …) and move each onto it, with one shared test. When in doubt, match Houdini. Feature: F15 follow-up, or a standalone milestone after F15 lands.

- **VDB grids in the geometry file cache** (PM, 2026-11-25, from the M417 - VDB Volume Primitive review): M403 - Geometry File Cache (in F18) does not serialize `GeoDetail::sharedGrids()`, but M417 (in F22) makes VDB primitives valid. When F18 and F22 meet, whichever lands second must add grid stream I/O to the GeoFileFormat writer and reader. Until then, a cached detail with VDB prims reloads with a dangling `vdbGrid` index and throws "VDB primitive has no grid". Add a round-trip test.

- **GUI scripts failing on main** (PM, 2026-11-25): an A/B run on main's own workflow (mainref-gui-ab, main 83241b652, AppImage from run 45653134112) fails the same checks as M599's runs. M599 - Runner Efficiency is not the cause. Needs a fix milestone:
  - `m146_panel_viewer_slot`: Roto/RotoPaint viewer interface and viewer slot hand-off (4 checks). Likely M511 - Viewer Toolbar Fits 1920x1080's combo eliding, unconfirmed.
  - `m275_viewer_widget_editing`: translate/scale snap back after release, a real value change, not timing.
  - `m121_routing_mix`: `[adjustbbox]` Num Pixels 0 leaves the centre empty.
  - `m224_shape_drag`: the script locks onto viewer text. Its fix (e9347fd229) is on M299 - Roto Render Performance's branch and lands with it.

- **Layered document flatten: clipping and group isolation** (PM, 2026-11-25, from the M79 - Layered Document Readers review): when a PSD or KRA has no stored composite, the flattened composite ignores clipping masks and group isolation; only Normal-over is documented. PSD and Krita clip differently, so this is a design task.
