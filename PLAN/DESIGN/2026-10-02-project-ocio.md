# Design: project-owned OCIO colour management (M50, Phase 50.1)

2026-10-02. This doc implements the M50 scope decision and the user's answers to Q1–Q3 (recorded below), and every M50 task cites it. It was scouted on `milestone/m37-channel-management-nodes`, the base M50 stacks on, plus the openfx-io worktree `build/wt/m66-io` (= pinned `OPENFX_IO_REF` 649ce94), `build/wt/m66-misc` and `build/openfx-arena-fork`. OCIO facts come from the natron-dev container (OCIO 2.5.2, `/usr/local/include/OpenColorIO`). Engine and Gui sites are cited by function, because M37 moved the line numbers. Plugin-repo line numbers are from the worktrees above.

## 0. User rulings (verbatim intent)

- **Scope.** Full replacement. A project OCIO config property, saved in the project and defaulting to ACES 2.0 Studio, drives:
  - the viewer's Display/View/Look menus;
  - every Read/Write/OCIO node's colourspace lists.

  A project working space and per-file-type defaults (Nuke-style) apply to **new** Read/Write nodes. Natron's built-in LUTs are retired, so every colour transform goes through OCIO. There is no backward compatibility.
- **Q1: the `OCIO` env var overrides, Nuke-style.** A set `OCIO` forces the config for every project, new or loaded. The project's config knobs are disabled, with a tooltip saying why. A loaded project's saved config stays in the file but is ignored while `OCIO` is set. There is no warning-only path. Unsetting `OCIO` and reloading restores the saved config.
- **Q2: explicit only.** At integer-only stages, Natron converts bit depth by plain linear quantisation (scale, clamp, round) and never applies an implicit colour transform. Colourspace changes are the user's job (for example an OCIOColorSpace node) or the plugin's own mapping option.
- **Q3: per-file-type defaults** (Read input and Write output alike):

  | Category | Default colourspace | Role it matches |
  |---|---|---|
  | 8-bit | `sRGB Encoded Rec.709 (sRGB)` | |
  | 16-bit | `sRGB Encoded Rec.709 (sRGB)` | |
  | log | `ACEScct` | `compositing_log` |
  | float | `ACEScg` | `scene_linear` |

  For 8-bit PNG output this supersedes the *label* in `docs/decisions/2026-09-01-png-output-srgb-display.md`, which was `sRGB - Display`. The pixels are identical.

## 1. Facts the rulings rest on (verified 2026-10-02)

**OCIO 2.5.2 built-in registry.** `OCIO::BuiltinConfigRegistry::Get()` lists 8 configs:
- `cg-config-v1.0.0_aces-v1.3_ocio-v2.1`, `cg-config-v2.1.0_aces-v1.3_ocio-v2.3`, `cg-config-v2.2.0_aces-v1.3_ocio-v2.4`;
- `cg-config-v4.0.0_aces-v2.0_ocio-v2.5` (recommended);
- the same four versions as `studio-config-*`, with v4.0.0 recommended.

`getBuiltinConfigUIName(i)` gives the "Academy Color Encoding System - … Config [COLORSPACES …] [ACES …] [OCIO …]" label.

**Studio v4.0.0 config:**
- Roles: `scene_linear` = ACEScg, `compositing_log` = ACEScct, `texture_paint` = `color_picking` = `sRGB Encoded Rec.709 (sRGB)`, `data` = Raw. There is **no `default` role**.
- Default display/view: `sRGB - Display` / `ACES 2.0 - SDR 100 nits (Rec.709)`. One look: `ACES 1.3 Reference Gamut Compression`.

**CG v4.0.0 config:** same roles and defaults. It has no `Camera Rec.709`.

**OCIO APIs available:**
- `LegacyViewingPipeline` (`OpenColorAppHelpers.h`) with `setDisplayViewTransform`, `setLinearCC`, `setDisplayCC`, `setLooksOverrideEnabled` and `setLooksOverride`.
- `Processor::getCacheID()`.
- `getOptimizedCPUProcessor(BIT_DEPTH_F32, BIT_DEPTH_F32, OPTIMIZATION_DEFAULT)`.
- `GPU_LANGUAGE_GLSL_1_2`.
- `Config::CreateFromFile` accepts `ocio://` URIs.

**Natron today:**
- **Preferences** (`Engine/Settings.cpp`):
  - Knobs: `ocioConfig` (choice; ids are the Studio URI, `share/OpenColorIO-Configs/<dir>` names and `NATRON_CUSTOM_OCIO_CONFIG_NAME` "Custom config"), `ocioCustomConfigFile`, `warnOCIOChanged` and `startupCheckOCIO`.
  - `Settings::tryLoadOpenColorIOConfig()` resolves env → custom → choice, then `qputenv("OCIO")` and calls `AppManager::onOCIOConfigPathChanged(dir)`.
  - `getDefaultOcioConfigPaths()` scans `share/OpenColorIO-Configs`.
  - `Settings::doOCIOStartupCheckIfNeeded()` is called from `GuiAppInstance::load`.
- **Project:**
  - `Project::initializeKnobs()` builds the "LUT" page: `defaultColorSpace8u/16u/32f`, with ids Linear/sRGB/Rec.709/BT1886 mapped through `colorspaceParamIndexToEnum`.
  - `Project::getDefaultColorSpaceForBitDepth`, forwarded by `AppInstance::getDefaultColorSpaceForBitDepth`.
  - `Project::onOCIOConfigPathChanged(path, block)` writes `[OCIO]` into `envVars`. It is called from `Project::initializeKnobs`, `Project::doResetEnd`, `ProjectPrivate::restoreFromSerialization` and `AppInstance::onOCIOConfigPathChanged`.
  - `Project::reportUnresolvedOCIOColorSpaces()` runs from `Project::loadProjectInternal`. It uses `createOcioConfig()`, which falls back to `CreateFromEnv()` when a node's `ocioConfigFile` is empty.
- **Choice knobs persist by id.**
  - `KnobSerialization` writes `ChoiceLabel` = `entries[idx].id`.
  - `ProjectPrivate::restoreFromSerialization` restores project knobs **in knob-creation order** through `KnobChoice::choiceRestorationId` / `choiceRestoration`.
  - An id that is not in the current entries is kept as `_currentEntry.id`, and the next `populateChoices()` re-matches it (`KnobChoice::findAndSetOldChoice`, which uses `choiceMatch`: exact id, then label, then case-insensitive).
  - `isActiveEntryPresentInEntries()` reports whether the id resolved.
- **Enabled and Secret are serialized per knob** (`KnobSerialization.h`, the `Enabled` and `Secret` nvps) and restored on load. So a knob disabled at save time comes back disabled unless the code re-derives the state after load.
- **OFX instance props.**
  - The host-side hook pattern is `OfxImageEffectInstance::OfxImageEffectInstance` → `getProps().setGetHook(kNatronOfxExtraCreatedPlanes, this)`, plus overrides of `getDimension` / `getUserCreatedPlanes`.
  - The property must also be declared in `OFX::Host::ImageEffect::Instance`'s PropSpec (`libs/OpenFX/HostSupport/src/ofxhImageEffect.cpp`, the `kNatronOfxExtraCreatedPlanes` entry), and it is dispatched in `Instance::getStringProperty` / `getStringPropertyN`.
  - `ofxColour.h` (`kOfxImageEffectPropOCIOConfig`, a string of dimension 1 that may be an `ocio://` URI) is absent from `libs/OpenFX` and from openfx-io.
