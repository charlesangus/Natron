## Pending

- **Shared geometry group selection** (user, 2026-11-25, raised during M600 - Native Geometry UAT Round 2): extract the Group / Group Type logic into one shared component used by every native geometry node, so no node reinvents it or differs from the others in how it presents or applies the rule. GeoUVProject only got a Group Type knob in M600. The component covers:
  - the knobs, labels, tooltips and Guess-from-Group resolution;
  - the rule that blank means every element of the class;
  - mapping a point or primitive selection onto points, vertices or primitives;
  - leaving unselected elements untouched.

  Audit every Geo* node that takes a Group (GeoTransform, GeoUVProject, GeoDisplace, GeoWrangle, …) and move each onto it, with one shared test. When in doubt, match Houdini. Feature: F15 follow-up, or a standalone milestone after F15 lands.
