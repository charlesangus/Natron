# M357 - Repo Cleanup

Clear what has built up in the tree: Nuke mentions in user-facing text and comments, comments that break the comment policy across all first-party code, stale TODO markers, non-Linux code and finished spikes. A CI check on PR diffs keeps the Nuke mentions, plan IDs and TODO markers from coming back. Surveyed on main at 59b162ed6 (2026-11-05); line numbers are from that commit.

## Scope

**In:**
- Nuke removed from tooltips, plugin descriptions, node docs, README, metainfo and comments.
- The LensDistortion model renamed to Radial Asymmetric, with its identifiers.
- A comment-policy read of every first-party file, by cheap reader agents, who also log code smells.
- All TODO, FIXME, XXX and HACK markers resolved.
- Windows, macOS and FreeBSD conditionals removed across first-party code.
- `tools/spikes` deleted.
- A PR-diff CI check.

**Out:**
- Licence text and headers, including "GPLv2" in README: M157 - Relicense To GPLv3 Or Later.
- The legacy scheduler option: M199 - Remove Legacy Scheduler.
- Knob label wording: M308 - Knob Label Cleanup.
- Fixing reported code smells, unless the user picks them in M357.P5.T7.
- Tracker track masks and interlaced OFX fields: dropped by the user, no follow-up.
- `Engine/OfxRetired`, which hides shipped OFX bundles that a native node replaces. It stays.

**Edge cases:**
- OFX host-compat names stay because they are functional: `uk.co.thefoundry.nuke`, `eKnownHostNameNuke`, `OFX_EXTENSIONS_NUKE`, `nuke/fnOfxExtensions.h`.
- The 3D viewer navigation preset named Nuke stays, with its internal enums, beside Maya, Blender and Houdini. Comments around it are still cleaned.
- Vendored code is not read: `libs/`, `Tests/google-test`, `Gui/Resources/TabTabTab` and `Gui/Resources/etc/fonts`. Also skip the noise libraries' vendored files if any survive M357.P4.T1.
- `.gitignore` is not a comment-policy target, apart from the entries M357.P4.T3 removes.
- `Gui/ExportGroupTemplateDialog.cpp` uses `XXX` as UI placeholder text. It stays, and the CI check must not flag it.
- A comment-only change that alters code by mistake fails the task's verify; that file's edit is redone by a stronger agent, not patched by hand.

**Assumptions:**
- Unconfirmed: `tabtabtab_nuke_core.py` keeps its name, since it is the vendored module's own name and tests import it.
- Unconfirmed: `docs/decisions` keeps its Nuke mentions, since it is history and not product text.
- Unconfirmed: README and metainfo lose their "similar to" comparison naming Nuke and the other products alike.
- The scout read the code for the TODO verdicts but built and ran nothing, so each task rechecks its verdict before acting.

## Phase 357.1: Nuke text

- [ ] M357.P1.T1 — Remove Nuke from tooltips and plugin descriptions
  - files: Engine/Nodes/Transform/LensDistortion.{h,cpp}, Engine/Nodes/Transform/{Transform,Reformat}.h, Engine/Nodes/Merge/Merge.h, Engine/Nodes/Color/{Clamp,ColorCorrect,ColorLookup,HueCorrect}.h, Engine/Nodes/Generator/{Constant,CheckerBoard}.h, Engine/Nodes/Filter/Blur.h
  - approach: Say what each control does without the comparison; drop the "Nuke:" tooltip prefixes on K1, K2, Center, Squeeze and Asymmetric, the opticalenquiry.com/nuke URLs and the "Nuke Expand Edges" and "Colour Smear for Nuke" references. A credited algorithm keeps its credit under its author's or paper's name. Leave `kLensDistortionModelNuke` and the option label to M357.P2.T1.
  - verify: `git grep -Ii nuke -- Engine/Nodes/**/*.h` shows only comments and the lens identifiers; the release build passes.
  - size: M