- **Value-change reasons.** `OfxEffectInstance::natronValueChangedReasonToOfxValueChangedReason` maps `eValueChangedReasonNatronInternalEdited` → `kOfxChangeUserEdited`. Only `eValueChangedReasonPluginEdited` maps to `kOfxChangePluginEdited` (`eChangePluginEdit` in the plugin).
- **Child processes inherit the environment.** `ProcessHandler` (background renders launched from the GUI) spawns `QProcess` without touching the environment, so children inherit whatever `OCIO` the parent exported.
- **Linking.** `NatronEngine` links `OpenColorIO::OpenColorIO` **PRIVATE** (`Engine/CMakeLists.txt`), so OCIO headers don't reach Gui or Tests.
- **GL.** The loader is glad `gl=2.0` compatibility (`Global/glad{Deb,Rel}`), and it exposes `glTexImage3D`. The 32f viewer path always uses shaders: `FrameKey::_useShaders` = depth is float, and `FrameKey::fillHash` skips gain/gamma/lut when shaders are used.
- **Viewer gain/offset/gamma.**
  - CPU 8-bit (`scaleToTexture8bits_generic`): `v*gain + offset` → gamma lookup (`interpolateGammaLut`) → `toColorSpaceUint8xxFromLinearFloatFast` with error-diffusion dither. Gamma ≤ 0 is a threshold mode.
  - GLSL (`fragRGB`): gain/offset → LUT → gamma.
  - `offset` is non-zero only for auto-contrast (`ViewerInstance::renderViewer_internal`).

**openfx-io (m66-io) facts** (`IOSupport/` unless noted):
- **Params.** `kOCIOParamConfigFile` "ocioConfigFile" is a file-path string, persistent, non-animating and not secret. Only `GenericOCIO::describeInContextInput` defines it. Its default is `getenv("OCIO")`.
- **Other names:** `ocioInputSpace`, `ocioOutputSpace`, `ocioInputSpaceIndex` and `ocioOutputSpaceIndex` (the choices are non-persistent). The set-flags are `ocioInputSpaceSet` (`GenericReader.cpp` `kParamInputSpaceSet`) and `ocioOutputSpaceSet` (`GenericWriter.cpp` `kParamOutputSpaceSet`).
- **Config reload.**
  - `GenericOCIO::changedParam` reloads on every reason except `eChangeTime`, which includes `eChangePluginEdit`.
  - `loadConfig` rebuilds the menus under `gHostIsNatron`.
  - An unresolved space falls back to `ROLE_DEFAULT`, then index 0: config-change at `GenericOCIO.cpp:974/978/992/996`, user edits at `:1181/:1228`.
  - `existingColorSpaceOrFallback(config, name)` (`:263`, order: name, `scene_linear`, `default`, index 0) exists only from commit 40764b2, which is on m65/m66 and **not** in arena's `OpenFX-IO` copy (f30a6a8).
- **OCIO nodes** (`OCIO/`):
  - `OCIODisplay` builds its display/view menus once, from `OCIO::GetCurrentConfig()`, in its constructor.
  - `OCIOLookTransform` rebuilds looks only on `eChangeUserEdit` (`:907`).
  - `OCIOLogConvert` has its own `ocioConfigFile` and `loadConfig`.
  - `OCIOCDLTransform` and `OCIOFileTransform` have **no** config param; they use `OCIO::GetCurrentConfig()`.
- **Reader guess.** `GenericReaderPlugin::changedFilename` guesses only on `eChangeUserEdit` with `kParamExistingInstance` false. It calls the per-reader virtual `guessParamsFromFilename(file, &colorspace, &components, &count)` (`GenericReader.h`), and a filename colourspace (`getColorSpaceFromFilepath`) wins.

  | Reader | Knows bit depth? | Legacy guesses |
  |---|---|---|
  | ReadEXR, ReadPFM | n/a | `scene_linear` |
  | ReadPNG | yes (`getPNGInfo`) | gAMA → Gamma1.8/Gamma2.2 chains; sRGB chunk → sRGB; else sRGB for 8-bit, Rec709 for 16-bit |
  | ReadFFmpeg | no | always the Rec709 chain (it does **not** read the trc) |
  | ReadOIIO (`guessColorspace`) | yes | `oiio:ColorSpace` → ICC → pixel type: 8-bit sRGB; 16-bit `.cin`/`.dpx` KodakLog; other 16-bit Rec709; float Linear |

  The only log guess is `KodakLog`.
- **Writer guess.**
  - `GenericWriterPlugin::outputFileChanged` runs on filename change **and on every restore**: `restoreStateFromParams` calls it with `eChangePluginEdit`. It re-guesses unless `ocioOutputSpaceSet`.
  - Per-writer `onOutputFileChanged(file, setColorSpace)`:

    | Writer | Output-space guess |
    |---|---|
    | WriteEXR, WritePFM | `scene_linear` |
    | WriteFFmpeg | Rec709 |
    | WriteOIIO | by bit depth: 8 → sRGB; 10/12/16 `.cin`/`.dpx` → Cineon; other 10/12/16 → Rec709; else Linear |
    | WritePNG | 8-bit sRGB; 16-bit `scene_linear` |
- **Depths.** GenericReader and GenericWriter advertise float only.
- **Arena.** Arena compiles only `GenericReader.cpp`, `GenericOCIO.cpp` and `SequenceParsing.cpp` from its `OpenFX-IO` submodule (`CMakeLists.txt:159-161`). Its readers are `GenericReader` subclasses.

## 2. Project knobs: a new "Color" page

`Project::initializeKnobs()` creates the page and knobs in **this order**, because restore follows creation order (§1):

| Name | Type | Default | Notes |
|---|---|---|---|
| `ocioConfig` | Choice | the Studio URI `ocio://studio-config-v4.0.0_aces-v2.0_ocio-v2.5` | Entries: one per `BuiltinConfigRegistry` entry, id = `"ocio://" + getBuiltinConfigName(i)`, label = `getBuiltinConfigUIName(i)`, with "(recommended)" appended when `isBuiltinConfigRecommended(i)`. Then `ChoiceOption(NATRON_CUSTOM_OCIO_CONFIG_NAME)` ("Custom config"). Not animated. |
| `ocioConfigFile` | File | "" | Enabled only when `ocioConfig` = Custom (and no override, §3). The value may use `[Project]`; it is expanded with `Project::canonicalizePath`. Not animated. |
| `workingSpace` | Choice | the colourspace the `scene_linear` role names (Studio: `ACEScg`) | |
| `colorSpace8Bit` | Choice | `sRGB Encoded Rec.709 (sRGB)` | Q3 |
| `colorSpace16Bit` | Choice | `sRGB Encoded Rec.709 (sRGB)` | Q3 |
| `colorSpaceLog` | Choice | `ACEScct` | Q3 |
| `colorSpaceFloat` | Choice | `ACEScg` | Q3 |
| `viewerDisplay` | Choice | `config->getDefaultDisplay()` | |
| `viewerView` | Choice | `config->getDefaultView(viewerDisplay)` | Repopulated when `viewerDisplay` changes. |

