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

- [x] M50.P1.T1 — Write the project-OCIO design doc, with a byte-only plugin survey
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
      - Reader file category, Nuke-style: float/half → float. A legacy log guess (Cineon/KodakLog/ADX) → log. Otherwise the file bit depth gives 8-bit or 16-bit. Metadata-derived display names (ReadPNG's gAMA/sRGB chunk, ReadOIIO's display-name heuristics) are dropped; ReadOIIO's `oiio:ColorSpace` naming a real config colourspace is kept. ReadFFmpeg never read the trc (design §10.8).
    - **Viewer pipeline:** OCIO `LegacyViewingPipeline`.
      - The pipeline carries only the look and `DisplayViewTransform(workingSpace → display/view)`. Gain/offset (before) and gamma (after, with the gamma ≤ 0 threshold mode) are applied by Natron around it (design §5.1, §10.4).
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
    - **Clean break:** delete `ocio-old-config.ntp` and the test cases that load it. The non-OCIO legacy fixtures `m65-legacy-color.ntp` (`ProjectSerialization_Test`) and `channel-set-legacy-defaults.ntp` (`DefaultChannelSet_Test`) stay; they test earlier milestones' load behaviour (design §10.10).
  - verify: PM review. The doc records the user's Q1–Q3 answers, and the survey list is in it. Done 2026-10-02: `.plan/PLAN/DESIGN/2026-10-02-project-ocio.md`; its §10 corrections are applied to the briefs below.
  - size: M

## Phase 50.2: Engine foundation

- [x] M50.P2.T1 — Add a ProjectColorManagement service that owns the project's OCIO config
  - files: `Engine/ProjectColorManagement.h` (new), `Engine/ProjectColorManagement.cpp` (new), `Engine/EngineFwd.h`, `Engine/CMakeLists.txt`, `Tests/ProjectColorManagement_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach:
    - In `Engine/CMakeLists.txt`, move `OpenColorIO::OpenColorIO` from `PRIVATE` to `PUBLIC`. The header exposes OCIO types, and `Gui/ViewerGL*` (P4.T2) and the tests need the OCIO headers (design §10.1).
    - Implement the class exactly as in design §2.1. It resolves a config source (a built-in `ocio://` URI, or a file path resolved against a base directory) to a `ConstConfigRcPtr` under a `QMutex`, and `load()` returns a `LoadErrorEnum` instead of throwing.
    - It exposes:
      - lists of active colourspaces, roles, displays, views per display, and looks; the default display/view;
      - `resolveRoleOrName()`;
      - a cached `getDisplayProcessor(src, display, view, look)` returning a `DisplayProcessor` (GPU processor, F32 CPU processor, cache ID and its `Hash64`). There are **no** exposure/gamma arguments: gain/offset/gamma stay Natron-side (design §5.1, §10.2);
      - a cached `getConversionProcessor(src, dst)`;
      - `setWorkingSpace(name)`, `workingToColorPicking(r,g,b)` and `colorPickingToWorking(r,g,b)` (working ↔ `color_picking` role). They live here rather than in P5.T3 so this file is touched by one task only (design §10.2);
      - the static `builtinConfigOptions()` (the registry entries with "(recommended)" labels, plus Custom), shared by P2.T2 and P2.T3;
      - the config-changed callback list (`addConfigChangedCallback`, `removeConfigChangedCallback`, `notifyConfigChanged`).
    - Add `ProjectColorManagementPtr` to `Engine/EngineFwd.h`.

    It must be thread-safe for render threads (the config pointer swapped under the mutex, both processor caches cleared on `load()`). It has no knob dependencies, and Project wires it in P2.T2.
  - verify: `ctest -R ProjectColorManagement` passes these cases:
    - The Studio URI resolves, `scene_linear` resolves to ACEScg, and the default display/view are `sRGB - Display` / `ACES 2.0 - SDR 100 nits (Rec.709)`.
    - A custom config written to a temp dir with `Config::CreateRaw()->serialize` resolves, and so does its relative path.
    - A bad path gives an error, not a throw.
    - The display processor maps ACEScg 0.18 to the same values as a directly built OCIO processor (EXPECT_NEAR 1e-5).
    - Two lookups with equal arguments return the same cache ID.
    - With the working space ACEScg, `workingToColorPicking` maps 0.18 to ≈ 0.4613 for Studio, and `colorPickingToWorking` round-trips within 1e-4.
    - `builtinConfigOptions()` lists the 8 registry configs plus Custom.
  - size: M

- [x] M50.P2.T2 — Add the project Color page: config, working space, file defaults and viewer defaults
  - files: `Engine/Project.cpp`, `Engine/Project.h`, `Engine/ProjectPrivate.h`, `Engine/ProjectPrivate.cpp`, `Tests/ProjectColorManagement_Test.cpp`
  - approach:
    - `ProjectPrivate` owns a `ProjectColorManagement`. Add the design-doc knobs on a new "Color" page, **in the creation order of design §2** (`ocioConfig`, `ocioConfigFile`, `workingSpace`, the 4 file defaults, `viewerDisplay`, `viewerView`): restore follows creation order. Colourspace entries are ids = names; defaults are set by id after the Studio config is populated. Leave the old "LUT" page in place until P5.T2.
    - Add the single refresh point `Project::refreshColorManagement(bool warnOnFallback)` (design §2):
      - load `getOCIOConfigSource()`, keeping the previous config and showing an error on failure;
      - repopulate the colourspace, display and view choices, keeping values by name and applying the role fallbacks. A fallback rewrites the knob value;
      - warn once per switch, listing each knob old → new;
      - re-derive the enabled state and tooltip of `ocioConfig`/`ocioConfigFile` (`ocioConfigFile` enabled only for Custom). Enabled is serialized, so this must run after every restore (design §10.3);
      - fire `configChanged`.

      Call it from `Project::onKnobValueChanged` (for `ocioConfig`/`ocioConfigFile`), the end of the knob loop in `ProjectPrivate::restoreFromSerialization`, `Project::doResetEnd` and `Project::initializeKnobs`. All Color-page knobs trigger auto-save.
    - Add accessors `getOCIOConfigSource()`, `getWorkingColorSpace()`, `getFileColorSpace(FileColorCategoryEnum)`, `getDefaultDisplayView()` and `getColorManagement()` (design §2; `FileColorCategoryEnum` goes in `ProjectColorManagement.h`, not `Global/Enums.h`). `getOCIOConfigSource()` returns the effective config. Here it is the knob value; P2.T3 adds the `OCIO` override inside it, so no caller reads the config knobs directly.
  - verify: `ctest -R ProjectColorManagement` passes these cases:
    - A new project has Studio, ACEScg, the Q3 defaults and the default display/view.
    - Setting every knob, saving and reloading round-trips the values.
    - Switching to `ocio://cg-config-v4.0.0_aces-v2.0_ocio-v2.5` repopulates the menus. The working space survives (CG also has ACEScg), and a Studio-only name falls back to the role with one warning; a save after the switch stores the resolved name.
    - Custom pointing at a temp raw config lists `raw`.
    - A project saved with Custom and reloaded after switching it back to a built-in has `ocioConfigFile` disabled, whatever Enabled flag was serialized.

    `ctest -R ProjectSerialization` stays green.
  - size: L

