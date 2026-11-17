# Layer IDs are plain layer names (user, 2026-11-17)

Raised in the F8 - Motion Vectors UAT: motion vectors written to EXR came back in a layer named `uk.co.thefoundry.ofxImagePlaneBackMotionVector.UV`.

- Every layer's ID is its plain name, and nothing user-facing differs from the ID. Built-ins: `rgba` (colour stays `rgba`), `forward` and `backward` with channels `u`, `v`; `disparityL` and `disparityR` with `x`, `y`.
- No separate "channels label": "Motion", "Disparity" and "Alpha" go away, so menus show `forward` and `forward.u`. The Python `ImageLayer` loses `componentsPrettyName`; no backward compatibility.
- Channel names are lowercase, except the OpenEXR-standard `R`, `G`, `B`, `A` and `Z`, which other applications detect by name.
- No OFX plane, layer or component strings are persisted or translated anywhere: not in project files, image files, the cache, Python or knob values. One mapping table at the OFX plugin boundary (the `Ofx*` host files) is the only translation, because OFX plugins name planes that way.
- File I/O writes `layer.channel` verbatim and reads it verbatim, so built-ins round-trip with no mapping. The colour part of a multi-part EXR is named `rgba`.
