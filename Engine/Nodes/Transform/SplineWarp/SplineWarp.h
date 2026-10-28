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

#ifndef Engine_Nodes_Transform_SplineWarp_SplineWarp_h
#define Engine_Nodes_Transform_SplineWarp_SplineWarp_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <memory>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpCache.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpField.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpMesh.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpInteract.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpPanelHook.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpSampling.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpTps.h"

#define PLUGINID_NATRON_SPLINEWARP "SplineWarp"
#define PLUGIN_MAJOR_NATRON_SPLINEWARP 1

#define kSplineWarpParamAmount "amount"
#define kSplineWarpParamInvert "invert"
#define kSplineWarpParamSolver "solver"
#define kSplineWarpParamSmoothness "smoothness"
#define kSplineWarpParamCurveResolution "curveResolution"
#define kSplineWarpParamBoundaryCurveResolution "boundaryCurveResolution"
#define kSplineWarpParamLayerAmount "layerAmount"
#define kSplineWarpParamLinkAmount "linkAmount"
#define kSplineWarpParamBoundaryBBox "boundaryBBox"
#define kSplineWarpParamFieldResolution "fieldResolution"
#define kSplineWarpParamBBox "bbox"
#define kSplineWarpParamMotionBlur "motionBlur"
#define kSplineWarpParamShutter "shutter"

#define kSplineWarpSolverTps "tps"
#define kSplineWarpSolverMesh "mesh"
#define kSplineWarpBBoxSource "source"
#define kSplineWarpBBoxUnion "union"

// The most sub-frames a motion-blurred render solves and averages.
#define kSplineWarpMaxSubFrames 64

#define kSplineWarpInputSource 0
#define kSplineWarpInputMask 1

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kSplineWarpInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

inline constexpr NodeSpec kSplineWarpNodeSpec = {
    .id = PLUGINID_NATRON_SPLINEWARP,
    .label = "SplineWarp",
    .description = "Warp an image with curves.\n"
                   "Each link pairs a source curve with a destination curve: what lies along the source curve is "
                   "moved onto the destination curve, and the rest of the image follows smoothly. Correspondences "
                   "pin points of the source curve to points of the destination curve. A link's amount moves its "
                   "destination toward its source, so at 0 the link holds its source curve in place.\n"
                   "With boundary curves, nothing outside every boundary's feather moves, and the warp eases out "
                   "between a boundary's curve and its feather.\n"
                   "Curves neither linked nor boundaries are ignored.",
    .grouping = PLUGIN_GROUP_TRANSFORM,
    .majorVersion = PLUGIN_MAJOR_NATRON_SPLINEWARP,
    .minorVersion = 0,
    .icon = NATRON_IMAGES_PATH "NativeNodes/SplineWarp.png",
    .inputs = kSplineWarpInputs,
};

inline constexpr ImagePipelineSpec kSplineWarpPipeline = {
    .stages = { Stage::MaskMix, Stage::PassThroughUnprocessed },
    .layers = LayerPolicy::all(),
    .op = OpKind::Custom,
};

/**
 * @brief Warps its Source by the curves of its SplineWarpContext: each output pixel samples the
 * Source where the solved map sends it, through a coordinate field cached per window. A transform
 * upstream of the Source is folded into the same resample.
 **/
