# USDExpandHierarchy

Sets how the viewers showing this node, or a node below it, draw the prims its
rules name. The scene passes through unchanged. The node authors no layer, so
renders, written files and the scene's prims and attributes never see the rules,
and changing a rule changes only how viewers draw.

Each rule is one row:

- Pattern names the prims the rule sets: a USD path expression such as
  `/World/Box`, `/World/*`, `//Lamp` or `"/World/A /World/B"`. A blank pattern
  sets no rule.
- Draw is what the matches draw as:
  - BBox draws one box around everything below the prim, whose payloads stay
    unloaded.
  - Children draws each child by its own draw, and children no rule names as
    boxes.
  - Full (the default) draws everything below the prim, as far as its payloads
    are loaded. Nothing is loaded.
  - Full + Load draws everything below the prim and has the viewers load every
    payload below it.
  - Hidden draws nothing, not even a box.
- Levels opens that many levels below each match before Draw applies. 0 sets
  Draw on the matches themselves, 1 on their children, and so on. The levels
  above draw by their children. An unloaded payload on the way takes Draw where
  it is.

Rule Count sets how many rows the node shows, up to 1024. Rows past it are kept
but set nothing.

A later row wins over an earlier one on the prims both name, and this node wins
over the nodes above it, a USDRead's Initial Draw among them. A draw state set
in a viewer's Scene Graph panel wins over every rule. Rows the rules decide name
the node as their origin.

Each viewer matches the patterns against the prims it has loaded, and matches
them again after each load. A pattern can therefore name prims inside a payload
that is not loaded yet. A Full + Load rule loads the payloads below its matches,
then matches again, and loads what the new matches add. It stops when a match
adds nothing new to load, and closing the viewer or showing another node
cancels it. A pattern that matches nothing does nothing and shows no error. A
pattern that does not parse sets no rule and puts a warning on the node, and the
other rows still apply.

Removing the node, or showing a node above it, puts the prims back to the draw
the nodes above it set. Payloads it had loaded stay loaded until they are
unloaded from the Scene Graph panel.

Tolerance: none recorded.
