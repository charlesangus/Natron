## Pending


### 2026-11-17T06:30:00Z — change-request
- refs: none
- New milestone (user, F8 - Motion Vectors UAT): easier access to a node's "extra" or hidden inputs. Today the user has to move the other inputs to reach an extra input such as RotoPaint's Vectors. For /cat-plan to plan; it is not part of F8.

### 2026-11-17T08:00:00Z — change-request
- refs: M153, M179
- New milestone (user): image-driven warp providers for distortion concatenation. IDistort, STMap and IWarp feeding a downstream warp provide their inverse map to the chain M153 - Distortion Concatenation builds. Their UV image renders over the pulled-back window as a probe (M179 - Render Graph Continuation's probe → extend → continue), then the frame is extended to request only the source footprint those UVs point at, then the continuation samples it. Deps: M153, M179.
