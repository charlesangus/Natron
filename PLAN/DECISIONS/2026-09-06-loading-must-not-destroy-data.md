# Loading a project must never destroy information

2026-09-06. Nothing about opening a project may discard data the user cannot get
back. Where a project cannot be fully realised — a plugin is unavailable, an edge
carries kinds a node cannot handle — the graph is preserved as saved and the
problem is surfaced on the affected node as an error state. The alternatives,
dropping the edge or dropping the node, silently rewrite the user's comp and are
worse than showing them something that will not run.

Two regimes follow. At **connection time**, an invalid connection is rejected
outright: the user is actively making it, so refusing is correct and no invalid
edge need exist. For **later invalidation** — a project load, a resolution change
propagating from elsewhere in the graph, a knob-driven policy change — the edge
stays and the downstream node errors.

The codebase already had the right precedent and was not following it
consistently: a project naming an OCIO colorspace the active config lacks loads
with that node errored rather than altered, while a missing plugin dropped the
node outright and a kind-invalid edge was disconnected on load.

This reverses M17.P1.T4's original drop-the-edge policy (M17's `## Decisions`) and
motivates M22, which makes a missing plugin non-destructive by preserving the node
as a placeholder carrying its serialization verbatim.
