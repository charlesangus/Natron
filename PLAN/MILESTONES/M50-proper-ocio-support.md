# Milestone 50: Proper OCIO support as a project property

Colour management becomes a property of the project, and all of it goes through OpenColorIO. Each project saves an OCIO config, which defaults to ACES 2.0 Studio (`ocio://studio-config-v4.0.0_aces-v2.0_ocio-v2.5`). It also saves a working space, per-file-type default colourspaces (8-bit, 16-bit, log and float, as in Nuke), and the default display and view for new viewers. That config populates the viewer's Display/View/Look menus and every colourspace menu on Read, Write and OCIO* nodes. Natron's built-in sRGB/Rec.709/BT1886/Linear LUTs are retired, together with the project's "LUT" page and the viewer's fixed colourspace combo. That covers the viewer, bit-depth conversion, node previews, colour swatches and the tracker overlay. This is a clean break: no old project is migrated and no legacy fixture is built.

**What the code does today (scouted 2026-10-02):**
- **Config selection is process-wide.** `Settings::tryLoadOpenColorIOConfig()` (`Engine/Settings.cpp` ~`:2190-2295`) resolves, in order:
  1. the `OCIO` env var;
  2. the preference `ocioConfig` choice (the built-in Studio URI, or a `share/OpenColorIO-Configs/<name>` directory);
  3. `ocioCustomConfigFile`.

  It then `qputenv("OCIO")`s the result and calls `AppManager::onOCIOConfigPathChanged`. That function writes an `[OCIO]` entry into each project's env-var table (`Project::onOCIOConfigPathChanged`, `Project.cpp` ~`:2721`). Changing the config needs a restart, which is why the preferences have the `warnOCIOChanged` and `startupCheckOCIO` knobs and `Settings::doOCIOStartupCheckIfNeeded`.
- **No host-side OFX colour extension exists.** Plugins learn the config through the env var. In openfx-io, `GenericOCIO::describeInContextInput/Output` (`IOSupport/GenericOCIO.cpp` ~`:1346-1496`) reads `getenv("OCIO")` once, at describe time. That sets the default of each instance's persistent `ocioConfigFile` string param. After that, each instance loads its config from that param (`loadConfig` ~`:486`). Natron supports rebuilding choice menus at runtime (`gHostIsNatron`), so setting `ocioConfigFile` on an instance already reloads its config and menus through `changedParam` (~`:945`), without a restart.
  - The exception is `OCIO/OCIOLookTransform.cpp:907`, which only reloads on `eChangeUserEdit`.
  - The OCIO* nodes (OCIOColorSpace, OCIODisplay, OCIOLookTransform, OCIOCDLTransform, OCIOFileTransform, OCIOLogConvert) live in **openfx-io** `OCIO/`, not openfx-misc. openfx-misc has no OCIO use.
  - openfx-arena's readers (ReadPSD, ReadMisc, ReadSVG, ReadPDF, ReadCDR, ReadKrita, OpenRaster) use openfx-io's `GenericReader`/`GenericOCIO` through arena's own `OpenFX-IO` submodule (pinned at `f30a6a8`).
  - SupportExt's `ofxsOGLUtilities` has no OCIO logic.
- **The env var cannot serve a per-project config.** It is process-wide, and Natron can have several AppInstances open at once. The per-instance `ocioConfigFile` param can.
- **The ASWF OFX 1.5 colour extension exists, but neither tree has it.** `ofxColour.h` defines `kOfxImageEffectPropOCIOConfig`, `kOfxImageEffectPropOCIODisplay/View` and `kOfxImageClipPropColourspace`. It is present in `build/assets/plugin-src/openfx-metadata/include/` but absent from Natron's `libs/OpenFX` (charlesangus/openfx-natron, OFX 1.4-based) and from openfx-io's openfx submodule. Natron already adds custom instance props through get-hooks (`kNatronOfxExtraCreatedPlanes`, `Engine/OfxImageEffectInstance.cpp:115`).
- **Host value changes look like user edits.** `OfxEffectInstance::natronValueChangedReasonToOfxValueChangedReason` (~`:3121`) maps `eValueChangedReasonNatronInternalEdited` to `kOfxChangeUserEdited`. So if the host set `ocioInputSpace` that way, GenericReader would wrongly mark it user-set (`ocioInputSpaceSet`, `GenericReader.cpp:2036`). Host-applied defaults must go through the plugin, or through the `eValueChangedReasonPluginEdited` reason.
- **How a Read picks its input colourspace now.** Each reader guesses a legacy name: ReadEXR gives `scene_linear`, ReadPNG gives `sRGB`/`Gamma2.2`/…, ReadFFmpeg gives `Rec709`/…. `colorSpaceName()` (`GenericOCIO.cpp` ~`:60-200`) maps that name onto the config by heuristics, so an sRGB PNG reads as `sRGB - Display`.
  - The Studio config has **no `default` role**, so every `ROLE_DEFAULT` fallback in GenericOCIO is dead.
  - Relevant Studio facts (queried in natron-dev):
    - Roles: `scene_linear` = ACEScg, `compositing_log` = ACEScct, `texture_paint`/`color_picking` = `sRGB Encoded Rec.709 (sRGB)`, `matte_paint` = ACEScct, `data` = Raw.
    - Displays: 9, default `sRGB - Display`. Views: `ACES 2.0 - SDR 100 nits (Rec.709)` (default), `Un-tone-mapped`, `Video (colorimetric)` and `Raw`.
    - Looks: one, `ACES 1.3 Reference Gamut Compression`.
    - File rules: EXR gives ACES2065-1, Default gives `sRGB - Display`.
    - `LegacyViewingPipeline` and GLSL 1.2 GPU shader generation are both available.
