# Inherited upstream documentation is not a source of requirements

An audit triggered by a concrete incident: `Documentation/source/maintainers/todo.rst`
was read during plan authoring as a source of *requirements*, and eight
`NatronGitHub/Natron` issue numbers (#192, #248, #795, #845, #864, #1008, #1029,
#1057) entered M5 as a task nobody asked for. The task text even carried the
premise — "same bugs regardless of fork" — as a stated fact rather than a
finding. Triage later showed most were Windows-only, or filed against the
shipped Qt5/PySide2 2.5.0 bundle, on a stack this fork does not have.

The cause is systemic. All 17 files in `Documentation/source/maintainers/` came
from upstream commits `9038672dd` and `1d8c88c83` (July 2026), **six weeks
before this fork's first commit** (`a56635515`), and are surfaced through a
Sphinx toctree as current maintainer guidance. Nothing validates them: no
workflow runs Sphinx, and `nightly.yml` sets `paths-ignore: [Documentation]`.
Four of them instruct a reader to undo M0, M1, M2, M4 and M10.

**The rule:** documentation inherited from upstream describes upstream's
codebase, upstream's priorities and upstream's roadmap. It is evidence about
history, never a statement of this fork's requirements. Work enters this project
through the plan, and the plan through the user — not through a `.rst` file that
predates the fork.

Two corollaries the audit made concrete:

- **Accuracy today is not the test; drift is.** `gui.rst` had roughly fifty
  class names, numbered-file ranges and interface pairings, and every claim
  verified true — because the GUI module is untouched by the Linux/Qt6/CMake
  work. It is still deleted. An accurate index that nothing forces to track the
  code is a liability with a delay fuse, and `codebase-map.rst` (already wrong
  in four of eight directory entries, and pointing at Jenkins while omitting
  `tools/ci/`) is the same file a year later.
- **Agent-facing files rank above human-facing ones.**
  `skills/natron-maintainer/SKILL.md:90` instructing `qmake Project.pro` is the
  same failure mode as `todo.rst` with a much shorter fuse, because it is read
  as instructions rather than as reference.

Knowledge that is genuinely not recoverable from the code — the `shared_ptr`
cycle rule, the serialization versioning requirement, the render-path TLS
discipline, the `QRegExp`-to-`QRegularExpression` semantic gap whose call sites
no longer exist — is salvaged into `docs/maintainer-notes.md` before deletion
and distributed to `CONTRIBUTING.md` and to code comments in M6. Everything that
merely describes the code is deleted, because the code is the better source.
