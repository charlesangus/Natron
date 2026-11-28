/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 * (C) 2013-2018 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

#ifndef Engine_Nodes_Scene_USDRender_h
#define Engine_Nodes_Scene_USDRender_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <atomic>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"

// Free of USD: NativeRegistryAll.inc includes every node header into translation units that keep
// Py_LIMITED_API, which USD's headers cannot be compiled under.

#define PLUGINID_NATRON_USDRENDER "USDRender"

#define kUSDRenderParamSettingsPrim "settingsPrim"
#define kUSDRenderParamHeadlight "headlight"
#define kUSDRenderParamRenderer "renderer"
#define kUSDRenderParamRenderPurpose "renderPurpose"
#define kUSDRenderParamProxyPurpose "proxyPurpose"
#define kUSDRenderParamGuidePurpose "guidePurpose"

// The id of the Settings Prim entry that renders with the stage's active render settings prim
// (its renderSettingsPrimPath metadata).
#define kUSDRenderSettingsPrimStageDefault "stageDefault"

// The frame metadata key of the prim path behind a primId.id value: this prefix followed by the
// value's bits as eight lowercase hexadecimal digits.
#define kUSDRenderMetadataPrimIdPrefix "primId/"

NATRON_NAMESPACE_ENTER

class SceneLoadSet;