- **The viewer uses Natron's own LUTs.**
  - `Gui/ViewerTab.cpp:496-508` has a fixed combo (Linear(None)/sRGB/Rec.709/BT1886). It calls `ViewerGL::setLut` and `ViewerInstance::onColorSpaceChanged` (`ViewerTab10.cpp:69`), and its selection is saved as `ViewerData::colorSpace` (`Gui/ProjectGuiSerialization.h:117,160`).
  - The 8-bit texture path, which is the default (`Settings` `texturesBitDepth` = 8u), applies `Color::Lut` on the CPU with dithering in `scaleToTexture8bits` (`ViewerInstance.cpp` ~`:2240-2360`). The lut int is part of the texture cache key (`Engine/FrameKey.{h,cpp}` `_lut`).
  - The 32f path uses `Gui/Shaders.cpp` `fragRGB`, which has hard-coded sRGB/709/BT1886 branches and a gain → LUT → gamma order.
  - The colour picker (`ViewerGL::getColorAt` ~`:4243`) converts through the same LUTs.
- **The project "LUT" page drives bit-depth conversion.** Its knobs are `defaultColorSpace8u/16u/32f` (`Project.cpp` ~`:1103-1133`), read through `Project::getDefaultColorSpaceForBitDepth`. They drive every bit-depth conversion in `Image::convertToFormat` (`ImageConvert.cpp`):
  - `EffectInstance.cpp` (~`:2602-2960`, at least 6 sites);
  - `RotoPaint.cpp` (~`:1444-1580`);
  - `DiskCacheNode.cpp:227`;
  - the viewer's source colourspace (`ViewerInstance.cpp:1630`);
  - node previews (`Node.cpp` ~`:4191`, `to_func_srgb`).
- **Other LUT users:** `TrackerNodeInteract.cpp:1215` (`sRGBLut`), and `KnobGuiColor.cpp:385` and `ColorSelectorWidget` for swatches. `ViewerColorSpaceEnum` is exported to Python (`Engine/typesystem_engine.xml:223`).
- **Packaging.** The ACES config ships inside libOpenColorIO as a built-in (decision `2026-08-31-aces-via-ocio-builtin-config`). `tools/ci/local/fetch-assets.sh:80-120` still fetches the 2018 `OpenColorIO-Configs` tarball, and `tools/release/stage-bundle.sh:321-331` stages it if it is present. `tools/ci/local/test.sh:150` unsets `OCIO`, and `tools/ci/smoke_test.py:474` asserts that the default config is Studio.
- **Existing tests.**
  - `Tests/ProjectOCIO_Test.cpp` checks the "unresolved colourspace fails loudly" behaviour (`Project::reportUnresolvedOCIOColorSpaces`, `Project.cpp:338`). It uses a **legacy** fixture, `Tests/fixtures/ocio-old-config.ntp`.
  - There is no viewer test in ctest.
  - GuiTests run offscreen (`Tests/CMakeLists.txt:102`).

## Design questions (answered by the user 2026-10-02)

