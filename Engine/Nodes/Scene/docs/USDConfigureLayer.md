# USDConfigureLayer

Names the file a group of layers is written to, and sets that file's layer
metadata. A USDWrite downstream writes each such group, a region, to its own
file and sublayers it from the file of the region around it, or from its own
output. The node authors nothing and does not change the scene.

**Applies To** chooses the layers:

- **Preceding Layers** gathers the layers the nodes before it added, back to the
  last boundary: a file a USDRead reads, a USDLayerBreak, or a layer already in
  another region. Across a USDMerge it reaches into every input, each up to its
  own boundary, so one file can hold edits from several branches. It stops at a
  read file because writing layers from both sides of it into one file would
  change their strength against it.
- **Following Layers** opens a region: the layers the nodes after it add go into
  it, until a USDLayerEnd, a USDMerge, a USDLayerBreak or the USDWrite. A region
  opened inside another nests in it and is written as its sublayer.

Inside a region a USDLayerBegin opened, the node configures that region rather
than opening a new one: in Following Layers mode when nothing has been added to
the region yet, in Preceding Layers mode when it would gather every layer of the
region.

**Save Path** is the file. A relative path is relative to the USDWrite's output
directory, and the USDWrite's path variables apply. Left blank, the region is
written into the file of the region around it, and its metadata applies to that
file.

The metadata controls each have a toggle; only those turned on are written:
Default Prim, Comment, Start Time, End Time, Time Codes Per Second, Frames Per
Second, Up Axis (Y or Z), Meters Per Unit, Color Configuration, Color Management
System and Render Settings Prim Path. Editing a control turns its toggle on.

**Custom Data** holds rows of Name, Type and Value, written to the file's custom
data. The types are String, String Array, Token, Token Array, Float, Integer and
Boolean. Array items are separated by spaces; a String Array item holding spaces
is quoted. Float is written as a double. A row that
does not parse, or a second row with the same name, is a node error naming the
row.

Tolerance: none recorded.
