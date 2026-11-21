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
  deleted and recreated are matched by their place in the graph.

Where both sides changed the same thing differently, the result keeps **mine** and records a
conflict. The conflicts are saved in the bundle (`merge/conflicts.yaml`), so a conflicted
project still opens normally, with the conflicts listed for resolving later. A summary is
printed on stderr.

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
key is updated on its own, and conflicts from updating one key are kept when another is
updated.
