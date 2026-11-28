# Diffing and merging Natron projects

`natron-merge` compares and merges project bundles (`.ntpz`) as node graphs rather than as
bytes. It needs nothing but itself: no Qt, Python or Natron install, so it runs on a build
farm, in CI or in a git hook.

## Commands

```
natron-merge diff [--yaml] <before> <after>
natron-merge merge [--scope template:<key>|all] <base> <mine> <theirs> -o <out>
natron-merge git-driver <base> <current> <other>
natron-merge textconv <file>
```

### diff

Prints what changed from `before` to `after`, one section per node:

```
~ KeyGrade (renamed from Grade1)
  multiply: default → 1.5
  input Source: Read1 → Blur2
+ Blur2 (Blur)
  input Source: KeyGrade
- Crop1 (Crop)
```

`+` is a node that only `after` has, `-` one that only `before` has, and `~` one both have
that changed; changes to the project's own settings come last, under `~ Project`. A project file
leaves out knobs at their defaults, so a knob one side leaves at its default shows as `default`. Nodes inside groups are shown by their dotted path (`Look.Blur1`). Only the
graph is compared: the save date, the Natron version that saved the file and the viewer and
timeline state are not. Re-saving an unchanged project therefore gives an empty diff.

`--yaml` prints the same changes as graph operations, the form scripts and the merge engine
use.

### merge

Merges two edits of a common ancestor and writes the result to `out`:

- `--scope all` (the default) merges two versions of the same project, for example two
  branches of a shot. Nodes are matched by their IDs, which survive renames, moves and
  re-saves.
- `--scope template:<key>` updates a template imported into `mine` as `<key>`: `base` is the
  template as it was imported, `theirs` its new version, and only the nodes that came from the
  template, plus anything the artist put between them, are merged. Template nodes the artist
  deleted and recreated are matched by their place in the graph. `mine` must have an import
  with that key, else the merge is an error. The import then records `theirs` as its base, with
  its path, content hash and the time of the merge.

Where both sides changed the same thing differently, the result keeps **mine** and records a
conflict. The conflicts are saved in the bundle (`merge/conflicts.yaml`), so a conflicted
project still opens normally, with the conflicts listed for resolving later. A later merge keeps
the conflicts saved earlier, by `mine` and, with `--scope all`, by `theirs`, while the node each
names is still in the result. A summary of the new conflicts is printed on stderr.

### git-driver and textconv

`git-driver` is a git merge driver: it merges `%O` (the ancestor), `%A` (the current branch)
and `%B` (the other branch) with scope `all`, writes the result into `%A` and reports a
conflict to git. `textconv` prints the project as YAML with nodes and knobs sorted by name,
so `git diff` and `git log -p` show readable changes.

### Exit status

| Status | `diff`               | `merge`, `git-driver`      |
|--------|----------------------|----------------------------|
| 0      | no differences       | clean merge                |
| 1      | the projects differ  | merged, conflicts recorded |
| 2      | error                | error, nothing written     |

## Setting up git

Tell git which files are Natron projects, in the repository's `.gitattributes`:

```
*.ntpz -text merge=natron diff=natron
```

Then register the driver, once per clone (or in `~/.gitconfig`):

```
git config merge.natron.name "Natron project merge"
git config merge.natron.driver "natron-merge git-driver %O %A %B"
git config diff.natron.textconv "natron-merge textconv"
```

`git merge`, `git rebase` and `git cherry-pick` now merge projects node by node. A merge with
conflicts stops like any other conflicted merge: open the project in Natron, resolve, save,
then `git add` it and continue. `git diff` shows project changes as YAML; for the per-node
summary, run `natron-merge diff` on two checkouts, for example:

```
git show main:shot.ntpz > /tmp/before.ntpz
natron-merge diff /tmp/before.ntpz shot.ntpz
```

## Updating a template in a shot

A shot that imported a template keeps the template's nodes tagged with where they came from,
and keeps a copy of the template as imported. When the template is updated, the artist's
edits and the new version are merged:

1. Get the template as it was imported, for example from version control:
   `git show v1:templates/grade.ntpz > grade-v1.ntpz`.
2. Merge, naming the key the template was imported under:

   ```
   natron-merge merge --scope template:grade_tpl grade-v1.ntpz shot.ntpz grade-v2.ntpz -o shot-updated.ntpz
   ```

3. Open `shot-updated.ntpz`. Nodes the artist added stay where they were; nodes the
   supervisor added to the template appear; knobs both changed keep the artist's value and are
   listed as conflicts.

A shot can import several templates, or the same template twice under different keys; each
key is updated on its own, and the conflicts saved by earlier updates are kept.

## Merging on the node graph

Inside Natron, both a comparison and the conflicts of an update are shown and resolved on the
node graph itself.

### Comparing and picking

**File > Compare with File...** and **Compare with Template Base** (on a template node's
context menu) show how the project differs from another version: added nodes are tinted
green, changed nodes amber, moved nodes are outlined, and nodes the other version has but the
project does not are drawn as ghosts where they would sit. Ghosts can be selected like nodes:
click one, Shift- or Ctrl-click to add or remove one, or drag a rubber band over them. Selected
ghosts get the selection outline. Delete, copy and drag leave ghosts alone.

Right-click the selection for the picks that apply to it. Each pick is one undo step, and the
comparison is made again after it, and after it is undone or redone, so the graph always shows
what still differs. A pick applies only to the selected nodes it fits, so a mixed selection can
offer several.

