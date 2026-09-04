Contributing to Natron
======================

Code contributions to Natron are always welcome. That's a big part of
why it's an open source project. Please review this document to get a
briefing on our process.

Code of Conduct
---------------

By contributing to Natron and the open source software projects managed
within the [Natron organization on GitHub](https://github.com/NatronGitHub),
you agree to follow the [Contributor Covenant Code of Conduct](https://www.contributor-covenant.org/version/1/4/code-of-conduct).

Examples of behavior that contributes to creating a positive environment
include:

* Using welcoming and inclusive language
* Being respectful of differing viewpoints and experiences
* Gracefully accepting constructive criticism
* Focusing on what is best for the community
* Showing empathy towards other community members

Examples of unacceptable behavior by participants include:

* The use of sexualized language or imagery and unwelcome sexual attention or
  advances
* Trolling, insulting/derogatory comments, and personal or political attacks
* Public or private harassment
* Publishing others' private information, such as a physical or electronic
  address, without explicit permission
* Other conduct which could reasonably be considered inappropriate in a
  professional setting


Bug Reports and Issue Tracking
------------------------------

We use GitHub's issue tracking system for bugs and enhancements:
https://github.com/charlesangus/Natron/issues

**If you are merely asking a question ("how do I...")**, please do not file an
issue, but instead ask the question on the [forum](https://discuss.pixls.us/c/software/natron).

If you are submitting a bug report, please be sure to note which version of
Natron you are using, and on what platform (OS/version).

Please give an account of

* what you tried
* what happened
* what you expected to happen instead

with enough detail that others can reproduce the problem.


Contributions Under Repository License
--------------------------------------

Whenever you make a contribution to a repository containing notice of a [license](LICENSE.txt), you license your contribution under the same terms, and you agree that you have the right to license your contribution under those terms.

Pull Requests and Code Review
-----------------------------

The best way to submit changes is via GitHub Pull Request.

All code must be formally reviewed before being merged into the official repository. The protocol is like this:

1. Get a GitHub account, fork [`charlesangus/Natron`](https://github.com/charlesangus/Natron) to create your own repository on GitHub, and then clone it to get a repository on your local machine.

2. Edit, compile, and test your changes.

3. Push your changes to your fork (each unrelated pull request to a separate
"topic branch", please).

4. Make a "pull request" on GitHub for your patch, targeting the "main" branch.

5. If your patch will induce a major compatibility break, or has a design
component that deserves extended discussion or debate among the wider Natron
community, then it may be prudent to make a post on our [forum](https://discuss.pixls.us/c/software/natron) pointing everybody to
the pull request URL and discussing any issues you think are important.

6. The reviewer will look over the code and critique on the "comments" area,
or discuss in email/forum. Reviewers may ask for changes, explain problems they
found, congratulate the author on a clever solution, etc. Please don't take it hard if your
first try is not accepted. It happens to all of us.

7. After approval, one of the senior developers (with commit approval to the official main repository) will merge your fixes into the "main" branch.


Coding Style
------------

There are two overarching rules:

1. When making changes, conform to the style and conventions of the
surrounding code. Do not modify spacing, indentation, or symbol names
if there is no change to the underlying code.

2. Strive for clarity, even if that means occasionally breaking the
guidelines. Use your head and ask for advice if your common sense seems to
disagree with the conventions.


Architecture Invariants
-----------------------

**Ownership and lifetime**

* Parent-to-child links are `shared_ptr`; child-to-parent back-references are
  `weak_ptr`. This avoids reference cycles that would otherwise leak the whole
  object graph. Follow this convention when adding relationships between
  classes.
* Classes that need to hand out a `shared_ptr` to themselves derive from
  `std::enable_shared_from_this`. Never construct a second `shared_ptr` from a
  raw `this`.
* PIMPL (`FooPrivate`) is deliberate: it keeps public headers small and
  stable, so changing a private member does not force a recompile of every
  includer, and it keeps heavy or platform-specific includes out of the
  public interface. When adding state to a class, put it in the `…Private`
  object, not the public header.
* Include a `Fwd` header (`Engine/EngineFwd.h`, `Gui/GuiFwd.h`) instead of a
  class's full header whenever you only need to name the type. This is a
  major reason the project compiles at all given its size.

**Why Node and EffectInstance are separate**

`Node` is the durable graph vertex: stable identity, connections, undo
history. `EffectInstance` is the (possibly replaceable) rendering behavior
behind it. Keep them apart. This separation is what lets Natron reset or
reload a plug-in, swap a Read node's decoder when the file type changes, or
run several render clones of the same effect concurrently, all without
disturbing the graph topology the user built.

**Headless operation drives the architecture**

The same Engine must run without a GUI (`NatronRenderer`, for command-line
and render-farm use). Any change in `Engine` must build and behave correctly
without `Gui`. If you find yourself wanting to call into `Gui` from `Engine`,
add a method to one of the abstract `…I` interfaces instead (implemented on
the `Gui` side) rather than including a `Gui` header.

**Rendering**

* Any in-flight render can be aborted. Check the abort flag periodically in
  long-running render loops, or the UI feels stuck.
* Cache entries are keyed by a 64-bit hash (`Hash64`) computed from
  everything that affects the result (parameters, input hashes, time, view,
  scale, region). If an input changes, the hash changes and the old entry is
  simply not found, which is why cache invalidation stays correct with no
  explicit dirty-tracking.

**Editing operations and views**

* Implement editing operations as undo commands rather than mutating state
  directly, or undo silently breaks.
* Plumb `ViewIdx` through render code exactly as the surrounding code does.
  Dropping it silently breaks stereo projects, and the type system will not
  catch the mistake.

**OpenFX host**

* Triage heuristic: a bug that reproduces with *all* plug-ins points at the
  host glue (`Engine/Ofx*` or `HostSupport`); a bug that reproduces with only
  *one* plug-in is probably in that plug-in's own repository, not here.
* The host-to-plug-in contract is effectively a public API: third-party and
  commercial plug-ins depend on it. Prefer additive, capability-flagged
  changes over breaking changes.
* A stale or corrupt OFX plug-in cache is a likely cause when a plug-in
  appears missing or stale after an update; clearing it forces a full
  rescan.

**Serialization and compatibility**

* Any change to a `…Serialization` struct can break existing users' project
  files. Always bump the class version (`BOOST_CLASS_VERSION`) and add a
  version-guarded branch that can still read the old layout; never silently
  change field meaning or order. This is the single easiest place to cause
  data-loss regressions.
* The `Py*` API (user scripts, PyPlugs, tutorials) is similarly
  compatibility-critical: keep it stable and minimal, and treat breaking it
  as seriously as breaking the serialization format.
* When fixing a bug that's unit-testable (color, image, curve, hashing,
  geometry), add a regression test. Rendering and GUI behaviour are harder to
  unit-test; describe manual test steps in the PR instead.

**Build and toolchain**

* `DEBUG` builds enable floating-point exception trapping at startup
  (`Global/FloatingPointExceptions.h`), so a stray NaN or divide-by-zero
  aborts at the source instead of silently propagating through the image
  pipeline.
* `QT_NO_CAST_FROM_ASCII` is defined project-wide: you cannot implicitly
  build a `QString` from a `const char*`. Wrap literals in
  `QString::fromUtf8(...)`.
* Install third-party Python packages into Natron's bundled interpreter
  (`natron-python -m pip install <pkg>`) rather than the system Python, so
  the package lands on the interpreter Natron actually runs.

**Numbered source files**

Numbered source files (`Gui20.cpp`, `NodeGraph45.cpp`, `ViewerTab30.cpp`, …)
are one class split across several files purely to keep translation units a
manageable size. The numbers carry no semantic meaning beyond grouping
related methods.

This Fork: Branching Model
---------------------------

**The rest of this document describes upstream `NatronGitHub/Natron`.** This
repository, [`charlesangus/Natron`](https://github.com/charlesangus/Natron),
is a Linux-only fork migrating to Qt6/C++20/Rocky Linux 9/CMake, and it
follows trunk-based development instead. Where the two disagree, this section
governs for this repo.

* **Default/target branch:** `main` (not `master`, and not `RB-2.6`). Branch
  from `main`, and open pull requests against `main`.

* **Branch naming:** `milestone/<id>-<slug>` for planned milestone work,
  `fix/<slug>` for everything else.

* **Merging:** pull requests are squash-merged into `main`. Three required
  status checks must pass before merge: `format` (clang-format on touched
  C/C++ files), `lint-ci` (actionlint on workflows and shellcheck on CI
  scripts), and `build-and-test` (the containerised debug build, ctest suite,
  and Python-bindings smoke test). The first two are defined in
  `.github/workflows/checks.yml`, the third in `.github/workflows/ci.yml`. If
  you rename a CI job, update `main`'s required status checks in the same
  change — a required check with no matching job leaves PRs pending forever
  instead of failing them, which is why this fork removed its old stand-in
  aggregator job.

* **`RB-2.6` and the other `RB-*`/legacy branches** (`RB-2.3-broken`,
  `RB-2.4-VisualStudio`, `coverity_scan`, `master`, etc.) are **frozen
  history, not merge targets.** `RB-2.6` is a pointer kept at the commit
  where the default branch was renamed to `main`, so old links keep
  resolving. The rest are kept deliberately as the historical record and to
  keep the upstream bridge (below) usable — they are not pruned and you
  should not branch from or merge into them.

* **Upstream bridge:** fixes that aren't Linux/Qt6/VFX-Platform-specific
  should also get a matching PR against `NatronGitHub/Natron`, to avoid
  fully diverging from that project's ongoing work. Add it as a remote with:

  ```
  git remote add upstream https://github.com/NatronGitHub/Natron.git
  ```

* **Building and testing locally:** see `tools/ci/local/README.md` for the
  full local build/test loop.

