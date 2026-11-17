# Porting the native interacts to the overlay draw list

This is the audit of every raw-GL drawing construct in the native interacts, and where each one
goes on `OverlayDrawList` (`Engine/OverlayDrawList.h`). The draw list holds canonical-space points
with screen-pixel sizes; `Gui/NodeOverlayRenderer.cpp` draws it. A port moves only the drawing: the
pen, key and hit-test code of each interact stays as it is.

## Files and call counts

Counted with `grep -cE '\bgl[A-Z][A-Za-z0-9]*\('` (lines holding a GL call) and
`grep -cE '\b(renderText|getPixelScale|getViewportSize|getScreenPixelRatio|getViewportRect|getCursorPosition)\('`
(lines calling `OverlaySupport`), on the base of `milestone/m434-interacts-on-the-overlay-draw-list`.

| File | GL lines | OverlaySupport lines | Drawing entry points |
|---|---:|---:|---|
| `Engine/RotoCurveInteract.cpp` | 200 | 20 | `drawOverlay`, `drawSelectedCp`, `drawEllipse`, `drawCloneSource`, `drawArrow`, `drawBendedArrow`, `drawSelectedCpsBBOX`, `drawRippleEditBorder`, `drawLineVertices` |
| `Engine/RotoPaintInteract.cpp` | 46 | 3 | `drawStrokePreviews`, `drawPaintToolOverlay` |
| `Engine/RotoObjectSelect.cpp` | 41 | 5 | `RotoObjectSelect::draw` |
| `Engine/TrackerNode.cpp` | 162 | 6 | `TrackerNode::drawOverlay` |
| `Engine/TrackerNodeInteract.cpp` | 110 | 6 | `drawEllipse`, `drawSelectedMarkerKeyframes`, `drawSelectedMarkerTexture`, `convertImageToDisplayOpenGLTexture` |
| `Engine/Nodes/Transform/SplineWarp/SplineWarpInteract.cpp` | 16 | 1 | `drawOverlayExtras`, `addDashes` (plus everything it inherits from `RotoCurveInteract::drawOverlay`) |
| `Gui/HostOverlay.cpp` | 208 | 14 | `PositionInteract::draw`, `RectangleInteract::draw`, `TransformInteract::draw`, `CornerPinInteract::draw`, `RampInteract::draw`, `HostOverlay::draw` |

Three files the milestone did not name hold drawing the ports must move:

- `Engine/TrackerNode.cpp` holds most of the Tracker's drawing (`TrackerNode::drawOverlay`,
  lines 722-1293). `TrackerNodeInteract.cpp` only draws the selected marker's magnifier.
- `Engine/RotoObjectSelect.cpp` (the Roto object-select tool) draws from
  `RotoCurveInteract::drawOverlay` (line 3623).
- `Gui/HostOverlay.cpp` has a fifth host interact, `RampInteract` (two points with dashed lines
  across the output), which the milestone's list of four leaves out.

Pixel-scale reads in pen and key code (`onOverlayPenDown`, `nudgeSelectedTracks`,
`moveSelectedCpsWithKeyArrows` and the like) are not drawing and stay.

## Draw-list additions

These are the additions the constructs below need. Everything not in this list maps onto the
primitives M165 - Rich Viewer Overlays shipped (points, lines, polylines, polygons, strokes,
handles, text).

| Addition | Why |
|---|---|
| `dash` (on/off length in screen pixels) on lines and polylines | `glLineStipple(2, 0xAAAA)` in Roto feather and the Ramp lines; SplineWarp's hand-made dashes |
| arrowheads on a polyline's ends, open or filled, sized in screen pixels | Transform skew/rotate arrows (open); Roto bbox arrows (filled) |
| anchored shapes: cross, circle, arc, box, filled square and a free-form glyph, about a canonical anchor, rotated by an optional canonical direction | crosshairs, rotation bar, skew bars, bbox arrows, brush cursor, tangent ends, Tracker cursor box |
| `layer` ordering | text and handles over geometry when emission order differs from draw order |
| `deformable` flag | geometry deforms through a warp (M435 - Overlays Deform Through Warps); glyphs do not |
| `shadow` flag per primitive | host interacts and Tracker draw a one-pixel black shadow pass; Roto curves and SplineWarp do not |
| `smooth` flag per primitive | square (unsmoothed) points in the host interacts and Tracker, round points in Roto; the aliased ripple-edit border |
| `screenSpace` flag (positions in viewport pixels) | not needed: the rubber band and ripple border are canonical rectangles built from `getViewportRect()`, marked non-deformable |