class USDRender
    : public NativeEffectBase {
public:
    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Scene", .kind = eDataKindScene },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_USDRENDER,
        .label = "USDRender",
        .description = "Render the input scene with a Hydra render delegate as a render settings prim of the scene "
                       "says: its camera, resolution, aspect ratio conform policy, renderer and that renderer's "
                       "settings (see USDRenderSettings). The renderer is the one the node names, else the one the "
                       "prim names, else Storm (USD's OpenGL renderer); the others render on the CPU. The render "
                       "fills the rgba, depth.Z and primId.id layers: rgba is as the delegate shades it, in the project's working space "
                       "with no colour transform; depth.Z is the distance along the camera's view axis in scene "
                       "units, +inf where nothing was hit; primId.id identifies the prim seen at each pixel by a "
                       "hash of its path, bit-cast to float as Cryptomatte does, 0 where nothing was hit, and the "
                       "frame metadata maps each hash to its path. The renderers other than Storm and Natron Utility also fill the "
                       "normal layer: the shading normal in camera space, 0 where nothing was hit. Only the scene's lights light it, unless Headlight "
                       "adds one at the camera. Render Purpose, Proxy Purpose and Guide Purpose choose which prims of "
                       "those purposes are drawn. Without a render settings prim, the scene's first camera renders the "
                       "project's format.",
        .grouping = PLUGIN_GROUP_3D_USD,
        .majorVersion = 1,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/USDRender.png",
        .outputKind = eDataKindImage,
        .inputs = kInputs,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new USDRender(node);
    }

    explicit USDRender(NodePtr node);

    virtual ~USDRender();

    virtual bool isMultiPlanar() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    // Storm renders whole frames: a tile would cost a whole render.
    virtual bool supportsTiles() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return false;
    }

    virtual StatusEnum getRegionOfDefinition(U64 hash,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void getComponentsNeededAndProduced(double time,
                                                ViewIdx view,
                                                EffectInstance::ComponentsNeededMap* comps,
                                                double* passThroughTime,
                                                int* passThroughView,
                                                int* passThroughInputNb) OVERRIDE FINAL;

    // The scene input is evaluated by render() itself, not pulled as an image.
    virtual FramesNeededMap getFramesNeeded(double time, ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void getRegionsOfInterest(double time,
                                      const RenderScale& scale,
                                      const RectD& outputRoD,
                                      const RectD& renderWindow,
                                      ViewIdx view,
                                      RoIMap* ret) OVERRIDE FINAL;

    // On the GPU, a plane is packed from Storm's AOVs on the GL side and handed to the device by the
    // rung HydraHandoff picks; when that rung is readback the node renders on the CPU.
    virtual std::optional<OpKind> opKind() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual bool canRenderOnGpu(const GpuQuery& query) const OVERRIDE FINAL WARN_UNUSED_RETURN;
    // Resident for Natron Utility, whose CPU render downloads the frame it renders on the device.
    virtual GpuWeight gpuWeight(double time,
                                ViewIdx view,
                                unsigned int mipmapLevel) const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual bool readsGpuInput(int inputNb,
                               const ImageLayerDesc& plane,
                               double time,
                               ViewIdx view) const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum renderGpu(const GpuRenderArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    // Drops the stage and Storm engine the node keeps between renders.
    virtual void purgeCaches() OVERRIDE FINAL;

    /**
     * @brief Starts a refresh of the Settings Prim menu on a worker: Stage Default and every
     * RenderSettings prim under /Render of the input scene at the current frame, on the load set
     * of the viewer showing the node (SceneMenuRefresh::getContext()). The menu keeps its entries
     * until applySettingsPrimMenu() fills it with the job's result, which the worker queues to
     * the main thread. Main thread only; the node calls it itself, queued, whenever its input or
     * anything upstream changes. Null when a load change does not reach what the last one searched.
     **/
    std::shared_ptr<SceneMenuJob> refreshSettingsPrimMenu();

    /**
     * @brief Fills the menu with job's entries, keeping the chosen one even when the scene no
     * longer has it. False, leaving it alone, when job was superseded, cancelled or is not done.
     * Main thread only.
     **/
    bool applySettingsPrimMenu(const std::shared_ptr<SceneMenuJob>& job);

    /**
     * @brief The renderer a render at time and view uses: the Renderer knob's, else the one the
     * active render settings prim names, else Storm's. False with *error set when the prim names
     * a renderer no plug-in has, or the Settings Prim names no RenderSettings prim.
     **/
    bool activeRenderer(double time, ViewIdx view, std::string* rendererId, std::string* error) const;

    // Of every render's setup, how many were read from the input's stage and how many reused one read
    // before for the same input scene, time, view, level, renderer, knobs and project format.
    static std::uint64_t renderSetupReadCount();
    static std::uint64_t renderSetupReuseCount();

private:
    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual void onKnobsLoaded() OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;
    virtual void onInputChanged(int inputNo) OVERRIDE FINAL;
    virtual void onNodeHashChanged(U64 hash) OVERRIDE FINAL;
    // Of the render settings knobs upstream (HashNamespaces), the active prim's in the active
    // renderer's namespaces.
    virtual void appendNodeContentHash(ContentHasher& h) const OVERRIDE FINAL;
    virtual bool consumesHashNamespaces() const OVERRIDE FINAL
    {
        return true;
    }
    virtual StatusEnum getPreferredMetadataAt(double time,
                                              ViewIdx view,
                                              NodeMetadata& metadata) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual ImageMetadata deriveOutputMetadata(double time, ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    void scheduleSettingsPrimMenuRefresh();

    // Keeps a loaded renderer that is not installed as an entry of its own, so the project keeps it.
    void keepLoadedRenderer();

    // The Renderer knob's id: a delegate's, or kRenderDelegateSceneDefaultId.
    std::string rendererOverride() const;

    // The Settings Prim knob's path, empty for Stage Default.
    std::string settingsPrimOverride() const;

    // What a render at time, view and mipmap level asks the delegate for; defined where USD is included.
    struct RenderSetup;

    // The input scene at time and view and what its active render settings prim says; defined
    // where USD is included.
    struct ActiveSettings;

    enum SettingsLoadEnum {
        // No payload open, for the main thread's provisional answer.
        eSettingsLoadNothing,
        // The payloads of the settings prim and of the camera it targets.
        eSettingsLoadSettingsPrim,
        // Every payload, for the render.
        eSettingsLoadEverything
    };

    // False with *error set to the message the node shows if the scene or its prim cannot be read.
    // The input is evaluated, and its stage composed, with load's payloads open.
    // sceneEvaluated: active->scene already holds the input evaluated with every payload, for load
    // eSettingsLoadEverything.
    bool readActiveSettings(double time, ViewIdx view, SettingsLoadEnum load, ActiveSettings* active, std::string* error, bool sceneEvaluated = false) const;

    // Sets active->scene to the input at time and view evaluated on loadSet under contextId,
    // everything without one.
    bool evaluateInput(double time, ViewIdx view, U64 contextId, const std::shared_ptr<const SceneLoadSet>& loadSet, ActiveSettings* active, std::string* error) const;

    // What the output format and the renderer take from the active render settings prim.
    struct SettingsSummary {
        bool read = false;
        std::string error;
        bool hasPrim = false;
        int width = 0;
        int height = 0;
        double pixelAspectRatio = 1.;
        std::string renderer;
        // Natron Utility's overscan in pixels, 0 for another renderer, and its device setting.
        int overscan = 0;
        std::string utilityDevice;

        bool operator==(const SettingsSummary& other) const
        {
            return (read == other.read) && (error == other.error) && (hasPrim == other.hasPrim) && (width == other.width) && (height == other.height) && (pixelAspectRatio == other.pixelAspectRatio) && (renderer == other.renderer) && (overscan == other.overscan) && (utilityDevice == other.utilityDevice);
        }
    };

    SettingsSummary summarizeSettings(double time, ViewIdx view, SettingsLoadEnum load) const;

    /**
     * @brief The summary at time and view under the node's hash, read with the settings prim's
     * payloads open. The main thread never evaluates the input: it is given the stored summary,
     * else, marked provisional (markOutputMetadataProvisional()), the last one stored for any
     * hash, time and view, or the project's format before any, while a worker reads the whole
     * one; when that differs, the output metadata is invalidated so that its readers ask again.
     **/
    SettingsSummary getSettingsSummary(double time, ViewIdx view) const;

    // Stores summary under hash, and tells the readers given another one for it to ask again.
    void storeSettingsSummary(U64 hash, double time, ViewIdx view, const SettingsSummary& summary) const;

    // On the worker: reads every summary the main thread was given a provisional one for under the node's hash.
    void readGivenSettingsSummaries() const;

    // The render of time, view and level as the node's settings and input scene make it. False with
    // *error set to the message the node shows if it cannot render. primIdPlane: the render draws
    // the primId plane, whose table it then stores for the main thread.
    bool prepareRender(double time,
                       ViewIdx view,
                       unsigned int level,
                       bool primIdPlane,
                       RenderSetup* setup,
                       std::string* error);

    // What a render setup depends on besides what the stage says.
    struct RenderSetupKey {
        U64 sceneHash = 0;
        double time = 0.;
        int view = 0;
        unsigned int level = 0;
        std::string renderer;
        U64 knobs = 0;
        U64 project = 0;

        bool operator==(const RenderSetupKey& other) const;
    };

    // False when the setup cannot be kept, because a knob has an expression.
    bool renderSetupKey(double time, ViewIdx view, unsigned int level, const ActiveSettings& active, RenderSetupKey* key) const;

    // prepareRender() past the evaluation of the input, which active holds.
    bool readRenderSetup(double time, ViewIdx view, unsigned int level, bool primIdPlane, ActiveSettings* active, RenderSetup* setup, std::string* error);

    // Renders the live textures of active's scene at time into their store, and lists them in setup.
    void prepareLiveTextures(const ActiveSettings& active, double time, RenderSetup* setup);

    // Whether the render under way is one a viewer waits for, rather than one for a file.
    bool rendersForViewer() const;

    // Shows warning, or clears the node's message when it is empty.
    void showWarning(const std::string& warning);

    /**
     * @brief Shows what a render that stands left to say: the delegate's errors, else the setup's and
     * the delegate's warnings, and the device it rendered on when it named one.
     **/
    void showRenderMessages(const std::string& setupWarning,
                            const std::string& resultWarning,
                            const std::string& errors,
                            const std::string& device);

    // The Natron Utility device the active settings prim asks for at time and view, Auto without one.
    std::string utilityDevice(double time, ViewIdx view) const;

    // The output format in pixels and its pixel aspect ratio: the active prim's resolution and
    // pixel aspect ratio, the project's format without one. *overscan, when given, is the pixels a
    // Natron Utility render adds on every side of it.
    void outputFormat(double time, ViewIdx view, RectI* format, double* par, int* overscan = nullptr) const;

    // The primId table of the output at time and view (every key under kUSDRenderMetadataPrimIdPrefix)
    // under primIdTableHash(), when a render or a reader off the main thread has stored it. Without
    // one, noteMiss records that a reader was given none, so that the store tells it to ask again.
    bool findPrimIdTable(U64 hash, double time, ViewIdx view, bool noteMiss, ImageMetadata* table);
    void storePrimIdTable(U64 hash, double time, ViewIdx view, const ImageMetadata& table);
    // The hash of the input, the only thing the table depends on: the node's own hash also moves
    // when a menu of its knobs is repopulated, which leaves the table it named unchanged.
    U64 primIdTableHash() const;

    KnobChoiceWPtr _settingsPrim;
    KnobBoolWPtr _headlight;
    KnobBoolWPtr _renderPurpose;
    KnobBoolWPtr _proxyPurpose;
    KnobBoolWPtr _guidePurpose;
    KnobChoiceWPtr _renderer;
    std::atomic<bool> _settingsPrimMenuRefreshPending;
    SceneMenuRefresh _settingsPrimMenuRefresh;
    // Each read of the settings evaluates the input on its own load set, under its own id.
    const U64 _settingsNothingContextId;
    const U64 _settingsPrimContextId;
    // A provisional summary the main thread was given, under the hash it was given for.
    struct GivenSettingsSummary {
        U64 hash = 0;
        SettingsSummary summary;
    };
    mutable std::mutex _settingsSummariesMutex;
    // Under _settingsSummariesMutex: the summaries of the node's hash _settingsSummariesHash, the
    // provisional ones the main thread was given while the worker reads them, the last one stored,
    // and whether a readGivenSettingsSummaries() is posted and not yet started.
    mutable U64 _settingsSummariesHash;
    mutable std::map<std::pair<double, int>, SettingsSummary> _settingsSummaries;
    mutable std::map<std::pair<double, int>, GivenSettingsSummary> _settingsSummariesGiven;
    mutable bool _hasLastSettingsSummary;
    mutable SettingsSummary _lastSettingsSummary;
    mutable bool _settingsSummariesPosted;
    std::mutex _primIdTablesMutex;
    // Under _primIdTablesMutex: the tables of the node's hash _primIdTablesHash, and the frames
    // whose readers were given none.
    U64 _primIdTablesHash;
    std::map<std::pair<double, int>, ImageMetadata> _primIdTables;
    std::set<std::pair<double, int>> _primIdTablesMissed;
    mutable SceneWorker _settingsWorker;
    typedef std::pair<RenderSetupKey, std::shared_ptr<const RenderSetup>> KeptRenderSetup;
    std::mutex _renderSetupsMutex;
    // Under _renderSetupsMutex: the setups read last, most recently used first.
    std::list<KeptRenderSetup> _renderSetups;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_USDRender_h
