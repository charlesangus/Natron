# Node body colour carries the category; a user colour is carried by a border

Node body colour carries the node's category; a user-chosen colour is carried by
a thick border instead of replacing the body. Today there is only one colour
channel, so the moment a user recolours a node the category signal is destroyed.
Two channels keep both.

Data kind is signalled by node silhouette shape and edge colour only — the edge
pen-width ladder introduced in M17 is removed as not noticeable in practice. This
supersedes M17's "width is the primary channel, colour is reinforcement only"
rationale documented in `Gui/Edge.cpp`. Okabe-Ito edge colour remains sound as
the sole edge-level channel because it is distinguishable under all common
colour-vision deficiencies, and node shape carries the same information
redundantly.

Decided by the user 2026-09-07 during `/cat-plan`. Implemented by M24.