## Construct map

Legend for the target column: **P** points, **L** lines, **PL** polyline (closed for
`GL_LINE_LOOP`), **PG** polygon, **T** text, **AS** anchored shape, **H** handle. "shadow" means
the primitive is drawn with the shadow pass, "no shadow" without; "nd" means non-deformable.

### `Engine/RotoCurveInteract.cpp`

| Construct | Lines | Target |
|---|---|---|
| GL state: blend over, `GL_LINE_SMOOTH`, line-smooth hint (`GLProtectAttrib`) | 3270-3275, 725-731 | renderer default state |
| `OverlayVertexArrayScope`, `glPushClientAttrib`, VAO/VBO save | 3207-3238, 3276 | renderer (drops) |
| Viewport culling of unselected shapes (`isOutsideView` with GL matrices) | 3167-3188, 3347-3364 | dropped: a cached list must not depend on the pan, so every shape is emitted and the GPU clips |
| Selected stroke: `GL_LINE_STRIP` 1.5 px per sub-stroke, curve colour | 3290-3329 | PL, no shadow |
| Bezier curve: cached `getOverlayPolygons()->curve` via `glDrawArrays` `GL_LINE_LOOP`/`GL_LINE_STRIP` 1.5 px | 3376-3378, 3241-3252 | PL (closed for loops), no shadow, reusing the cached polygon |
| Feather: same, `glLineStipple(2, 0xAAAA)` | 3383-3390 | PL with dash 2 on / 2 off, no shadow |
| Control and feather points: `GL_POINTS` smooth, `kControlPointMidSize * 2` px | 3459-3487, 3533-3537 | P smooth, no shadow |
| Point colours: cyan when the only dragged point, light green when selected, gold otherwise | 3451-3456, 3466-3484, 3512-3518 | P colour per point |
| Track-linked point square: `GL_LINE_LOOP` 1.5 px, `cpWidth * 1.5` px half-size | 3491-3510 | AS box, nd |
| Tangents (`drawSelectedCp`): `GL_POINTS` at tangent ends, `GL_LINE_STRIP` through point | 418-472 | P (tangent ends) + PL, no shadow |
| Feather link extended 20 px beyond the feather point: `GL_LINE_STRIP` | 3520-3562 | PL to the feather point, then an AS glyph (a 20 px line along the link direction) |
| Feather hint line (feather equals point): `GL_LINES` to a point 20 px out | 3563-3600 | AS glyph along the expansion direction |
| Selected points bbox: `GL_LINE_LOOP` 1.5 px, hover orange (drawn from 3629) | 733-748 | PL closed, nd |
| Bbox pivot cross: `GL_LINES`, `kXHairSelectedCpsBox` px, clipped to the box when not moved | 750-784 | AS cross (pivot moved); L clipped (not moved, needs canonical clipping at emit time) |
| Bbox corner and mid points: `GL_POINTS` 5 px | 789-806 | P |
| `drawArrow`: rotated `GL_LINES` + two filled `GL_POLYGON` heads, sizes in px | 630-671, 808-850 | AS glyph (line with filled arrowheads), rotated |
| `drawBendedArrow`: rotated `GL_LINE_STRIP` + two filled heads | 674-715 | AS glyph |
| Sketch preview: `GL_LINE_STRIP` 1.5 px orange to the cursor | 3631-3642 | PL, no shadow |
| Ripple-edit border: aliased `GL_LINE_LOOP` 3 px inset from the viewport | 3647-3672 | PL closed, smooth off, nd, from `getViewportRect()` |
| `drawEllipse`: 40-segment `GL_LINE_LOOP` via `glTranslatef` | 475-502 | AS circle (radius in canonical units), or PL of 40 canonical points |
| `drawCloneSource`: shadowed cross + `GL_LINES` from cursor + ellipse | 581-616 | L + PL with shadow |
| `glCheckError` | 787, 3615, 3643 | dropped |

### `Engine/RotoPaintInteract.cpp`

