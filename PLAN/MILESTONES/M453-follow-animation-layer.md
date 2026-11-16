# M453 - Follow Animation Layer

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Requested by the user on 2026-11-16 during the F8 - Motion Vectors UAT: follow results go into a separate animation layer, so the user keeps the follow's motion and can still keyframe the shape on top; the keyframe-reduction pass could then run on the follow layer only. Today follow (`VectorFollowControls::Imp::follow`, `Engine/VectorFollowKnobs.cpp`; baking in `Engine/VectorFollowBake.cpp`) writes straight into the shape's user-visible keys — whole-curve control-point keys (`Bezier::setFollowedPoints`), the item's translate keys, or the 9-value `extraMatrix` — so later hand keys and follow keys share one curve. Nothing in the roto/SplineWarp model is an animation layer; the layer needs new storage and an evaluation step that composes it under the user's keys.

Blocked on: F8 - Motion Vectors landing — its UAT round is reworking the follow code on the Feature branch (keyframe reduction becomes a separate post-process on any shape, a Vectors input replaces the source dropdown, follow over existing keys is being fixed), so the layer design has to start from the landed code.

Acceptance sketch:
- Running follow writes into a follow layer, leaving the shape's own keys untouched; the shape evaluates as follow layer composed with the user's keys (offset on top).
- Keying the shape after a follow adjusts it on top of the followed motion instead of fighting the follow keys.
- Keyframe reduction can run on the follow layer alone.
- The layer can be cleared, baked down into the shape's keys, and toggled off; it saves and loads with the project and works for the per-point, translate and affine/perspective follow models.
- Undo covers every layer operation.
