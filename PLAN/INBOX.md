## Pending

### 2026-11-14T19:00:00+00:00 — change-request
- refs: F11, M427, M428, M429, M430
- Tag M427 - Native Geometry Texture Painting, M428 - Native UV Operators, M429 - GeoAttribPaint Node and M430 - Viewer 2D/3D Switch with F11 - Texture Painting (user, 2026-11-14, /cat-discuss). Dependents outside F11: none (only M427 depends on M428, and both join). Consequence the user should confirm at drain: M427, M428, M429 and M430 depend on F15 - Native Geometry members (M358, M359, M376), and a done F15 member satisfies those deps only once F15 lands. So these four can't start, and F11 can't ship, until F15 - Native Geometry lands. Also update each stub's "Blocked on: F11 - Texture Painting landing" wording, since they now merge into F11's branch instead, and widen F11's Goal to cover native geometry, UV operators, attribute painting and the Viewer 2D/3D switch.