class SplineWarp
    : public StagedImageEffect {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new SplineWarp(node);
    }

    explicit SplineWarp(NodePtr node);

    virtual ~SplineWarp();

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual const ImagePipelineSpec& imagePipelineSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual std::string getInputHint(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual CurveContextKindEnum getCurveContextKind() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual RotoCurveContextPtr createGeometryCurveContext(const NodePtr& node) const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual RotoCurvePanelHook* getCurvePanelHook() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief The Source's region of definition; with the bbox knob at union, grown by where the
     * warp moves the Source's edges and by the destination curves.
     **/
    virtual StatusEnum getRegionOfDefinition(U64 hash,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief The Source, so a transform upstream of it is applied in this node's resample.
     **/
    virtual bool getInputsHoldingTransform(std::list<int>* inputs) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief The times a render at time averages: with motion blur, the centres of motionBlur's
     * stratified sub-frames over the shutter; otherwise, or when the warp is the same at every
     * sub-frame, the one time whose warp it renders.
     **/
    std::vector<double> getRenderTimes(double time, ViewIdx view);
    const SplineWarpInteractPtr& getInteract() const
    {
        return _ui;
    }

    virtual bool hasOverlay() const OVERRIDE FINAL
    {
        return true;
    }

    virtual void getPluginShortcuts(std::list<PluginActionShortcut>* shortcuts) OVERRIDE FINAL;
    virtual void refreshExtraStateAfterTimeChanged(bool isPlayback, double time) OVERRIDE FINAL;
    virtual void drawOverlay(double time, const RenderScale& renderScale, ViewIdx view) OVERRIDE FINAL;
    virtual bool onOverlayPenDown(double time, const RenderScale& renderScale, ViewIdx view, const QPointF& viewportPos, const QPointF& pos, double pressure, double timestamp, PenType pen) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual bool onOverlayPenMotion(double time, const RenderScale& renderScale, ViewIdx view, const QPointF& viewportPos, const QPointF& pos, double pressure, double timestamp) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual bool onOverlayPenUp(double time, const RenderScale& renderScale, ViewIdx view, const QPointF& viewportPos, const QPointF& pos, double pressure, double timestamp) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual bool onOverlayPenDoubleClicked(double time, const RenderScale& renderScale, ViewIdx view, const QPointF& viewportPos, const QPointF& pos) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual bool onOverlayKeyDown(double time, const RenderScale& renderScale, ViewIdx view, Key key, KeyboardModifiers modifiers) OVERRIDE FINAL;
    virtual bool onOverlayKeyUp(double time, const RenderScale& renderScale, ViewIdx view, Key key, KeyboardModifiers modifiers) OVERRIDE FINAL;
    virtual bool onOverlayFocusLost(double time, const RenderScale& renderScale, ViewIdx view) OVERRIDE FINAL;
    virtual void onInteractViewportSelectionCleared() OVERRIDE FINAL;
    virtual void onInteractViewportSelectionUpdated(const RectD& rectangle, bool onRelease) OVERRIDE FINAL;

    // Draft renders are cached apart from full-quality ones.
    virtual bool supportsRenderQuality() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    // The field steps of the cached coordinate fields, for tests.
    std::vector<double> getCachedFieldResolutions() const;

private:
    // The knob values a render reads, at one time and view.
    struct Settings {
        double amount = 1.;
        bool invert = false;
        bool mesh = false;
        double smoothness = 0.;
        double curveResolution = 4.;
        double boundaryCurveResolution = 4.;
        // The Source's format in canonical coordinates, held still when not null.
        RectD boundaryFormat;
        double fieldResolution = 8.;
        Resampler::FilterEnum filter = Resampler::eFilterCubic;
        bool clamp = false;
        bool blackOutside = true;
        bool bboxUnion = false;
        double motionBlur = 0.;
        double shutter = 0.5;
        Resampler::ShutterOffsetEnum shutterOffset = Resampler::eShutterOffsetStart;
        double shutterCustomOffset = 0.;
        bool warped = true;
    };

    // The solved warp at one time: its samples, the map of the chosen solver, and whether it moves
    // nothing.
    struct Warp {
        std::shared_ptr<const SplineWarpSampleSet> samples;
        SplineWarpTpsConstPtr map;
        SplineWarpMeshConstPtr mesh;
        // The box whose corners the mesh pins.
        RectD meshBox;
        U64 hash = 0;
        bool identity = true;
    };

    // One time a render samples, with its warp.
    struct TimedWarp {
        double time = 0.;
        Warp warp;
    };

    struct SampleKey {
        U64 contextHash;
        double time;
        double amount;
        bool invert;
        double spacing;
        double boundarySpacing;
        RectD boundaryFormat;

        bool operator==(const SampleKey& o) const
        {
            return contextHash == o.contextHash && time == o.time && amount == o.amount && invert == o.invert && spacing == o.spacing && boundarySpacing == o.boundarySpacing && boundaryFormat == o.boundaryFormat;
        }
    };

    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;
    virtual void onKnobsLoaded() OVERRIDE FINAL;

    /**
     * @brief The Source over the box holding the sources of the render window's pixels, grown by
     * the filter's support and clipped to the Source's region of definition. The mask keeps the
     * render window.
     **/
    virtual void getRegionsOfInterest(double time,
                                      const RenderScale& scale,
                                      const RectD& outputRoD,
                                      const RectD& renderWindow,
                                      ViewIdx view,
                                      RoIMap* ret) OVERRIDE FINAL;

    virtual bool isOpIdentity(double time,
                              const RenderScale& scale,
                              const RectI& roi,
                              ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum renderOp(const RenderActionArgs& args,
                                const StagePlan& plan) OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief A Filter whose weight grows with the farthest a link moves its curve, at the
     * rendered scale.
     **/
    virtual GpuWeight gpuWeight(double time,
                                ViewIdx view,
                                unsigned int mipmapLevel) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief Accepts what the stages accept in float, unless the Source comes through a
     * concatenated upstream transform, whose matrix the GPU path is not handed.
     **/
    virtual bool canRenderOpOnGpu(const GpuQuery& query) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief Builds the field over args.outputRect on the CPU, uploads it and samples the Source
     * through it with splinewarp.slang. Fails, so the region renders on the CPU, for a field larger
     * than a dispatch can bind.
     **/
    virtual StatusEnum renderOpGpu(const GpuRenderArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual bool getOpGpuInputLayer(int inputNb,
                                    const ImageLayerDesc& plane,
                                    double time,
                                    ViewIdx view,
                                    ImageLayerDesc* layer) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief For the Source, the region getRegionsOfInterest() names, in the Source's pixels.
     **/
    virtual RectI getOpGpuInputRoI(int inputNb,
                                   double time,
                                   ViewIdx view,
                                   const RectI& outputRect,
                                   const RenderScale& scale) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    // In draft, the curves are sampled half as finely, the field step is four times as coarse,
    // and the filter is impulse.
    void readSettings(double time, ViewIdx view, Settings* settings, bool draft = false) const;
    // Only the knobs the warp is solved from, which a sub-frame reads at its own time.
    void readWarpSettings(double time, ViewIdx view, Settings* settings) const;

    // Whether the render this thread is in is a draft one.
    bool isDraftRender() const;

    // Samples and solves the warp; false when the node has no SplineWarp curve context.
    // The pairs sampled from the curves at time, cached by the curves' hash and the settings
    // sampling reads. Null without a curve context.
    std::shared_ptr<const SplineWarpSampleSet> sampleWarp(double time, const Settings& settings);

    bool computeWarp(double time, ViewIdx view, const Settings& settings, Warp* warp);

    // The Source's region of definition at full scale, null without a Source or when it fails.
    RectD getSourceBounds(double time, ViewIdx view);

    /**
     * @brief The warps a render at time averages, as getRenderTimes() gives their times. False
     * when the node has no SplineWarp curve context.
     **/
    bool computeShutterWarps(double time, ViewIdx view, const Settings& settings, std::vector<TimedWarp>* warps);

    // Grows *extra by where the warp moves the edges of rod and by its destination curves, for
    // the union bbox.
    void growUnionRoD(const Warp& warp, const Settings& settings, const RectD& rod, RectD* extra, bool* any);

    // The field over window, from the sub-frame cache when the render averages several.
    SplineWarpFieldConstPtr computeField(const Warp& warp, double time, const Settings& settings, const RectI& window, double scaleX, double scaleY, bool subFrame);

    double getOutputPixelAspectRatio(double time, ViewIdx view) const;

    void updateClampEnabled(double time, ViewIdx view);
    void updateSmoothnessVisibility();

    // Loads layerAmount and linkAmount from the selected layers and links, and enables each when
    // the selection is one it edits.
    void syncAmountKnobs();
    // Writes knob, layerAmount when layer and linkAmount otherwise, to the selected targets.
    void writeAmountKnob(const KnobDoublePtr& knob, bool layer);

    KnobDoubleWPtr _amount;
    KnobBoolWPtr _invert;
    KnobChoiceWPtr _solver;
    KnobDoubleWPtr _smoothness;
    KnobDoubleWPtr _curveResolution;
    KnobDoubleWPtr _fieldResolution;
    KnobChoiceWPtr _filter;
    KnobBoolWPtr _clamp;
    KnobBoolWPtr _blackOutside;
    KnobChoiceWPtr _bbox;
    KnobDoubleWPtr _motionBlur;
    KnobDoubleWPtr _shutter;
    KnobChoiceWPtr _shutterOffset;
    KnobDoubleWPtr _shutterCustomOffset;
    KnobDoubleWPtr _layerAmount;
    KnobDoubleWPtr _linkAmount;
    KnobBoolWPtr _boundaryBBox;
    KnobDoubleWPtr _boundaryCurveResolution;
    // While syncAmountKnobs() sets the amount knobs, which must not write back.
    bool _syncingAmounts;

    SplineWarpInteractPtr _ui;
    std::unique_ptr<SplineWarpPanelHook> _panelHook;
    // Whether a drag of the interact turned the viewer's draft render on.
    bool _draftDrag;

    SplineWarpLruCache<SampleKey, SplineWarpSampleSet> _samples;
    SplineWarpTpsCache _maps;
    SplineWarpTpsCache _forwardMaps;
    SplineWarpMeshCache _meshes;
    SplineWarpMeshCache _forwardMeshes;
    SplineWarpFieldCache _fields;
    SplineWarpFieldCache _subFrameFields;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Transform_SplineWarp_SplineWarp_h
