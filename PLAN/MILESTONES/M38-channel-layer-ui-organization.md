# Milestone 38: Improve channel/layer information organization in the node UI

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

How nodes organize and display channel/layer information is awkward today.
Nuke's approach is a reference point, not a model to copy outright. Redesign
the layout for clarity.

Blocked on: not yet prioritized — captured as backlog (user request
2026-09-18); benefits from M39's terminology decision (layers vs. planes)
landing first.

Acceptance sketch:
- Channel/layer selection UI is redesigned and demonstrably clearer than
  the current layout.

## Decisions

- 2026-09-19 — **Rescoped (user decision): M38 is a new layer/channel selection widget, absorbing M35.** Nuke-style but more powerful: dropdown None / All / Regex / list of layers, dynamic channel buttons matching the chosen layer's actual channels, "Add layer" adds another instance (instances beyond the first have a remove button). Nodes process exactly the selected layers/channels in place; non-Shuffle nodes get no shuffle capability. Not the earlier "reorder the existing knobs" proposal.
- 2026-09-19 — **Clean break for old projects** (user decision): no load-time mapping of `processAllLayers` / `channels` / R,G,B,A values; old nodes come up on the default selection. `NATRON_CACHE_VERSION` bumps.
- 2026-09-19 — **Plugin-declared R/G/B/A rows are adopted into the widget, Merge included** (user decision): the 28 openfx-misc plugins' `NatronOfxParamProcess*` rows are hidden and driven by the widget; Merge uses a widget *variant* with no add-rows and no regex. Goal is one consistent channel-selection UI across essentially all nodes with small variations for specific purposes — treated as a foundational design, written up in `PLAN/DESIGN/2026-09-19-layer-channel-widget.md` for approval before implementation.
- 2026-09-19 — **Regex matches layer labels, whole-string anchored, case-sensitive, `QRegularExpression`** (user decision): users type what they see.
- 2026-09-19 — **ZRemap and ZMask PyPlugs are dropped until M34** (user decision): they depend on the implicit shuffle; the native Shuffle milestone re-adds them.
- 2026-09-19 — **One undo step per user action**; the regex editor commits on `editingFinished`, not per keystroke.
- 2026-09-19 — **Design review round 1 (user)**: (a) prefer *three knob types* by value shape — a channel **set** (multi-row, None/All on row 0 only, regex), a single **layer** select (optional channel buttons), a single **channel** select — over one knob with variant flags; the shared part is the GUI row and the population source, not the knob. (b) **Merge is out of M38**: it becomes a native node in its own milestone (A/B/output layer+channel selects, an "also merge" channel set, an alpha rule); the OFX Merge is left alone and may break. (c) **M43 (drop premult) and M36 ("New layer…") fold into M38** as phases. (d) Read gets *no* channel mechanics (its `outputComponents` param goes); Write gets the full channel set; Tracker selects exactly one layer; Roto/RotoPaint select exactly one layer with channels. (e) Open questions answered: full channel set even for Transform/Switch-style nodes; reset channels to all-on when the layer changes; Roto in M38. (f) Deep layers/channels are a hole (no layer grouping, processing nodes hardcode RGBA) → new milestone **M60** after M38, not a phase here.
- 2026-09-19 — **Blocking design gap: script-level layer knowledge.** Natron has no project-wide layer registry — user-created layers live on the node that created them and only appear downstream, so a Roto/generator/Shuffle cannot target a layer that is not yet upstream, and "New layer…" cannot mean what it means in Nuke. The user's call: a global layer cannot live on a node; design the registry first, then return to the widget's open questions.
