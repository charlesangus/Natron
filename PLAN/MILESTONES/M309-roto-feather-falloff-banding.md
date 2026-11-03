# M309 - Roto Feather Falloff Banding

Stub. Follow-up from M224 - Fluid Roto And RotoPaint Interaction UAT (user, 2026-11-02): the feather falloff of Roto/RotoPaint shapes shows visible banding in some cases. Screenshots are on the bosley-native-nodes host only: `.local/rotopaint_banding_annotated.png` and `.local/rotopaint_banding_clean.png` (copy them into the milestone's investigation, as small crops, before elaborating).

Investigate first: whether the banding comes from the feather mesh/ramp generation (too few falloff steps, linear interpolation across large triangles), the falloff curve, quantisation in an 8-bit intermediate, or the viewer's display path; then fix at the source.

Blocked on: nothing; elaborate when claimed.