- [ ] M357.P1.T2 — Remove Nuke from node docs
  - files: Engine/Nodes/Time/docs/*.md, Engine/Nodes/Merge/docs/Merge.md, Engine/Nodes/Filter/docs/{Defocus,ZDefocus,VectorBlur}.md, Engine/Nodes/Transform/docs/LensDistortion.md, Engine/Nodes/README.md
  - approach: Delete the port-audit tables, which are knob maps belonging in reports only, and reword any remaining sentence. Name the lens model Radial Asymmetric here.
  - verify: `git grep -Ii nuke -- 'Engine/Nodes/**/docs/*.md' Engine/Nodes/README.md` is empty.
  - size: M
- [ ] M357.P1.T3 — Remove the product comparison from README and metainfo
  - files: README.md, Gui/Resources/Metainfo/fr.natron.Natron.appdata.xml
  - approach: Describe Natron as a node-based video compositor with no named products, in both places. Leave the licence wording to M157 - Relicense To GPLv3 Or Later.
  - verify: `git grep -iE 'nuke|after effects|fusion' -- README.md Gui/Resources/Metainfo` is empty; the XML still parses with `xmllint --noout`.
  - size: M
- [ ] M357.P1.T4 — Remove Nuke from code comments
  - files: Engine/Nodes/Color/{Grade.cpp,ColorMathNode.cpp}, Engine/Nodes/Deep/DeepReformat.h, Engine/Nodes/Merge/Merge.h, Engine/Nodes/Filter/BokehKnobs.h, Engine/Gpu/Kernels/mergeops.slang, Gui/{ActionShortcuts.cpp,GuiApplicationManager10.cpp,ScaleSliderQWidget.cpp,GuiDefines.h,InfoViewerWidget.cpp,ViewerGL.cpp,HostOverlay.cpp}
  - approach: Comments only. Reword a real why without the name and delete the rest. Delete the commented-out `glScalef` and `glTranslatef` block in `Gui/ViewerGL.cpp`. Leave comments that document the OFX host names.
  - verify: the comment-only check (see `## Decisions`) passes on every touched file; the remaining `git grep -Ii nuke` hits in these files are identifiers from the Edge cases.
  - size: M

## Phase 357.2: Lens model rename

- [ ] M357.P2.T1 — Rename the Nuke lens model to Radial Asymmetric
  - files: Engine/Nodes/Transform/LensModels.{h,cpp}, Engine/Nodes/Transform/LensDistortion.{h,cpp}, Engine/Gpu/Kernels/lensmodels.slang, Tests/Native/NativeLensDistortion_Test.cpp, Tests/Native/LensModels_Test.cpp, Tests/gui/m132_lens_distortion.py, tools/bench/node_profile.py
  - approach: Label "Radial Asymmetric", script value `radialAsymmetric`. Rename `eModelNuke`, `NukeParams`, `makeNuke`, `kNukeK1` and friends, `coreNuke` and `kLensDistortionModelNuke`. The C++ and Slang constants change together. No old-value import. Golden images should not change: a golden diff means a rename went wrong.
  - verify: `git grep -Ii nuke -- Engine/Nodes/Transform Engine/Gpu/Kernels/lensmodels.slang Tests/Native/NativeLensDistortion_Test.cpp Tests/Native/LensModels_Test.cpp Tests/gui/m132_lens_distortion.py` is empty; both lens suites and `m132_lens_distortion.py` pass with no re-recorded references.
  - size: M

## Phase 357.3: Stale markers

- [ ] M357.P3.T1 — Delete the markers with nothing behind them
  - files: Engine/{AppInstance.h:137,EffectInstance.h:319,EffectInstance.h:3388,Knob.h:420,Node.h:105,RotoItem.h:87,TLSHolder.h:56,OSGLContext_x11.cpp:817,OfxEffectInstance.cpp:1295,ProcessHandler.cpp:282,Project.cpp:2426,RotoContextPrivate.h:2072,RotoContextPrivate.h:2130}, Gui/{KnobGui.h:79,NodeGui.h:155,CurveEditor.h:243,KnobGuiParametric.h:123,KnobUndoCommand.h:368,SequenceFileDialog.h:571,DocumentationManager.cpp:278-279,DopeSheetView.cpp:1509,NodeGraph10.cpp:313,SequenceFileDialog.cpp:1246,SequenceFileDialog.cpp:2575}, Gui/Resources/Stylesheets/mainstyle.qss:71
  - approach: Delete each marker comment. Also delete the commented-out `maskInput` at `Engine/TrackerContextPrivate.cpp:138`, the marker at `Engine/TrackerFrameAccessor.cpp:458` and the `// TODO: support interlaced data` tails at `Engine/OfxEffectInstance.cpp:2700,2911,3862`, keeping the code on those lines. For `enable_shared_from_this` markers, first check the class still derives from it. Keep the explanatory comment at `Engine/Project.cpp:2424`.
  - verify: the comment-only check passes; none of these lines still holds a marker.
  - size: M
- [ ] M357.P3.T2 — Keep the reason and drop the marker
  - files: Engine/OSGLContext_x11.cpp:647,963, Engine/OfxImageEffectInstance.h:264, Engine/RotoContextPrivate.h:1243, Tests/OSGLContext_Test.cpp:59,93
  - approach: Rewrite each as a plain why: Chromium and VirtualBox GL set no window bit on FBConfigs; some Mesa versions fail context creation with GLXBadProfileARB; knobs need a valid node pointer at creation; the filter table must stay in sync with `ofxsFilter.h`; the bundled gtest has no `GTEST_SKIP`.
  - verify: the comment-only check passes; `git grep -E '\b(TODO|FIXME|HACK)\b'` on these files is empty.
  - size: M
- [ ] M357.P3.T3 — Remove the duplicate cloneExtraData body
  - files: Engine/KnobTypes.cpp:725
  - approach: Have the five-argument overload call the three-argument one at `:677`; keep the override if the offset and range path dispatches to it.
  - verify: the build and the knob tests pass.
  - size: M
- [ ] M357.P3.T4 — Restore GL state after the histogram draw
  - files: Gui/Histogram.cpp:1035
  - approach: First check whether its callers rebind what it changes. If they do, delete the marker and say so in the report. If not, save and restore the bound VAO, buffer and texture around the draw.
  - verify: the viewer and histogram GUI checks pass; the comment-only check applies if only the marker went.
  - size: M
- [ ] M357.P3.T5 — Load plugins with host-like flags in the load check
  - files: tools/ci/verify_plugin_loads.cpp:15
  - approach: Read the dlopen flags in `ofxhBinary.cpp`. If they are not `RTLD_NOW|RTLD_LOCAL`, call `dlopen` directly with those flags and report `dlerror()`. If they match, delete the marker.
  - verify: the tool loads the shipped plugin set with no new failures, and exits non-zero on a deliberately unloadable `.ofx` file.
  - size: M
- [ ] M357.P3.T6 — Settle the three unsure markers
  - files: Engine/ParallelRenderArgs.cpp:1030, Engine/Transform.cpp:652,670, Gui/HostOverlay.cpp:82
  - approach: `ParallelRenderArgs`: check whether a draft flag is available at that point, and pass it if so. `Transform`: interlaced fields are dropped, so check what `Engine/EffectInstance.cpp:2663` passes as `fielded`; if nothing can pass true, delete the marker. `HostOverlay`: try `setValues` on a Roto overlay drag; if it works, use it and delete the comment; if not, rewrite the comment as the why.
  - verify: the marker is gone from all three files; the Roto and overlay GUI checks pass, and any behaviour change is named in the report.
  - size: M

## Phase 357.4: Leftovers

- [ ] M357.P4.T1 — Delete tools/spikes
  - files: tools/spikes/, .github/workflows/checks.yml:114-115
  - approach: Confirm with `git grep tools/spikes` that only the CI exclusions refer to it, then delete the tree and those exclusions. Stop and report if anything live depends on it. `tools/gpu-spike` is live and stays.
  - verify: `git grep tools/spikes` is empty; the `format` and `lint-ci` jobs pass.
  - size: M
- [ ] M357.P4.T2 — Remove non-Linux conditionals
  - files: the 69 files from `git grep -lE '(_WIN32|__APPLE__|Q_OS_(WIN|MAC|DARWIN)|__NATRON_(WIN32|OSX)__|__FreeBSD__)' -- Engine Gui Global App Renderer Tests HostSupport Serialization PythonBin Shiboken`
  - approach: Keep the Linux path and delete the other branches, along with includes and helpers only those branches used. Use `unifdef` where it fits and review the result. One coherent sweep, so the file count is expected. Leave vendored code alone.
  - verify: the grep above is empty, or each remaining hit is named as necessary; the release build, ctest and a GUI launch check pass.
  - size: L
- [ ] M357.P4.T3 — Remove non-Linux files and ignore entries
  - files: Gui/Resources/Images/natronIcon.icns, .gitignore, Tests/wmain.cpp, Tests/CMakeLists.txt:197
  - approach: Delete the `.icns` file if nothing refers to it, remove the Visual Studio and NuGet entries from `.gitignore`, and rename `Tests/wmain.cpp` to `Tests/main.cpp`, updating CMake.
  - verify: `git grep natronIcon.icns` is empty; the build configures and the tests build.
  - size: M
- [ ] M357.P4.T4 — Audit retired plugin IDs in live code
  - files: Engine/OfxEffectInstance.cpp:1378 onward, Engine/EffectInstance.h, Engine/Node.cpp, Engine/CMakeLists.txt, tools/ci/smoke_test.py, Gui/Resources/PyPlugs/
  - approach: Compare each `net.sf.*` reference, and each node ID a PyPlug creates, against the registered and retired plugin lists. Remove references to plugins that no longer exist, and fix a PyPlug that creates one. References to still-shipped OFX bundles stay.
  - verify: the report lists every ID checked with its outcome; the plugin list tests and `tools/ci/smoke_test.py` pass.
  - size: M

## Phase 357.5: Comment sweep

Each reader task runs several haiku readers over batches of files, per the user. Readers change comments only, against `~/.claude/skills/comment-policy`, and report code smells as `path:line: smell`, which the PM collects for M357.P5.T7.

- [ ] M357.P5.T1 — Read Engine core comments
  - files: Engine/, excluding Engine/Nodes, Engine/Gpu and Engine/OfxRetired
  - approach: Delete narration, change history, plan IDs and commented-out code, including the plan ID at `Engine/AppManager.cpp:2149`; keep copyright headers and every real why.
  - verify: the comment-only check passes on the whole diff; `check-comments.py --files` is clean on a sample of touched files.
  - size: S
- [ ] M357.P5.T2 — Read Engine/Nodes and Engine/Gpu comments
  - files: Engine/Nodes, Engine/Gpu
  - approach: As M357.P5.T1. Includes the Slang kernels, and the `qDebug` calls at `Engine/Nodes/IO/OiioWriteSupport.cpp:998` and in `Engine/Gpu` go in the smell list rather than being removed.
  - verify: as M357.P5.T1, with `slangc` compiling the touched kernels through the normal build.
  - size: S
- [ ] M357.P5.T3 — Read Gui comments
  - files: Gui/, excluding Gui/Resources
  - approach: As M357.P5.T1.
  - verify: as M357.P5.T1.
  - size: S
- [ ] M357.P5.T4 — Read test comments
  - files: Tests/, excluding Tests/fixtures and Tests/google-test
  - approach: As M357.P5.T1. Includes the plan ID at `Tests/Native/NativeAutoStraighten_Test.cpp:589`. Python tests are compared by AST in the comment-only check.
  - verify: as M357.P5.T1.
  - size: S
- [ ] M357.P5.T5 — Read tooling comments
  - files: tools/, .github/, cmake/, .git-hooks/
  - approach: As M357.P5.T1. Includes the plan IDs in `tools/ci/smoke_test.py:19,54,68,1064`, `tools/ci/local/Dockerfile:14`, `tools/ci/local/build.sh:63` and `tools/bench/gpu_region_bench.sh:2`. Shell and YAML have no comment-stripping proof, so the PM reads this diff.
  - verify: the PM read the diff and it changes comments only; `lint-ci` passes.
  - size: S
- [ ] M357.P5.T6 — Read the remaining first-party comments
  - files: Global/, App/, Renderer/, HostSupport/, Serialization/, PythonBin/, Shiboken/, Gui/Resources/PyPlugs/
  - approach: As M357.P5.T1.
  - verify: as M357.P5.T1.
  - size: S
- [ ] M357.P5.T7 — Verify the smell list and put it to the user
  - files: the collected reader reports; a list in this file's `## Decisions`
  - approach: A stronger read-only reviewer checks each reported smell against the code and drops false ones. Bring the rest to the user. What the user picks becomes new tasks in a new phase here, or new stubs; the rest is recorded as declined.
  - verify: every smell on the verified list has a recorded outcome.
  - size: M

## Phase 357.6: CI check

- [ ] M357.P6.T1 — Add a PR-diff check for Nuke, plan IDs and markers
  - files: tools/ci/check-added-lines.py, a test beside it, .github/workflows/checks.yml
  - approach: Fails only on lines the PR adds, against the merge base, so old code blocks nobody. Rules: `nuke` outside an allowlist (the Edge cases' OFX names and navigation preset, `Gui/Resources/TabTabTab`, `docs/decisions`, `libs`); plan IDs such as `M123` or `M12.P3.T4` in code comments; `TODO`, `FIXME`, `HACK` and `XXX` markers in code comments, where `Gui/ExportGroupTemplateDialog.cpp` placeholder text is not a comment. The comment policy's checker lives outside the repo, so port only the rules needed.
  - verify: the test shows each rule failing on an added line and passing on an unchanged line; a real PR run of `Checks` passes.
  - size: M

**Verification gate:** the release build, full ctest, debug PR CI and `m132_lens_distortion.py` pass, with no re-recorded references. `git grep -Ii nuke` outside `libs` hits only the allowlist. `git grep -E '\b(TODO|FIXME|HACK)\b'` in first-party code is empty. The non-Linux grep from M357.P4.T2 is empty or its hits are named. The verified smell list has an outcome for every item. The PR's `Checks` run includes the new check.

## Decisions

- 2026-11-05 — The lens model becomes Radial Asymmetric, `radialAsymmetric` (user): it describes the maths, and no backward compat is needed.
- 2026-11-05 — The navigation preset named Nuke stays (user): the presets name mouse conventions users know, and the list reads consistently.
- 2026-11-05 — Delete `tools/spikes` (user): finished experiments stay in git history.
- 2026-11-05 — The comment sweep reads the whole first-party tree with haiku readers, using grep where it can, and readers also surface code smells (user): comment deletion is mechanical, and a comment-only proof catches a reader that strays.
- 2026-11-05 — Comment-only proof: for C, C++ and Slang, `gcc -fpreprocessed -dD -E -P` output of the old and new file is identical; for Python, `ast.dump` is identical once docstrings are stripped. A mismatch fails the task.
- 2026-11-05 — Reported smells are not fixed by readers; they are verified, then the user picks (user): this keeps behaviour changes out of the comment diff.
- 2026-11-05 — Tracker track masks and interlaced OFX fields are dropped with no follow-up (user): the Tracker has no active mask control, so only comments and markers go.
- 2026-11-05 — Remove all non-Linux conditionals (user): the fork is Linux-only.
- 2026-11-05 — A CI check runs on PR diffs, not the full tree (user): old code never blocks an open lane.
- 2026-11-05 — The whole milestone waits until in-flight lanes have landed, with no exemption from Start Nothing New (user): it touches hundreds of files and would conflict with every open lane.
