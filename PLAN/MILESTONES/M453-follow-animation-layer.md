# M453 - Follow Animation Layer

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Requested by the user on 2026-11-16 during the F8 - Motion Vectors UAT: follow results go into a separate animation layer, so the user keeps the follow's motion and can still keyframe the shape on top; the keyframe-reduction pass could then run on the follow layer only. Today follow (`VectorFollowControls::Imp::follow`, `Engine/VectorFollowKnobs.cpp`; baking in `Engine/VectorFollowBake.cpp`) writes straight into the shape's user-visible keys — whole-curve control-point keys (`Bezier::setFollowedPoints`), the item's translate keys, or the 9-value `extraMatrix` — so later hand keys and follow keys share one curve. Nothing in the roto/SplineWarp model is an animation layer; the layer needs new storage and an evaluation step that composes it under the user's keys. Animation layers are built on channel networks (user, 2026-11-17; `PLAN/DECISIONS/2026-11-17-channel-networks.md`), as Houdini builds its layers from CHOPs. The follow result is a layer channel set, M488 - Channel Operators And Channel VOP's Layer Mix composes it with the user's keys, and M489 - Channel Sources From Shapes, Tracks And Cameras's shape export drives the shape. Member of F32 - Channel Networks.

Blocked on: F8 - Motion Vectors landing — its UAT round is reworking the follow code on the Feature branch (keyframe reduction becomes a separate post-process on any shape, a Vectors input replaces the source dropdown, follow over existing keys is being fixed), so the layer design has to start from the landed code. Also M486 - Channel Network Core, M487 - Channel Export And chop(), M488 - Channel Operators And Channel VOP and M489 - Channel Sources From Shapes, Tracks And Cameras, which this layer is built on.

Acceptance sketch:
- Running follow writes into a follow layer, leaving the shape's own keys untouched; the shape evaluates as follow layer composed with the user's keys (offset on top).
- Keying the shape after a follow adjusts it on top of the followed motion instead of fighting the follow keys.
- Keyframe reduction can run on the follow layer alone.
- The layer can be cleared, baked down into the shape's keys, and toggled off; it saves and loads with the project and works for the per-point, translate and affine/perspective follow models.
- Undo covers every layer operation.

## Decisions

- 2026-11-17 — **Layers on channel networks (user):** chosen over a separate per-curve layer stack. It means one compose mechanism, as in Houdini, at the cost of waiting for F32 - Channel Networks.
