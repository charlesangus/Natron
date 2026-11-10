# M389 - Node Reference Export

Turn `--export-docs` into the generator for the docs site's node reference. It should write MkDocs-ready Markdown for every node a user can create from the node menu, with index pages per menu group, and retire the pandoc/RST post-processing (`tools/genStaticDocs.sh`). `tools/docs/build.sh --binary` from M388 - Docs Site Scaffold then fills `reference/nodes/` from the binary.

Context:
- **Export entry point:** `AppInstance::exportDocs` (`Engine/AppInstance.cpp:1671-1824`), reached through `--export-docs <dir>` (`Engine/CLArgs.cpp:904-916`, dispatched at `AppInstance.cpp:701-703`). NatronRenderer supports it headless (`NatronRenderer --no-settings --export-docs <dir>`).
- **What it writes today:**
  - `plugins/<pluginID>.md`, from `node->makeDocumentation(false)`, plus `<pluginID>.png` (the icon).
  - RST index pages built by hand as strings: `_group<Group>.rst` and `_group.rst`.
  - `_prefs.md`.
- **Per-node Markdown:** `Node::makeDocumentation(genHTML)` (`Engine/NodeDocumentation.cpp:139`) emits the title, a version line, Description, Inputs (Input / Description / Optional) and Controls ("Parameter / script name" / Type / Default / Function).
  - Markers at `NodeDocumentation.cpp:320,338,447` and `Engine/Utils.cpp:133,183` exist only for `genStaticDocs.sh`'s sed pass.
  - `genHTML=true` is the live in-app server path (`Gui/DocumentationManager.cpp:238`, which says it mirrors `exportDocs`). This milestone must not break it.
- **`tools/genStaticDocs.sh`** (on main) needs pandoc 2.3.1 exactly. It goes away.
- **Names:** may land before F17 - OpenNucleon Flag Day. Use the current binary and flag names; the rename script rewrites them.

## Scope

**In:**
- Markdown output only:
  - `reference/nodes/<pluginID>.md` with the icon beside it.
  - `reference/nodes/index.md`, listing groups.
  - one `reference/nodes/group-<slug>.md` per menu group.
  - `reference/preferences.md`.
  - `.pages` nav files.
- Nodes included (user, 2026-11-10): everything that appears in the node menu, meaning native nodes, bundled OFX (io, misc, arena) and bundled PyPlugs. Hidden, deprecated and internal plug-ins are skipped.
- Node pages that render correctly in MkDocs Material: tables, escaped pipes, choice lists, multi-dimension defaults, and the icon.
- The `tools/docs/build.sh` hook that runs the export.
- A test of the export.
- Deleting `tools/genStaticDocs.sh` and its markers.

**Out:**
- Serving the pages in the app: M391 - In-App Docs Bundle.
- Rewriting individual nodes' descriptions. Node text is owned by each node's milestone. Pages whose description is empty are reported in the PR body, not fixed here.
- The Python reference: M390 - Python API Reference.

**Edge cases:**
- **Several versions of one plug-in ID.** Document the highest major version only, the one the menu creates.
- **Plug-in IDs with characters unsafe in file names.** File names use the ID as-is; IDs are dotted ASCII. Assert this, and fail the export loudly on anything else.
- **Group names with `/` (sub-menus).** These become nested group pages with a slug per level. The node page lists the full path.
- **Markdown in descriptions.** Descriptions already in Markdown pass through unchanged. Literal `|` inside table cells is escaped.
- **The live HTML path** (`genHTML=true`) keeps its current output.

**Assumptions:**
- The plug-in registry exposes a "shown in the node menu" test that `exportDocs` can reuse (the toolbar and the node menu filter on it). If it's spread across several checks, gather them in one helper used by both.

## Phase 389.1: Markdown Export

- [ ] M389.P1.T1 — Write MkDocs Markdown from exportDocs and drop the RST pages
  - files: `Engine/AppInstance.cpp` (`exportDocs`), `Engine/NodeDocumentation.cpp`, `Engine/Utils.cpp`, `tools/genStaticDocs.sh` (delete)
  - approach:
    - Restrict the plug-in list to what the node menu shows (see Assumptions), at the highest major version.
    - Write the `reference/nodes/` layout from Scope, building group pages and `.pages` nav files from each plug-in's grouping. Write preferences to `reference/preferences.md`.
    - Remove the `genStaticDocs.sh` markers from the Markdown path (`genHTML=false`): emit the final Markdown directly, image links relative to the page, and pipes escaped in table cells. Leave the `genHTML=true` output unchanged.
    - Delete `tools/genStaticDocs.sh` and any reference to it.
  - verify: `NatronRenderer --no-settings --export-docs build/docs/export` on a `fast` build writes the layout from Scope. `grep -rl '\.rst' build/docs/export` finds nothing. A native node page (e.g. Blur) and an OFX page (e.g. `net.sf.openfx.GradePlugin`) have Description, Inputs and Controls tables with the right column counts. The in-app help for one node still renders: an Xvfb screenshot of the live page for Blur, compared with one from before the change.
  - size: L

- [ ] M389.P1.T2 — Hook the export into the docs build
  - files: `tools/docs/build.sh`
  - approach: Fill M388's node-reference hook: when `--binary` is given, run `<binary> --no-settings --export-docs <tmp>` with a throwaway HOME, and copy `reference/` into `build/docs/src/reference/`. Without `--binary`, keep the placeholder.
  - verify: `tools/docs/build.sh --binary <fast build>/NatronRenderer --out build/docs/site` passes `--strict`. The built site has a page per menu node and a working group index. Spot-check three pages in the built HTML.
  - size: M

- [ ] M389.P1.T3 — Test the export
  - files: `Tests/` (a new gtest beside the existing tests that start an AppInstance and create nodes; find the closest existing fixture), `Tests/CMakeLists.txt`
  - approach: Run `exportDocs` into a temp dir. Assert that:
    - a known native node's page exists with the three sections and its control table lists a known knob's script name and default;
    - a hidden or internal plug-in has no page;
    - the node's group page links to it;
    - no `.rst` file is written.
  - verify: The new test passes in the remote `fast` build with `--tests`, and fails if the hidden-plug-in filter is removed (check by temporarily reverting it locally and running syntax-check plus the test in the build).
  - size: M

**Verification gate:**
- The remote `fast` build and full ctest pass, including the new export test.
- `tools/docs/build.sh --binary …` passes `--strict` with the node reference filled.
- The live in-app help page for one native and one OFX node still renders: Xvfb screenshots go in the PR body.
- The PR body lists the menu nodes whose description is empty.

## Decisions

- 2026-11-10 — The reference covers everything in the node menu, meaning native, bundled OFX and bundled PyPlugs, and skips hidden, deprecated and internal plug-ins: it matches what a user can create (user).
- 2026-11-10 — The export writes final Markdown directly, and the pandoc 2.3.1 post-process is deleted: MkDocs reads Markdown, and the pinned pandoc was a fragile dependency (planner).
- 2026-11-10 — Node pages sit flat at `reference/nodes/<pluginID>.md`, with group pages beside them: URLs stay stable when a node moves menu group, and the in-app help can map a plug-in ID straight to a page (planner).
