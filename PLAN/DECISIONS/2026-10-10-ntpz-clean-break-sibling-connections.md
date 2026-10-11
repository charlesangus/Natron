# 2026-10-10 — `.ntpz` bundle, clean break, sibling-relative connections

**Decision (user, 2026-10-10).** The `.ntp` successor saves as **`.ntpz`**. It is a **clean break**: Natron
does not import old XML `.ntp`/`.nps`/`.nl` files at all (opening one gives a clear
old-format message), and it never writes the XML format again. Connections name their
upstream node by **sibling-relative script name**, resolved within the consumer's
enclosing group; group membership is stored by nesting (a group's `children:` map).
Expressions and links stay fully qualified. Presets keep `.nps`, as plain YAML.

**Rationale.** The fork already ruled out upstream project compatibility
(`docs/decisions/2026-09-18-clean-break-no-project-compat.md`), so an import bridge would
only serve the fork's own short-lived files. Natron connections never cross a group
boundary, and fully-qualified targets would rewrite every inner connection when a group
is renamed, which defeats diff locality. This **amends**
`2026-09-05-project-format-bundle-design.md` on connection naming and drops its import
bridge.

**Sequencing.** The engine-decoupling refactor is split out as M93 - Serialization
Boundary, which runs first (no format change, verified byte for byte against
golden XML) and rebases onto M76 - Native Write when it merges. M92 - Linked Nodes builds on
M93's plain structs.