- **Restore** (ghosts) recreates the node where its ghost is drawn, with its parameters, and
  wires it as the other version had it: an input comes from the matching live node, or from a
  ghost restored with it, and is left empty otherwise. Ghosts restored together are wired to
  each other, so a run of adjacent ghosts comes back as a chain. A removed Group comes back
  whole, with everything inside it. A restored node whose script name is taken gets a fresh one,
  and the expressions and inputs of the other restored nodes that name it follow.
- **Restore (Keep My Wiring)** and **Restore (Splice Everywhere)** restore the same way but
  settle rewired readers without asking (see below).
- **Revert to Other** (changed nodes) gives the node every changed parameter, input and
  appearance the other version has.
- **Remove** (added nodes) deletes the node and connects its main input to whatever it fed, as
  a normal delete does.
- **Restore Position** (moved nodes) moves the node back to where the other version has it.

### Splicing restored nodes back in

Restoring a node also reconnects the nodes that read from it in the other version. For each such
input in the project:

- an input that is empty, or still connected to what fed the removed node, is connected to the
  restored node;
- an input the artist connected to something else since is a choice. **Restore** leaves it and
  adds a pending splice choice on the reader to the merge conflicts, with **Keep My Wiring**
  (leave it) and **Splice In** (connect it to the restored node). **Restore (Keep My Wiring)**
  leaves it with no choice, and **Restore (Splice Everywhere)** connects it.

Pending splice choices are merge conflicts like any other: they carry the badge, appear in the
popover and the Merge Conflicts panel, and are saved with the project.

### The merge popover

Click the conflict badge of a node with merge conflicts (its tooltip says "Click to resolve")
to open the merge popover beside it. It lists each of the node's conflicts with a button for
each choice it can take, and **All Mine** and **All Theirs** for all of them. Each choice is
one undo step. While a comparison shows a node as changed, click the change marker on the node
to open the same popover with a **Changes** section: **Take** beside each changed parameter
gives it the other version's value, and **Revert Node** does what Revert to Other does.

The popover is not modal. It refreshes after every choice, and closes on Escape, on a click
elsewhere, or once nothing on the node is left to choose.

Only choices that are possible and lead somewhere new are offered: a choice that would give
the same result as one listed before it is left out, and Keep Both Keys is offered only for
keys at different frames, since two keys cannot share a frame.

### Previews of structural conflicts

Input, insert-on-the-same-link, reparent, delete-modify and pending splice conflicts change the
wiring rather than a value. While the popover of a node with such a conflict is open, each side
is drawn faintly on the graph: dashed wires in the side's colour for its connections, the
deleted node of a delete-modify conflict as a ghost, and for an insert the side's nodes
outlined and the other side's dimmed. Hover a choice, in the popover or the Merge Conflicts
panel, to draw that side in full. Click a side's preview ghost or the end of its wire to take
that side. Conflicts reopened from a saved project keep their previews.

### The merge bar

A bar along the bottom of the node graph shows either the comparison or the conflicts, never
both, and is hidden when there is neither.

- During a comparison it names what is compared, counts the added, changed, removed and moved
  nodes, and offers **Previous Change**, **Next Change**, **Restore Selected**, **Revert
  Selected** and **Clear Comparison**.
- While the project has merge conflicts it counts them and offers **Previous Conflict**,
  **Next Conflict**, **All Mine**, **All Theirs** and **Open Panel**.

Starting a comparison hides the conflict badges until it is cleared; an update that raises
conflicts clears the comparison first. After such an update the graph frames the first node
with a conflict. The Merge Conflicts panel does not open by itself, after an update or when a
project with conflicts is opened; **Open Panel** shows it. It offers the same choices as the
popover.

The node graph's menu has the same navigation, which frames the node and opens its popover:

| Action            | Shortcut          |
|-------------------|-------------------|
| Next Conflict     | Alt+Down          |
| Previous Conflict | Alt+Up            |
| Next Change       | Alt+Shift+Down    |
| Previous Change   | Alt+Shift+Up      |

### Scripting

The GUI app (`app` in the script editor) has a call for every pick, each one undo step that
returns `False`, with the reason in the script editor, when it did nothing:

- `app.restoreFromComparison(nodeNames=[], splice="ask")` restores the ghosts named by script
  name or node ID, or the selected ghosts when the list is empty. `splice` is `"ask"`,
  `"keepMine"` or `"everywhere"`, as Restore, Restore (Keep My Wiring) and Restore (Splice
  Everywhere).
- `app.revertFromComparison(effects)`, `app.removeAddedFromComparison(effects)` and
  `app.restorePositionFromComparison(effects)` are Revert to Other, Remove and Restore Position.
- `app.takeKnobFromComparison(effect, knobName)` is Take on one parameter.
- `app.getComparisonGhosts()` lists the ghosts as dicts with `name`, `nodeId`, `group` (empty
  at the top level) and `selected`; `app.selectComparisonGhosts(nodeNames)` selects exactly the
  ghosts named.
- `app.openMergePopover(effect)` and `app.isMergePopoverOpen()` open and query the popover, and
  `app.mergeBarMode()` returns `"comparison"`, `"conflicts"` or `"none"`.

Conflicts are listed and resolved on the app, in the GUI or not:

- `app.getMergeConflicts()` returns each unresolved conflict as a dict with `index`, `node` (the
  fully qualified node name), `nodeId`, `kind`, `label`, and `choices` (`"mine"`, `"theirs"`,
  `"base"` or an option's name) with their labels in `choiceLabels`. A pending splice choice
  has kind `"splice-choice"`, with `"mine"` for Keep My Wiring and `"theirs"` for Splice In.
- `app.resolveMergeConflict(index, choice)` resolves one with a name from its `choices`, as one
  undo step in the GUI.
