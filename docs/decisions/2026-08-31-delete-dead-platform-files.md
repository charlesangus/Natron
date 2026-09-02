# The dead Windows/macOS/qmake files are deleted, in M3

M0 cut Windows, macOS and qmake from this fork, but left their artifacts
tracked: `Gui/QtMac.mm` and `Gui/TaskBarMac.mm` (Objective-C++ nothing builds),
`.github/workflows/gen_config.sh` (generates a qmake `config.pri` no workflow
invokes), `tools/travis/`, `tools/jenkins/`, `Project-*.xcodeproj/`, and
`Natron.spec`. These are deleted as a task inside M3 rather than deferred to a
milestone of their own.

The cost of leaving them is not hypothetical. M10 wired a `shellcheck` gate over
`gen_config.sh` and spent real effort clearing its findings and writing
suppressions for a script that generates a build file for a build system this
fork no longer has. Dead code attracts maintenance because tooling cannot tell
it is dead.

Accepted downside: it widens the diff against `NatronGitHub/Natron`, which the
upstream bridge in `CONTRIBUTING.md` depends on. Deletions conflict less badly
than edits when cherry-picking platform-agnostic fixes in either direction, so
this is judged affordable — but it is the reason the question was worth asking
rather than assuming.
