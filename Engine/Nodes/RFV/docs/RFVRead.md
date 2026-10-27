# RFVRead

Tolerance: none recorded.

RFVRead passes its input through unchanged, whatever its data kind, and lists the Reverse-Flow Variables that reach it. It reads no variable itself, so it never splits a render or a cache entry.

## Parameters

- **Variables** (read-only, selectable for copy): a header line `from <output node>, frame <t>`, then one `name = value` line per variable, sorted by name. Values are written as Python's `repr()` writes them: strings quoted (`'sh010'`), lists as `[1, 2, 3]`, bools as `True`/`False`, and floats with a decimal point (`2.0`). Unset variables are not listed. A variable whose expression fails is listed after the others with its error.

## Which values are listed

The list is what `rfv` would give an expression of this node at the timeline's frame, worked out without rendering:

- The path is the one from this node down to the active viewer's displayed inputs: input A, and input B while a wipe or other compositing operator is on. The active viewer is the one last clicked or typed in.
- The Set nodes on that path are folded with the same function a render uses, so the nearest Set below wins.
- An animated or expression-driven variable is evaluated at the timeline's frame.
- Off every path to the active viewer, or with no viewer, the header reads `from the project` and the project's variables are listed.

Time remapping along the path (TimeOffset, FrameHold) is not followed: every node is asked at the timeline's frame. Only the first view is shown.

## When it refreshes

Without a render, whenever the timeline moves (while the panel is open), the active viewer or its displayed inputs change, a connection changes, or a variable of a Set node or the project is edited, added or removed. Opening the panel refreshes it too.
