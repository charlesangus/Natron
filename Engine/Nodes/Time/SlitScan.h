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

#ifndef Engine_Nodes_Time_SlitScan_h
#define Engine_Nodes_Time_SlitScan_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"

#define PLUGINID_NATRON_SLITSCAN "SlitScan"
#define PLUGIN_MAJOR_NATRON_SLITSCAN 1

#define kSlitScanParamRetimeFunction "retimeFunction"
#define kSlitScanParamRetimeOffset "retimeOffset"
#define kSlitScanParamRetimeGain "retimeGain"
#define kSlitScanParamRetimeAbsolute "retimeAbsolute"
#define kSlitScanParamFrameRange "frameRange"
#define kSlitScanParamFilter "filter"
#define kSlitScanParamRetimeMapChannel "retimeMapChannel"

#define kSlitScanRetimeFunctionHorizontalSlit "horizontalslit"
#define kSlitScanRetimeFunctionVerticalSlit "verticalslit"
#define kSlitScanRetimeFunctionRetimeMap "retimemap"

#define kSlitScanFilterNearest "nearest"
#define kSlitScanFilterLinear "linear"

NATRON_NAMESPACE_ENTER

inline constexpr int kSlitScanInputSource = 0;
inline constexpr int kSlitScanInputRetimeMap = 1;

// The most frames one output frame gathers from, to bound the memory a render holds.
inline constexpr int kSlitScanMaxFrames = 64;

inline constexpr InputSpec kSlitScanInputs[] = {
    InputSpec { .label = "Source", .time = InputTime::Any },
    InputSpec { .label = "Retime Map", .role = InputRole::Aux, .optional = true },
};

inline constexpr NodeSpec kSlitScanNodeSpec = {
    .id = PLUGINID_NATRON_SLITSCAN,
    .label = "SlitScan",
    .description = "Gathers each output pixel from the Source at its own time: offset + gain * f, relative to the "
                   "current frame unless Absolute is on, where f is a ramp from 0 at the bottom row (or the leftmost "
                   "column) of the Source to 1 at its top row (or rightmost column), or a channel of the Retime Map "
                   "input. Times are clamped to the frames the node reads, at most 64 of them; Nearest reads the "
                   "nearest frame and Linear blends the two around the time.",
    .grouping = PLUGIN_GROUP_TIME,
    .majorVersion = PLUGIN_MAJOR_NATRON_SLITSCAN,
    .minorVersion = 0,
    .icon = NATRON_IMAGES_PATH "NativeNodes/SlitScan.png",
    .inputs = kSlitScanInputs,
};

inline constexpr ImagePipelineSpec kSlitScanPipeline = {
    .stages = {},
    .layers = LayerPolicy::none(),
    .op = OpKind::Custom,
};

namespace SlitScanTime {
enum RetimeFunction {
    eRetimeFunctionHorizontalSlit = 0,
    eRetimeFunctionVerticalSlit,
    eRetimeFunctionRetimeMap,
};

enum Filter {
    eFilterNearest = 0,
    eFilterLinear,
};

struct Settings {
    RetimeFunction function = eRetimeFunctionHorizontalSlit;
    Filter filter = eFilterNearest;
    double offset = 0.;
    double gain = -10.;
    bool absolute = false;
    // frameRange, in either order: the frames the retime map may ask for.
    int rangeA = -10;
    int rangeB = 0;
    // The retime map is connected and its channel resolves; without it the map reads 0.
    bool hasMap = false;
};

/**
 * @brief The whole frames [first, last] SlitScan reads at time. With the slits they span
 * base + [min(0, gain), max(0, gain)], with the map frameRange (offset by time unless absolute),
 * rounded with Nearest and widened to whole frames with Linear; base is offset, plus time unless
 * absolute. Above kSlitScanMaxFrames frames the end nearest base is kept and clipped is set. When
 * every pixel has the same time, constant is set and the window narrowed to the frames that time
 * reads.
 **/
struct Window {
    int first = 0;
    int last = 0;
    bool clipped = false;
    bool constant = false;
    // With constant, the clamped time every pixel reads.
    double time = 0.;
    // The time of a ramp or map value v is v * gain + base, clamped to [first, last].
    double base = 0.;
    double gain = 0.;
};

Window window(const Settings& settings,
              double time);

// The time of ramp or map value v, as the CPU and the GPU both compute it, in float.
inline float
pixelTime(float v,
          float gain,
          float base,
          float first,
          float last)
{
    const float t = v * gain + base;

    return (t < first) ? first : ((t > last) ? last : t);
}
} // namespace SlitScanTime

