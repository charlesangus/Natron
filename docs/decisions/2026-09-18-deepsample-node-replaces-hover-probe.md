User reviewed M18.P2.T3's hover/tooltip deep-sample probe after the milestone's
manual GUI checklist passed and judged the design wrong: a tooltip that only
appears on hover-pause and vanishes the instant the mouse leaves the image is
hard to discover and impossible to park on one pixel while scrubbing frames.
Chose to mimic Nuke's model instead — a `DeepSample` node the user places and
leaves connected, with a position picker and a settings-panel table of raw
per-sample values — and to replace the hover probe outright rather than keep
both.

Filed as a new milestone (M32) rather than reopened inside M18: M18's gate
already passed against its original spec, and this is follow-up UX work, not
a defect in what M18 shipped. M32 is a stub, not elaborated yet — the picker
handle and the panel table each need a codebase-scouting pass (existing
knob/overlay conventions; any precedent for a custom panel widget) that goes
beyond what a discussion session should plan blind.
