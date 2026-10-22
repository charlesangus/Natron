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

// Registrable classes: ErodeDilate (plugin ID net.sf.cimg.CImgErode, a positive size erodes) and
// Dilate (plugin ID net.sf.cimg.CImgDilate, a positive size dilates).

#ifndef Engine_Nodes_Filter_ErodeDilate_h
#define Engine_Nodes_Filter_ErodeDilate_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <list>
#include <optional>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/RectI.h"

#define PLUGINID_NATRON_ERODE "net.sf.cimg.CImgErode"
#define PLUGIN_MAJOR_NATRON_ERODE 3
#define PLUGINID_NATRON_DILATE "net.sf.cimg.CImgDilate"
#define PLUGIN_MAJOR_NATRON_DILATE 3

#define kErodeDilateParamSize "size"
#define kErodeDilateParamExpandRoD "expandRoD"

NATRON_NAMESPACE_ENTER

namespace ErodeDilateKernels {
struct LineScratch {
    std::vector<float> padded;
    std::vector<float> prefix;
    std::vector<float> suffix;
};

/**
 * @brief Replaces the `length` values data[0], data[stride], ... by the minimum (or the maximum
 * with takeMax) over the window of 2 * halfWidth + 1 values centred on each, the window being
 * clamped to the line (nearest-pixel boundary). van Herk / Gil-Werman, constant time per value.
 * A line no longer than halfWidth + 2 values becomes its own global extremum, which is what
 * CImg's erode() and dilate() do there.
 **/
void filterLine(float* data,
                int length,
                std::ptrdiff_t stride,
                int halfWidth,
                bool takeMax,
                LineScratch& scratch);

/// The column count filterColumns() works on at once: one cache line of floats.
const int kColumnBlock = 16;

/**
 * @brief filterLine() over the `count` lines starting at data, data + 1, ..., data + count - 1,
 * each of `length` values `stride` apart: adjacent columns of a row-major plane. Equal to calling
 * filterLine() on each, but walks kColumnBlock columns together, reading each row once per block.
 **/
void filterColumns(float* data,
                   int length,
                   std::ptrdiff_t stride,
                   int count,
                   int halfWidth,
                   bool takeMax,
                   LineScratch& scratch);
} // namespace ErodeDilateKernels

/**
 * @brief The knob values one render, region of definition or region of interest reads.
 **/
struct ErodeDilateParams {
    int sizeX;
    int sizeY;
    bool expandRoD;

    ErodeDilateParams()
        : sizeX(1)
        , sizeY(1)
        , expandRoD(true)
    {
    }
};

/**
 * @brief Which operation a positive size runs: the erosion of Erode, the dilation of Dilate.
 **/
enum class ErodeDilateMode {
    Erode,
    Dilate
};

inline constexpr InputSpec kErodeDilateInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

/**
 * @brief Erode (or dilate) by a rectangular structuring element, as openfx-misc's CImgErode and
 * CImgDilate do; MODE picks which of the two this is. Each takes a size per axis: a positive size
 * runs the node's own operation over a window of floor(size * renderScale) * 2 + 1 pixels, a
 * negative size runs the opposite operation. Min and max filters don't commute, so parity needs
 * the OpenFX order: positive axes before negative ones, X before Y.
 *
 * Its passes are those line filters over one buffer covering the render window plus the halo,
 * clipped to the output's region of definition, as the OpenFX plug-ins do: outside the source
 * image the buffer reads as zero, and the window clamps at the buffer's edge. Each pass runs over
 * whole lines, which gives the same pixels at any thread count.
 **/
template <ErodeDilateMode MODE>
struct ErodeDilateOp {
    using Params = ErodeDilateParams;

    static constexpr bool kDilate = (MODE == ErodeDilateMode::Dilate);

