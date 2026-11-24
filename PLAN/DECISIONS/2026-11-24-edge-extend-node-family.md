# Edge-extend node family (2026-11-24)

The user decided that edge extension ships as a family of single-method nodes on a shared CPU+GPU kit (`EdgeFillKit`), not as one uber node with an opaque method choice:

| Node | Method | Milestone |
|---|---|---|
| EdgePush | Push-pull pyramid; replaces and renames EdgeExtend, no compatibility | M238 - EdgePush And Edge Fill Kit |
| EdgeClamp | Nearest edge colour via 1+JFA; optional distance-scaled seam blur (close enough to slEdgeHammer) | M591 - Native EdgeClamp |
| EdgeSmear | Push-pull of an identity UV map, then resample (moves real pixels) | M592 - Native EdgeSmear |
| EdgePushVector | Iterative short pushes along the matte normal | M593 - Native EdgePushVector |
| EdgeDistort | One gradient-driven displacement warp over the whole image; optional Vector input | M594 - Native EdgeDistort |
| EdgeDetail | Separate detail restoration: src + amount × high-pass(orig) × edge mask | M595 - Native EdgeDetail |

These shared stages are common to the family:
- A Matte input and choice.
- Source Premultiplied and Premultiply Output, both off by default (straight in and out).
- Threshold plus Softness, blended as `w·src + (1 − w)·fill`.
- Erode.
- An edge mask `alpha × (1 − w)` written to a named layer.
- A fill extent of the source RoD by default, optionally the union of the source RoD and the format.
- Black and never an error when nothing is solid.

Rejected: Poisson, Telea / Navier-Stokes, PatchMatch, Inpaint-style detail cloning, additive keyer, colour wrap, and Farbman kernels. Prior-art survey: the session report of 2026-11-24, which covered Nuke EdgeExtend and Inpaint, Nukepedia gizmos, Blender Inpaint, Houdini Extrapolate Boundaries and Smooth Fill, Silhouette and Flame. This resolves M238's Q1–Q3.
