# Project file format

A Natron project is a `.ntpz` bundle: a stored (uncompressed) zip archive of YAML documents
and binary tables. This page describes what the code in `Serialization/` reads and writes. The
encoders are in `Serialization/Yaml/`, the container in `Serialization/Bundle/`. To add a field,
see [Serialization/README.md](../Serialization/README.md).

## Files Natron saves

| Extension | Content | Written by |
| --- | --- | --- |
| `.ntpz` | Project bundle (zip) | `ProjectBundleWriter` |
| `.nps` | Node preset: one YAML document | `encodePresetYaml` |
| `.nl` | Application layout: one YAML document | `encodeGuiLayoutYaml` |
| (clipboard) | Copied nodes: one YAML document | `encodeClipboardYaml` |

All YAML is UTF-8. Floating point values are written with `std::to_chars`, so they round-trip
exactly; infinities and NaN are `.inf`, `-.inf` and `.nan`.

## Bundle layout

The archive is a classic 32-bit zip (no zip64, no compression, no encryption). Members are
written in a fixed order:

1. `mimetype`, containing `application/vnd.natron.project+zip`. It has no extra field, so the
   mime type always starts at byte offset 38 of the file and file sniffers can match it. A reader
   treats a zip whose first member is not this `mimetype` as not a Natron project.