- **Ids.** Colourspace entries are `ChoiceOption(name, name, description)` built from `config->getColorSpaceNameByIndex(SEARCH_REFERENCE_SPACE_ALL, COLORSPACE_ALL, i)`, excluding inactive colourspaces, so each knob persists the **name**. Display/view entries use display and view names.
- **Default values are set by id** (`setDefaultValueFromID`) after the Studio config is populated. The defaults are written as the literal Q3 names, so they stay stable even if a role changes in a future config.
- **Fallback when a name doesn't resolve after a config switch** (via `isActiveEntryPresentInEntries()` after `populateChoices`):

  | Knob | Fallback order |
  |---|---|
  | `workingSpace`, `colorSpaceFloat` | `scene_linear` |
  | `colorSpaceLog` | `compositing_log` → `scene_linear` |
  | `colorSpace8Bit`, `colorSpace16Bit` | `texture_paint` → `color_picking` → `scene_linear` |
  | `viewerDisplay` | `getDefaultDisplay()` |
  | `viewerView` | `getDefaultView(display)` |

  - A role is resolved with `config->getRoleColorSpace(role)`. If every candidate is empty, the knob takes index 0.
  - All fallbacks fired by one switch are collected and reported in **one** warning: `Dialogs::warningDialog` in the GUI, the log in background, plus `appPTR->writeToErrorLog_mt_safe`. The warning lists each knob as old → new.
  - The fallback rewrites the knob value, so a later save stores the resolved name. That applies under the env override too (§3); only `ocioConfig`/`ocioConfigFile` are guaranteed untouched.
- **Single refresh point.** `void Project::refreshColorManagement(bool warnOnFallback)` (private, in `Project.cpp`):
  1. loads `getOCIOConfigSource()` into `ProjectColorManagement`;
  2. on failure, keeps the previous config and shows an error. A failure on a fresh project leaves the Studio config;
  3. repopulates the 6 colourspace/display/view choices and applies the fallbacks;
  4. re-derives the enabled state and tooltip of `ocioConfig`/`ocioConfigFile` (§3);
  5. updates `[OCIO]` (§3);
  6. fires `configChanged`.

  It is called from:
  - `Project::onKnobValueChanged` for `ocioConfig`/`ocioConfigFile`;
  - the end of the knob loop in `ProjectPrivate::restoreFromSerialization`. It is needed there because pending ids re-match on populate (§1), so the order in which other knobs restore doesn't matter;
  - `Project::doResetEnd`;
  - `Project::initializeKnobs`.
- All Color-page knobs trigger auto-save, like the LUT knobs in `Project::onKnobValueChanged` today.
- **Accessors on `Project`** (P2.T2):
  ```cpp
  std::string getOCIOConfigSource() const;              // effective source: URI or absolute path (§3)
  std::string getWorkingColorSpace() const;             // workingSpace active id
  std::string getFileColorSpace(FileColorCategoryEnum category) const;
  void getDefaultDisplayView(std::string* display, std::string* view) const;
  ProjectColorManagementPtr getColorManagement() const;
  ```
  `FileColorCategoryEnum { eFileColorCategory8Bit, eFileColorCategory16Bit, eFileColorCategoryLog, eFileColorCategoryFloat }` lives in `Engine/ProjectColorManagement.h`, not in `Global/Enums.h`, so it stays out of Shiboken. `ProjectColorManagementPtr` goes in `Engine/EngineFwd.h`. Accessors are thread-safe: they read the knobs' active ids via `getActiveEntry()` under the knob mutex. Render threads use the `ProjectColorManagement` object, never the knobs.

### 2.1 `ProjectColorManagement` (P2.T1)

`Engine/ProjectColorManagement.{h,cpp}`. It has no knob dependencies, and its header includes `<OpenColorIO/OpenColorIO.h>`, which needs the Engine link change in §10, correction 1.

```cpp
class ProjectColorManagement {
public:
    enum LoadErrorEnum { eLoadErrorNone, eLoadErrorNoSuchFile, eLoadErrorOCIO };
    // source: "ocio://..." or a path; a relative path resolves against baseDir. Never throws.
    LoadErrorEnum load(const std::string& source, const std::string& baseDir, std::string* error);
    std::string getConfigSource() const;            // as resolved (absolute path or URI)
    std::string getConfigDirectory() const;         // "" for a URI
    OCIO_NAMESPACE::ConstConfigRcPtr getConfig() const;

    std::vector<std::string> getColorSpaces() const;   // active only
    std::vector<std::string> getRoles() const;
    std::vector<std::string> getDisplays() const;
    std::vector<std::string> getViews(const std::string& display) const;
    std::vector<std::string> getLooks() const;
    std::string getDefaultDisplay() const;
    std::string getDefaultView(const std::string& display) const;
    std::string resolveRoleOrName(const std::string& roleOrName) const;   // "" when unresolved

    struct DisplayProcessor {
        OCIO_NAMESPACE::ConstProcessorRcPtr processor;     // for the GPU path
        OCIO_NAMESPACE::ConstCPUProcessorRcPtr cpu;        // F32 -> F32
        std::string cacheID;                               // processor->getCacheID()
        U64 cacheHash;                                     // Hash64 of cacheID, for FrameKey
    };
    typedef std::shared_ptr<const DisplayProcessor> DisplayProcessorPtr;
    // look == "" means no look override. Returns null and fills error on failure.
    DisplayProcessorPtr getDisplayProcessor(const std::string& src, const std::string& display,
                                            const std::string& view, const std::string& look,
                                            std::string* error = 0) const;
    OCIO_NAMESPACE::ConstCPUProcessorRcPtr getConversionProcessor(const std::string& src,
                                                                  const std::string& dst) const;
    // P5.T3: working space -> color_picking role and back, used for swatches and the tracker overlay
    void workingToColorPicking(float* r, float* g, float* b) const;
    void colorPickingToWorking(float* r, float* g, float* b) const;

    typedef std::function<void()> ConfigChangedCallback;
    int addConfigChangedCallback(const ConfigChangedCallback& cb);
    void removeConfigChangedCallback(int id);
    void notifyConfigChanged();                     // Project calls this from refreshColorManagement
};
```

- **Thread safety.** The config is a `ConstConfigRcPtr`, swapped under a `QMutex`. Readers copy the pointer under the lock.
- **Processor caches.** There are two `std::map<std::string, …>` caches, keyed by `src|display|view|look` and by `src|dst`. Both are guarded by the same mutex and cleared on `load()`. Processor objects are immutable and shared.
- `workingToColorPicking` takes the working space as a parameter set by Project (`setWorkingSpace(name)`), so the class still has no knob access.

## 3. Config precedence (Q1)

**`Settings` (P2.T3):**
- `std::string Settings::getOCIOEnvOverride() const`.
  - It returns the `OCIO` value captured **once**, in `Settings::tryLoadOpenColorIOConfig()` before its first `qputenv`. A file-static flag guards the capture.
  - It is empty when `OCIO` was unset, **or** when `NATRON_OCIO_ENV_IS_PREFERENCE=1` is also set (see below).
  - Test hook: `static void Settings::recaptureOCIOEnvOverrideForTests()`.
- `std::string Settings::getDefaultOCIOConfigSourceForNewProjects() const`. It returns the URI of the preference choice, or the `ocioCustomConfigFile` path for Custom. If that is empty, it returns the Studio URI.
- **Preference knobs.** `ocioConfig` and `ocioCustomConfigFile` are relabelled "Default OpenColorIO config for new projects" and "Custom OpenColorIO config file for new projects".
  - `ocioConfig` lists the same registry entries + Custom as §2, through a shared helper `std::vector<ChoiceOption> ProjectColorManagement::builtinConfigOptions()`.
  - When an override is set, both are disabled with the override tooltip.
- **Startup export.**
  - With an override, `qputenv("OCIO", override)`.
  - Otherwise `qputenv("OCIO", preference)` **and** `qputenv("NATRON_OCIO_ENV_IS_PREFERENCE", "1")`.

  The export lets plugin describe-time defaults (`GenericOCIO::describeInContextInput/Output`, `OCIO::GetCurrentConfig()` users) and OIIO resolve. The marker stops child processes, which inherit the environment through `ProcessHandler`, from mistaking Natron's own export for a user override.
