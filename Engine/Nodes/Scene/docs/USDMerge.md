# USDMerge

Unites the scenes of its inputs, numbered 0 to 63, by stacking their layers into
one scene. It authors nothing itself and does not touch colour.

The Opinion Order setting chooses which inputs win where their opinions
conflict (the same attribute of the same prim, say):

- Higher Inputs Win (the default): inputs stack in input order, so a
  higher-numbered input's layers are stronger than a lower-numbered input's.
- Lower Inputs Win: the order is reversed, so input 0 is the strongest.

Opinions only one input authors come through as they are under both orders. The
order also decides everything else that follows strength: stage metadata (up
axis, meters per unit, default prim, time codes per second) comes from the
strongest layer that authors it, the entries of prepend and append lists (API
schemas, references) are ordered by it, and a transform in Append mode below the
merge orders its ops by it. Under Lower Inputs Win, where several inputs author
the same point of a mesh, input 0's value is the one that survives.

Lower Inputs Win suits bringing a new branch back into the scene it was taken
from: the existing prims' transforms beat the branch's, while the branch's new
prims still come through.

A layer that reaches the node through more than one input, as when one source
feeds two branches that meet at the merge, is used once under both orders. It
keeps its weakest position, under every layer authored over it in any branch, so
each branch's edits of the source still apply.

Unconnected inputs are skipped. With one input connected, or the same scene on
every input, the output is that input's scene unchanged; with none, the output is
an empty scene.

This is the layer-stack union only: modes that flatten the inputs or copy prims
from one input into another are not offered.

Tolerance: none recorded.