- **Q1: the `OCIO` env var overrides the project config, Nuke-style.** When `OCIO` is set, it forces the config for every project, new or loaded. The project's config knobs are disabled, with a tooltip that says why. A loaded project's saved config stays in the file but is ignored while `OCIO` is set. There is no warning-only path. Unsetting the variable and loading again restores the saved config.
- **Q2: bit-depth conversion is explicit, with no implicit colour transform.** At integer-only stages (plugins that can't process float, RotoPaint, the DiskCache), conversion is plain linear quantisation: scale, clamp, round. If the data needs a colourspace change, either the user converts beforehand (for example with an OCIOColorSpace node), or the plugin uses a colourspace-mapping option of its own. Natron never converts implicitly. P1.T1's survey of integer-only plugins exists only so the UAT and the docs can list the plugins users must convert around.
- **Q3: per-file-type defaults (Read input and Write output alike):**
  - 8-bit and 16-bit: `sRGB Encoded Rec.709 (sRGB)`;
  - log: `ACEScct` (the `compositing_log` role);
  - float: `ACEScg` (the `scene_linear` role, which equals the working space, so float EXRs read as a no-op).

  For 8-bit PNG output this supersedes the label chosen in `docs/decisions/2026-09-01-png-output-srgb-display.md`. The pixels are numerically identical; only the label goes from display-referred to scene-referred.

## Phase 50.1: Design

- [ ] M50.P1.T1 — Write the project-OCIO design doc, with a byte-only plugin survey
  - files: `.plan/PLAN/DESIGN/2026-10-02-project-ocio.md` (new, in the plan worktree; commit it there)
  - approach: the doc pins the following, and every later task cites it.
    - **Project knobs** (a "Color" page that replaces "LUT"):
      - `ocioConfig`: a choice listing every `OCIO::BuiltinConfigRegistry` entry plus "Custom"; the default is the Studio URI.
      - `ocioConfigFile`: a file knob, enabled only for Custom. It may use `[Project]`.
      - `workingSpace`: defaults to the colourspace the `scene_linear` role names.
      - `colorSpace8Bit`, `colorSpace16Bit`, `colorSpaceLog`, `colorSpaceFloat`: per Q3.
      - `viewerDisplay` and `viewerView`: default to the config's defaults.
      - Choice values are stored as colourspace **names** (ChoiceOption ids), not indices. When a config switch leaves a name unresolvable, fall back through the roles in this order: float → `scene_linear`, log → `compositing_log`, 8/16-bit → `texture_paint` → `color_picking` → `scene_linear`. Warn once per switch.
    - **Config precedence (Q1):**
      - When `OCIO` is set, it is the effective config for every project, new or loaded. The project's `ocioConfig`/`ocioConfigFile` knobs are disabled, with a tooltip naming the variable. A loaded project's saved config stays in the file, untouched, and comes back when the project is loaded without `OCIO`.
      - When `OCIO` is unset, a new project takes the preference, falling back to Studio, and a loaded project uses its saved config.
      - `Project::getOCIOConfigSource()` is the single resolution point for the effective config. Every consumer (plugins, the viewer, menus, the unresolved-colourspace check) goes through it.
      - The preference becomes "Default OCIO config for new projects", and the restart warning and the startup check are deleted.
      - At startup Natron still exports the preference default into `OCIO`, so plugin describe-time defaults and OIIO's internal colour config resolve.
      - The project's `[OCIO]` path variable follows the **project's** config directory. For a built-in URI it is absent.
    - **Plugin contract:**
      - The host implements `kOfxImageEffectPropOCIOConfig` as an instance get-hook that returns the project config.
      - It adds Natron instance props `NatronOfxImageEffectPropOCIOWorkingColourspace` and `NatronOfxImageEffectPropOCIOFileColourspaces` (4 strings, in the order 8-bit, 16-bit, log, float).
      - It also writes the project config into each node's `ocioConfigFile` with the plugin-edit reason, at creation and on every config change. That param is the change channel, and the plugin hides it once the host prop exists.
      - The plugin reads the working space and file defaults only when it guesses for a **new** filename. Changing a project default never edits existing nodes.
      - Reader file category, Nuke-style: float/half → float. A legacy log guess (Cineon/KodakLog/ADX) → log. Otherwise the file bit depth gives 8-bit or 16-bit. Metadata-derived display names (PNG gAMA, FFmpeg trc) are dropped.
    - **Viewer pipeline:** OCIO `LegacyViewingPipeline`.
      - Gain becomes the linear CC (exposure in the working space). Then the look, then `DisplayViewTransform(workingSpace → display/view)`. Gamma becomes the display CC, as a dynamic property, so no shader rebuild is needed.
      - On the 8-bit path, the CPU processor runs in F32 and Natron's existing dither does the quantisation.
      - On the 32f path, OCIO generates a GLSL 1.2 shader and its LUT textures are uploaded.
      - The texture cache key carries `processor->getCacheID()`.
      - Each viewer has its own Display/View/Look menus, seeded from the project defaults and saved in the viewer's GUI state.
      - The picker's non-linear readout uses the CPU processor.
    - **Retirement map:** the replacement for every `Color::Lut`/`LutManager`/`ViewerColorSpaceEnum` site listed above.
      - Swatches and the tracker overlay go working → `color_picking`.
      - Node previews use the project default display/view.
      - Bit-depth conversion is plain linear quantisation (Q2).
      - `Color::floatToInt`/`hsv_to_rgb` stay as utilities.
    - **Integer-only plugin survey:** list the shipped plugins (openfx-io, misc, arena) whose `kOfxImageEffectPropSupportedPixelDepths` lacks float. The list feeds the P6.T2 UAT notes and the docs, so users know which plugins to convert around. It changes no design ruling.
    - **Clean break:** delete `ocio-old-config.ntp`, and with it every test that loads a pre-M50 project.
  - verify: PM review. The doc records the user's Q1–Q3 answers, and the survey list is in it.
  - size: M

## Phase 50.2: Engine foundation

- [ ] M50.P2.T1 — Add a ProjectColorManagement service that owns the project's OCIO config
  - files: `Engine/ProjectColorManagement.h` (new), `Engine/ProjectColorManagement.cpp` (new), `Tests/ProjectColorManagement_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach:
    - The class resolves a config source (a built-in `ocio://` URI, or a file path resolved against a base directory) to a `ConstConfigRcPtr` under a mutex, and returns a typed error on failure.
    - It exposes:
      - lists of colourspaces, roles, displays, views per display, and looks;
      - `resolveRoleOrName()`;
      - a cached `getDisplayProcessor(src, display, view, look, exposure, gamma)` that returns the CPU processor and the cache ID;
      - `getConversionProcessor(src, dst)`;
      - a `configChanged` callback list.

    It must be thread-safe for render threads (const config, plus a processor cache keyed by a string). It has no knob dependencies, and Project wires it in P2.T2.
  - verify: `ctest -R ProjectColorManagement` passes these cases:
    - The Studio URI resolves, `scene_linear` resolves to ACEScg, and the default display/view are `sRGB - Display` / `ACES 2.0 - SDR 100 nits (Rec.709)`.
    - A custom config written to a temp dir with `Config::CreateRaw()->serialize` resolves, and so does its relative path.
    - A bad path gives an error, not a throw.
    - The display processor maps ACEScg 0.18 to the same values as a directly built OCIO processor (EXPECT_NEAR 1e-5).
    - Two lookups with equal arguments return the same cache ID.
  - size: M

- [ ] M50.P2.T2 — Add the project Color page: config, working space, file defaults and viewer defaults
  - files: `Engine/Project.cpp`, `Engine/Project.h`, `Engine/ProjectPrivate.h`, `Engine/ProjectPrivate.cpp`, `Tests/ProjectColorManagement_Test.cpp`
  - approach:
    - `ProjectPrivate` owns a `ProjectColorManagement`. Add the design-doc knobs on a new "Color" page. Leave the old "LUT" page in place until P5.T2.
    - When `ocioConfig` or `ocioConfigFile` changes (`Project::onKnobValueChanged`):
      - reload the config;
      - repopulate the colourspace, display and view choices, keeping values by name and applying the role fallbacks;
      - warn once when a fallback fires;
      - fire `configChanged`.
    - `ocioConfigFile` is enabled only for Custom.
    - Add accessors `getOCIOConfigSource()`, `getWorkingColorSpace()`, `getFileColorSpace(category)`, `getDefaultDisplayView()` and `getColorManagement()`. `getOCIOConfigSource()` returns the effective config. Here it is the knob value; P2.T3 adds the `OCIO` override inside it, so no caller reads the config knobs directly.
  - verify: `ctest -R ProjectColorManagement` passes these cases:
    - A new project has Studio, ACEScg, the Q3 defaults and the default display/view.
    - Setting every knob, saving and reloading round-trips the values.
    - Switching to `ocio://cg-config-v4.0.0_aces-v2.0_ocio-v2.5` repopulates the menus. The working space survives (CG also has ACEScg), and a Studio-only name falls back to the role with a warning.
    - Custom pointing at a temp raw config lists `raw`.

    `ctest -R ProjectSerialization` stays green.
  - size: L

- [ ] M50.P2.T3 — Let the OCIO env var override every project's config, make the preference the new-project default, and drop restart-era OCIO plumbing
  - files: `Engine/Settings.cpp`, `Engine/Settings.h`, `Gui/GuiAppInstance.cpp`, `Engine/Project.cpp`, `Tests/ProjectColorManagement_Test.cpp`
  - approach:
    - Relabel `ocioConfig`/`ocioCustomConfigFile` as "Default OpenColorIO config for new projects", listing the same built-in registry as P2.T2. When `OCIO` is set, disable these preference knobs too, with the same tooltip.
    - Delete `warnOCIOChanged`, `startupCheckOCIO`, `doOCIOStartupCheckIfNeeded` (and its call at `GuiAppInstance.cpp:340`), the `share/OpenColorIO-Configs` directory scan (`getDefaultOcioConfigPaths`), and the restart dialogs.
    - Add `Settings::getOCIOEnvOverride()`, which returns the `OCIO` value captured at startup before Natron exports anything, or empty. Add `Settings::getDefaultOCIOConfigSourceForNewProjects()`, which returns the preference.
    - In `Project` (Q1):
      - `getOCIOConfigSource()` returns the env override when it is set, otherwise the knob value.
      - On project creation and load with the override set, disable `ocioConfig`/`ocioConfigFile`, with the tooltip "Overridden by the OCIO environment variable (<value>)". The saved knob values are kept, untouched, and written back on save.
      - Load the effective config into `ProjectColorManagement`.
      - There is no warning-only path.
    - Keep exporting `OCIO` at startup: the override when it is set, otherwise the preference.
  - verify: `ctest -R ProjectColorManagement` passes these cases:
    - With `OCIO` unset, a new project takes the preference value.
    - With `OCIO` set (via `qputenv` before the override is captured, with a test hook to re-capture it), a new project uses the env config and both config knobs are disabled with the tooltip.
    - A project saved with the CG config, loaded with `OCIO` = the Studio URI, uses Studio, has its knobs disabled, and still saves CG in the file.
    - Unsetting `OCIO`, re-capturing and loading the same file again restores CG with the knobs enabled.

    `tools/ci/local/test.sh smoke debug` stays green (the default-config check).
  - size: M

- [ ] M50.P2.T4 — Point the project's [OCIO] path variable at the project config, and remove the AppManager broadcast
  - files: `Engine/Project.cpp`, `Engine/AppManager.cpp`, `Engine/AppManager.h`, `Engine/AppInstance.cpp`, `Engine/AppInstance.h`
  - approach:
    - Delete `AppManager::onOCIOConfigPathChanged`, `getOCIOConfigPath` and `currentOCIOConfigPath` (in `AppManagerPrivate.h`), and `AppInstance::onOCIOConfigPathChanged`.
    - `Project::onOCIOConfigPathChanged` is driven by the project's own config change. It sets `[OCIO]` to the custom file's directory, or removes the entry for a built-in URI. It keeps `fixRelativeFilePaths`.
    - Update the call sites at `Project.cpp:969`, `:2182` and `ProjectPrivate.cpp:197`.
  - verify: `ctest -R 'ProjectColorManagement|ProjectSerialization'` passes this new case: a custom config in `<tmp>/cfg/config.ocio` gives `[OCIO]` = `<tmp>/cfg`, and switching back to Studio removes it. The debug build is clean with no reference to the deleted AppManager API.
  - size: M

## Phase 50.3: OFX plugins follow the project config

- [ ] M50.P3.T1 — Expose the project config to OFX instances through kOfxImageEffectPropOCIOConfig and Natron colour props
  - files: `libs/OpenFX` (submodule charlesangus/openfx-natron: add `include/ofxColour.h` from ASWF openfx, and add `NatronOfxImageEffectPropOCIOWorkingColourspace` and `NatronOfxImageEffectPropOCIOFileColourspaces` to `include/ofxNatron.h`), `Engine/OfxImageEffectInstance.cpp`, `Engine/OfxImageEffectInstance.h`, `Tests/ProjectOCIOPlugins_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach:
    - Register get-hooks next to `kNatronOfxExtraCreatedPlanes` (`:115`). They return the current project's config source, working space and the 4 file colourspaces, always read live from the node's `getApp()->getProject()`.
    - Push the fork submodule commit, then bump the pin.
  - verify: `ctest -R ProjectOCIOPlugins` passes these cases:
    - A created ReadOIIO instance's property set returns the Studio URI, `ACEScg` and the 4 defaults.
    - After switching the project to CG, the same instance returns the CG URI with no node recreation.
  - size: M

- [ ] M50.P3.T2 — Push the project config into every OCIO node's ocioConfigFile, and check colourspaces against it
  - files: `Engine/Project.cpp`, `Engine/Node.cpp`, `Engine/ReadNode.cpp`, `Engine/WriteNode.cpp`, `Tests/ProjectOCIO_Test.cpp`
  - approach:
    - After a node with an `ocioConfigFile` knob is created, and for every such node on `configChanged`, set the knob to the **effective** config (`getOCIOConfigSource()`, which is the `OCIO` override when one is set) with `eValueChangedReasonPluginEdited`. Make the knob secret on the host side. On load, the push runs after node restore, so a node's saved `ocioConfigFile` never wins over the effective config.
    - In the ReadNode/WriteNode wrappers, forward the same push to the embedded plugin (they keep `ocioConfigFile` at `ReadNode.cpp:151` and `WriteNode.cpp:143`).
    - Rewrite `reportUnresolvedOCIOColorSpaces` (`Project.cpp:338`) to check against the project config instead of `CreateFromEnv`, and to run on load **and** on config change.
    - Delete `Tests/fixtures/ocio-old-config.ntp`. Rebuild `ProjectOCIO_Test` on projects created in the test.
  - verify: `ctest -R ProjectOCIO_` passes these cases:
    - A new Read, Write and OCIOColorSpace each carry the project config in `ocioConfigFile`, and the knob is secret.
    - Switching to CG updates all three, and OCIOColorSpace's `ocioInputSpaceIndex` entries are CG names.
    - A node holding `Camera Rec.709` (Studio-only) goes into the persistent error state after the switch to CG, and clears when switched back.
    - A saved, reloaded project keeps the config and the clean state.
    - A CG project loaded with `OCIO` = the Studio URI gives every OCIO node the Studio config, and the unresolved-colourspace check runs against Studio.
  - size: L

- [ ] M50.P3.T3 — openfx-io fork: GenericOCIO and the OCIO nodes take their config from the host
  - files (charlesangus/openfx-io): `IOSupport/GenericOCIO.h`, `IOSupport/GenericOCIO.cpp`, `OCIO/OCIOLookTransform.cpp`, `OCIO/OCIODisplay.cpp`, `OCIO/OCIOLogConvert.cpp`
  - approach:
    - Add guarded defines for `kOfxImageEffectPropOCIOConfig` and the two Natron props (`#ifndef`, with a pointer to ofxColour.h/ofxNatron.h).
    - At instance construction, when the host prop is present, load the config from it and make `ocioConfigFile` secret. Otherwise fall back to the param, as today.
    - Reload on every `ocioConfigFile` change whose reason is not `eChangeTime`; this fixes OCIOLookTransform's UserEdit-only gate at `:907`. Do the same in OCIODisplay (display/view menus) and OCIOLogConvert. OCIOCDLTransform and OCIOFileTransform use GenericOCIO and need no change.
    - Replace the dead `ROLE_DEFAULT` fallbacks with `existingColorSpaceOrFallback`.
    - Keep the commit self-contained, as decision `2026-09-01-no-upstream-pr-for-ocio-sentinel` requires.
  - verify: the fork builds in natron-dev (`build/build-m65-plugins.sh`-style one-off). After P3.T5 pins it, `ctest -R ProjectOCIO_` additionally passes: OCIODisplay's `displayIndex` entries rebuild to CG displays after a project switch, and OCIOLookTransform's look menu rebuilds.
  - size: L

- [ ] M50.P3.T4 — openfx-io fork: new Reads and Writes take the working space and per-file-type defaults from the host
  - files (charlesangus/openfx-io): `IOSupport/GenericReader.cpp`, `IOSupport/GenericReader.h`, `IOSupport/GenericWriter.cpp`, `IOSupport/GenericWriter.h`
  - approach:
    - In GenericReader's guess path (~`:1795-1855`), when the host provides the file colourspaces and `ocioInputSpaceSet` is false, choose the category per the design doc and set the input space from the host list.
      - The OCIO filename rule still wins when it matches.
      - Set the output space to the host working space.
    - In GenericWriter: the input space is the working space and the output space is the category default for the chosen format and bit depth, unless `ocioOutputSpaceSet`.
    - Without the host props, the old behaviour stays, so other hosts are unaffected.
  - verify: the fork builds in natron-dev. Behaviour is verified host-side in P3.T6.
  - size: L

- [ ] M50.P3.T5 — Pin the openfx-io fork and carry it into openfx-arena
  - files: `tools/ci/local/fetch-assets.sh` (`OPENFX_IO_REF`, `OPENFX_ARENA_REF` and their delta comments), charlesangus/openfx-arena (`OpenFX-IO` submodule bump)
  - approach:
    - Bump arena's `OpenFX-IO` submodule to the P3.T3+P3.T4 commit and push.
    - Re-pin both refs, and record the deltas in the comment blocks (`:134-150`, `:201-212`).
    - Drop the `OpenColorIO-Configs` tarball fetch (`:80-120`) and its staging in `tools/release/stage-bundle.sh:321-331`. Nothing reads it any more after P2.T3.
  - verify: `tools/ci/local/fetch-assets.sh` rebuilds the plugins. `PLUGINS_WANT` shows the new refs. `build/assets/OpenColorIO-Configs` is no longer created, and the smoke test stays green.
  - size: M

- [ ] M50.P3.T6 — Test the Read and Write defaults against the project settings
  - files: `Tests/ProjectOCIODefaults_Test.cpp` (new), `Tests/CMakeLists.txt`, `Tests/fixtures/` (reuse the existing flat EXR/PNG fixtures; add an 8-bit PNG and a 16-bit PNG only if none exists)
  - approach: drive a real ReadOIIO/ReadPNG/ReadEXR and WritePNG/WriteEXR through the Natron Read/Write wrappers in the test project.
  - verify: `ctest -R ProjectOCIODefaults` passes these cases:
    - An 8-bit PNG reads as `sRGB Encoded Rec.709 (sRGB)`, a 16-bit PNG as the 16-bit default, and a float EXR as ACEScg. The output space is the working space.
    - Changing `colorSpace8Bit` to `Gamma 2.2 Encoded Rec.709` affects a **new** PNG Read and leaves the existing one alone.
    - A user-set input space survives a filename change, and `ocioInputSpaceSet` stays false after host-driven defaults.
    - WritePNG's output space is the 8-bit default.
    - A pixel probe of 0.18 ACEScg written to an 8-bit PNG gives 118 ±1.
  - size: M

## Phase 50.4: Viewer display through OCIO

- [ ] M50.P4.T1 — Run the viewer's CPU (8-bit texture) path through the OCIO display processor
  - files: `Engine/ViewerInstance.cpp`, `Engine/ViewerInstance.h`, `Engine/ViewerInstancePrivate.h`, `Engine/UpdateViewerParams.h`, `Engine/FrameKey.{h,cpp}` with `Engine/FrameEntrySerialization.h`
  - approach:
    - Replace `viewerParamsLut`/`onColorSpaceChanged` with `setDisplayTransform(display, view, look)` (thread-safe; it triggers a re-render).
    - In `scaleToTexture8bits`, apply `getDisplayProcessor(workingSpace, …)` on F32 scanlines, then the existing dither.
      - Gain becomes the exposure in the pipeline; gamma becomes the display CC.
      - The matte overlay is converted through the same processor.
    - Drop the `srcColorSpace` lookups (`:1630`) and `lutFromColorspace`.
    - `FrameKey` replaces `int _lut` with the hashed processor cache ID.
    - Factor the per-scanline transform into a free function so a test can drive it without GL.
  - verify:
    - A new `Tests/ViewerDisplayTransform_Test.cpp` (and its `Tests/CMakeLists.txt` entry) passes these cases:
      - ACEScg 0.18 through `sRGB - Display`/`ACES 2.0 - SDR 100 nits (Rec.709)` gives 89 ±1 (the code value measured in the PNG decision).
      - `Un-tone-mapped` gives 118 ±1, and `Raw` gives 46 ±1.
      - Gain 2 equals exposure +1 stop.
      - Different views give different FrameKey hashes.
    - A one-off timing logged in the test shows a 1920×1080 frame transformed in under 150 ms on the container's threads. Record the figure; it doesn't fail the test.
  - size: L

- [ ] M50.P4.T2 — Generate the 32f viewer shader from OCIO and upload its LUT textures
  - files: `Gui/ViewerGL.cpp`, `Gui/ViewerGL.h`, `Gui/ViewerGLPrivate.cpp`, `Gui/ViewerGLPrivate.h`, `Gui/Shaders.cpp`/`Gui/Shaders.h`
  - approach:
    - Replace `fragRGB`'s LUT branches.
      - From the processor, build an `OCIO::GpuShaderDesc` (`GPU_LANGUAGE_GLSL_1_2`, function `OCIODisplay`, a resource prefix).
      - Compose `main()` as: sample Tex, call `OCIODisplay`, write gl_FragColor.
      - Upload the 1D/2D/3D textures to units ≥1 and set the uniforms. Exposure and gamma are dynamic properties, so they cost no rebuild.
    - Rebuild the program only when the cache ID changes.
    - `ViewerGL::setLut` becomes `setDisplayTransform(display, view, look)`, which both texture indices (A/B wipe) share.
    - `getColorAtInternal` drops its `Color::Lut` arguments. Its non-linear readout uses the CPU processor.
  - verify: the Xvfb script `build/m50-gui/viewer_32f.py` (recipe `build/m61-gui/run-gui.sh`) sets `texturesBitDepth`=32f and views a constant 0.18 ACEScg. It grabs the viewer and checks that the centre pixel is 89 ±2, and within 2 code values of the 8-bit path for the same view. The script logs no GL shader compile error.
  - size: L

- [ ] M50.P4.T3 — Replace the viewer colourspace combo with Display/View/Look menus
  - files: `Gui/ViewerTab.cpp`, `Gui/ViewerTab.h`, `Gui/ViewerTab10.cpp`, `Gui/ViewerTab30.cpp`, `Gui/ViewerTabPrivate.h` (the serialization change is in P4.T4)
  - approach:
    - Swap `viewerColorSpace` (`ViewerTab.cpp:496-508`) for three `ComboBox`es:
      - Display;
      - View, which repopulates per display;
      - Look, which has "None" plus the config's looks.
    - Seed them from the project's `viewerDisplay`/`viewerView`, and repopulate them on the project's `configChanged`, keeping the values by name and otherwise using the defaults.
    - Each change calls `ViewerGL::setDisplayTransform` and `ViewerInstance::setDisplayTransform`.
    - Replace `getColorSpace`/`setColorSpace` with `getDisplayTransform`/`setDisplayTransform` (strings).
    - Update the gamma tooltip: gamma is applied after the display transform.
  - verify: the Xvfb script `build/m50-gui/viewer_menus.py`:
    - grabs the viewer toolbar with Display = `sRGB - Display`, View = `ACES 2.0 - SDR 100 nits (Rec.709)` and Look = None;
    - switches the project to CG and grabs again (the menus are repopulated);
    - picks `Raw` and checks that the image changes.

    Share the screenshots with the user before sign-off.
  - size: M

- [ ] M50.P4.T4 — Save each viewer's display, view and look in the project GUI state
  - files: `Gui/ProjectGuiSerialization.h`, `Gui/ProjectGuiSerialization.cpp`, `Gui/ProjectGui.cpp`, `Tests/ProjectColorManagement_Test.cpp` (or a GuiTests case if the GUI state can't be reached from the Engine tests)
  - approach:
    - Replace `ViewerData::colorSpace` with `display`, `view` and `look`. Bump the boost serialization version.
    - On restore (`ProjectGui.cpp:375`), apply the saved values, falling back to the project defaults when a name doesn't resolve.
    - Don't read the retired `ColorSpace` field.
  - verify: a GuiTests (offscreen) or Engine test round-trips `ViewerData` with a non-default view and look. The P4.T3 Xvfb script additionally saves the project, reloads it, and checks that the menus keep `Un-tone-mapped`.
  - size: M

## Phase 50.5: Retire the built-in LUTs

- [ ] M50.P5.T1 — Make bit-depth conversion colour-neutral (Q2) and drop colourspaces from convertToFormat
  - files: `Engine/Image.h`, `Engine/ImageConvert.cpp`, `Engine/EffectInstance.cpp`, `Engine/RotoPaint.cpp`, `Engine/DiskCacheNode.cpp`
  - approach:
    - Remove the `srcColorSpace`/`dstColorSpace` parameters from `convertToFormat*` and the private converters. Remove `lutFromColorspace` (`ImageConvert.cpp:107`).
    - Integer↔float conversion becomes plain linear quantisation: scale, clamp, round. Natron never applies an implicit colour transform. A colourspace change is the user's job (an OCIOColorSpace node upstream) or the plugin's own mapping option (Q2).
    - Update every caller: about 6 in EffectInstance (~`:2602-2960`), 3 in RotoPaint (~`:1444-1580`) and DiskCacheNode `:227`.
    - Follow-up, out of scope: a hint on integer-only nodes saying that pixels arrive linear-quantised. It would need a new path in `NodeDocumentation.cpp` or the node info panel, which is more than a small change. For now, P6.T2's UAT notes list the integer-only plugins from the P1.T1 survey.
  - verify: a new `Tests/ImageConvert_Test.cpp` (and its `Tests/CMakeLists.txt` entry) passes: float 0.5 → byte gives 128 → 0.50196; float → short → float round-trips within 1/65535; values above 1 clamp. The full debug ctest is green, including RotoPaint and DiskCache-touching tests.
  - size: L

- [ ] M50.P5.T2 — Remove the project LUT page, and render node previews through the default display/view
  - files: `Engine/Project.cpp`, `Engine/Project.h`, `Engine/ProjectPrivate.h`, `Engine/AppInstance.cpp`/`Engine/AppInstance.h`, `Engine/Node.cpp`
  - approach:
    - Delete `colorSpace8u/16u/32f`, the "LUT" page, `colorspaceParamIndexToEnum`, `getDefaultColorSpaceForBitDepth` (both in Project and AppInstance) and the `onKnobValueChanged` branch at `Project.cpp:1829`.
    - `Node::makePreviewImage` (~`:3990-4191`) replaces `to_func_srgb` with the project's `getDisplayProcessor(working, viewerDisplay, viewerView)` on its float scanlines.
  - verify: the debug build is clean. `grep -rn getDefaultColorSpaceForBitDepth Engine Gui` is empty. A new case in `ProjectColorManagement_Test` renders the preview of a constant 0.18 ACEScg node and gets the ACES SDR value (89 ±2) at its centre.
  - size: M

- [ ] M50.P5.T3 — Convert colour swatches, the colour selector and the tracker overlay through OCIO
  - files: `Gui/KnobGuiColor.cpp`, `Gui/ColorSelectorWidget.cpp`, `Engine/TrackerNodeInteract.cpp`, `Engine/ProjectColorManagement.{h,cpp}` (a helper `workingToColorPicking(r,g,b)`)
  - approach:
    - Swatches (`KnobGuiColor.cpp:385`) and the selector's displayed colour go working → `color_picking` role through a cached conversion processor. Picked values convert back through the inverse.
    - `TrackerNodeInteract.cpp:1215` drops `sRGBLut` for the same helper.
  - verify: `ctest -R ProjectColorManagement` covers the helper: ACEScg 0.18 maps to `sRGB Encoded Rec.709 (sRGB)` ≈ 0.4613 for Studio, and the round-trip holds within 1e-4. The Xvfb script `build/m50-gui/project_color_page.py` grabs the Project Settings "Color" page and a Grade's colour swatch. Share the screenshots with the user.
  - size: M

- [ ] M50.P5.T4 — Delete the LUT classes and ViewerColorSpaceEnum
  - files: `Engine/Lut.h`, `Engine/Lut.cpp`, `Global/Enums.h`, `Engine/typesystem_engine.xml`, `Tests/Lut_Test.cpp`
  - approach:
    - Remove `LutManager`, `Lut` and every named LUT, keeping only the free helpers still used (`floatToInt`, `hsv_to_rgb`, …).
    - Remove `ViewerColorSpaceEnum` and its typesystem entry.
    - Cut `Lut_Test` down to the surviving helpers.
  - verify: the debug and release builds are clean (the Shiboken wrappers regenerate). `grep -rnE 'LutManager|ViewerColorSpaceEnum|eViewerColorSpace' Engine Gui Global Tests` finds nothing. The full debug ctest is green.
  - size: M

## Phase 50.6: Checkpoint

- [ ] M50.P6.T1 — Publish the project-OCIO decision
  - files: `.plan/PLAN/DECISIONS/2026-10-02-project-ocio-full-replacement.md` (new), `.plan/PLAN/DECISIONS/INDEX.md`
  - approach: record:
    - the project config as the single source of truth;
    - the Nuke-style `OCIO` override, with its disabled knobs and the saved config kept but ignored (Q1);
    - the plugin contract (host prop plus the hidden `ocioConfigFile` change channel, and defaults applied only to new nodes);
    - explicit, linear-only bit-depth conversion (Q2), and the Q3 defaults, including the PNG-label supersession;
    - LUT retirement;
    - the clean break.

    The PM publishes it to `docs/decisions/` at the gate.
  - verify: the file exists and INDEX links it.
  - size: S

- [ ] M50.P6.T2 — Package the release AppImage for the user checkpoint
  - files: `build/appimages/M50-<sha>.AppImage`, `build/appimages/M50-uat.md`
  - approach: build with the release `package.sh`. The UAT script walks through:
    - a new project's Color page defaults;
    - the viewer's Display/View/Look menus and the switch between ACES SDR, Un-tone-mapped and Raw;
    - gain and gamma on the 8-bit and 32f texture settings;
    - switching the project to the CG config, with the menus on Read, Write, OCIOColorSpace and OCIODisplay and on the viewer all repopulating, and the Studio-only colourspace error;
    - a custom config file;
    - PNG/EXR Reads picking up the 8-bit/float defaults;
    - a PNG Write;
    - node previews and colour swatches;
    - save, reload and reopen, keeping the config and the viewer state;
    - launching with `OCIO` set: the env config is used, the project's config knobs are disabled with the tooltip, and relaunching without `OCIO` restores the saved config;
    - a note listing the integer-only plugins from the P1.T1 survey, which receive linear-quantised pixels and need an explicit conversion around them.

    Launch-check it with `build/appimages/run-launch-check.sh`. The UAT itself is deferred to the parcel UAT.
  - verify: the AppImage launches clean through the launch check, and `M50-uat.md` lists the steps above.
  - size: M

**Verification gate:** all of the following hold:
- `tools/ci/local/test.sh ctest debug` and `smoke debug` are green. That covers ProjectColorManagement, ProjectOCIO_, ProjectOCIOPlugins, ProjectOCIODefaults, ViewerDisplayTransform, ImageConvert, Lut, ProjectSerialization and the existing suite.
- `grep -rnE 'LutManager|ViewerColorSpaceEnum|defaultColorSpace(8u|16u|32f)|doOCIOStartupCheckIfNeeded' Engine Gui Global` finds nothing.
- The openfx-io and openfx-arena forks are pinned in `fetch-assets.sh`.
- The user has approved the P4.T2, P4.T3 and P5.T3 screenshots and signed off the P6.T2 UAT (in the parcel UAT).
- The decision is published.

Execution notes:
- **Branching:** stacked on M37. Branch `milestone/m50-proper-ocio-support` off `milestone/m37-channel-management-nodes` and open the PR against it (`DECISIONS/2026-09-22-stacked-milestone-prs.md`). M60 stacks on M50 in the parcel.
- **Builds:** the `natron-dev` container is single-tenant. Implementers edit in parallel and don't build. Each batch gets one detached build plus ctest (setsid+nohup with a fresh `.done` marker). Check that `pgrep -x ninja` is 0 before relaunching, and never pgrep-wait on a build. New `Engine/*.cpp` files are globbed, so reconfigure (re-run cmake) in the batch that adds them.
- **Plugin builds:** the fork tasks P3.T3 and P3.T4 build the plugins in their own detached container job, never alongside a Natron build. Run P3.T5's `fetch-assets.sh` rebuild as that batch's single build.
- **Tests:** the debug build defines NDEBUG, so tests use EXPECT/ASSERT, never assert(). `test.sh` unsets `OCIO`. Tests that need it set use `qputenv`/`qunsetenv` and restore it.
- **GUI checks:** run under Xvfb with the recipe in `build/m61-gui/run-gui.sh`. Scripts and fixtures go under `build/m50-gui/`. Pre-seed `checkForUpdates=false` in the fresh HOME. Share the screenshots before sign-off.
- **Batches.** Tasks within a batch touch disjoint files:
  - B1: P1.T1
  - B2: P2.T1, P3.T3 (fork)
  - B3: P2.T2, P3.T1, P3.T4 (fork)
  - B4: P2.T3, P3.T5, P4.T1
  - B5: P2.T4, P4.T2
  - B6: P3.T2, P4.T3
  - B7: P3.T6, P4.T4, P5.T1
  - B8: P5.T2, P5.T3
  - B9: P5.T4, P6.T1
  - B10: P6.T2
- **Shared files:**
  - `Engine/Project.cpp` is touched by P2.T2, P2.T3, P2.T4, P3.T2 and P5.T2, each in a different batch.
  - `Tests/CMakeLists.txt` is touched by P2.T1, P3.T1, P3.T6, P4.T1 and P5.T1, at most one per batch except B7 (P3.T6 and P5.T1): append one line each and rebase carefully.
  - `ProjectColorManagement.{h,cpp}` is created by P2.T1 and extended by P5.T3.
- **Line numbers** are from 2026-10-02 on M66's tip. M37 will move `EffectInstance.cpp`/`Node.cpp` cites, so re-grep by function name.

## Decisions

- 2026-10-02 — Scope (user): full replacement. A project OCIO config property (saved in the project, defaulting to ACES 2.0 Studio) drives the viewer's display/view/look menus and every Read/Write/OCIO node's colourspace lists; a project working space and per-file-type default input colourspaces (Nuke-style) apply to new Read/Write nodes; Natron's built-in sRGB/Rec709/Linear LUTs are retired so every colour transform goes through OCIO. No backward compatibility with old projects' colourspace settings.
- 2026-10-02 — Design questions (user):
  - **Q1:** Nuke-style override. A set `OCIO` env var forces the config for every project, new or loaded. The project's config knobs are disabled, with a tooltip saying why. A loaded project's saved config stays in the file but is ignored while `OCIO` is set; there is no warning-only path.
  - **Q2:** explicit handling, no magic. Integer-only stages get plain linear quantisation (scale, clamp, round) with no implicit colour transform. Colourspace changes are made by the user (for example with OCIOColorSpace) or by the plugin's own mapping option.
  - **Q3:** defaults are ACEScg for float, `sRGB Encoded Rec.709 (sRGB)` for 8-bit and 16-bit, and ACEScct for log.
