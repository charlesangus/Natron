# Clean break: no compatibility with pre-fork Natron projects or PyPlugs

Renames of serialized identifiers (knob script-names, `.ntp` XML tags, Python
API method names) need no compatibility shims, deprecated aliases, or
load-time renames. This fork is a clean break from previous Natron work;
projects and PyPlugs saved by upstream Natron are not a supported input.

Rationale (user decision, 2026-09-18, during M39 scoping): upstream Natron is
largely defunct and its project corpus is not worth carrying migration code
for. Later milestones (M34–M38, M43, M16) may rely on this — a serialization
change needs only a version bump so stale disk caches are wiped, not a
migration path. The OpenFX boundary is the exception: OFX property names,
suite entry points and plugin-facing choice IDs (`uk.co.thefoundry.OfxImagePlane*`)
are an ABI shared with third-party plugins and are never renamed.
