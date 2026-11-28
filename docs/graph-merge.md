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

## Templates and versions

A template is a part of a script that is shared between shots and updated over time: a grade
setup, a despill, a whole comp structure. A supervisor defines and publishes it; artists import
it into their shots and edit it there; when the supervisor publishes a new version, each shot
takes it with a three-way merge that keeps the artist's edits.

### The Template node

Every template in a script is shown by a **Template** node. It has no inputs or output and never
renders. Its knobs show the template's name, its template ID (which stays the same across all
its versions), the version the shot was last merged with or published as, the publish folder
and, when one exists, the path of a newer published version. Its buttons are Publish, Update to
Latest, Update from File..., Compare with Base and Detach.

A hull in the Template node's colour is drawn around the node and its members. Click the band
along the hull's edge to select all of them, for example to move them or delete them together.
Deeper inside the hull, clicks and rubber-band selections work as anywhere else in the graph.

### Membership

A node belongs to a template when:

- it came from the template, or was added to it in a publish;
- it was added by the artist between two members, wired below one and above another; or
- it was added with **Add to Template** from the node graph's right-click menu.

**Remove from Template** takes a node out: it stays in the shot, the hull no longer goes around
it, and the next publish leaves it out. Only the selected nodes are taken out, including nodes
the hull goes around only because they sit between members. Both entries appear in the
right-click menu when the selected nodes that belong to a template, as the hull counts them, all
belong to the same one, and each is one undo step.

### Workflow

1. **The supervisor defines the template.** Select the nodes and choose File > Define
   Template (with nothing selected, the template is made of the whole script). A Template node
   appears above the nodes and its settings open: set the template's name and its publish
   folder. Each new template gets a name of its own (`template`, `template_2`, ...) until you
   rename it.
2. **The supervisor publishes version 1.** Press Publish on the Template node. It writes
   `<name>_v001.ntpz` in the publish folder.
3. **The artist imports it.** File > Import Template... brings the nodes and the Template node
   into the shot. The artist edits it like any other part of the script: changes knobs, adds
   nodes, rewires.
4. **The supervisor publishes version 2,** either from the same script, or after importing the
   template into a fresh script. Versions are numbered by template ID, not by file name: Publish
   writes the version after the one the shot was last merged with or published as, under the
   template's current name, and renaming the template does not restart the numbering. Publish
   is not an undo step: a published file stays published.
5. **The artist updates.** Press Update to Latest on the Template node, or choose it from the
   right-click menu with any member selected. The newest version with the same template ID in
   the publish folder is merged into the shot:
   - nodes the artist added stay where they are, and nodes the supervisor added appear;
   - knobs only one side changed take that side's value;
   - knobs both changed keep the artist's value and are listed in the Merge Conflicts panel,
     where each can be switched to the template's value;
   - the whole update is one undo step.

   When the shot already has the newest version, nothing changes and the status bar says the
   template is up to date. When a newer version is published, the Template node shows its path.

Update from File... merges any file instead of the newest version, for example to go back to an
older one. Compare with Base shows what the shot changed since the version it was last merged
with or published as. Errors from both go to the status bar.

Publishing from a shot is allowed too. If the folder already holds that version or a newer one
of the template, because someone else published it in the meantime, Publish refuses and says
so: update to that version first, then publish. Publish also refuses, naming the clash, when
another template's versions in the folder use the same file name; rename the template or
publish it to another folder.

### Copying and pasting

A copied or duplicated Template node starts a new template: the pasted nodes that came from the
original become the members of the copy, under a name and template ID of their own, and the
original template is unchanged. Cut and paste moves the template instead: the pasted Template
node keeps the template, its name, ID and version, but saved merge conflicts are dropped.

### Safeguards

- **Another template.** When the file picked with Update from File... is a version of a
  different template, or is not a published template at all (none of its Template nodes has the
  template's ID), a warning asks whether to cancel or to merge anyway. Cancel leaves the shot as it was.
- **Too few matching nodes.** When fewer than a quarter of the nodes of the version the shot
  was last merged with are found in the new file, the update refuses and names the counts, instead of
  re-adding every node as new.

### Detaching

Detach, or deleting the Template node in any way (in the graph, with the group that holds it,
or from Python), makes the template's nodes ordinary nodes of the shot:
they stay, but they no longer belong to the template, the stored copy of the imported version is
dropped and the hull goes away. Later versions can then no longer be merged in. One undo brings
it all back. To delete the nodes as well, click the hull to select them all first.

### From Python

```python
tpl = app.defineTemplate([app.getNode("Grade1"), app.getNode("Blur1")], "grade")
key = tpl.getParam("templateKey").getValue()
tpl.getParam("publishDir").setValue("/shows/templates")
path = app.publishTemplate(key)          # /shows/templates/grade_v001.ntpz
status = app.updateTemplateToLatest(key) # "updated", "upToDate" or "lowMatch"
```

`app.updateFromTemplate(key, path, allowMismatch=False)` merges a given file and returns the same
statuses, or `"templateMismatch"` when the file is not a version of the template. Failures
raise `RuntimeError`.

### From the command line

The same three-way merge runs outside Natron. With the version the shot was last merged with as
the base, and the key the template was imported under (the Template nodes of the base and the
new version are left out, as in Natron):

```
natron-merge merge --scope template:grade_tpl grade_v001.ntpz shot.ntpz grade_v002.ntpz -o shot-updated.ntpz
```

A shot can import several templates, or the same template twice under different keys; each
key is updated on its own, and the conflicts saved by earlier updates are kept.