| Construct | Lines | Target |
|---|---|---|
| Stroke preview: dab triangles from `glDrawArrays(GL_TRIANGLES)` with per-vertex colours | 722-842 | **unmapped**, see below |
| Stroke preview mask in destination alpha: `glColorMask`, `glBlendEquation(GL_MAX)`, `GL_DST_ALPHA` blend, box fills | 795-831 | **unmapped** |
| Brush cursor: shadowed `drawEllipse` at brush radius (canonical) | 845-881 | PL (40 canonical points) with shadow |
| Clone source (`drawCloneSource`) | 882-884 | as in RotoCurveInteract |
| Cursor visibility from `getCursorPosition` and `getViewportRect` | 850-853 | stays in the interact (it reads the viewport, not GL) |

### `Engine/RotoObjectSelect.cpp`

| Construct | Lines | Target |
|---|---|---|
| Traced contours: `GL_LINE_LOOP` 2 px, hole/outer colours | 1220-1236 | PL closed, no shadow |
| Commit points: two passes of `GL_POINTS`, 6 px dark rim then 4 px colour | 1239-1255 | two P primitives |
| Box prompts and drag rubber band: `GL_LINE_LOOP` 1.5 px | 1257-1278 | PL closed (rubber band nd) |
| Click prompts: 10 px rim then 7 px colour `GL_POINTS` | 1281-1298 | two P primitives |
| Status text at the viewport's top left | 1302-1312 | T at a canonical point from `getViewportRect()`, nd |

### `Engine/TrackerNode.cpp` (`TrackerNode::drawOverlay`)

| Construct | Lines | Target |
|---|---|---|
| Unselected marker: point + cross (`CROSS_SIZE` px), drawn black then colour, shifted 1/256 px | 790-829 | P square + AS cross, no shadow, black copy under the colour copy |
| Selected marker shadow pass via `GL_PROJECTION` one-pixel shift | 833-837, 993-999 | shadow flag |
| Centre trajectory: `GL_LINE_STRIP` 1.5 px grey | 1001-1013 | PL with shadow |
| Keyframe centres: smooth `GL_POINTS`, error colour per point | 1015-1046 | P smooth with shadow, colour per point |
| Pattern and search boxes: `GL_LINE_LOOP` 1.5 px | 1048-1064 | PL closed with shadow (deformable: they follow the image) |
| Centre, offset and box handle points: `GL_POINTS` square, green when hovered | 1066-1154 | P square with shadow, colour per point |
| Offset line: `GL_LINES` half colour | 1156-1163 | L with shadow |
| Mid-edge handle ticks: `GL_LINES` from mid point to `computeMidPointExtent` (`HANDLE_SIZE` px) | 1165-1234 | L (emitted canonical, extent already from the pixel scale) or AS glyph along the edge normal |
| Marker name: `renderText` at the centre, both passes | 1236-1238 | T with shadow |
| Add-track cursor: box and cross at the pen, alpha 0.8 | 1248-1290 | AS box + AS cross, nd |

### `Engine/TrackerNodeInteract.cpp`

| Construct | Lines | Target |
|---|---|---|
| Keyframe thumbnails and magnifier: `GL_TEXTURE_2D` quads from PBO-uploaded textures | 745-756, 931-942, 1178-1282 | **unmapped**, see below |
| Thumbnail labels and frame numbers: `renderText` | 759-764, 967 | T |
| Thumbnail contour: `GL_LINE_LOOP`, colour by keyframe time | 766-816, 944-956 | PL closed, nd (placed in screen-sized slots) |
| Inner marker in the magnifier: `GL_LINE_LOOP` + point, 1/256 px shadow | 821-843, 969-991 | PL + P, no shadow, black copy first |
| Magnifier corner point | 958-962 | P |
| Scale ellipse (`drawEllipse`, 40-segment loop) | 576-602, 993-1008 | PL of 40 canonical points |

### `Engine/Nodes/Transform/SplineWarp/SplineWarpInteract.cpp`

| Construct | Lines | Target |
|---|---|---|
| Curves, feather, points and tangents | inherited | as RotoCurveInteract |
| Pair lines: `addDashes` in canonical units (4 px at the current zoom), `GL_LINES` 1 px | 1112-1128, 1201-1210 | L with dash 4 on / 4 off, no shadow, deformable |
| Correspondence lines: dashed `GL_LINES` 1.5 or 2 px | 1212-1225 | L with dash 4/4 |
| Correspondence ends: smooth `GL_POINTS` 7 or 9 px | 1226-1233 | P smooth |

