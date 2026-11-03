# M318 - LensCorrect Bounded Input Region

Stub. From M160 - Lensfun Lens Correction (user, 2026-11-02): LensCorrect requests its Source's whole RoD for every render (`getRegionsOfInterest`, LensCorrect.cpp), because the distortion map has no cheap bound. Zoomed viewer regions and tiled renders therefore cost a full-frame upstream render. Compute a bounded input region by evaluating the distortion map over the render window's edges and an interior grid, plus a safety margin, on CPU and GPU (`warpGpuInputRoI`). The "tiled equals full-frame" test must keep passing, including on extreme profiles (fisheye).

Blocked on: M160 - Lensfun Lens Correction merging.
