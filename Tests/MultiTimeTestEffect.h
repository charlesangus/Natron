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

#ifndef Tests_MultiTimeTestEffect_h
#define Tests_MultiTimeTestEffect_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <algorithm>
#include <list>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include <QString>
#include <QStringList>

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/LibraryBinary.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/ImagePipelineSpec.h"
#include "Engine/Nodes/Image/Op.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/KnobReader.h"
#include "Engine/Nodes/NodeSpec.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Plugin.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuKernelRegistry.h"
#include "Engine/Gpu/GpuRecord.h"
#include "Engine/Gpu/GpuWrap.h"
#endif

#define kTestPluginIDTimeOffsetRoute "test.natron.built-in.TimeOffsetRoute"
#define kTestPluginIDTimeOffsetCustom "test.natron.built-in.TimeOffsetCustom"
#define kTestPluginIDNeighbourAverage "test.natron.built-in.NeighbourAverage"
#define kTestPluginIDUndeclaredTimeRead "test.natron.built-in.UndeclaredTimeRead"

NATRON_NAMESPACE_ENTER

namespace MultiTimeTest {

// How far the time-offset test nodes move time.
inline constexpr int kOffset = 3;

inline constexpr InputSpec kSourceInputs[] = {
    InputSpec { .label = "Source" },
};

/**
 * @brief A Route op answering its Source at time + kOffset, as a TimeOffset does.
 **/
struct TimeOffsetRouteOp {
    struct Params {
        int unused;
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = kTestPluginIDTimeOffsetRoute,
        .label = "Test Time Offset Route",
        .grouping = "Other/Test",
        .inputs = kSourceInputs,
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .stages = {},
        .layers = LayerPolicy::none(),
        .op = OpKind::Route,
    };

    Params bake(const KnobReader& /*reader*/) const
    {
        return Params { 0 };
    }

    Route route(const Params& /*params*/,
                const RouteContext& context) const
    {
        return context.isConnected(0) ? Route::input(0, context.time() + kOffset) : Route::black();
    }
};

static_assert(RouteOp<TimeOffsetRouteOp>);

typedef ImageNode<TimeOffsetRouteOp> TimeOffsetRouteNode;

inline constexpr NodeSpec kTimeOffsetCustomSpec = {
    .id = kTestPluginIDTimeOffsetCustom,
    .label = "Test Time Offset Custom",
    .grouping = "Other/Test",
    .inputs = kSourceInputs,
};

inline constexpr NodeSpec kNeighbourAverageSpec = {
    .id = kTestPluginIDNeighbourAverage,
    .label = "Test Neighbour Average",
    .grouping = "Other/Test",
    .inputs = kSourceInputs,
};

inline constexpr NodeSpec kUndeclaredTimeReadSpec = {
    .id = kTestPluginIDUndeclaredTimeRead,
    .label = "Test Undeclared Time Read",
    .grouping = "Other/Test",
    .inputs = kSourceInputs,
};

inline constexpr ImagePipelineSpec kCustomPipeline = {
    .stages = {},
    .layers = LayerPolicy::none(),
    .op = OpKind::Custom,
};

/**
 * @brief A Custom op whose output is the mean of its Source at time - 1 and at time + READ_AFTER,
 * declaring time - 1 and time + 1 to the scheduler. With READ_AFTER other than 1 it reads a time
 * it did not declare. With ROUTE_OFFSET other than 0 it is instead an identity of its Source at
 * time + ROUTE_OFFSET, through route() rather than a Route op. On the GPU, which needs
 * NATRON_HAVE_GPU, it renders the same mean with the dissolve kernel, reading both times from the
 * views GpuRenderArgs::inputAt() hands it.
 **/
template <const NodeSpec& SPEC, int READ_AFTER, int ROUTE_OFFSET>
class NeighbourAverageEffect
    : public StagedImageEffect {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new NeighbourAverageEffect(node);
    }