- [x] M50.P2.T3 — Let the OCIO env var override every project's config, make the preference the new-project default, and drop restart-era OCIO plumbing
  - files: `Engine/Settings.cpp`, `Engine/Settings.h`, `Gui/GuiAppInstance.cpp`, `Engine/Project.cpp`, `Tests/ProjectColorManagement_Test.cpp`
  - approach:
    - Relabel `ocioConfig`/`ocioCustomConfigFile` as "Default OpenColorIO config for new projects" / "Custom OpenColorIO config file for new projects", listing `ProjectColorManagement::builtinConfigOptions()`. When an override is set, disable these preference knobs too, with the same tooltip.
    - Delete `warnOCIOChanged`, `startupCheckOCIO`, `doOCIOStartupCheckIfNeeded` (and its call in `GuiAppInstance::load`), the `share/OpenColorIO-Configs` directory scan (`getDefaultOcioConfigPaths`), both restart dialogs, and the `appPTR->onOCIOConfigPathChanged` call in `tryLoadOpenColorIOConfig`.
    - Add `Settings::getOCIOEnvOverride()`, which returns the `OCIO` value captured once in `tryLoadOpenColorIOConfig()` before its first `qputenv`. It is empty when `OCIO` was unset **or** when `NATRON_OCIO_ENV_IS_PREFERENCE=1` is also set. Add the test hook `static Settings::recaptureOCIOEnvOverrideForTests()`, and `Settings::getDefaultOCIOConfigSourceForNewProjects()` (the preference URI or custom path, else Studio).
    - Startup export: with an override, `qputenv("OCIO", override)`; otherwise `qputenv("OCIO", preference)` **and** `qputenv("NATRON_OCIO_ENV_IS_PREFERENCE", "1")`. Child processes (`ProcessHandler`/`QProcess` background renders) inherit the environment, and without the marker they would treat the preference as an override (design §3, §10.3).
    - In `Project` (Q1):
      - `getOCIOConfigSource()` returns the env override when it is set, otherwise the knob value.
      - New projects (`initializeKnobs`, `doResetEnd`) take their config knob defaults from `getDefaultOCIOConfigSourceForNewProjects()`.
      - `refreshColorManagement` disables `ocioConfig`/`ocioConfigFile` under the override, with the tooltip "Overridden by the OCIO environment variable (<value>)", and restores the normal state and tooltip otherwise. It re-derives this after every restore, because Enabled is serialized. The saved config knob values are kept, untouched, and written back on save. Colourspace fallbacks under the override do rewrite `workingSpace` etc.; only the config knobs stay untouched.
      - There is no warning-only path.
  - verify: `ctest -R ProjectColorManagement` passes these cases:
    - With `OCIO` unset, a new project takes the preference value.
    - With `OCIO` set (via `qputenv` before the override is captured, with the test hook to re-capture it), a new project uses the env config and both config knobs are disabled with the tooltip.
    - With `OCIO` set **and** `NATRON_OCIO_ENV_IS_PREFERENCE=1`, the override is empty.
    - A project saved with the CG config, loaded with `OCIO` = the Studio URI, uses Studio, has its knobs disabled, and still saves CG in the file.
    - Unsetting `OCIO`, re-capturing and loading that saved file again restores CG with the knobs enabled (the serialized disabled state does not stick).

    `tools/ci/local/test.sh smoke debug` stays green (the default-config check).
  - size: M

