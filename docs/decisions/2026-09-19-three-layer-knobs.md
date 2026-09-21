# Three knob types for layer and channel selection, not one knob with flags

## Context

Every node that lets a user pick which layer(s) and channels it reads or
writes needs one of three value shapes: a filter needs a set of rows, each a
layer (or a regex over layer names, or None/All) with its own enabled
channels; a node that reads or writes exactly one layer (a generator, a
tracker, Roto) needs a single layer plus optional per-channel buttons; a
mask or side-channel input needs a single `layer.channel` pair. An earlier
design tried to cover all three shapes with one knob type and a set of mode
flags; that was rejected because the flags made the common case (a plain
per-channel filter) carry state it never used, and because the meaning of a
row changed depending on flags a reader couldn't see at the call site.

Separately, the previous per-node UI accreted piecemeal: `KnobChoice` layer
selectors backed by an OFX multiplane suite, `KnobBool` quads for
`NatronOfxParamProcessR/G/B/A`, and node-specific mask-channel choices, each
with its own persistence format and its own notion of "channel this plugin
doesn't have right now." None could represent "the user picked a channel
that used to exist and no longer does" without silently snapping to a
different value.

## Decision

**Three knob types, chosen by the shape of the value, not by a flag.**
`KnobChannelSet` (multiple rows: a layer or a regex per row, each with its
own enabled channels; row 0 also allows None/All), `KnobLayerSelect` (one
layer, optional per-channel buttons), `KnobChannelSelect` (one
`layer.channel`, or none). All three derive from `KnobTable`, sharing a
single-string-per-dimension value, no animation, and one `<Tag>cell</Tag>`
codec instead of each inventing its own serialization. They are deliberately
not `KnobChoice`: a choice re-derives its persisted value from whatever
entries are currently in its list, but a selected layer or channel that is
no longer available has to persist as an ID independent of the list that's
showing. Column tags are untranslated ASCII literals, so the on-disk value
doesn't depend on the UI locale it was saved under. A GUI-only marker —
`(not in input)` on an input-bound knob, `(not in project)` on a target
knob — decorates whichever combo entry represents the current, unavailable
value; the list itself never carries greyed-out or placeholder entries.

**Listing is a service the node provides, keyed on the knob's role**, not
something the knob looks up itself. An input-bound knob (an ordinary
filter's channel set, a mask channel select, Tracker's layer select) lists
the layers actually present on its input, with Color always included since
every image stream carries one. A target knob (a generator, Roto/RotoPaint)
lists the project-level layer registry — the single place layers are
declared, independent of any one node's current input — appended with a
"New layer…" entry that creates a registry entry and reads back the new
selection.

**Host masking is uniform**: any node with a channel set or a layer select
with buttons has its unprocessed channels masked by the host, per output
plane, from that knob. Plugins declaring the standard
`NatronOfxParamProcessR/G/B/A` quad have those four parameters adopted —
hidden, non-persistent, forced true — so the host's masking is what actually
happens; safe because that quad's documented job is exactly "mask this
channel of the output." Three plugins are excluded because their quad
answers a different question, not a per-channel mask: KeyMix (source A vs.
source B per channel), DenoiseSharpen (a "process chroma" flag folded from
R/G/B), and ClipTest (ORs the selected channels into one zebra-stripe
decision); forcing those true and masking around them would silently change
what they compute, so they keep their own quad.

**Every node processes in place; nothing moves data between layers or
channels except by explicit action.** A channel a knob does not select is
copied unchanged from the preferred input's same plane; a plane not
selected at all passes through untouched — the one meaning a channel row
has everywhere the three knobs appear. Read keeps its own file-driven
layer/component guess, host-side hidden rather than replaced, since naming
what's in the file is a reader's job; a file with no R/G/B/A channels
duplicates its first layer into Color so the stream always has one, while
keeping that layer as its own plane too. Write's container gets an
input-bound channel set that drives the embedded encoder through a host
filter on its available-planes list, with the encoder's own "process all
layers" and channel/plane parameters forced on and hidden; Color subsets
round up to the nearest of RGBA/RGB/Alpha, non-Color subsets are exact.
Tracker gets an input-bound layer select without buttons, replacing three
enable bools with one layer plus its full channel set. Roto, RotoPaint and
every generator get a target layer select with buttons: the result is
written into exactly the selected layer, every other layer passing through
untouched.

**The premultiplied/unpremultiplied concept is removed from the host**,
ahead of and independent of the three knob types. The OFX premultiplication
property is still answered, since third-party plugins depend on it, but as
a constant rather than something tracked or derived: every clip and image
answers "unpremultiplied," the do-nothing default most compositing hosts
already use. Premult and Unpremult keep doing real, unconditional
arithmetic regardless of what the now-constant property says, instead of
short-circuiting to identity because the property claims the work is
already done. Readers and writers never convert between premultiplied and
unpremultiplied; that responsibility belongs entirely to those two nodes,
placed explicitly by the user who needs them.

**Old projects are not migrated.** Renamed or restructured knobs, and the
premult removal's change to what's cached on disk, get a cache version bump
and nothing else — no load-time translation from the previous
choice-and-bool representation. `ZRemap` and `ZMask`, which existed to work
around the lack of a native multi-plane shuffle, are dropped rather than
ported, and stay dropped until a native Shuffle exists to replace what they
did. A native Merge that understands the three knob types is out of scope
here; the existing OFX Merge keeps its own, differently-shaped channel
parameters untouched.

**Expressions are refused on all three knob types.** A table-valued
selection is not a value an expression graph can meaningfully drive, so
`KnobI` grows an explicit `supportsExpressions()` query, true everywhere
except `KnobTable`, checked before an expression is attached; the GUI's
"Set expression…" entry disappears for these knobs. Linking and PyPlug
aliasing are unaffected.

## Consequences

A plugin author relying on the standard channel-process quad for anything
other than a per-channel mask has to use different parameter names, or their
plugin's behavior changes silently under this host — documented at the
quad's declaration, since no OFX property exists to negotiate opt-out.
Third-party PyPlugs that scripted the old choice/bool knobs, or implemented
their own layer bookkeeping now superseded by the registry, no longer load
correctly. Every node that reads or writes more than one plane now agrees on
what "channel selection" means, so a shared GUI row, a shared serialization
codec, and a shared masking implementation cover all of them instead of each
node inventing its own.