2. `manifest.yaml`
3. `project.yaml`
4. `blobs/**`, sorted by path. Present only when the project holds roto or tracker data too
   large to inline (see [Blob tables](#blob-tables)).

Every member except `mimetype` is stored with an extra field of id `0x1986` whose padding places
the start of its data on a 64-byte boundary. A reader that maps the file can then hand out the
bytes of a blob in place, correctly aligned for `double`. The reader does not require the
alignment: misaligned columns are copied instead of read in place.

A reader ignores members it does not know. Saving the same project twice gives the same bytes
except in `manifest.yaml`, which carries the save date. A project is saved by writing a new file
and renaming it over the old one, because the reader maps the archive.

The bundle holds no window or pane layout, viewer state, selection or open panels. See
[What a project does not save](#what-a-project-does-not-save).

## manifest.yaml

Everything that changes on every save lives here, so that re-saving an unchanged project leaves
`project.yaml` byte-identical.

| Key | Meaning |
| --- | --- |
| `format` | Always `natron-bundle`. |
| `formatVersion` | Bundle format version, currently `1`. |
| `app` | `{version, commit}` of the Natron build that saved the file. |
| `saved` | Save time, ISO 8601 UTC. |
| `author` | The user and host that saved the file. |
| `created` | Project creation time, milliseconds since the epoch. |
| `members` | Every member after the manifest itself, in archive order: `{name, size, crc32}`, the CRC as 8 lowercase hexadecimal digits. |

Example (the size and CRC values are illustrative):

```yaml
format: natron-bundle
formatVersion: 1
app: {version: "2.6.0", commit: "5acedc397"}
saved: "2026-10-23T14:02:11Z"
author: "alice@workstation"
created: 1760000000000
members:
  - {name: "project.yaml", size: 2411, crc32: "1a2b3c4d"}
  - {name: "blobs/Roto1/Stroke1.stroke.bin", size: 12864, crc32: "9f8e7d6c"}
```

On load the reader opens the archive, checks `mimetype`, checks the CRC-32 of every member,
requires `manifest.yaml` and `project.yaml`, and refuses a manifest whose `formatVersion` is
higher than it knows.

## project.yaml

The graph. Top-level keys, in the order they are written:

| Key | Meaning |
| --- | --- |
| `format` | Always `natron-project`. |
| `formatVersion` | Currently `1`. |
| `savedBy` | `{version: [major, minor, revision], branch, commit, os, bits}`. |
| `knobs` | Project knobs, sparse (see below). Omitted when empty. |
| `formats` | The project's additional formats: a list of `{name, rect: [x1, y1, x2, y2], par}`. Omitted when empty. |
| `layers` | The layer registry: a map from layer ID to `{label, channelsLabel, channels, origin}`, sorted by ID. Omitted when empty. |
| `timeline` | `{current: <frame>}`. |
| `nodes` | The nodes, a map keyed by script name and sorted by it. |

### Nodes

A node is a map with these keys; every key except `plugin` and `version` is omitted when it
holds its default.

| Key | Meaning |
| --- | --- |
| `plugin` | Plugin ID. |
| `version` | `[major, minor]` of the plugin. |
| `label` | Written only when it differs from the script name. |
| `python` | `{module, version}` for a node made from a PyPlug. |
| `pos` | `[x, y]`, the position in the node graph. |
| `size` | `[width, height]`; only backdrops carry one. |
| `color` | `[r, g, b]`, the user-chosen node colour. |
| `overlay` | `[r, g, b]`, the overlay colour. |
| `knobs` | The node's knobs, a map from knob script name to value. |
| `inputs` | A map from input label to the script name of a **sibling**, a node in the same map. |
| `pages` | The order of the knob pages. |
| `userPages` | User-created knobs, nested as pages, groups and knobs, with their definitions under `user:`. |
| `roto` | The roto context (layers, beziers, strokes). |
| `tracker` | The tracker context. |
| `children` | For a group, a map of its nodes, in this same schema. |

`pos`, `size`, `color` and `overlay` are the node's layout. A node that has no saved layout
writes none of them. Backdrops are nodes, so they use the same fields.

Group membership is nesting: a node is in a group because it sits in that group's `children`
map. A connection names a sibling by script name, never a path. Expressions and links between knobs name their targets with the full
dotted name (`thisGroup.amount`), as written in the expression.

On load a connection to a node that does not exist, or to an input label the plugin does not
have, becomes a load warning and the input stays disconnected.

### Knobs

A knob appears under its script name. Its value is written as the bare value when only the
value differs from the default (`size: 3.5`), a list for several dimensions, and a map when more
is needed. The map form can hold `value`, `expr` (with `hasRet`), `secret`, `alias`, `option`
(a choice written by option ID), `strings` (animated strings), `curves` (parametric curves) and
`user`, the definition of a knob the user added. Animated values are written as curves, see
[Curves](#curves). Only knobs that differ from their default are saved; defaults are saved for
user knobs, whose definition the file must carry.

### Example

This is the document the tests pin (`kExampleProjectYaml` in
`Tests/Serialization/ProjectYaml_Test.cpp`): a reader, a blur, a viewer and a group made from a
PyPlug that has a user page and holds its own blur.

```yaml
format: natron-project
formatVersion: 1
savedBy: {version: [2, 6, 0], branch: "main", commit: "5acedc397", os: "Linux", bits: 64}
knobs:
  frameRange: [1, 240]
  frameRate: 23.976
formats:
  - {name: "Anamorphic", rect: [0, 0, 2048, 858], par: 2}
layers:
  diffuse: {label: "diffuse", channelsLabel: "RGB", channels: ["R", "G", "B"], origin: 2}
  zdepth: {label: "Depth", channelsLabel: "Z", channels: ["Z"], origin: 1}
timeline: {current: 12}
nodes:
  Blur1:
    plugin: "net.sf.cimg.CImgBlur"
    version: [4, 0]
    label: "Soft blur"
    pos: [120, -40]
    knobs:
      size: 3.5
    inputs: {Source: "Read1"}
    pages: ["Controls", "Node"]
  Group1:
    plugin: "fr.inria.built-in.Group"
    version: [1, 0]
    python: {module: "SoftGlow", version: 2}
    inputs: {Input1: "Blur1"}
    userPages:
      Controls:
        group: {label: "Controls"}
        children:
          amount:
            value: 0.5
            user:
              type: "Double"
              dimension: 1
              label: "Amount"
              newLine: true
              evaluates: true
              animates: true
              persistent: true
              default: 0.5
              range: [0, 1, 0, 1]
          advanced:
            group: {label: "Advanced", opened: true}
            children:
              mode:
                value: "add"
                user:
                  type: "Choice"
                  dimension: 1
                  label: "Mode"
                  newLine: false
                  evaluates: true
                  animates: false
                  persistent: true
                  default: 1
                  entries: ["screen", "add"]
                  helps: ["", ""]
    children:
      Blur2:
        plugin: "net.sf.cimg.CImgBlur"
        version: [4, 0]
        pos: [-35.5, 80]
        knobs:
          size: {expr: "thisGroup.amount.get() * 20", hasRet: false}
        inputs: {Source: "Input1"}
      Input1:
        plugin: "fr.inria.built-in.Input"
        version: [1, 0]
      Output1:
        plugin: "fr.inria.built-in.Output"
        version: [1, 0]
        inputs: {Source: "Blur2"}
  Read1:
    plugin: "fr.inria.built-in.Read"
    version: [1, 0]
    knobs:
      filename: "plates/shot010.####.exr"
  Viewer1:
    plugin: "fr.inria.built-in.Viewer"
    version: [1, 0]
    inputs: {"1": "Group1"}
```

### What a project does not save

Pane layout, viewer state, the selection, histograms, open panels, Python panels opened at
runtime and the script editor input are not in a project. Node position, size and colours are,
as node data. The pane layout is the application's: it is saved in `.nl` files only (see
[Layouts](#presets-layouts-and-the-clipboard)).

## Curves

A curve is a YAML flow sequence of keyframes (`Serialization/Yaml/CurveYaml.cpp`). Each keyframe
is a time and a value, optionally preceded by an interpolation token and followed by
derivatives:

```
[L, 1, 0, 24, 1, S, 48, 0.5, F, 72, 2, 0.1, X, 96, 3, 0.2, -0.2]
```

The tokens, which map to the interpolation types by position, are `K` constant, `L` linear,
`S` smooth, `R` Catmull-Rom, `C` cubic, `H` horizontal, `F` free, `X` broken and `N` none.

- A token applies to the keyframes that follow it, up to the next token. The encoder writes a
  token before the first keyframe and each time the interpolation changes; a reader that sees no
  token before the first keyframe assumes linear.
- Every keyframe is `time value`. A `F` (free) keyframe adds one number, the derivative, which is
  used for both sides. An `X` (broken) keyframe adds two, the right derivative then the left.
  The other types store no derivatives.
- Times must strictly increase and cannot be NaN. A curve that ends in the middle of a keyframe
  is an error naming the missing element.

## Blob tables

Heavy roto and tracker data (strokes, bezier points and feathers, tracker curves) is stored as
column tables. In `project.yaml` the table's descriptor is a flow map:

`columns` lists each column as `"name:dtype"` (for example `"x:f64"`).

- A table with fewer than 64 rows is written inline: `{rows: N, columns: [...], values: [...]}`,
  with the values row by row.
- From 64 rows on the data goes into a blob member and the descriptor names it:
  `{blob: "blobs/Roto1/Stroke1.stroke.bin", rows: N, columns: [...]}`. The path is
  `blobs/<node full name>/<item name>.<kind>.bin`, with characters outside `A-Z a-z 0-9 _ - .`
  replaced by `_` and `-2`, `-3`, ... added to keep paths unique.
- A document with no blob members (`.nps` presets and the clipboard) writes the table into the
  descriptor as `data:`, a base64 `!!binary` scalar holding the same bytes.

If a named blob is missing from the bundle, the loader warns and leaves that item's data empty.

The blob itself is little-endian (`Serialization/Bundle/BlobTable.*`). The columns are typed
arrays stored one after the other (structure of arrays):

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | Magic `NTBL` |
| 4 | 2 | Version (`1`) |
| 6 | 2 | Column count |
| 8 | 8 | Row count |
| 16 | 8 | Offset of the column directory (`64`) |
| 24 | 40 | Zero padding |
| 64 | | Column directory |

Each directory entry is: `u64` offset of the column's data, `u16` name length, `u8` dtype,
5 zero bytes, then the name, padded to a multiple of 8 bytes. The dtypes are `1` f64, `2` f32,
`3` i32 and `4` u8. Each column's data starts on a 64-byte boundary and holds exactly `rows`
values.

Readers match columns by name. A column the reader expects and the blob lacks reads as default
values, and a column the reader does not know is ignored, so tables can grow columns without a
version bump. A column stored with a different dtype than the reader expects is an error.

## Presets, layouts and the clipboard

These are single YAML documents, each with `format` and `formatVersion` like `project.yaml`.

**`.nps` presets** (`Serialization/Yaml/PresetYaml.*`): `format: natron-preset`, and `nodes`, a
list in which each item is a one-node map in the `project.yaml` node schema. The first item is
the node the preset was exported from, followed by the instances of a multi-instance node. The
document is self-contained (inline tables). It holds no node it cannot find a plugin for? That is
a load-time concern, not a format one: a preset with no node is rejected.

**`.nl` layouts** (`Serialization/Yaml/ProjectGuiYaml.*`): `format: natron-layout`, and `windows`,
a list of windows. A window is `{main?, pos: [x, y], size: [w, h]}` plus one child: `splitter`
(`{orientation, sizes, children}`, each child a nested `splitter` or a `pane`), `pane`
(`{name, tabs, current, anchor?}`) or `panel`. This is the application layout, and the only
place pane layout is saved.

**Clipboard** (`Serialization/Yaml/ClipboardYaml.*`): `format: natron-clipboard`, and `nodes` in
the `project.yaml` node schema, so inputs name siblings by script name and position, size and
colours sit on each node. It travels on the system clipboard as
`application/vnd.natron.nodes+yaml` next to the same text as `text/plain`. A pasted clone is not
linked to its original.

## Versioning

Every document carries `formatVersion`, and the manifest has its own. The rules:

- A reader refuses a document whose `formatVersion` is higher than it knows, with a message
  that the file was saved by a newer Natron. It reads no part of it.
- A reader warns about a key it does not know and ignores it, naming the key and where it is
  (`unknown key "x" ignored`). The warnings are shown as a load summary. So a newer writer can add
  keys without breaking older readers, and a field that an old reader does not know is lost
  only if the project is saved again by the old reader.
- `formatVersion` is raised only for a change that would make an older reader misread a file: a
  renamed or removed key, or a changed meaning. Adding an optional key does not bump it.
- A `formatVersion` below `1` is invalid.

The current versions: `project.yaml` 1 (`kProjectYamlFormatVersion`), bundle 1
(`kBundleFormatVersion`), blob table 1 (`kBlobTableVersion`), preset 1, clipboard 1, layout 1.

## Clean break

There is no import of the old boost XML formats. Opening an old `.ntp`, `.nps` or `.nl` gives a
message that the file uses the old XML format and that this version cannot open it ("no
compatibility, by design"), and the application carries on. A file is recognised as XML from its
first bytes (`<?xml`, or `<!DOCTYPE boost_serialization`). A zip that is not a Natron bundle, or
any other file that is not readable, gives "Unrecognized or damaged project file".

## Repairing a bundle by hand

A bundle is a zip, so standard tools open it:

```sh
unzip -l f.ntpz                  # members, in archive order
unzip -p f.ntpz project.yaml     # print the graph
unzip -p f.ntpz manifest.yaml
```

To change a file, extract it, edit it and write a new archive with Python. The members must be
stored, not compressed, with `mimetype` first:

```python
import zipfile

with zipfile.ZipFile("fixed.ntpz", "w", zipfile.ZIP_STORED) as out:
    out.writestr("mimetype", "application/vnd.natron.project+zip")
    for name in ["manifest.yaml", "project.yaml"]:
        out.write(name)
```

Add any `blobs/**` members the same way. The result loads, but a hand-written archive lacks the
64-byte alignment padding, so blobs are copied instead of read in place, until the next Natron
save rewrites the file. The CRC of each member is checked on load, which `zipfile` computes
for you; the sizes and CRCs listed in `manifest.yaml` are descriptive and a reader does not
compare them with the archive.
