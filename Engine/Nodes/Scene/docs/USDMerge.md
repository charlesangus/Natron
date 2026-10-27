# USDMerge

Unites the scenes of its inputs, numbered 0 to 63, by stacking their layers into
one scene. It authors nothing itself and does not touch colour.

Inputs stack in input order: a higher-numbered input's layers are stronger than a
lower-numbered input's, so where two inputs author the same opinion (the same
attribute of the same prim, say) the higher-numbered input wins, as in Houdini's
Merge LOP. Opinions only one input authors come through as they are. Stage
metadata (up axis, meters per unit, default prim, time codes per second) comes
from the strongest layer that authors it.

A layer that reaches the node through more than one input, as when one source
feeds two branches that meet at the merge, is used once. It keeps its weakest
position, under every layer authored over it in any branch, so each branch's
edits of the source still apply.

Unconnected inputs are skipped. With one input connected, or the same scene on
every input, the output is that input's scene unchanged; with none, the output is
an empty scene.

This is the layer-stack union only: modes that flatten the inputs or copy prims
from one input into another are not offered.

Tolerance: none recorded.