    explicit NeighbourAverageEffect(NodePtr node)
        : StagedImageEffect(node)
    {
    }

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<SPEC>();
    }

    virtual const ImagePipelineSpec& imagePipelineSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticImagePipelineSpec<kCustomPipeline, SPEC>();
    }

    virtual Route route(double time,
                        ViewIdx /*view*/) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        if ((ROUTE_OFFSET != 0) && getInput(0)) {
            return Route::input(0, time + ROUTE_OFFSET);
        }

        return Route::none();
    }

    virtual FramesNeededMap getFramesNeeded(double time,
                                            ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        if ((ROUTE_OFFSET != 0) || !getInput(0)) {
            return StagedImageEffect::getFramesNeeded(time, view);
        }
        FramesNeededMap frames;
        RangeD before;
        before.min = before.max = time - 1.;
        RangeD after;
        after.min = after.max = time + 1.;
        frames[0][view].push_back(before);
        frames[0][view].push_back(after);

        return frames;
    }

private:
    virtual void initializeKnobs() OVERRIDE FINAL
    {
        addStageKnobs(createControlsPage());
    }

    virtual bool isOpIdentity(double /*time*/,
                              const RenderScale& /*scale*/,
                              const RectI& /*roi*/,
                              ViewIdx /*view*/) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return false;
    }

    virtual StatusEnum renderOp(const RenderActionArgs& args,
                                const StagePlan& /*plan*/) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        for (const std::pair<ImageLayerDesc, ImagePtr>& output : args.outputLayers) {
            const ImagePtr& dst = output.second;
            if (!dst || (dst->getBitDepth() != eImageBitDepthFloat)) {
                return eStatusFailed;
            }
            // Both are fetched before any image is locked: fetching renders upstream.
            const ImagePtr before = fetchInputAt(0, args.time - 1., args, &output.first);
            const ImagePtr after = before ? fetchInputAt(0, args.time + READ_AFTER, args, &output.first) : ImagePtr();
            if (!before || !after || (before->getBitDepth() != eImageBitDepthFloat) || (after->getBitDepth() != eImageBitDepthFloat)) {
                return eStatusFailed;
            }
            const int nComps = (int)dst->getComponentsCount();
            const int beforeComps = std::min(nComps, (int)before->getComponentsCount());
            const int afterComps = std::min(nComps, (int)after->getComponentsCount());
            Image::ReadAccess beforeAccess = before->getReadRights();
            Image::ReadAccess afterAccess = after->getReadRights();
            Image::WriteAccess dstAccess(dst.get());
            for (int y = args.roi.y1; y < args.roi.y2; ++y) {
                for (int x = args.roi.x1; x < args.roi.x2; ++x) {
                    float* pixel = (float*)dstAccess.pixelAt(x, y);
                    if (!pixel) {
                        return eStatusFailed;
                    }
                    const float* a = (const float*)beforeAccess.pixelAt(x, y);
                    const float* b = (const float*)afterAccess.pixelAt(x, y);
                    for (int c = 0; c < nComps; ++c) {
                        const float va = (a && (c < beforeComps)) ? a[c] : 0.f;
                        const float vb = (b && (c < afterComps)) ? b[c] : 0.f;
                        pixel[c] = (va + vb) * 0.5f;
                    }
                }
            }
        }

        return eStatusOK;
    }

    virtual bool canRenderOpOnGpu(const GpuQuery& query) const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
#ifdef NATRON_HAVE_GPU
        if ((READ_AFTER != 1) || (ROUTE_OFFSET != 0) || (query.bitDepth != eImageBitDepthFloat) || query.hasMask || (query.inputTimes.size() != 1)) {
            return false;
        }
        const std::vector<double>& times = query.inputTimes.front().times;

        return (query.inputTimes.front().inputNb == 0) && (std::find(times.begin(), times.end(), query.time - 1.) != times.end()) && (std::find(times.begin(), times.end(), query.time + 1.) != times.end());
#else
        Q_UNUSED(query);

        return false;