### `Gui/HostOverlay.cpp`

All host interacts draw everything twice, a black pass shifted one pixel down-right and the colour
pass (`glTranslated` on `GL_PROJECTION` by `shadow`), with blend over, line smoothing and square
points. That is the draw list's existing shadow pass.

| Construct | Lines | Target |
|---|---|---|
| `HostOverlay::draw`: projection read for the shadow offset, overlay colour, font | 2775-2800 | renderer shadow pass; node colour from `OverlayDrawState` |
| Position: square point, label above it (`fm.height()` px up) | 1481-1546 | P square + T offset in px |
| Rectangle: `GL_LINE_LOOP`, nine handle points coloured by hover, centre cross | 1617-1711 | PL closed + P per handle + AS cross |
| Transform: `drawRotationBar` (bar, bent arrow, inverted cross arrows), rotated by `rotate` | 2385-2498, 2596-2597 | AS glyphs anchored at the target centre, direction from `rotate` |
| Transform: `drawEllipse` through the skew matrix (`glMultMatrixd`) | 2301-2328, 2598-2601 | PL of 40 points mapped through rotate and skew on the CPU |
| Transform: `drawSkewBar` (bar + open double arrow), rotated | 2330-2383, 2603-2626 | AS glyphs, direction from rotate, skew and flip |
| Transform: `drawSquare` filled `GL_POLYGON` handles in mean pixel scale | 2268-2299, 2629-2633 | AS filled square, mapped through rotate and skew for the anchor only |
| Corner pin: from-to `GL_LINES` half colour, `GL_LINE_LOOP`, corner points, corner names | 2641-2771 | L + PL closed (deformable) + P square + T |
| Ramp: two square points, stippled `GL_LINES` across the output, labels | 1099-1177 | P square + L dash 2/2 + T |
| `glPushAttrib` / `glPopAttrib` (commented out: the caller protects the state) | 1526, 2570, 2637, 2709, 2771 | renderer |

## Unmapped constructs

Reported before any port, as the milestone's assumption asks:

1. **RotoPaint stroke preview** (`RotoPaintInteract::drawStrokePreviews`). It is a paint preview, not
   overlay chrome: per-vertex coloured dab triangles composited through destination alpha with
   `GL_MAX` or build-up blending, so that overlapping dabs combine as the renderer combines them.
   No draw-list primitive blends this way. Options: keep it as raw GL drawn by the interact after
   the list (it never deforms: the stroke is drawn where the pen is), or add an "image-like" primitive
   with its own blend modes. Keeping it raw GL is the smaller change and leaves the look unchanged.
2. **Tracker magnifier and keyframe thumbnails** (`TrackerNodeInteract::drawSelectedMarkerTexture`,
   `drawSelectedMarkerKeyframes`). Textured quads from textures the interact uploads through a pixel
   buffer. The draw list has no textured primitive, and M165's overlay images are render-produced
   coverage planes, not arbitrary textures. Options: a textured-quad primitive referencing a texture
   the node owns (the list stays GL-free by holding an id the viewer resolves), or keep the quads raw
   GL after the list. The thumbnails sit in screen-sized slots and never deform.
3. **Roto viewport culling**. Not a GL construct, but it relies on the GL modelview and projection
   matrices, and a list cached across redraws cannot depend on the pan. The port emits every shape.
   With hundreds of shapes the vertices still come from `getOverlayPolygons()`, which is cached.

Everything else maps onto the existing primitives plus the additions above.

## Rules for the ports

- Emit canonical geometry for anything that sits on the image (curves, feather, boxes, corner-pin
  edges); emit anchored shapes for glyphs whose size is in screen pixels (crosses, arrows, bars,
  handle squares). Mark geometry built from the viewport rectangle non-deformable.
- The interact keeps its hover and drag colouring: it emits points (not handles) in the colour it
  draws today, so the list's own hover highlight stays off. Handles are only for nodes whose pen
  logic lives in `onOverlayHandlePressed`.
- Lists are cached between redraws by content version, time, view and a zoom bucket
  (`EffectInstance::getOverlayContentVersion`). Anything else that changes the drawing (selection,
  hover, drag state, tool) must bump the node's version.