- **Deleted:**
  - `warnOCIOChanged`, `startupCheckOCIO`, `Settings::doOCIOStartupCheckIfNeeded` and its call in `GuiAppInstance::load`;
  - `getDefaultOcioConfigPaths`, the `share/OpenColorIO-Configs` scan, and both restart dialogs (in `Settings::onKnobValueChanged` and in the "Warn on OpenColorIO config change" block of the `Settings` value-change loop);
  - the `appPTR->onOCIOConfigPathChanged` call.

**`Project` (P2.T2 / P2.T3):**
- `Project::getOCIOConfigSource()` is the **only** resolution point. It returns `Settings::getOCIOEnvOverride()` when non-empty. Otherwise it returns the `ocioConfig` id, or the canonicalized `ocioConfigFile` for Custom. Every consumer goes through it: `ProjectColorManagement`, the OFX props, the `ocioConfigFile` push, the viewer, the menus and `reportUnresolvedOCIOColorSpaces`. Nothing else reads `ocioConfig`/`ocioConfigFile`.
- **New project.** `Project::initializeKnobs` (and `doResetEnd`) set the knob defaults from `getDefaultOCIOConfigSourceForNewProjects()`: a URI selects the matching entry; a path selects Custom plus the path.
- **Loaded project.** The saved `ocioConfig`/`ocioConfigFile` are restored as written, and they are written back unchanged on save even while overridden.
- **Enabled state.** It is re-derived in `refreshColorManagement`, after every restore, because Enabled is serialized (§1):
  - `ocioConfig` is enabled iff there is no override;
  - `ocioConfigFile` is enabled iff there is no override and the choice is Custom.

  With an override, both get the tooltip `Overridden by the OCIO environment variable (<value>)`; otherwise the normal tooltip is restored.
- **`[OCIO]` path variable (P2.T4).** `Project::onOCIOConfigPathChanged(const std::string& path, bool block)` is kept, with its `fixRelativeFilePaths` behaviour, but its only caller becomes `refreshColorManagement`.
  - It passes `ProjectColorManagement::getConfigDirectory()` of the **effective** config. Under an override, that is the override's directory.
  - When the directory is empty (a URI), the `[OCIO]` row is **removed** from `envVars` rather than set empty. The `Settings::tryLoadOpenColorIOConfig` comment explains why an empty value is unsafe for `simplifyPath`.
  - Deleted with it: `AppManager::onOCIOConfigPathChanged`, `AppManager::getOCIOConfigPath`, `AppManagerPrivate::currentOCIOConfigPath` and `AppInstance::onOCIOConfigPathChanged`.

## 4. Plugin contract

### 4.1 Host side

**OFX headers** (`libs/OpenFX`, the charlesangus/openfx-natron fork):
- Add `include/ofxColour.h`, verbatim from ASWF openfx 1.5. The copy is in `build/assets/plugin-src/openfx-metadata/include/`.
- In `include/ofxNatron.h`, add:
  ```c
  #define NatronOfxImageEffectPropOCIOWorkingColourspace "NatronOfxImageEffectPropOCIOWorkingColourspace" // string, dim 1
  #define NatronOfxImageEffectPropOCIOFileColourspaces  "NatronOfxImageEffectPropOCIOFileColourspaces"   // string, dim 4: 8-bit, 16-bit, log, float
  ```

**HostSupport** (`libs/OpenFX/HostSupport/src/ofxhImageEffect.cpp` and `include/ofxhImageEffect.h`, same submodule):
- Add 3 entries to the `Instance` PropSpec, inside `#ifdef OFX_EXTENSIONS_NATRON`:
  - `kOfxImageEffectPropOCIOConfig`, eString, dim 1;
  - the working prop, eString, dim 1;
  - the file prop, eString, dim 4.
- Add virtuals:
  ```cpp
  virtual const std::string& getOCIOConfigSource() const;                // default: static ""
  virtual const std::string& getOCIOWorkingColourspace() const;          // default: static ""
  virtual const std::vector<std::string>& getOCIOFileColourspaces() const; // default: empty
  ```
- Dispatch them in `Instance::getStringProperty`, `getStringPropertyN` and `getDimension`.

**Natron** (`Engine/OfxImageEffectInstance.{h,cpp}`):
- The constructor adds `setGetHook` for the 3 props, next to `kNatronOfxExtraCreatedPlanes`.
- The overrides read `_ofxEffectInstance.lock()->getApp()->getProject()`:
  - `getOCIOConfigSource()`;
  - `getWorkingColorSpace()`;
  - `getFileColorSpace(…)` × 4.
- Values are **live**: every get re-reads the project. The overrides return references into `mutable` member strings refreshed under a `QMutex` on each call. Plugins read them on the main thread (construction, `changedParam`).

**`ocioConfigFile` push (P3.T2):** `void Project::pushOCIOConfigToNodes()`.
- **When it runs:**
  - from `Node::initializeKnobs`'s end (or `Node::load` completion) for a newly created node;
  - for every node in `getNodes_recursive` on `configChanged`;
  - once after project load, after node restore, so a node's saved value never wins.
- **What it does.** For each node that has a `KnobStringBase` named `ocioConfigFile`, set it to `getOCIOConfigSource()` with `eValueChangedReasonPluginEdited`, then `setSecret(true)`. Secret is serialized, so it must be re-applied on every push, not once.
- **Read/Write wrappers.** For the Read and Write wrappers (`ReadNode`/`WriteNode`), the push targets the wrapper's own knob and the embedded decoder's or encoder's knob. Both carry `ocioConfigFile`, per the persistent-param tables `kOCIOParamConfigFile` in `ReadNode.cpp`/`WriteNode.cpp`.

**`reportUnresolvedOCIOColorSpaces`** checks each node's `ocioInputSpace`/`ocioOutputSpace` against `getColorManagement()->getConfig()`, the effective config, instead of the per-node `createOcioConfig`/`CreateFromEnv`. It runs on load and from `configChanged`. Nodes that resolve after a switch get `clearPersistentMessage` (only for the message this function set; use a dedicated key prefix in the message, or a `Node` flag).

### 4.2 Plugin side (openfx-io fork, P3.T3/P3.T4)

- **Guarded defines** in `IOSupport/GenericOCIO.h`: `#ifndef kOfxImageEffectPropOCIOConfig …` plus the two Natron props, pointing at `ofxColour.h`/`ofxNatron.h`.
- **Helper:**
  ```cpp
  bool GenericOCIO::hostConfigSource(std::string* source) const;   // propGetString(kOfxImageEffectPropOCIOConfig, false); true when non-empty
  ```
- **Construction.** When the host prop is present, `GenericOCIO` loads the config from it and sets `ocioConfigFile` secret. Without the prop, the old param-driven path stays.
- **Reload.** Every `ocioConfigFile` change with a reason other than `eChangeTime` reloads the config and rebuilds the menus. The reload logic applies this to the following nodes:

  | Node | Change |
  |---|---|
  | `OCIOLookTransform` | Drop the `eChangeUserEdit` + `configIsDefault()` gate at `:907` and in the constructor. |
  | `OCIODisplay` | Rebuild `display`/`view` from the instance config, not `GetCurrentConfig()`. |
  | `OCIOLogConvert` | Its own `loadConfig` reads the host prop first. |
  | `OCIOCDLTransform`, `OCIOFileTransform` | Add a secret, persistent `ocioConfigFile` string param, so the host push reaches them and invalidates their render hash. Replace `GetCurrentConfig()` with the instance config (correction 6). |

