# Documentation/templates/ is exempt from the clang-format gate

2026-09-02. The `format` job in `.github/workflows/checks.yml` now runs
`git clang-format` with `-- . ':(exclude)Documentation/templates/'`.

`Documentation/templates/` holds files named `*.c` that are **not buildable C**.
Per that directory's own `README.md` they are hand-maintained verbatim snapshots
of the HTML that `Engine/NodeDocumentation.cpp` and `Engine/Settings.cpp` emit as
chained string literals, kept so a human can diff the snapshot against the
literal. Their whole value is being a byte-faithful mirror. `--extensions c`
swept them in, and clang-format's reindentation would have destroyed precisely
that property.

## What forced the question

M13 deleted a single `<li>` line — the link to the now-deleted `_groupGMIC.html`
page — from those literals. clang-format responds to a one-line deletion inside a
long concatenation by wanting to reformat the entire enclosing statement: about
990 lines across five files, ~554 of them in the template snapshots.

Three options were weighed. Accepting all 990 lines would have kept CI config
untouched but silently broken the snapshots' mirror property. Reverting the
sidebar fix would have left a dangling in-app help link and left
`AppInstance::exportDocs` regenerating an empty `_groupGMIC.rst`, undoing the
deletion at the next doc export. Excluding the templates and taking the ~396-line
reindent in the two real `Engine/` sources keeps the gate meaningful where it
applies and off files it was never meant to police.

## The rule going forward

The format gate governs **compilable sources**. A file that merely carries a C-ish
extension while serving as data, fixture, or documentation sample does not belong
to it. If more such files appear, extend the pathspec rather than reformatting
them — and if a directory of them grows, that is a hint the extension is the
wrong signal and the files should be renamed instead.

Nothing about the gate's substance changed: it is still `git clang-format` over
changed lines only, per `2026-09-01-format-gate-changed-lines-only.md`.
