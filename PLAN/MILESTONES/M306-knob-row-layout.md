# M306 - Knob Row Layout

Follow-up from the user's 2026-11-02 UAT of M229 - 3D Viewer Guides And Display: in properties panels, single-value number knobs (USDLight's Intensity, Exposure, Width, Height, Uniform Scale) put their field far to the right, lined up with the second column of the three-value knobs, instead of next to their label like Prim Path, Type and Color. It "is happening everywhere across milestone files, maybe it was always like that, but it needs to be fixed" (user). Screenshot: `.local/knob_layout.png`.

M229 (7e55a1ca1) packed number knobs without a display range to the left; knobs with a display range (and so a slider) still spread out. Find the layout rule in `Gui/KnobGuiValue*`/`Gui/KnobGuiContainerHelper*` and make every single-value number knob sit straight after its label, in every panel, at 1920x1080.

Deps: M229.

## Scope

**In:** single- and multi-value number knobs' row layout in all properties panels; a GUI check that measures the field's x position against the label for a sample of nodes.

**Out:** slider redesign.

Stub: to be broken down into tasks when claimed.
