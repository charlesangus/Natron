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

// Registrable class: ErodeDilate (plugin ID ErodeDilate).

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

#define PLUGINID_NATRON_ERODEDILATE "ErodeDilate"
#define PLUGIN_MAJOR_NATRON_ERODEDILATE 1

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

inline constexpr InputSpec kErodeDilateInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

/**
 * @brief Erode or dilate by a rectangular structuring element, with a signed size per axis: a
 * positive size erodes (a minimum over a window of floor(size * renderScale) * 2 + 1 pixels), a
 * negative size dilates (a maximum), and the two axes may have opposite signs. It is one line
 * filter on X, then one on Y, each with its own axis's operation; min and max filters don't
 * commute, so the order is part of the result.
 *
 * Its passes are those line filters over one buffer covering the render window plus the halo,
 * clipped to the output's region of definition: outside the source image the buffer reads as
 * zero, and the window clamps at the buffer's edge. Each pass runs over whole lines, which gives
 * the same pixels at any thread count.
 **/
struct ErodeDilateOp {
    using Params = ErodeDilateParams;

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_ERODEDILATE,
        .label = "ErodeDilate",
        .description = "Erode or dilate the input by a rectangular structuring element of 2*size+1 pixels on each axis.\n"
                       "A positive size erodes (each pixel takes the minimum of the pixels around it), a negative size dilates (the maximum), and 0 leaves the axis unchanged. "
                       "The two axes may have opposite signs: X is filtered first, then Y, each by its own sign.\n"
                       "Pixels outside the image read as transparent black, and the window is clamped to the processed region.",
        .grouping = PLUGIN_GROUP_FILTER,
        .majorVersion = PLUGIN_MAJOR_NATRON_ERODEDILATE,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/ErodeDilate.png",
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
     * @brief ErodeDilate::paramsAreIdentity() at the context's scale.
     **/
    bool isIdentity(const Params& params,
                    const PassContext& context) const;

    /**
     * @brief One line filter per axis with a non-zero size, X then Y, each the minimum where its
     * size is positive and the maximum where it is negative. Each carries its axis's halo,
     * ErodeDilate::getSourceRoI(), and a dilating pass its axis's spread,
     * ErodeDilate::getExpansion(); one whose half width is zero at the context's scale only
     * contributes them. The GPU recorder is left out where a half width exceeds the morphology
     * kernel's tile.
     **/
    PassList passes(const Params& params,
                    const PassContext& context) const;

    /**
     * @brief The Filter weight of the larger axis's size in pixels at the context's scale,
     * rounded down, even when the op is an identity.
     **/
    std::optional<GpuWeight> gpuWeight(const Params& params,
                                       const PassContext& context) const;

    KnobIntWPtr size;
    KnobBoolWPtr expandRoD;
};

static_assert(PassesOp<ErodeDilateOp>);

class ErodeDilate
    : public ImageNode<ErodeDilateOp> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new ErodeDilate(node);
    }

    explicit ErodeDilate(NodePtr node);

    virtual ~ErodeDilate();

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
     * @brief Whether params leave the image unchanged at scale: both axes' half widths,
     * floor(|size| * scale), are zero, whatever the signs.
     **/
    static bool paramsAreIdentity(const RenderScale& scale, const ErodeDilateParams& params) WARN_UNUSED_RETURN;

    /**
     * @brief How far the region of definition grows on each axis at scale: the size of the axes
     * that dilate, in pixels rounded up.
     **/
    static void getExpansion(const RenderScale& scale, const ErodeDilateParams& params, int* deltaX, int* deltaY);

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

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Filter_ErodeDilate_h
