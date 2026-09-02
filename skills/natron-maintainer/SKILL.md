---
name: natron-maintainer
description: Domain knowledge for working on the Natron compositor's own C++ source code (Engine/Gui/HostSupport). Use when navigating, fixing, or extending the Natron codebase, doing Qt 6 work, building Natron, or building its Sphinx docs.
version: 1.0.0
tags: [skill, natron, compositor, openfx, qt, cpp]
---

# Natron Maintainer

## Overview
Natron is a node-based OpenFX-host video compositor. Its code is a stack:
OpenFX host (``HostSupport`` + ``libs/OpenFX``) → engine (``Engine``) → Qt GUI
(``Gui``), with thin ``App``/``Renderer`` entry points. Dependencies point
**downward only**: ``Engine`` must never include a ``Gui`` header (so the
headless ``NatronRenderer`` can link ``Engine`` without ``Gui``).

## Usage
Use this skill when:
- Navigating or modifying the Natron C++ source (Engine/Gui/Global/HostSupport).
- Adding or fixing a node, knob (parameter), render behavior, or the cache.
- Doing Qt 6 migration cleanup work or touching the Python (Shiboken/PySide) bindings.
- Building Natron, running its tests, or building the Sphinx documentation.

This skill is a quick map, not a full reference; for depth beyond it, read
the source itself.

## Core Concepts

- **Runtime object tree:** ``appPTR`` (``AppManager`` singleton) → ``AppInstance``
  (one per project) → ``Project`` (a ``NodeCollection`` + ``KnobHolder``) →
  ``Node`` → ``EffectInstance``. A ``Node`` is the stable graph vertex; its
  ``EffectInstance`` is the (replaceable) behavior — either an
  ``OfxEffectInstance`` (an OpenFX plug-in) or a built-in C++ effect
  (``ViewerInstance``, ``RotoPaint``, ``TrackerNode``, ``ReadNode``/``WriteNode``,
  ``NodeGroup``, ``Backdrop``, ``Dot``, …).
- **Rendering is pull-based:** an output effect calls
  ``EffectInstance::renderRoI(RenderRoIArgs&, …)`` which recurses upstream by
  region of interest, time, view and mip-map level; results are stored in a
  hashed RAM/disk ``Cache``. Per-render context lives in thread-local
  ``ParallelRenderArgs`` installed by ``ParallelRenderArgsSetter``.
- **Params are "knobs":** ``KnobI`` → ``KnobHelper`` → concrete ``KnobInt`` /
  ``KnobDouble`` / … ; owned by a ``KnobHolder``; created via
  ``appPTR->getKnobFactory().createKnob<K>(...)``. Knobs are NOT ``QObject`` —
  each has a ``KnobSignalSlotHandler`` companion for signals.
- **Natron IS an OpenFX host:** most nodes are OFX plug-ins; ``Engine/Ofx*`` glues
  the generic host in ``HostSupport``/``libs/OpenFX`` to Natron's node/knob/image
  model.

- **The two executables** share the Engine (``Gui`` links it; ``NatronRenderer``
  links it without ``Gui``). A third binary, ``natron-python``, is a full Python
  interpreter with Natron's modules; it also installs PyPI packages into Natron's
  bundled Python via ``natron-python -m pip install <package>``.

## File-Naming Conventions (learn these first)

| Suffix | Meaning |
|--------|---------|
| ``FooFwd.h`` | Forward decls + ``FooPtr``/``FooWPtr`` typedefs. ``Engine/EngineFwd.h`` and ``Gui/GuiFwd.h`` are the master catalogs — start here for unknown types. |
| ``FooI.h`` | Abstract interface (pure virtual). The Engine↔Gui seam: ``NodeGuiI``, ``KnobGuiI``, ``OpenGLViewerI``, ``NodeGraphI``, ``DockablePanelI``. Engine holds these; Gui implements them. |
| ``FooPrivate.h/.cpp`` | PIMPL implementation of ``Foo`` (public header holds only ``_imp``). |
| ``FooSerialization.h`` | Boost.Serialization (XML) description; version with ``BOOST_CLASS_VERSION``. |
| ``PyFoo.h/.cpp`` | Python-facing facade (Shiboken-bound). |
| ``OfxFoo.h/.cpp`` | OpenFX host glue. |
| ``Gui20.cpp``, ``ViewerTab30.cpp`` … | One big class split across numbered files; numbers are grouping only. |

## Key Idioms

- Everything is inside ``NATRON_NAMESPACE_ENTER``/``_EXIT`` (macros in
  ``Global/Macros.h``); Python-exposed classes use ``NATRON_PYTHON_NAMESPACE``.
- **Include ``<Python.h>`` first** in every TU (the "PYTHON BLOCK"); it must
  precede standard headers.
- ``std::shared_ptr``/``weak_ptr`` everywhere; parent→child is ``shared_ptr``,
  child→parent is ``weak_ptr`` (avoid cycles). Add new types to the ``Fwd``
  catalog.
- ``QT_NO_CAST_FROM_ASCII`` is set: wrap literals in ``QString::fromUtf8("…")``
  or ``tr()``.
- Serialization and the ``Py*`` API are **backward-compatibility-critical** —
  version serialization changes; keep the Python API stable.

## Quick Reference

Build (CMake, the only build system; Linux only; requires Qt 6.8+):
```bash
cmake -S . -B build                           # Qt6.8+/Shiboken6/PySide6
cmake --build build -j
ctest --test-dir build                        # unit tests (or -DNATRON_BUILD_TESTS=OFF)
```
Reproducible local build/test matching CI, in the same container CI uses
(``aswf/ci-vfxall:2027-clang21.1``):
```bash
tools/ci/local/build.sh [debug|release]
tools/ci/local/test.sh <ctest|smoke> [debug|release]
tools/ci/local/devshell.sh                    # interactive shell in the same container
```
See ``tools/ci/local/README.md`` for setup (image build, one-time test-asset fetch).

Code style: ``clang-format``, pinned to 21.1.8. The ``format`` CI check only
requires changed lines to be clean (``git clang-format --diff <base-rev>``), not
the whole file:
```bash
git clang-format --diff HEAD~1                # or another base revision
```

Docs (Sphinx; sources in ``Documentation/source``):
```bash
cd Documentation && sphinx-build -b html source html
```
Do NOT hand-edit ``index.rst`` (except to add a whole new guide to the toctree),
``_``-prefixed files, or ``plugins/`` (generated by ``tools/genStaticDocs.sh``).

## Qt 6 Migration Status (as of 2026)

- **Done:** GL widgets already use ``QOpenGLWidget``; the build requires
  Qt 6.8+/Shiboken6/PySide6 unconditionally; ``QtCompat.h`` has version shims.
  This fork has dropped all prior major-version Qt support entirely —
  remaining ``#if QT_VERSION`` guards only gate Qt 6 minor-version features.
- **To do:** ``QDesktopWidget``/``QApplication::desktop()`` → ``QScreen``
  (a handful of files in ``Gui``); regenerate PySide6/Shiboken6 bindings
  (fixes enum/flag issue #854).

## Common Mistakes
- Including a ``Gui`` header from ``Engine`` (breaks the headless renderer). Use a
  ``…I`` interface instead.
- Reading live node state during a render instead of the captured hash/args in
  ``ParallelRenderArgs`` (causes inconsistent renders / stalls).
- Changing a ``*Serialization`` struct without a version bump (breaks users'
  project files).
- Making a core value-type a ``QObject`` for signals — use the
  ``KnobSignalSlotHandler`` companion pattern.