- [x] M50.P2.T4 — Point the project's [OCIO] path variable at the project config, and remove the AppManager broadcast
  - files: `Engine/Project.cpp`, `Engine/ProjectPrivate.cpp`, `Engine/AppManager.cpp`/`Engine/AppManager.h`/`Engine/AppManagerPrivate.h`, `Engine/AppInstance.cpp`/`Engine/AppInstance.h`
  - approach:
    - Delete `AppManager::onOCIOConfigPathChanged`, `getOCIOConfigPath` and `currentOCIOConfigPath` (in `AppManagerPrivate.h`), and `AppInstance::onOCIOConfigPathChanged`.
    - `Project::onOCIOConfigPathChanged` keeps `fixRelativeFilePaths`, and its only caller becomes `refreshColorManagement`. It is passed `ProjectColorManagement::getConfigDirectory()` of the **effective** config (under an override, the override's directory). For a built-in URI the `[OCIO]` row is removed from `envVars`, not set empty (design §3).
    - Remove the other call sites (`Project::initializeKnobs`, `Project::doResetEnd`, `ProjectPrivate::restoreFromSerialization`).
  - verify: `ctest -R 'ProjectColorManagement|ProjectSerialization'` passes this new case: a custom config in `<tmp>/cfg/config.ocio` gives `[OCIO]` = `<tmp>/cfg`, and switching back to Studio removes it. The debug build is clean with no reference to the deleted AppManager API.
  - size: M

## Phase 50.3: OFX plugins follow the project config

- [x] M50.P3.T1 — Declare the OCIO colour props in the openfx-natron fork (headers and HostSupport)
  - files: `libs/OpenFX` (submodule charlesangus/openfx-natron): `include/ofxColour.h` (new, verbatim from ASWF openfx 1.5, copy in `build/assets/plugin-src/openfx-metadata/include/`), `include/ofxNatron.h`, `HostSupport/src/ofxhImageEffect.cpp`, `HostSupport/include/ofxhImageEffect.h`; the submodule pin in Natron
  - approach:
    - Add `NatronOfxImageEffectPropOCIOWorkingColourspace` (string, dim 1) and `NatronOfxImageEffectPropOCIOFileColourspaces` (string, dim 4: 8-bit, 16-bit, log, float) to `ofxNatron.h`.
    - A get-hook only fires for a property in the instance PropSpec, and `Instance::getStringProperty`/`getStringPropertyN`/`getDimension` only dispatch known names (design §10.5). So, inside `#ifdef OFX_EXTENSIONS_NATRON`, add the 3 props to the `Instance` PropSpec next to `kNatronOfxExtraCreatedPlanes`, add the virtuals `getOCIOConfigSource()`, `getOCIOWorkingColourspace()` and `getOCIOFileColourspaces()` (defaults: empty), and dispatch them in those three functions (design §4.1).
    - Push the fork submodule commit, then bump the pin.
  - verify: the debug build is clean against the bumped pin, and the full debug ctest stays green (the default virtuals return empty, so behaviour is unchanged).
  - size: M

- [x] M50.P3.T7 — Answer the OCIO colour props from the project in OfxImageEffectInstance
  - files: `Engine/OfxImageEffectInstance.cpp`, `Engine/OfxImageEffectInstance.h`, `Tests/ProjectOCIOPlugins_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach:
    - In the constructor, register `setGetHook` for the 3 props next to `kNatronOfxExtraCreatedPlanes`.
    - Override the P3.T1 virtuals. They read `getApp()->getProject()` live on every get (`getOCIOConfigSource()`, `getWorkingColorSpace()`, `getFileColorSpace(…)` × 4) and return references into `mutable` member strings refreshed under a `QMutex`.
  - verify: `ctest -R ProjectOCIOPlugins` passes these cases:
    - A created ReadOIIO instance's property set returns the Studio URI, `ACEScg` and the 4 defaults.
    - After switching the project to CG, the same instance returns the CG URI with no node recreation.
  - size: M

- [x] M50.P3.T2 — Push the project config into every OCIO node's ocioConfigFile, and check colourspaces against it
  - files: `Engine/Project.cpp`, `Engine/Node.cpp`, `Engine/ReadNode.cpp`, `Engine/WriteNode.cpp`, `Tests/ProjectOCIO_Test.cpp`
  - approach:
    - Add `Project::pushOCIOConfigToNodes()` (design §4.1). For each node with a `KnobStringBase` named `ocioConfigFile`, set it to the **effective** config (`getOCIOConfigSource()`, which is the `OCIO` override when one is set) with `eValueChangedReasonPluginEdited`, then `setSecret(true)`. Secret is serialized, so it is re-applied on **every** push, not once (design §10.3).
    - Run it for a newly created node (end of `Node::initializeKnobs` or `Node::load` completion), for every node in `getNodes_recursive` on `configChanged`, and once after project load, after node restore, so a node's saved `ocioConfigFile` never wins.
    - Read/Write wrappers: the `{kOCIOParamConfigFile, true}` tables in `ReadNode.cpp`/`WriteNode.cpp` are only the persistent-param lists carried across a decoder/encoder swap; they hold no knob. Set the knob on the wrapper **and** on the embedded node (`ReadNode::getEmbeddedReader()` / `WriteNode::getEmbeddedWriter()`), and re-push after a decoder/encoder swap (design §10.15).
    - Rewrite `reportUnresolvedOCIOColorSpaces` to check each node's `ocioInputSpace`/`ocioOutputSpace` against `getColorManagement()->getConfig()` instead of `createOcioConfig`/`CreateFromEnv`, and to run on load **and** on `configChanged`. Nodes that resolve after a switch get `clearPersistentMessage`, only for the message this function set.
    - Delete `Tests/fixtures/ocio-old-config.ntp` and the cases that load it. Rebuild `ProjectOCIO_Test` on projects created in the test. The non-OCIO legacy fixtures (`m65-legacy-color.ntp`, `channel-set-legacy-defaults.ntp`) stay.
  - verify: `ctest -R ProjectOCIO_` passes these cases:
    - A new Read, Write and OCIOColorSpace each carry the project config in `ocioConfigFile` (the Read/Write on both wrapper and embedded node), and the knob is secret.
    - Switching to CG updates all three, and OCIOColorSpace's `ocioInputSpaceIndex` entries are CG names.
    - Changing a Read's filename to another format (decoder swap) leaves the new embedded decoder with the project config.
    - A node holding `Camera Rec.709` (Studio-only) goes into the persistent error state after the switch to CG, and clears when switched back.
    - A saved, reloaded project keeps the config, the clean state and the secret knob.
    - A CG project loaded with `OCIO` = the Studio URI gives every OCIO node the Studio config, and the unresolved-colourspace check runs against Studio.
    - With the P3.T5 pin: OCIOLookTransform's look menu and OCIODisplay's `displayIndex` entries rebuild after a project switch, and OCIOCDLTransform/OCIOFileTransform carry the pushed `ocioConfigFile` (P3.T3, P3.T8).
  - size: L

- [x] M50.P3.T3 — openfx-io fork: GenericOCIO and OCIOLookTransform take their config from the host
  - files (charlesangus/openfx-io): `IOSupport/GenericOCIO.h`, `IOSupport/GenericOCIO.cpp`, `OCIO/OCIOLookTransform.cpp`
  - approach:
    - Work on a branch that descends from m66 (`OPENFX_IO_REF` 649ce94), so the later arena bump also brings commit 40764b2 (`existingColorSpaceOrFallback`), which arena's `OpenFX-IO` (f30a6a8) lacks (design §10.9).
    - Add guarded defines for `kOfxImageEffectPropOCIOConfig` and the two Natron props (`#ifndef`, with a pointer to ofxColour.h/ofxNatron.h), and the helper `GenericOCIO::hostConfigSource(std::string*)`.
    - At instance construction, when the host prop is present, load the config from it and make `ocioConfigFile` secret. Otherwise fall back to the param, as today.
    - Reload and rebuild the menus on every `ocioConfigFile` change whose reason is not `eChangeTime`. In OCIOLookTransform, drop both the `eChangeUserEdit` gate at `:907` and the `configIsDefault()` gate (there and in the constructor).
    - Replace the dead `ROLE_DEFAULT` → index-0 fallbacks (config-change and user-edit branches) with `existingColorSpaceOrFallback`.
    - Keep the commit self-contained, as decision `2026-09-01-no-upstream-pr-for-ocio-sentinel` requires.
  - verify: the fork builds in natron-dev (`build/build-m65-plugins.sh`-style one-off). Host-side behaviour is checked in P3.T2 after the P3.T5 pin.
  - size: M

- [x] M50.P3.T8 — openfx-io fork: OCIODisplay, OCIOLogConvert, OCIOCDLTransform and OCIOFileTransform use the instance config
  - files (charlesangus/openfx-io): `OCIO/OCIODisplay.cpp`, `OCIO/OCIOLogConvert.cpp`, `OCIO/OCIOCDLTransform.cpp`, `OCIO/OCIOFileTransform.cpp`
  - approach (design §4.2, §10.6), on the same m66-descended branch as P3.T3:
    - OCIODisplay builds its display/view menus once from `OCIO::GetCurrentConfig()` in its constructor. Build them from the instance config instead, and rebuild them on every non-`eChangeTime` `ocioConfigFile` change.
    - OCIOLogConvert's own `loadConfig` reads the host prop first (`hostConfigSource`).
    - OCIOCDLTransform and OCIOFileTransform don't use GenericOCIO and have no config param. Add a secret, persistent `ocioConfigFile` string param, so the host push reaches them and invalidates their render hash, and replace `GetCurrentConfig()` with the instance/host config.
  - verify: the fork builds in natron-dev. Host-side behaviour is checked in P3.T2 after the P3.T5 pin.
  - size: M

- [x] M50.P3.T11 — On a config switch, keep a node's colourspace name the new config lacks, so the host flags it instead of the plugin remapping it
  - files: openfx-io fork `IOSupport/GenericOCIO.cpp` (the config-source-changed branch of `changedParam` that applies `existingColorSpaceOrFallback`, plus the persistent-message clear); `Tests/ProjectOCIO_Test.cpp`
  - approach: when the host provides `kOfxImageEffectPropOCIOConfig` and the config source changes, leave unresolved input/output colourspace names untouched and don't clear the node's persistent message. Without the host prop, the old fallback stays. `Project::reportUnresolvedOCIOColorSpaces`, which runs after every push, then flags the node.
  - verify: `ctest -R ProjectOCIO_` passes the new case: a node holding `Camera Rec.709` keeps the name after a switch to CG, shows the persistent error, and clears it when switched back to Studio. All existing ProjectOCIO_ cases still pass.
  - size: M

- [x] M50.P3.T4 — openfx-io fork: new Reads and Writes take the working space and per-file-type defaults from the host
  - files (charlesangus/openfx-io): `IOSupport/GenericReader.cpp`, `IOSupport/GenericReader.h`, `IOSupport/GenericWriter.cpp`, `IOSupport/GenericWriter.h`
  - approach (design §4.2, §4.3), on the same m66-descended branch as P3.T3:
    - Add the virtual `guessFileColourCategory(...)` to `GenericReaderPlugin` (from the legacy guess string) and `GenericWriterPlugin` (from the filename and bit depth), with the default rule: `scene_linear`/Linear → float; KodakLog/Cineon/ADX/`compositing_log` → log; otherwise 8-bit. The defaults live here, so arena readers need no change.
    - Reader: in `changedFilename`, only on `eChangeUserEdit` with `kParamExistingInstance` false and `ocioInputSpaceSet` false, and when the host provides the file colourspaces: input space = `fileColourspaces[category]`, output space = the host working space. The OCIO filename rule still wins for the input space.
    - Writer: input space = the working space, output space = `fileColourspaces[category]`, unless `ocioOutputSpaceSet`. `outputFileChanged` is called by `restoreStateFromParams` with `eChangePluginEdit` on every project load; with the host props present it must **not** re-guess there, or reloading would re-apply the current project defaults to existing Writes (design §10.7). Gate it on the same existing-instance and `ocioOutputSpaceSet` checks.
    - Without the host props, the old behaviour stays, so other hosts are unaffected.
  - verify: the fork builds in natron-dev. Behaviour is verified host-side in P3.T6.
  - size: L

- [x] M50.P3.T9 — openfx-io fork: depth-aware file categories for PNG and OIIO, and drop metadata-derived display names
  - files (charlesangus/openfx-io): `PNG/ReadPNG.cpp`, `OIIO/ReadOIIO.cpp`, `PNG/WritePNG.cpp`, `OIIO/WriteOIIO.cpp`
  - approach: GenericReader/GenericWriter don't know the file bit depth, so these plugins override `guessFileColourCategory` (design §4.3, §10.7):
    - ReadPNG (`getPNGInfo`): 16-bit → 16-bit; 8-bit and below → 8-bit. With the host props present, drop the gAMA → Gamma1.8/2.2 and sRGB-chunk → sRGB display-name chain.
    - ReadOIIO (`guessColorspace`'s spec): float/half → float; a `.cin`/`.dpx` log guess → log; 16-bit integer → 16-bit; else 8-bit. With the host props present, `oiio:ColorSpace` naming a real config colourspace is still honoured (it is file metadata naming a colourspace); only its display-name heuristics are dropped.
    - WritePNG: 16-bit → 16-bit; else 8-bit. WriteOIIO (`_bitDepth`): as ReadOIIO.
    - ReadFFmpeg is unchanged: it never read the trc and always walked the Rec709 chain (design §10.8).
  - verify: the fork builds in natron-dev. Behaviour is verified host-side in P3.T6.
  - size: M

- [x] M50.P3.T5 — Pin the openfx-io fork and carry it into openfx-arena
  - files: `tools/ci/local/fetch-assets.sh` (`OPENFX_IO_REF`, `OPENFX_ARENA_REF` and their delta comments), charlesangus/openfx-arena (`OpenFX-IO` submodule bump)
  - approach:
    - The P3.T3/P3.T4/P3.T8/P3.T9 commits sit on a branch descending from m66 (649ce94), so the bump also brings 40764b2 (`existingColorSpaceOrFallback`) into arena (design §10.9).
    - Bump arena's `OpenFX-IO` submodule to that tip and push.
    - Re-pin both refs, and record the deltas in the comment blocks (`:134-150`, `:201-212`).
  - verify: `tools/ci/local/fetch-assets.sh` rebuilds the plugins. `PLUGINS_WANT` shows the new refs, arena builds against the bumped `OpenFX-IO`, and the smoke test stays green.
  - size: M

- [x] M50.P3.T10 — Stop fetching, bundling and caching the 2018 OpenColorIO-Configs tarball
  - files: `tools/ci/local/fetch-assets.sh`, `tools/release/stage-bundle.sh`, `cmake/NatronBundleAssets.cmake`, `.github/workflows/ci.yml`, `.github/workflows/nightly.yml`, `.github/workflows/release.yml`
  - approach (design §10.9): nothing reads `share/OpenColorIO-Configs` after P2.T3.
    - Drop the tarball fetch in `fetch-assets.sh` (`:80-120`) and its staging in `stage-bundle.sh` (`:321-331`).
    - In `NatronBundleAssets.cmake`, drop the `FATAL_ERROR` on a missing `assets/OpenColorIO-Configs` under `NATRON_BUNDLE_ASSETS=ON` and its install rule.
    - Remove the `OpenColorIO-Configs` cache paths from the three workflows.
  - verify: a fresh `fetch-assets.sh` no longer creates `build/assets/OpenColorIO-Configs`; a configure with `NATRON_BUNDLE_ASSETS=ON` and no such directory succeeds; `git grep -n OpenColorIO-Configs tools cmake .github` is empty; the smoke test stays green.
  - size: M

- [ ] M50.P3.T6 — Test the Read and Write defaults against the project settings
  - files: `Tests/ProjectOCIODefaults_Test.cpp` (new), `Tests/CMakeLists.txt`, `Tests/fixtures/` (reuse the existing flat EXR/PNG fixtures; add an 8-bit PNG and a 16-bit PNG only if none exists)
  - approach: drive a real ReadOIIO/ReadPNG/ReadEXR and WritePNG/WriteEXR through the Natron Read/Write wrappers in the test project.
  - verify: `ctest -R ProjectOCIODefaults` passes these cases:
    - An 8-bit PNG reads as `sRGB Encoded Rec.709 (sRGB)`, a 16-bit PNG as the 16-bit default, and a float EXR as ACEScg. The output space is the working space.
    - Changing `colorSpace8Bit` to `Gamma 2.2 Encoded Rec.709` affects a **new** PNG Read and leaves the existing one alone.
    - A user-set input space survives a filename change, and `ocioInputSpaceSet` stays false after host-driven defaults.
    - WritePNG's output space is the 8-bit default.
    - An existing Write keeps its output space after a project-default change followed by save and reload (no re-guess on restore).
    - A pixel probe of 0.18 ACEScg written to an 8-bit PNG gives 118 ±1.
  - size: M

## Phase 50.4: Viewer display through OCIO

- [x] M50.P4.T5 — Key the viewer texture cache on a display-transform hash instead of a LUT int
  - files: `Engine/FrameKey.h`, `Engine/FrameKey.cpp`, `Engine/FrameEntrySerialization.h`, `Engine/ViewerInstance.cpp` (the two `FrameKey` construction sites), `Tests/ViewerDisplayTransform_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach:
    - Replace `int _lut`/`getLut()` with `U64 _displayTransformHash`. It stays in `fillHash` and `operator==` only when `!_useShaders`, as today.
    - Rename the nvp `Lut` to `DisplayTransformHash` and bump the `FrameKey` serialization version; old viewer disk-cache entries drop out on the version mismatch (clean break).
    - Until P4.T1 lands, `ViewerInstance.cpp` passes the existing LUT int widened to `U64`, so behaviour is unchanged.
  - verify: `ctest -R ViewerDisplayTransform` passes: two `FrameKey`s differing only in `_displayTransformHash` hash differently with 8-bit textures and identically with shaders, and a key round-trips through serialization. The full debug ctest stays green.
  - size: S

- [ ] M50.P4.T1 — Run the viewer's CPU (8-bit texture) path through the OCIO display processor
  - files: `Engine/ViewerInstance.cpp`, `Engine/ViewerInstance.h`, `Engine/ViewerInstancePrivate.h`, `Engine/UpdateViewerParams.h`, `Tests/ViewerDisplayTransform_Test.cpp`
  - approach (design §5.1, §5.2, §10.4):
    - Replace `viewerParamsLut`/`onColorSpaceChanged` with `setDisplayTransform(display, view, look)` (`viewerParamsDisplay/View/Look` under `viewerParamsMutex`; it triggers a re-render). `UpdateViewerParams::lut` becomes `ProjectColorManagement::DisplayProcessorPtr displayProcessor`, filled in `setupMinimalUpdateViewerParams`. Delete `getLutType` and `lutFromColorspace`.
    - The processor carries only the look and the display/view. Gain/offset and gamma are **not** OCIO exposure/display CC: the auto-contrast `offset` and the gamma ≤ 0 threshold mode can't be expressed that way, and mutating dynamic properties on a shared CPU processor isn't thread-safe. Add the namespace-scope `applyViewerDisplayTransform(processor, rgba, width, gain, offset, gamma)`: `rgb*gain + offset` → processor → gamma (threshold when ≤ 0), alpha untouched.
    - In `scaleToTexture8bits_generic`, fill an RGBA F32 scanline (byte/short sources through plain `intToFloat`, Q2), call `applyViewerDisplayTransform`, then dither with `Color::floatToInt<0xff01>` in place of `toColorSpaceUint8xxFromLinearFloatFast`. The matte overlay goes through the same processor. Delete `RenderViewerArgs::srcColorSpace/colorSpace`.
    - In `scaleToTexture32bitsGeneric`, drop `srcColorSpace` (byte/short through `intToFloat`, no transform). In `renderViewer_internal`, drop the `getDefaultColorSpaceForBitDepth` lookup.
    - `FrameKey` gets `displayProcessor->cacheHash` (field from P4.T5).
    - The `endTransferBufferFromRAMToGPU` call drops its `lut` argument; P4.T2 changes the interface in the same batch.
  - verify:
    - `ctest -R ViewerDisplayTransform` passes these cases:
      - ACEScg 0.18 through `sRGB - Display`/`ACES 2.0 - SDR 100 nits (Rec.709)` gives 89 ±1 (the code value measured in the PNG decision).
      - `Un-tone-mapped` gives 118 ±1, and `Raw` gives 46 ±1.
      - Gain 2 equals exposure +1 stop.
      - Gamma ≤ 0 gives the threshold; a non-zero offset is added before the processor.
      - Different views give different processor cache hashes, and so different FrameKey hashes.
    - A one-off timing logged in the test shows a 1920×1080 frame transformed in under 150 ms on the container's threads. Record the figure; it doesn't fail the test.
  - size: L

- [ ] M50.P4.T2 — Generate the 32f viewer shader from OCIO and upload its LUT textures
  - files: `Gui/ViewerGL.cpp`, `Gui/ViewerGL.h`, `Gui/ViewerGLPrivate.cpp`, `Gui/ViewerGLPrivate.h`, `Gui/Shaders.cpp`/`Gui/Shaders.h`, `Engine/OpenGLViewerI.h`
  - approach (design §5.3, §10.4):
    - Replace `fragRGB`'s LUT branches with a runtime-composed source: the OCIO shader text (`GpuShaderDesc`, `GPU_LANGUAGE_GLSL_1_2`, function `OCIODisplay`, resource prefix `ocio_`), and a `main()` that does `rgb*gain + offset` → `OCIODisplay` → gamma (threshold when ≤ 0). Gain, offset and gamma are plain uniforms, not OCIO dynamic properties.
    - Upload the 3D LUTs (`glTexImage3D`) and the 1D/2D LUTs (`glTexImage2D`) to units ≥1, and read the uniform list anyway. Rebuild the program in `activateShaderRGB` only when the processor cache ID changes.
    - `ViewerGL::setLut` becomes `setDisplayTransform(display, view, look)`, which both texture indices (A/B wipe) share; delete `displayingImageLut`.
    - `OpenGLViewerI::endTransferBufferFromRAMToGPU` (and `ViewerGL`'s override) drops its `int lut` parameter. This lands in the same batch as P4.T1, which updates the caller.
    - `getColorAtInternal`/`getColorAt`/`getColorAtRect` drop their `Color::Lut` arguments; the linear readout is the image value (byte/short via `intToFloat`), and the non-linear readout uses `applyViewerDisplayTransform` with gain 1, offset 0, gamma 1. Delete the commented-out GPU-readback block in `getColorAt` that names `eViewerColorSpaceLinear`.
    - If a driver rejects the shader, log the compile log and fall back to the 8-bit CPU path for that viewer.
  - verify: the Xvfb script `build/m50-gui/viewer_32f.py` (recipe `build/m61-gui/run-gui.sh`) sets `texturesBitDepth`=32f and views a constant 0.18 ACEScg. It grabs the viewer and checks that the centre pixel is 89 ±2, and within 2 code values of the 8-bit path for the same view, both at default gain/gamma and at gain 2. The script logs no GL shader compile error.
  - size: L

- [ ] M50.P4.T3 — Replace the viewer colourspace combo with Display/View/Look menus
  - files: `Gui/ViewerTab.cpp`, `Gui/ViewerTab.h`, `Gui/ViewerTab10.cpp`, `Gui/ViewerTab30.cpp`, `Gui/ViewerTabPrivate.h` (the serialization change is in P4.T4)
  - approach:
    - Swap `viewerColorSpace` for three `ComboBox`es:
      - Display;
      - View, which repopulates per display;
      - Look, which has "None" plus the config's looks.
    - Seed them from the project's `getDefaultDisplayView()`, and repopulate them on the project's `configChanged`, keeping the values by name and otherwise using the project defaults, then the config defaults.
    - Each change calls `ViewerGL::setDisplayTransform` and `ViewerInstance::setDisplayTransform`.
    - Replace `getColorSpace`/`setColorSpace`/`onColorSpaceComboBoxChanged` with `getDisplayTransform`/`setDisplayTransform` (strings)/`onDisplayTransformComboBoxChanged`, and drop the `getLutType` use in `ViewerTab30.cpp`. The callers in `ProjectGui*` change in P4.T4, in the same batch.
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
    - Replace `ViewerData::colorSpace` with `display`, `view` and `look`. Add `VIEWER_DATA_INTRODUCES_OCIO_DISPLAY 15` and make it `VIEWER_DATA_SERIALIZATION_VERSION`.
    - The writer is `ProjectGuiSerialization::initialize` (`tab->getDisplayTransform`). The restore site is the static `loadNodeGuiSerialization` in `Gui/ProjectGui.cpp`: it calls `tab->setDisplayTransform`, falling back to the project defaults when a name doesn't resolve.
    - Don't read the retired `ColorSpace` nvp at any version.
  - verify: a GuiTests (offscreen) or Engine test round-trips `ViewerData` with a non-default view and look. The P4.T3 Xvfb script additionally saves the project, reloads it, and checks that the menus keep `Un-tone-mapped`.
  - size: M

## Phase 50.5: Retire the built-in LUTs

- [x] M50.P5.T1 — Make bit-depth conversion colour-neutral (Q2)
  - files: `Engine/ImageConvert.cpp`, `Engine/Image.h`, `Tests/ImageConvert_Test.cpp` (new), `Tests/CMakeLists.txt`, `Tests/Image_Test.cpp`
  - approach:
    - Integer↔float conversion becomes plain linear quantisation, `Image::convertPixelDepth` semantics: scale, clamp, round, with **no** error-diffusion dither (the 0.5 → 128 anchor requires it, design §10.11). Delete `lutFromColorspace` and `convertToFormatInternalForColorSpace`; the colourspace parameters are still accepted but ignored until P5.T5 removes them.
    - Natron never applies an implicit colour transform. A colourspace change is the user's job (an OCIOColorSpace node upstream) or the plugin's own mapping option (Q2).
    - Delete the sRGB-LUT case in `Image_Test.cpp` (the sRGB→sRGB RGBA→RGB case); its subject no longer exists.
    - Follow-up, out of scope: a hint on integer-only nodes saying that pixels arrive linear-quantised. The P1.T1 survey found no shipped integer-only plugin, so P6.T2's UAT notes carry the third-party wording from design §8.
  - verify: `ctest -R 'ImageConvert|Image_'` passes: float 0.5 → byte gives 128 → 0.50196; float → short → float round-trips within 1/65535; values above 1 clamp to 255/65535. The full debug ctest is green.
  - size: M

- [x] M50.P5.T5 — Drop the colourspace parameters from convertToFormat and update every caller
  - files: `Engine/Image.h`, `Engine/ImageConvert.cpp`, `Engine/EffectInstance.cpp`, `Engine/EffectInstanceRenderRoI.cpp`, `Engine/Image.cpp`, `Engine/ImagePremult.cpp`, `Engine/RotoPaint.cpp`, `Engine/DiskCacheNode.cpp`, `Tests/Image_Test.cpp`
  - approach: a mechanical signature change that must land atomically, so it stays one task despite the file count (design §6, §10.11).
    - New signature: `void convertToFormat(const RectI& renderWindow, int channelForAlpha, bool copyBitMap, Image* dstImg) const;` Remove the `srcColorSpace`/`dstColorSpace` parameters from the private converters too.
    - Update every caller: `EffectInstance::Implementation::renderHandler` (6 calls) and `EffectInstance::convertRAMImageToOpenGLTexture` (`EffectInstance.cpp`), `EffectInstance::convertLayersFormatsIfNeeded` (`EffectInstanceRenderRoI.cpp`), `Image::pasteFrom` (`Image.cpp`), `Image::premultByChannel` (`ImagePremult.cpp`), `RotoPaint::render` (3 calls), `DiskCacheNode::render`, and the 5 calls in `Image_Test.cpp`. Each drops its `getDefaultColorSpaceForBitDepth` lookup or Linear/Linear arguments.
  - verify: the debug build is clean; `git grep -n getDefaultColorSpaceForBitDepth Engine/EffectInstance*.cpp Engine/RotoPaint.cpp Engine/DiskCacheNode.cpp Engine/Image*.cpp` is empty. The full debug ctest is green, including the RotoPaint and DiskCache-touching tests.
  - size: M

- [ ] M50.P5.T2 — Remove the project LUT page, and render node previews through the default display/view
  - files: `Engine/Project.cpp`, `Engine/Project.h`, `Engine/ProjectPrivate.h`, `Engine/AppInstance.cpp`/`Engine/AppInstance.h`, `Engine/Node.cpp`, `Tests/ProjectColorManagement_Test.cpp`
  - approach:
    - Delete `defaultColorSpace8u/16u/32f`, the "LUT" page, `colorspaceParamIndexToEnum`, `getDefaultColorSpaceForBitDepth` (both in Project and AppInstance) and the LUT auto-save branch in `Project::onKnobValueChanged`. P4.T1 and P5.T5 removed the last callers.
    - `Node::makePreviewImage` → `renderPreview` replaces the `to_func_srgb` path: per scanline, `applyViewerDisplayTransform` with `getDisplayProcessor(working, viewerDisplay, viewerView, "")`, gain 1, offset 0, gamma 1, then `floatToInt<256>`. Byte/short sources go through `intToFloat`.
  - verify: the debug build is clean. `git grep -n getDefaultColorSpaceForBitDepth Engine Gui` is empty. A new case in `ProjectColorManagement_Test` renders the preview of a constant 0.18 ACEScg node and gets the ACES SDR value (89 ±2) at its centre.
  - size: M

- [x] M50.P5.T3 — Convert colour swatches, the colour selector and the picker swatch through OCIO
  - files: `Gui/KnobGuiColor.cpp`, `Gui/ColorSelectorWidget.cpp`, `Gui/ColorSelectorWidget.h`, `Gui/InfoViewerWidget.cpp`
  - approach (design §6, §10.12), using P2.T1's `workingToColorPicking`/`colorPickingToWorking`:
    - `KnobGuiColor::updateLabel` uses `workingToColorPicking` unless `_useSimplifiedUI` (simplified knobs hold UI colours and stay identity). Knobs without an `AppInstance` (Settings) use identity.
    - `KnobGuiColor` passes the project's `ProjectColorManagementPtr` (or null for identity) into `ColorSelectorWidget`. `ColorSelectorPaletteButton::updateColor` and the swatch (the `to_func_srgb` block) use `workingToColorPicking`; `handleTriangleColorChanged` (`from_func_srgb`) uses `colorPickingToWorking`. The HSV wheel stays on the stored values.
    - The `InfoViewerWidget` picker swatch uses `workingToColorPicking` before `col.setRgbF`; the HSV/L readouts are unchanged.
  - verify: the debug build is clean, and `git grep -nE 'to_func_srgb|from_func_srgb' Gui/KnobGuiColor.cpp Gui/ColorSelectorWidget.cpp Gui/InfoViewerWidget.cpp` is empty. The Xvfb script `build/m50-gui/project_color_page.py` grabs the Project Settings "Color" page, a Grade's colour swatch and the colour selector. Share the screenshots with the user.
  - size: M

- [x] M50.P5.T6 — Convert the tracker overlay and the debug image dump through OCIO
  - files: `Engine/TrackerNodeInteract.cpp`, `Engine/TrackerNodeInteract.h`, `Gui/Gui40.cpp`
  - approach (design §6, §10.12):
    - Rename `TrackerNodeInteract::convertImageTosRGBOpenGLTexture` to `convertImageToDisplayOpenGLTexture`. It drops `LutManager::sRGBLut()` for `workingToColorPicking` per pixel, then `floatToInt<0xff01>` with the existing dither.
    - `Gui::debugImage` uses `workingToColorPicking` through the current project, or `floatToInt<256>` when there is none.
  - verify: the debug build is clean, and `git grep -nE 'LutManager|sRGBLut' Engine/TrackerNodeInteract.* Gui/Gui40.cpp` is empty. An Xvfb grab of a Tracker's selected-marker overlay on a constant 0.18 ACEScg source shows ≈ 118 (0.4613 sRGB) ±2. Share the screenshot with the user.
  - size: S

- [ ] M50.P5.T4 — Delete the LUT classes, the named transfer curves and ViewerColorSpaceEnum
  - files: `Engine/Lut.h`, `Engine/Lut.cpp`, `Global/Enums.h`, `Engine/typesystem_engine.xml`, `Tests/Lut_Test.cpp`
  - approach:
    - Remove `LutManager`, `Lut` and every named LUT (`sRGBLut` … `VLogLut`), and the `to_func_*`/`from_func_*` curve pairs (sRGB, Rec709, BT1886 and the rest); after P5.T2, P5.T3 and P5.T6 they have no users (design §10.13). Keep `floatToInt`, `intToFloat`, `rgb_to_hsv`, `hsv_to_rgb` and the `uint8xx` helpers.
    - Remove `ViewerColorSpaceEnum` and its typesystem entry.
    - Cut `Lut_Test` down to `floatToInt`/`intToFloat`/hsv round-trips.
  - verify: the debug and release builds are clean (the Shiboken wrappers regenerate). `git grep -nE 'LutManager|ViewerColorSpaceEnum|eViewerColorSpace|to_func_srgb|from_func_srgb|getDefaultColorSpaceForBitDepth' Engine Gui Global Tests` finds nothing, commented code included. The full debug ctest is green.
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
    - a note on integer-only plugins: the P1.T1 survey found none shipped, so the note carries the third-party wording from design §8 (linear-quantised working-space values, convert explicitly around them).

    Launch-check it with `build/appimages/run-launch-check.sh`. The UAT itself is deferred to the parcel UAT.
  - verify: the AppImage launches clean through the launch check, and `M50-uat.md` lists the steps above.
  - size: M

**Verification gate:** all of the following hold:
- `tools/ci/local/test.sh ctest debug` and `smoke debug` are green. That covers ProjectColorManagement, ProjectOCIO_, ProjectOCIOPlugins, ProjectOCIODefaults, ViewerDisplayTransform, ImageConvert, Image, Lut, ProjectSerialization, DefaultChannelSet and the existing suite.
- `git grep -nE 'LutManager|ViewerColorSpaceEnum|eViewerColorSpace|to_func_srgb|from_func_srgb|getDefaultColorSpaceForBitDepth|defaultColorSpace(8u|16u|32f)|doOCIOStartupCheckIfNeeded' Engine Gui Global Tests` finds nothing, commented code included, and `git grep -n OpenColorIO-Configs tools cmake .github` is empty.
- The openfx-io and openfx-arena forks are pinned in `fetch-assets.sh`.
- The user has approved the P4.T2, P4.T3, P5.T3 and P5.T6 screenshots and signed off the P6.T2 UAT (in the parcel UAT).
- The decision is published.

Execution notes:
- **Branching:** stacked on M37. Branch `milestone/m50-proper-ocio-support` off `milestone/m37-channel-management-nodes` and open the PR against it (`DECISIONS/2026-09-22-stacked-milestone-prs.md`). M60 stacks on M50 in the parcel.
- **Builds:** the `natron-dev` container is single-tenant. Implementers edit in parallel and don't build. Each batch gets one detached build plus ctest (setsid+nohup with a fresh `.done` marker). Check that `pgrep -x ninja` is 0 before relaunching, and never pgrep-wait on a build. New `Engine/*.cpp` files are globbed, so reconfigure (re-run cmake) in the batch that adds them.
- **Plugin builds:** the fork tasks P3.T3, P3.T8, P3.T4 and P3.T9 all commit to one openfx-io branch that descends from m66 (`OPENFX_IO_REF` 649ce94), and build the plugins in their own detached container job, never alongside a Natron build. Run P3.T5's `fetch-assets.sh` rebuild as that batch's single build.
- **Tests:** the debug build defines NDEBUG, so tests use EXPECT/ASSERT, never assert(). `test.sh` unsets `OCIO`. Tests that need it set use `qputenv`/`qunsetenv` and restore it.
- **GUI checks:** run under Xvfb with the recipe in `build/m61-gui/run-gui.sh`. Scripts and fixtures go under `build/m50-gui/`. Pre-seed `checkForUpdates=false` in the fresh HOME. Share the screenshots before sign-off.
- **Batches.** Tasks within a batch touch disjoint files:
  - B1: P1.T1 (done)
  - B2: P2.T1, P3.T1, P3.T3 (fork)
  - B3: P2.T2, P4.T5, P3.T4 (fork), P3.T8 (fork)
  - B4: P2.T3, P3.T7, P3.T9 (fork)
  - B5: P2.T4, P3.T5, P5.T1
  - B6: P3.T2, P3.T10, P5.T5, P5.T3, P5.T6
  - B7: P4.T1, P4.T2, P4.T3, P4.T4, P3.T6. P4.T1–P4.T4 are the viewer API cutover: P4.T1 removes `onColorSpaceChanged`/`getLutType`/the `lut` argument whose Gui users P4.T2/P4.T3 change, and P4.T3 removes `get/setColorSpace` whose users P4.T4 changes. They only compile together, so they share one batch build and each verifies after it.
  - B8: P5.T2
  - B9: P5.T4, P6.T1
  - B10: P6.T2
- **Shared files:**
  - `Engine/Project.cpp` is touched by P2.T2, P2.T3, P2.T4, P3.T2 and P5.T2, each in a different batch.
  - `Tests/ProjectColorManagement_Test.cpp` is touched by P2.T1, P2.T2, P2.T3, P4.T4 and P5.T2, each in a different batch.
  - `Tests/CMakeLists.txt` is touched by P2.T1, P4.T5, P3.T7, P5.T1 and P3.T6, one per batch. Append one line each.
  - `Engine/ViewerInstance.cpp` (P4.T5, P4.T1), `Engine/ImageConvert.cpp`/`Engine/Image.h`/`Tests/Image_Test.cpp` (P5.T1, P5.T5), `Engine/Node.cpp` (P3.T2, P5.T2) and `tools/ci/local/fetch-assets.sh` (P3.T5, P3.T10) are each split across batches.
  - `ProjectColorManagement.{h,cpp}` is touched only by P2.T1 (the colour-picking helpers moved there from P5.T3).
- **Line numbers** are from 2026-10-02 on M66's tip. M37 will move `EffectInstance.cpp`/`Node.cpp` cites, so re-grep by function name.

## Decisions

- 2026-10-02 — Scope (user): full replacement. A project OCIO config property (saved in the project, defaulting to ACES 2.0 Studio) drives the viewer's display/view/look menus and every Read/Write/OCIO node's colourspace lists; a project working space and per-file-type default input colourspaces (Nuke-style) apply to new Read/Write nodes; Natron's built-in sRGB/Rec709/Linear LUTs are retired so every colour transform goes through OCIO. No backward compatibility with old projects' colourspace settings.
- 2026-10-02 — Design questions (user):
  - **Q1:** Nuke-style override. A set `OCIO` env var forces the config for every project, new or loaded. The project's config knobs are disabled, with a tooltip saying why. A loaded project's saved config stays in the file but is ignored while `OCIO` is set; there is no warning-only path.
  - **Q2:** explicit handling, no magic. Integer-only stages get plain linear quantisation (scale, clamp, round) with no implicit colour transform. Colourspace changes are made by the user (for example with OCIOColorSpace) or by the plugin's own mapping option.
  - **Q3:** defaults are ACEScg for float, `sRGB Encoded Rec.709 (sRGB)` for 8-bit and 16-bit, and ACEScct for log.
- 2026-10-02 — Plan corrections from the design doc (`DESIGN/2026-10-02-project-ocio.md` §10), applied to the briefs. P1.T1 is done, and its wording no longer cites an FFmpeg trc or deletes the non-OCIO legacy fixtures (`m65-legacy-color.ntp` and `channel-set-legacy-defaults.ntp` stay). P2.T1 makes OCIO a PUBLIC Engine link, drops exposure/gamma from `getDisplayProcessor`, and takes over the colour-picking helpers from P5.T3. P2.T2/P2.T3 add the single `refreshColorManagement` point, which re-derives the serialized Enabled state after restore, plus the knob order and the `NATRON_OCIO_ENV_IS_PREFERENCE` marker for child processes. In P4.T1/P4.T2, gain/offset/gamma stay Natron-side around the OCIO pipeline, and the `lut` argument goes from `OpenGLViewerI`. P3.T2 re-applies Secret on every push and reaches the embedded Read/Write nodes. P3.T4 stops the writer re-guessing on restore. P4.T4 names `loadNodeGuiSerialization` and version 15. P5.T4 also deletes the transfer curves and greps commented code. To stay within the sizing rule, these tasks split. P3.T1 is the OFX submodule (now with HostSupport) and P3.T7 the Natron hooks. P3.T3 is GenericOCIO plus LookTransform, and P3.T8 is Display/LogConvert/CDL/FileTransform, which have no GenericOCIO config. P3.T4 is the generic reader/writer, and P3.T9 the per-plugin PNG/OIIO depth categories. P3.T5 is the pins (on an m66-descended branch), and P3.T10 drops the OpenColorIO-Configs fetch, bundling and CI cache. P4.T5 is the FrameKey hash. P5.T1 is the colour-neutral, dither-free conversion, and P5.T5 is the mechanical `convertToFormat` signature change across all callers, including the missed RenderRoI/pasteFrom/premult/GL-texture/Image_Test sites. P5.T3 is the Gui swatches plus InfoViewer, and P5.T6 the tracker overlay plus `Gui::debugImage`. The batches are re-cut, and P4.T1–P4.T4 share one batch because the viewer API cutover only compiles as a whole.
- 2026-10-02 — **B2+B3 landed.**
  - **Commits.** Natron: `c20213cc1` P4.T5, `8de19f3d3` P2.T1+P2.T2, `38ce7f853` pin. openfx-natron: `72b20b81` P3.T1. openfx-io: `773512e` P3.T3+P3.T4, `1ce6864` P3.T8.
  - **Tests.** Full debug ctest 749/749. The fix round fixed a null `projectPath` knob at `initializeKnobs` time.
  - **P3.T3 extra guard (accepted).** Colourspace rewrites run only when the config source actually changes, so a host push on load doesn't rename explicit spaces to roles or hide unresolved ones.
  - **P3.T4 writer gate (accepted).** It skips the re-guess on the restore path (a non-user-edit, existing instance) and whenever `ocioOutputSpaceSet`. A user filename edit still re-guesses, because the output space depends on the format. Gating on the existing-instance flag alone would have starved new Writes, since the flag is already set before their first filename.
  - **Build note.** `tools/ci/local/build.sh` resets submodules to their pins, so the openfx-natron branch has to be pinned before a build sees it.
- 2026-10-02 — **B4 landed** (Natron `1b5fdc3d3` P3.T7, `13789370e` P2.T3; io `547aa04` P3.T9), full debug ctest 756/756.
  - **P2.T3 extra (accepted):** before a load, the config knobs are reset to Studio and "". Otherwise a project left on the Studio default, which doesn't save the knob, would load as whatever the machine's preference is.
  - **P3.T9 extra (accepted):** a default-off `guessFileColourspaceFromMetadata` reader hook keeps a file's valid `oiio:ColorSpace` over the category default.
- 2026-10-02 — **B5 landed** (`fe43bf859` P5.T1, `dc67345bb` P2.T4, `ca8a6486e` pins: io `547aa04`, arena `49f5dd2`), with the full debug ctest at 763/763.
  - **Arena wasn't build-clean against the new io, despite the design doc.** The new GenericReader dropped the `filePremult` out-parameter, so six arena readers were updated. Arena's `.gitmodules` now points OpenFX-IO at charlesangus/openfx-io.
  - **P5.T1 also removed the error-diffusion dither** from float-to-int conversion, which follows from Q2's plain quantisation.
- 2026-10-02 — **Unresolved colourspace on a config switch (user): flag an error and keep the name.** A node whose colourspace the new config lacks keeps the saved name and shows the persistent unresolved-colourspace error until it's fixed or the config is switched back. Nothing is remapped silently. New task P3.T11 turns off the io fork's switch-time `existingColorSpaceOrFallback` when the host supplies the config.
- 2026-10-02 — **B6 landed** (`ac4b7f398` P5.T5, `66f3075eb` P5.T3, `de23eca99` P5.T6, `a80d12b95` P3.T10, `a20ee3ec3` P3.T2+P3.T11 + pins io `a7a511e`/arena `d51cb41`), with the full debug ctest at 769/769.
  - **P3.T2:** the config is pushed at the end of `Node::load` (NodeMain.cpp) and in the Read/Write `create*Node`. That covers new, pasted and swapped nodes.
  - **Open, for review:** an OCIOColorSpace created with no env config gets an empty output colourspace by default, because the plugin's describe-time default comes from the process env. In the GUI, Natron exports OCIO at startup, so the default resolves against the preference config.
  - **Fixed in the B6 round:** `Gui::debugImage` (static) used `getApp()`, so it now goes through `appPTR->getTopLevelInstance()`.