    static constexpr NodeSpec kNodeSpec = {
        .id = kDilate ? PLUGINID_NATRON_DILATE : PLUGINID_NATRON_ERODE,
        .label = kDilate ? "Dilate" : "Erode",
        .description = kDilate ? "Dilate (or erode) input stream by a rectangular structuring element of specified size and Neumann boundary conditions (pixels out of the image get the value of the nearest pixel).\n"
                                 "A negative size will perform an erosion instead of a dilation.\n"
                                 "Different sizes can be given for the x and y axis.\n"
                                 "The operation is the 'dilate' and 'erode' behaviour of the CImg library (http://cimg.eu), "
                                 "distributed under the CeCILL-C licence."
                               : "Erode (or dilate) input stream by a rectangular structuring element of specified size and Neumann boundary conditions (pixels out of the image get the value of the nearest pixel).\n"
                                 "A negative size will perform a dilation instead of an erosion.\n"
                                 "Different sizes can be given for the x and y axis.\n"
                                 "The operation is the 'erode' and 'dilate' behaviour of the CImg library (http://cimg.eu), "
                                 "distributed under the CeCILL-C licence.",
        .grouping = PLUGIN_GROUP_FILTER,
        .majorVersion = kDilate ? PLUGIN_MAJOR_NATRON_DILATE : PLUGIN_MAJOR_NATRON_ERODE,
        .minorVersion = 0,
        .icon = kDilate ? NATRON_IMAGES_PATH "NativeNodes/net.sf.cimg.CImgDilate.png" : NATRON_IMAGES_PATH "NativeNodes/net.sf.cimg.CImgErode.png",
        .inputs = kErodeDilateInputs,
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .stages = { Stage::MaskMix, Stage::PassThroughUnprocessed },
        .layers = LayerPolicy::all(),
        .op = OpKind::Passes,
    };

    void createKnobs(KnobHolder* holder,
                     const KnobPagePtr& page);

    Params bake(const KnobReader& reader) const;

    /**
     * @brief ErodeDilateNode::paramsAreIdentity() at the context's scale.
     **/
    bool isIdentity(const Params& params,
                    const PassContext& context) const;

    /**
     * @brief One line filter per axis with a non-zero size, the positive axes' then the negative
     * axes', X before Y. Each carries its axis's halo, ErodeDilateNode::getSourceRoI(), and the
     * filter that dilates its axis's spread, ErodeDilateNode::getExpansion(); one whose half width
     * is zero at the context's scale only contributes them. The GPU recorder is left out where a
     * half width exceeds the morphology kernel's tile.
     **/
    PassList passes(const Params& params,
                    const PassContext& context) const;

    /**
     * @brief The larger axis's size in pixels at the context's scale, rounded down, even when the
     * op is an identity.
     **/
    std::optional<GpuCostHint> costHint(const Params& params,
                                        const PassContext& context) const;

    KnobIntWPtr size;
    KnobBoolWPtr expandRoD;
};

extern template struct ErodeDilateOp<ErodeDilateMode::Erode>;
extern template struct ErodeDilateOp<ErodeDilateMode::Dilate>;

static_assert(PassesOp<ErodeDilateOp<ErodeDilateMode::Erode>>);
static_assert(PassesOp<ErodeDilateOp<ErodeDilateMode::Dilate>>);

template <ErodeDilateMode MODE>
class ErodeDilateNode
    : public ImageNode<ErodeDilateOp<MODE>> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new ErodeDilateNode(node);
    }

    explicit ErodeDilateNode(NodePtr node);

    virtual ~ErodeDilateNode();

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;

    /**
     * @brief The Params the op renders with at (time, view).
     **/
    void getParams(double time, ViewIdx view, ErodeDilateParams* params) const;

    /**
     * @brief The pixels needed to filter `rect` (pixel coordinates at `scale`), before clipping
     * to the output's region of definition.
     **/
    static RectI getSourceRoI(const RectI& rect, const RenderScale& scale, const ErodeDilateParams& params) WARN_UNUSED_RETURN;

    /**
     * @brief Whether params leave the image unchanged at scale, including the OpenFX plug-ins'
     * rule that a negative size smaller than one pixel at scale is never identity.
     **/
    static bool paramsAreIdentity(const RenderScale& scale, const ErodeDilateParams& params) WARN_UNUSED_RETURN;

    /**
     * @brief How far the region of definition grows on each axis at scale: the size of the axes
     * whose sign selects a dilation, in pixels rounded up.
     **/
    static void getExpansion(const RenderScale& scale, const ErodeDilateParams& params, bool dilate, int* deltaX, int* deltaY);

protected:
    /**
     * @brief Without expandRoD, the source's region, unexpanded.
     **/
    virtual bool clipRegionOfDefinition(double time,
                                        const RenderScale& scale,
                                        ViewIdx view,
                                        const EffectInstancePtr& source,
                                        const RectI& srcRoD,
                                        RectI* rod) OVERRIDE FINAL;
};

extern template class ErodeDilateNode<ErodeDilateMode::Erode>;
extern template class ErodeDilateNode<ErodeDilateMode::Dilate>;

using ErodeDilate = ErodeDilateNode<ErodeDilateMode::Erode>;
using Dilate = ErodeDilateNode<ErodeDilateMode::Dilate>;

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Filter_ErodeDilate_h
