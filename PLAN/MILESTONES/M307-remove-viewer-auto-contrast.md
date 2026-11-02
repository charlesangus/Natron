# M307 - Remove Viewer Auto-Contrast

Follow-up from the user's 2026-11-02 answer during M234 - Camera Projection: the 3D viewport ignores the viewer's auto-contrast, and the auto-contrast feature itself is to be removed. Delete the viewer's auto-contrast toggle, its knob (M164 - Viewer Node Knobs made it a Viewer knob), the min/max computation and the shader and CPU paths that apply it, its Python API and its documentation. No backward compatibility: saved projects with it on simply lose it.

Deps: M164.

Stub: to be broken down into tasks when claimed.