/**
 * @brief SlitScan: a per-pixel gather over a window of frames of the Source, the time of each pixel
 * from a ramp across the Source or a channel of the Retime Map. An identity when every pixel reads
 * one whole frame.
 **/
class SlitScan
    : public StagedImageEffect {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new SlitScan(node);
    }

    explicit SlitScan(NodePtr node);

    virtual ~SlitScan();

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual const ImagePipelineSpec& imagePipelineSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual Route route(double time,
                        ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    // Every frame of the window, and the Retime Map at time when it is read.
    virtual FramesNeededMap getFramesNeeded(double time,
                                            ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    // The union of the Source's regions over the window.
    virtual StatusEnum getRegionOfDefinition(U64 hash,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;

    // The Retime Map is read only while its channel resolves.
    virtual bool readsGpuInput(int inputNb,
                               const ImageLayerDesc& plane,
                               double time,
                               ViewIdx view) const OVERRIDE FINAL WARN_UNUSED_RETURN;

private:
    // How a render reads the ramp: from rampOrigin, rampScale apart per pixel along rows (the
    // horizontal slit) or columns (the vertical one).
    struct Plan {
        SlitScanTime::Settings settings;
        SlitScanTime::Window window;
        int rampOrigin = 0;
        float rampScale = 0.f;
        ImageLayerDesc mapLayer;
        int mapChannel = -1;
    };

    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL WARN_UNUSED_RETURN;

    // The Retime Map's selected layer.
    virtual bool getInputLayersRead(int inputNb,
                                    double time,
                                    ViewIdx view,
                                    std::list<ImageLayerDesc>* layers) OVERRIDE FINAL;

    virtual bool isOpIdentity(double time,
                              const RenderScale& scale,
                              const RectI& roi,
                              ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum renderOp(const RenderActionArgs& args,
                                const StagePlan& plan) OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief On a float colour plane without a mask, when every frame of the window is a view the
     * region holds with something there.
     **/
    virtual bool canRenderOpOnGpu(const GpuQuery& query) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual bool getOpGpuInputLayer(int inputNb,
                                    const ImageLayerDesc& plane,
                                    double time,
                                    ViewIdx view,
                                    ImageLayerDesc* layer) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum renderOpGpu(const GpuRenderArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    // The Retime Map's layer and channel, false when it is disconnected or its channel does not
    // resolve.
    bool resolveMapChannel(double time,
                           ViewIdx view,
                           ImageLayerDesc* layer,
                           int* channel) const WARN_UNUSED_RETURN;

    SlitScanTime::Settings settingsAt(double time,
                                      ViewIdx view) const WARN_UNUSED_RETURN;

    // The plan at time and scale, false without a Source.
    bool planAt(double time,
                ViewIdx view,
                const RenderScale& scale,
                Plan* plan) const WARN_UNUSED_RETURN;

    void updateClipMessage(double time);

    KnobChoiceWPtr _retimeFunction;
    KnobDoubleWPtr _retimeOffset;
    KnobDoubleWPtr _retimeGain;
    KnobBoolWPtr _retimeAbsolute;
    KnobIntWPtr _frameRange;
    KnobChoiceWPtr _filter;
    KnobChannelSelectWPtr _retimeMapChannel;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Time_SlitScan_h
