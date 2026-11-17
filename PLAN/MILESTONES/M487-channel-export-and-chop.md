# M487 - Channel Export And chop()

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Channels drive knobs. An Export node, or an export flag on any channel node, maps channels to knobs by path with globbing, as Houdini's Export CHOP does. An active export overrides the target knob's value at every time. The knob's keys and expression are kept but inactive, and the knob draws in a distinct "driven" colour with a jump-to-network action. Expressions gain `chop("<network>/<node>/<channel>")`, in Python and, once it exists, in M265 - Native Expression Evaluator. Exported values feed the knob's hash, so renders and caches see the driven value.

Blocked on: M486 - Channel Network Core.

Acceptance sketch:
- Exporting a Noise channel to a Transform's `translate.x` animates it on render, Python reads and viewer overlays, and turning the export off restores the original keys untouched.
- `chop("net1/wave1/chan1")` in an expression matches the exported value at sub-frames.
- Save and load keep the exports. Deleting the network un-drives its knobs and warns once.

## Scope

**In:**
- Export overrides, the driven-knob state in the GUI, the curve editor and the Dope Sheet, `chop()`, hashing, and serialisation.

**Out:**
- Roto control points as export targets: M489 - Channel Sources From Shapes, Tracks And Cameras.

**Edge cases:**
- Two exports on one knob: the later in board order of the network's nodes wins, with a warning on both.
- An export to a knob with an expression that reads the network: a cycle, refused.
- User edits on a driven knob: refused, with a message pointing to the network.

**Assumptions:**
- Unconfirmed: `chop()` evaluates through the cook cache with no Python on render threads once M265 - Native Expression Evaluator lands; until then it takes the Python path.
