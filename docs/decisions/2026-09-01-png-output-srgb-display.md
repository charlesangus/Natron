# 8-bit PNG output is labelled `sRGB - Display`, and that is not a colour change

WritePNG's output colorspace resolves to `sRGB - Display` rather than
`sRGB Encoded Rec.709 (sRGB)` (aliased `sRGB - Texture`).

**This changes no pixels.** Both produce 118/255 for scene-linear 0.18, and the
two are numerically identical at every level in both directions — the difference
is metadata (`REFERENCE_SPACE_DISPLAY` vs `REFERENCE_SPACE_SCENE`), not maths.
Naming a display-referred colorspace does **not** invoke the ACES output
transform, as was assumed when the question was posed: crossing the
scene→display boundary uses the config's default view transform, and in every
ACES built-in that is `Un-tone-mapped`.

`sRGB - Display` is chosen because it is the honest label for a deliverable, and
because it sits at colorspace index 0 and collides with no role, so
`canonicalizeColorSpace()` leaves it alone — which incidentally avoids a wart
where the resolved name was the role `color_picking`, correct in colour but
strange in the UI and in serialized project files.

Its one cost is a hidden coupling: a config that set a tone-mapped
`default_view_transform` would silently change output, where the scene-referred
spelling could not. Accepted, since this fork pins its config.

Note the shared helper `colorSpaceName()` serves both readers and writers and
cannot tell them apart, so this also sets ReadPNG's input default. Numerically a
no-op in the read direction too. Making reader and writer differ would need a
different edit, not a one-word one.

**The genuinely ACES-correct deliverable is a separate, unresolved question.**
Baking in the display rendering — `ACES 2.0 - SDR 100 nits (Rec.709)` — gives
89/255 at mid-grey, a −29 code-value shift, and rolls highlights off to 248.6
where the plain encode runs to 840 and clips hard. That is the colour-correct
answer for an 8-bit deliverable out of a compositor, it is user-visible, and it
cannot be reached by naming a colorspace — it needs an OCIODisplay node or a
view-transform-aware writer. It must not ride along inside a bug fix.
