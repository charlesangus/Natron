# RFVRead

Decided 2026-10-08 with the user, extending `2026-10-08-reverse-flow-variables-design.md`.

RFVRead is a pass-through that lists every variable, with its value, as seen at its position in the graph. "At that point" means the context reaching the node from downstream. The node resolves it statically, along the path to the active viewer's displayed input chain at the timeline frame (the project's base values if it is on no viewer path), using the same overlay function as the render request pass. It does not observe real renders: a render-time side channel would show whichever path rendered last and would need cross-thread publishing. The cost is that it shows the viewer's path only, not a Write's. It reads nothing, so it never widens an upstream context key.
