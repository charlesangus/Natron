# Motion vectors and curves that follow them

Decided 2026-10-25 with the user in a /cat-plan --grill session ("add motion vector generation and a way to move roto/paint/eventually spline warp curves using motion vectors").

- **New epic E18 - Motion Vectors.** It holds the native `VectorGenerator` node and its GPU port, the curve follower, and the SplineWarp hook-up. The neural flow method sits under E16 - AI Tools, on the ONNX backend.
- **Flow is classical first:** DIS with variational refinement as the fast default, and TV-L1 for quality, both native on the CPU and then the GPU. A RAFT-class ONNX method follows later as a third `method` choice.
- **The node writes the existing Forward and Backward motion layers in pixels, plus a `vectorConfidence` layer** from forward/backward consistency. It takes an optional Matte input. Frames are independent by default, and temporal smoothing is a knob.
- **Curves follow vectors from any node with motion layers** (VectorGenerator or CG renders) and bake into ordinary keyframes, with optional keyframe reduction. There are no live links.
  - Existing keys are anchors, and gaps between them are bridged forward and backward with a crossfade.
  - The motion model is chosen per follow: per-point, or a robust translate, affine or perspective fit that keys the shape's transform. Paint strokes always use a fit, since they have no per-frame points.
  - Occluded points are inferred from the shape's confident motion, and the inferred frames are reported.
- **The follower lives in the M146 geometry base,** so Roto, RotoPaint and SplineWarp share it. SplineWarp follows whatever is selected, with no pairing logic.