- **Fallback.** Replace the `ROLE_DEFAULT` → index-0 fallbacks (`GenericOCIO.cpp` config-change and user-edit branches) with `existingColorSpaceOrFallback`.
- **Defaults apply only to new nodes.** The host's working space and file defaults are consulted **only** when a new instance guesses:
  - **Reader:** `GenericReaderPlugin::changedFilename` with `eChangeUserEdit`, `kParamExistingInstance` false and `ocioInputSpaceSet` false.
  - **Writer:** `GenericWriterPlugin::outputFileChanged` with the same existing-instance and `ocioOutputSpaceSet` gates. With the host props present it does **not** re-guess from `restoreStateFromParams` (correction 7).

  Changing a project default never edits an existing node.

### 4.3 Reader and writer file-category rule

New virtuals; the defaults live in `GenericReader`/`GenericWriter`, so arena readers need no change:

```cpp
enum FileColourCategoryEnum { eFileColourCategory8Bit = 0, eFileColourCategory16Bit, eFileColourCategoryLog, eFileColourCategoryFloat };
// GenericReaderPlugin
virtual FileColourCategoryEnum guessFileColourCategory(const std::string& filename, const std::string& legacyGuess) const;
// GenericWriterPlugin
virtual FileColourCategoryEnum guessFileColourCategory(const std::string& filename, int bitDepth /* 0 = format default */) const;
```

**Default implementation** (both), applied to the legacy guess string the reader/writer already computes:
- `scene_linear`, `Linear`, `linear` or any `ROLE_SCENE_LINEAR` result → float;
- `KodakLog`, `Cineon`, `ADX` or `compositing_log` → log;
- otherwise → 8-bit.

**Overrides that know the depth:**