#endif
    }

    virtual bool getOpGpuInputLayer(int /*inputNb*/,
                                    const ImageLayerDesc& plane,
                                    double /*time*/,
                                    ViewIdx /*view*/,
                                    ImageLayerDesc* layer) const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        *layer = plane;

        return true;
    }

    virtual StatusEnum renderOpGpu(const GpuRenderArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
#ifdef NATRON_HAVE_GPU
        const GpuImageView* before = args.inputAt(0, args.query.time - 1.);
        const GpuImageView* after = args.inputAt(0, args.query.time + 1.);
        if (!before || !after || (before->nComps != args.output.nComps) || (after->nComps != args.output.nComps)) {
            return eStatusFailed;
        }
        gpu::GpuWrapView base;
        gpu::GpuWrapView folded;
        gpu::GpuWrap wrap;
        if (!toWrapView(*before, &base) || !toWrapView(*after, &folded) || !toWrapView(args.output, &wrap.dst)) {
            return eStatusFailed;
        }
        wrap.src = base;
        wrap.output = gpu::GpuWrapRect { args.outputRect.x1, args.outputRect.y1, args.outputRect.x2, args.outputRect.y2 };
        wrap.nComps = (uint32_t)args.output.nComps;

        // The dissolve kernel's parameter block is its blend, (to - from) * blend + from.
        const float blend = 0.5f;
        gpu::GpuBuffer* params = nullptr;
        if (!args.context.scratch(sizeof(blend), params) || !params || !args.context.updateBuffer(*params, 0, std::as_bytes(std::span<const float>(&blend, 1)))) {
            return eStatusFailed;
        }

        return gpu::recordWrapFold(args.context, gpu::GpuKernelId::dissolve, wrap, base, std::span<const gpu::GpuWrapView>(&folded, 1), params) ? eStatusOK : eStatusFailed;
#else
        Q_UNUSED(args);

        return eStatusFailed;
#endif
    }

#ifdef NATRON_HAVE_GPU
    static bool toWrapView(const GpuImageView& view,
                           gpu::GpuWrapView* out)
    {
        if (!view.buffer || ((view.byteOffset % sizeof(float)) != 0)) {
            return false;
        }
        out->buffer = view.buffer;
        out->offsetFloats = view.byteOffset / sizeof(float);
        out->bounds = gpu::GpuWrapRect { view.bounds.x1, view.bounds.y1, view.bounds.x2, view.bounds.y2 };
        out->strideFloats = (uint32_t)view.rowStrideFloats;
        out->nComps = (uint32_t)view.nComps;

        return true;
    }
#endif
};

typedef NeighbourAverageEffect<kNeighbourAverageSpec, 1, 0> NeighbourAverageNode;
typedef NeighbourAverageEffect<kUndeclaredTimeReadSpec, 2, 0> UndeclaredTimeReadNode;
typedef NeighbourAverageEffect<kTimeOffsetCustomSpec, 1, kOffset> TimeOffsetCustomNode;

template <typename PLUGIN>
void
registerPlugin()
{
    EffectInstancePtr node(PLUGIN::BuildEffect(NodePtr()));
    std::map<std::string, void (*)()> functions;

    functions.insert(std::make_pair("BuildEffect", (void (*)())&PLUGIN::BuildEffect));
    LibraryBinary* binary = new LibraryBinary(functions);

    std::list<std::string> grouping;
    node->getPluginGrouping(&grouping);
    QStringList qgrouping;
    for (std::list<std::string>::iterator it = grouping.begin(); it != grouping.end(); ++it) {
        qgrouping.push_back(QString::fromUtf8(it->c_str()));
    }

    Plugin* p = appPTR->registerPlugin(QString(), qgrouping, QString::fromUtf8(node->getPluginID().c_str()), QString::fromUtf8(node->getPluginLabel().c_str()),
                                       QString::fromUtf8(""), QStringList(), node->isReader(), node->isWriter(), binary, node->renderThreadSafety() == eRenderSafetyUnsafe, node->getMajorVersion(), node->getMinorVersion(), false);
    // Registered after AppManager::load(), so the label-without-suffix the script name is made
    // from was never assigned.
    p->setLabelWithoutSuffix(Plugin::makeLabelWithoutSuffix(p->getPluginLabel()));
    p->setOpenGLRenderSupport(node->supportsOpenGLRender());
}

} // namespace MultiTimeTest

/**
 * @brief Registers the time-offset and multi-time test plug-ins, once per process.
 **/
inline void
registerMultiTimeTestPluginsOnce()
{
    static std::once_flag once;

    std::call_once(once, []() {
        MultiTimeTest::registerPlugin<MultiTimeTest::TimeOffsetRouteNode>();
        MultiTimeTest::registerPlugin<MultiTimeTest::TimeOffsetCustomNode>();
        MultiTimeTest::registerPlugin<MultiTimeTest::NeighbourAverageNode>();
        MultiTimeTest::registerPlugin<MultiTimeTest::UndeclaredTimeReadNode>();
    });
}

NATRON_NAMESPACE_EXIT

#endif // Tests_MultiTimeTestEffect_h