| Plugin | Rule |
|---|---|
| ReadPNG (`getPNGInfo`) | 16-bit → 16-bit; 8-bit and below → 8-bit. |
| ReadOIIO (`guessColorspace`'s spec) | float/half → float; a `.cin`/`.dpx` log guess → log; 16-bit integer → 16-bit; otherwise 8-bit. |
| WritePNG | 16-bit → 16-bit; else 8-bit. |
| WriteOIIO (`_bitDepth`) | float/half → float; a `.cin`/`.dpx` log guess → log; 16-bit integer → 16-bit; else 8-bit. |

- **Reader result.** Input space = `fileColourspaces[category]`, then output space = the working space, unless a filename colourspace rule matched; that rule still wins for the input space.
- **Writer result.** Input space = the working space; output space = `fileColourspaces[category]`.
- **Metadata-derived display names are dropped when the host props exist.** That covers ReadPNG's gAMA → Gamma1.8/2.2 and sRGB-chunk → sRGB chain. (ReadFFmpeg never read the trc; see correction 8.)
- **The legacy guess still decides the category,** so EXR (`scene_linear`) → float and `.cin`/`.dpx` → log.

## 5. Viewer pipeline (P4.T1–P4.T4)

### 5.1 The processor

`ProjectColorManagement::getDisplayProcessor(workingSpace, display, view, look)` builds an OCIO `LegacyViewingPipeline`:
- `DisplayViewTransform`: src = the working space, display, view.
- `setLooksOverrideEnabled(!look.empty())` and `setLooksOverride(look)`.
- **No `linearCC` and no `displayCC`.** Gain, offset and gamma stay Natron-side, around the processor. This is correction 4: the auto-contrast offset and the gamma ≤ 0 threshold mode can't be expressed as OCIO exposure/gamma, and dynamic properties on a CPU processor shared across viewers and render threads aren't safe.

The processor depends only on (config, working space, display, view, look), so its `getCacheID()` stays stable while the user drags gain or gamma.

**Order (both paths), per pixel:** `rgb' = rgb * gain + offset` (in the working space) → processor (look + display/view) → gamma: `gamma <= 0` gives the threshold, else `pow(c, 1/gamma)` → output.

This changes the CPU path, which today applies gamma *before* the LUT. Both paths now agree. Alpha is never transformed.

### 5.2 CPU path (8-bit textures, the default `texturesBitDepth` = 8u)

- **Engine params.**
  - `ViewerInstance::setDisplayTransform(const std::string& display, const std::string& view, const std::string& look)` replaces `onColorSpaceChanged`. It stores the strings under `viewerParamsMutex` and re-renders when the texture depth is byte, as `onColorSpaceChanged` does today.
  - `ViewerInstancePrivate::viewerParamsLut` becomes `viewerParamsDisplay`, `viewerParamsView` and `viewerParamsLook`.
  - `UpdateViewerParams::lut` becomes `ProjectColorManagement::DisplayProcessorPtr displayProcessor`, filled in `ViewerInstance::setupMinimalUpdateViewerParams`.
  - Delete `ViewerInstance::getLutType` and `ViewerInstance::lutFromColorspace`.
- **Free function for tests:**
  ```cpp
  // Engine/ViewerInstance.h (namespace scope)
  void applyViewerDisplayTransform(const ProjectColorManagement::DisplayProcessor& p, float* rgba, int width,
                                   double gain, double offset, double gamma);
  ```
  It applies gain/offset, then `p.cpu->apply(OCIO::PackedImageDesc(rgba, width, 1, 4))`, then gamma, in place, on one RGBA F32 scanline.
- **`scaleToTexture8bits_generic`.**
  - It fills a per-scanline RGBA float buffer from the source (byte/short sources go through plain `intToFloat`, per Q2).
  - It calls `applyViewerDisplayTransform`, then runs the existing error-diffusion dither with `Color::floatToInt<0xff01>` (the linear quantiser, replacing `toColorSpaceUint8xxFromLinearFloatFast`).
  - The alpha matte overlay (`applyMatte`) goes through the same processor.
  - The `srcColorSpace`/`colorSpace` members of `RenderViewerArgs` (`ViewerInstancePrivate.h`) are deleted.
- **`scaleToTexture32bitsGeneric`.** Drop `srcColorSpace`. Byte/short sources use plain `intToFloat`. No transform here; the shader does it.
- **`ViewerInstance::renderViewer_internal`.** Drop the `srcColorSpace` lookup through `getDefaultColorSpaceForBitDepth`.
- **Cache key.** `FrameKey` (`Engine/FrameKey.{h,cpp}`) replaces `int _lut` with `U64 _displayTransformHash` (`DisplayProcessor::cacheHash`).
  - It stays in `fillHash` and `operator==` only when `!_useShaders`, as today.
  - `FrameEntrySerialization.h` renames the nvp `Lut` to `DisplayTransformHash` and bumps the `FrameKey` serialization version. Old viewer disk-cache entries are dropped by the version mismatch; that's the clean break.

### 5.3 GLSL path (32f textures)

**`Gui/Shaders.cpp`.** `fragRGB` is replaced by a composed source built at runtime:

```glsl
uniform sampler2D Tex; uniform float gain; uniform float offset; uniform float gamma;
<OCIO shader text: GpuShaderDesc::getShaderText(), function OCIODisplay(vec4), resources prefixed "ocio_">
void main() {
  vec4 c = texture2D(Tex, gl_TexCoord[0].st);
  c.rgb = c.rgb * gain + offset;
  c = OCIODisplay(c);
  if (gamma <= 0.) { c.rgb = vec3(greaterThanEqual(c.rgb, vec3(1.))); } else { c.rgb = pow(max(c.rgb, 0.), vec3(1./gamma)); }
  gl_FragColor = c;
}
```

`vertRGB` is unchanged.

**`ViewerGL::Implementation`** (`Gui/ViewerGLPrivate.{h,cpp}`) owns:
- `std::string shaderCacheID`;
- `std::vector<GLuint> ocioTextures`, uploaded to texture units 1…N;
- the sampler uniform names.

**Building the shader:**
1. `OCIO::GpuShaderDesc::CreateShaderDesc()`;
2. `setLanguage(GPU_LANGUAGE_GLSL_1_2)`, `setFunctionName("OCIODisplay")`, `setResourcePrefix("ocio_")`;
3. `processor->getDefaultGPUProcessor()->extractGpuShaderInfo(desc)`;
4. 3D LUTs: `getNum3DTextures` / `get3DTexture` / `get3DTextureValues` → `glTexImage3D` (GL_RGB32F_ARB when available, otherwise GL_RGB16F; `GL_LINEAR` / `GL_NEAREST` per the interpolation);
5. 1D/2D LUTs: `getNumTextures` / `getTexture` / `getTextureValues` → `glTexImage2D` (OCIO packs 1D LUTs as 2D when its max width is exceeded);
6. uniforms: `getNumUniforms` / `getUniform`. These are none for this pipeline, because it has no dynamic properties; read them anyway for robustness.

The program is rebuilt in `ViewerGL::Implementation::activateShaderRGB` only when `processor->getCacheID()` differs from `shaderCacheID`. `activateShaderRGB` binds Tex = 0, the OCIO samplers = 1…N, and gain/offset/gamma per texture index.

**`ViewerGL` API:**
- `void ViewerGL::setDisplayTransform(const std::string& display, const std::string& view, const std::string& look)` replaces `setLut(int)`, and the `displayingImageLut` member is deleted. A/B wipe shares one transform; gain/offset stay per texture index, as today.
- `OpenGLViewerI::endTransferBufferFromRAMToGPU` drops its `int lut` parameter (`Engine/OpenGLViewerI.h`, `Gui/ViewerGL.h`).

**Picker.**
- The `ViewerGL::getColorAt` / `getColorAtRect` / `getColorAtInternal` template drop their `Color::Lut*` arguments.
- The linear readout is the image value: byte/short through `intToFloat`, per Q2.
- The non-linear readout (`forceLinear` false) runs the pixel through `applyViewerDisplayTransform` with gain 1, offset 0, gamma 1.
- Delete the commented-out GPU-readback block in `getColorAt` (it references `eViewerColorSpaceLinear`).

**GLSL compatibility risk.** The context is GL 2.0 compatibility (glad `gl=2.0`). OCIO's GLSL 1.2 output uses `texture3D`/`texture2D` and no `#version`. If a driver rejects it, P4.T2 logs the compile log and falls back to the 8-bit CPU path for that viewer (`texturesBitDepth` behaves as 8u). The P4.T2 Xvfb check asserts that no compile error is logged.

### 5.4 Viewer menus and state

**`ViewerTab`** (P4.T3). `ViewerTabPrivate::viewerColorSpace` becomes three `ComboBox`es:
- `viewerDisplay`;
- `viewerView`, repopulated per display;
- `viewerLook`, "None" + `getLooks()`.

They are seeded from `Project::getDefaultDisplayView()` and Look = None.

- On `configChanged`, they repopulate and keep the selection by name. Otherwise they fall back to the project defaults, then the config defaults.
- Each change calls `ViewerGL::setDisplayTransform` and `ViewerInstance::setDisplayTransform`.
- `ViewerTab::getColorSpace` / `setColorSpace` and `onColorSpaceComboBoxChanged` (`ViewerTab10.cpp` / `ViewerTab30.cpp`) are replaced by:
  ```cpp
  void getDisplayTransform(std::string* display, std::string* view, std::string* look) const;
  void setDisplayTransform(const std::string& display, const std::string& view, const std::string& look);
  void onDisplayTransformComboBoxChanged(int);
  ```
- Gamma tooltip: "applied after the display transform".

**`ViewerData`** (P4.T4, `Gui/ProjectGuiSerialization.h`).
- `std::string colorSpace` becomes `display`, `view` and `look`, with a new `VIEWER_DATA_INTRODUCES_OCIO_DISPLAY 15` = `VIEWER_DATA_SERIALIZATION_VERSION`.
- The `ColorSpace` nvp is not read at any version (clean break). An older archive fails to load the viewer GUI state, and that is acceptable.
- Writer: `ProjectGuiSerialization::initialize`. Reader: the static `loadNodeGuiSerialization` in `Gui/ProjectGui.cpp`, which calls `tab->setDisplayTransform`, falling back to the project defaults for unresolved names.

## 6. Retirement map

Every `Color::Lut` / `LutManager` / `ViewerColorSpaceEnum` / `getDefaultColorSpaceForBitDepth` / `to_func_srgb` / `from_func_srgb` site on the branch (`git grep`, 2026-10-02):

| Site (function) | Today | Replacement | Task |
|---|---|---|---|
| `Image::convertToFormat`, `convertToFormatInternal`, `convertToFormatInternalForColorSpace`, `convertToFormatInternalForDepth`, `convertToFormatInternal_sameComps` (`Engine/Image.h`, `Engine/ImageConvert.cpp`), `lutFromColorspace` (static in `ImageConvert.cpp`) | src/dst `ViewerColorSpaceEnum` → LUTs, error-diffusion dither to 8-bit | Drop both colourspace params; plain `convertPixelDepth` (`intToFloat` / `floatToInt<256>` / `floatToInt<65536>`): scale, clamp, round, **no dither**. `convertToFormatInternalForColorSpace` is deleted. New signature: `void convertToFormat(const RectI& renderWindow, int channelForAlpha, bool copyBitMap, Image* dstImg) const;` | P5.T1 |
| `EffectInstance::Implementation::renderHandler` (6 calls) | `getDefaultColorSpaceForBitDepth(src/dst depth)` | the new `convertToFormat` | P5.T1 |
| `EffectInstance::convertLayersFormatsIfNeeded` (`EffectInstanceRenderRoI.cpp`) | same | same | P5.T1 (missing from brief) |
| `EffectInstance::convertRAMImageToOpenGLTexture` | Linear/Linear | same | P5.T1 (missing) |
| `Image::pasteFrom` (`Image.cpp`), `Image::premultByChannel` (`ImagePremult.cpp`) | Linear/Linear | same | P5.T1 (missing) |
| `RotoPaint::render` (3 calls), `DiskCacheNode::render` | `getDefaultColorSpaceForBitDepth` | same | P5.T1 |
| `Tests/Image_Test.cpp` (5 `convertToFormat` calls; the sRGB→sRGB RGBA→RGB case) | enum args | new signature. The sRGB-LUT case is deleted, since its subject no longer exists. | P5.T1 (missing) |
| `Project::getDefaultColorSpaceForBitDepth`, `colorspaceParamIndexToEnum`, `defaultColorSpace8u/16u/32f`, the "LUT" page, the auto-save branch in `Project::onKnobValueChanged`; `AppInstance::getDefaultColorSpaceForBitDepth` | LUT page | deleted (the Color page replaces it) | P5.T2 |
| `Node::makePreviewImage` → `renderPreview` (`Node.cpp`) | `convertToSrgb` = 32f LUT is Linear → `Color::to_func_srgb` | per-scanline `applyViewerDisplayTransform` with `getDisplayProcessor(working, viewerDisplay, viewerView, "")`, gain 1, gamma 1; then `floatToInt<256>`. Byte/short sources go through `intToFloat`. | P5.T2 |
| `ViewerInstance::lutFromColorspace`, `getLutType`, `onColorSpaceChanged`, `viewerParamsLut`, `UpdateViewerParams::lut`, `RenderViewerArgs::srcColorSpace/colorSpace`, `scaleToTexture8bits_generic`, `scaleToTexture32bitsGeneric`, `renderViewer_internal` | LUTs | §5.2 | P4.T1 |
| `FrameKey::_lut` / `getLut`, `FrameEntrySerialization` | int | `_displayTransformHash` | P4.T1 |
| `fragRGB` (`Gui/Shaders.cpp`), `ViewerGL::setLut`, `ViewerGL::Implementation::displayingImageLut`, `activateShaderRGB`, `OpenGLViewerI::endTransferBufferFromRAMToGPU(… int lut …)` | hard-coded shader branches | §5.3 | P4.T2 |
| `getColorAtInternal`, `ViewerGL::getColorAt`, `ViewerGL::getColorAtRect` | `lutFromColorspace` | §5.3 picker | P4.T2 |
| `ViewerTab` combo, `onColorSpaceComboBoxChanged`, `getColorSpace` / `setColorSpace` | fixed combo | §5.4 | P4.T3 |
| `ViewerData::colorSpace` | string | display/view/look | P4.T4 |
| `KnobGuiColor::updateLabel` | `to_func_srgb` unless `_useSimplifiedUI` | `workingToColorPicking` unless `_useSimplifiedUI`. Simplified knobs hold UI colours and stay identity. Knobs without an `AppInstance` (Settings) use identity. | P5.T3 |
| `ColorSelectorPaletteButton::updateColor`, the `ColorSelectorWidget` swatch (the `to_func_srgb` block), `ColorSelectorWidget::handleTriangleColorChanged` (`from_func_srgb`) | sRGB curve | `workingToColorPicking` / `colorPickingToWorking`. `KnobGuiColor` passes the project's `ProjectColorManagementPtr` (or null for identity) into the widget. The HSV wheel stays on the stored values (`rgb_to_hsv` unchanged). | P5.T3 |
| `InfoViewerWidget` colour swatch (`Color::to_func_srgb` before `col.setRgbF`) | sRGB curve | `workingToColorPicking`. HSV/L readouts unchanged (linear, as today). | P5.T3 (missing) |
| `TrackerNodeInteract::convertImageTosRGBOpenGLTexture` | `LutManager::sRGBLut()` + dither | `workingToColorPicking` per pixel, then `floatToInt<0xff01>` dither. Rename to `convertImageToDisplayOpenGLTexture`. | P5.T3 |
| `Gui::debugImage` (`Gui/Gui40.cpp`, dev-only image dump) | `LutManager::sRGBLut()` | `workingToColorPicking` through the current project, or `floatToInt<256>` when there is none | P5.T3 (missing) |
| `Color::Lut`, `LutManager` and every named LUT (`sRGBLut` … `VLogLut`), `from/to_func_srgb`, `from/to_func_Rec709`, `from/to_func_bt1886` and the other named curve pairs (`Engine/Lut.{h,cpp}`) | | deleted | P5.T4 |
| `Color::floatToInt`, `intToFloat`, `rgb_to_hsv`, `hsv_to_rgb` and the `uint8xx` helpers | | **kept**. Users: `ComboBox`, `LogWindow`, `Gui20`, `ViewerGLPrivate` checkerboard, `TrackerNode`, `ImageConvert`, `ViewerInstance`. | |
| `ViewerColorSpaceEnum` (`Global/Enums.h`), `<enum-type name="ViewerColorSpaceEnum"/>` (`Engine/typesystem_engine.xml`) | | deleted. No Python script, doc or `.rst` in the repo references it. | P5.T4 |
| `Tests/Lut_Test.cpp` | named LUTs | cut to `floatToInt` / `intToFloat` / `hsv` round-trips | P5.T4 |

After P5.T4, `git grep -nE 'LutManager|ViewerColorSpaceEnum|eViewerColorSpace|to_func_srgb|from_func_srgb|getDefaultColorSpaceForBitDepth' Engine Gui Global Tests` must be empty, commented code included.

## 7. Q2: integer-only stages

- Natron never transforms colour implicitly. Integer↔float conversion is `Image::convertPixelDepth` semantics:
  - float → byte: `floatToInt<256>(v)` = clamp(round(v·255));
  - byte → float: `v/255`;
  - the same for short with 65535.
- Q2 test anchors (P5.T1): float 0.5 → byte 128 → 0.50196; float → short → float within 1/65535; values > 1 clamp to 255/65535.
- **Where it can fire.** Natron hands each effect the deepest depth it supports (`Node::getClosestSupportedBitDepth` / `getBestSupportedBitDepth`). So the conversion only fires at:
  - effects that don't support float (§8: none shipped);
  - RotoPaint/DiskCache when their buffers differ in depth from the input;
  - the viewer when it is fed a byte/short image.
- The follow-up "integer-only input" hint on nodes stays out of scope (P5.T1).

## 8. Byte-only plugin survey (feeds P6.T2's UAT notes and the docs only)

The survey grepped `addSupportedBitDepth` / `kOfxImageEffectPropSupportedPixelDepths` / `OFX_ADD_PIXELDEPTH_*` across `build/wt/m66-io`, `build/wt/m66-misc` and `build/openfx-arena-fork`. It excluded submodule copies (SupportExt, openfx, arena's `OpenFX-IO`), followed shared describe helpers, and checked which bundles are actually built.

**Result: no shipped plugin lacks float.** The list of plugins users must convert around is **empty**.

**Notable depth sets** (all include float):

| Tree | Plugins | Depths |
|---|---|---|
| openfx-io | every `Read*` / `Write*` (`GenericReader.cpp` / `GenericWriter.cpp` describe) | **float only** |
| openfx-io | `SeNoise`, `SeGrain`, `OIIOResize` | byte, short, float |
| openfx-io | `SeExpr`, `OIIOText` | byte, short, half, float |
| openfx-io | `RunScript` | byte, short, half, float, custom |
| openfx-misc | `ChromaKeyer`, `Keyer`, `PIK` | short, float |
| openfx-misc | `Constant`, `CheckerBoard`, `ColorWheel`, `Radial`, `Rand`, `ColorBars`, `Rectangle` | byte, short, float (`kSupportsXxx` defines, float on) |
| openfx-misc | Grade, Invert, the CImg family and most others | byte, short, float |
| arena | readers (`ReadPSD`, `ReadMisc`, `ReadSVG`, `ReadPDF`, `ReadCDR`, `ReadKrita`, `OpenRaster`), via the shared `GenericReader` | float only |
| arena | `Text`, `RichText`, the Magick plugins | float |

**Not shipped / excluded:**
- misc `DebugProxy` (behind a debug make option; it mirrors the host's depths), `Test/*`, `Templates/*`, `ClipTest` (test; float);
- io `tests/*`;
- arena `OCL/*` (not in `SOURCES`), `Audio/AudioCurve` (CMake option off by default), and OpenFX `Examples`.

**UAT/docs wording for P6.T2:** "No shipped plugin is integer-only. A third-party plugin that only accepts 8- or 16-bit receives linear-quantised working-space values (scale, clamp, round) with no colour conversion; convert explicitly (e.g. OCIOColorSpace) around it if it expects display-referred data."

## 9. Clean break

- Delete `Tests/fixtures/ocio-old-config.ntp`. Rewrite `Tests/ProjectOCIO_Test.cpp` on projects created in the test: create the Read/OCIOColorSpace nodes, set the colourspace, save and reload (P3.T2).
- **No migration:**
  - `defaultColorSpace8u/16u/32f` in old files are ignored (unknown project knob names are skipped by `ProjectPrivate::restoreFromSerialization`);
  - `ViewerData` `ColorSpace` is not read (§5.4);
  - old viewer disk-cache entries are dropped (§5.2).
- No legacy fixture is created for any M50 test.
- **Other pre-M50 fixtures stay.** `Tests/fixtures/m65-legacy-color.ntp` (`ProjectSerialization_Test`) and `channel-set-legacy-defaults.ntp` (`DefaultChannelSet_Test`) are pre-M50 projects, but they test node-serialization gates unrelated to colour, and they contain no colour knobs. M50 does not delete them; see correction 10.

## 10. Plan corrections

1. **P2.T1** (also P4.T2 and P2.T2's test). `NatronEngine` links OpenColorIO **PRIVATE**, so `ProjectColorManagement.h` (which exposes OCIO types), `Gui/ViewerGL*` (`GpuShaderDesc`) and `Tests/ProjectColorManagement_Test.cpp` won't find OCIO headers. Add `Engine/CMakeLists.txt` to P2.T1 and move `OpenColorIO::OpenColorIO` to `PUBLIC`.
2. **P2.T1** signature. Drop `exposure` and `gamma` from `getDisplayProcessor` (§2.1, §5.1). Add `workingToColorPicking`/`colorPickingToWorking` there rather than in P5.T3, because `ProjectColorManagement.{h,cpp}` is otherwise touched by two batches.
3. **P2.T2 / P2.T3.**
   - Enabled and Secret are serialized and restored. A project saved under an override would reload with the config knobs disabled, and a node's `ocioConfigFile` comes back with whatever Secret it was saved with. So `refreshColorManagement` must re-derive the enabled state and tooltip after every restore, and `pushOCIOConfigToNodes` must re-apply Secret on every push.
   - The knob-creation order in §2 is required.
   - Under the override, colourspace fallbacks do rewrite `workingSpace` etc.; only the config knobs stay untouched.
   - Child processes inherit the exported `OCIO` through `ProcessHandler`/`QProcess`, so the preference export must carry the `NATRON_OCIO_ENV_IS_PREFERENCE=1` marker (§3). Without it, every GUI-launched background render would treat the preference as an override and ignore the project's config.
4. **P4.T1 / P4.T2 viewer math.**
   - Gain → `linearCC` exposure and gamma → `displayCC` dynamic property can't represent the auto-contrast `offset` (`ViewerInstance::renderViewer_internal`) or the gamma ≤ 0 threshold mode.
   - Mutating dynamic properties on a CPU processor shared across viewers and render threads isn't safe.
   - Ruling: `LegacyViewingPipeline` carries only DisplayView + look; gain/offset/gamma are applied by Natron around it (§5.1). The P4.T1 test "gain 2 equals exposure +1 stop" still applies.
   - P4.T1 must also update `scaleToTexture32bitsGeneric` and `renderViewer_internal`, and `OpenGLViewerI::endTransferBufferFromRAMToGPU` loses its `lut` param (add `Engine/OpenGLViewerI.h` to P4.T1, or to P4.T2 since `Gui/ViewerGL.h` implements it, in the same batch as the Engine change).
5. **P3.T1.** A get-hook only fires for a property in the instance PropSpec, and `Instance::getStringProperty` / `getStringPropertyN` / `getDimension` only dispatch known names. The task must also edit `libs/OpenFX/HostSupport/src/ofxhImageEffect.cpp` and `include/ofxhImageEffect.h` in the same submodule (§4.1).
6. **P3.T3.** It says "OCIOCDLTransform and OCIOFileTransform use GenericOCIO and need no change". They don't use GenericOCIO: they have no `ocioConfigFile` and call `OCIO::GetCurrentConfig()` (process `$OCIO`). So they must:
   - gain a secret `ocioConfigFile` param;
   - use the instance/host config.

   Also, `OCIODisplay` builds its display/view menus once from `GetCurrentConfig()`, and `OCIOLogConvert` has its own `loadConfig`. Both are in the brief's file list but need real work, not a reason-gate fix. LookTransform also has a `configIsDefault()` gate.
7. **P3.T4.** `GenericWriterPlugin::outputFileChanged` re-guesses the output space on **every project load**: `restoreStateFromParams` calls it with `eChangePluginEdit`. With host defaults, reloading a project would re-apply the *current* project defaults to existing Writes, which breaks "defaults only for new nodes". Gate it as in §4.2.

   The category can't be computed in GenericReader/GenericWriter, which don't know the file bit depth. That needs the per-plugin `guessFileColourCategory` virtuals of §4.3, so P3.T4's files must include `PNG/ReadPNG.cpp`, `OIIO/ReadOIIO.cpp`, `PNG/WritePNG.cpp` and `OIIO/WriteOIIO.cpp`.
8. **P1.T1 / P3.T4 brief wording.** "FFmpeg trc" is not a metadata path. ReadFFmpeg never reads the transfer characteristic; it always walks the Rec709 chain. The only metadata-derived names are ReadPNG's gAMA/sRGB chunk and ReadOIIO's `oiio:ColorSpace`/ICC. For ReadOIIO, `oiio:ColorSpace` naming a real config colourspace is kept, because it is file metadata naming a colourspace; only its *display-name heuristics* are dropped.
9. **P3.T5.**
   - Arena's `OpenFX-IO` (f30a6a8) lacks commit 40764b2 (`existingColorSpaceOrFallback`). The P3.T3/P3.T4 commit must be on a branch that descends from m66 (`OPENFX_IO_REF` 649ce94), so the bump brings both.
   - Dropping the `OpenColorIO-Configs` fetch also requires editing `cmake/NatronBundleAssets.cmake`, which `FATAL_ERROR`s when `assets/OpenColorIO-Configs` is missing with `NATRON_BUNDLE_ASSETS=ON` and installs it, and the cache paths in `.github/workflows/ci.yml`, `nightly.yml` and `release.yml`.
10. **P1.T1 clean-break wording.** "Every test that loads a pre-M50 project" would also catch `ProjectSerialization_Test` (`m65-legacy-color.ntp`) and `DefaultChannelSet_Test` (`channel-set-legacy-defaults.ntp`). Those test non-colour serialization gates, and their fixtures have no colour knobs. M50 deletes only `ocio-old-config.ntp` and its test cases. Whether to delete the other two belongs in a separate "no backward compat" sweep, for the PM to decide.
11. **P5.T1.** Its file list misses:
    - `Engine/EffectInstanceRenderRoI.cpp` (`convertLayersFormatsIfNeeded`);
    - `Engine/Image.cpp` (`pasteFrom`);
    - `Engine/ImagePremult.cpp` (`premultByChannel`);
    - `EffectInstance::convertRAMImageToOpenGLTexture`;
    - `Tests/Image_Test.cpp` (5 calls, with one sRGB-LUT case to delete).

    Also, Q2 "round" means no error-diffusion dither in `convertToFormat`; the brief's 0.5 → 128 anchor requires that.
12. **P5.T3.** It misses `Gui/InfoViewerWidget.cpp` (the picker swatch, `to_func_srgb`) and `Gui/Gui40.cpp` (`Gui::debugImage`, `LutManager::sRGBLut()`). `ColorSelectorWidget` also has the inverse direction (`handleTriangleColorChanged`, `from_func_srgb`), which needs `colorPickingToWorking`.
13. **P5.T4.** Delete the `to_func_*`/`from_func_*` curve pairs too; after P5.T2/P5.T3 they have no users. The verify grep must cover commented code: `ViewerGL::getColorAt` keeps a commented block naming `eViewerColorSpaceLinear`, which P4.T2 deletes.
14. **P4.T4.** The restore site is the static `loadNodeGuiSerialization` in `Gui/ProjectGui.cpp`, not "`ProjectGui.cpp:375`" as a function. The new version constant is `VIEWER_DATA_INTRODUCES_OCIO_DISPLAY` = 15.
15. **P3.T2.** The "keep" tables at `ReadNode.cpp`/`WriteNode.cpp` (`{kOCIOParamConfigFile, true}`) are the persistent-param lists carried across a decoder/encoder swap; they don't hold a knob. The push must set the knob on the wrapper and on the embedded node (`ReadNode::getEmbeddedReader()` / `WriteNode::getEmbeddedWriter()`), and re-push after a decoder swap.
