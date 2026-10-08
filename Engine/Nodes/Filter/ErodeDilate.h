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
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/NativeImageEffect.h"
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
 * @brief Erode (or dilate) by a rectangular structuring element, as openfx-misc's CImgErode and
 * CImgDilate do; this class is the erosion and Dilate the dilation. Each takes a size per axis:
 * a positive size runs the node's own operation over a window of floor(size * renderScale) * 2 + 1
 * pixels, a negative size runs the opposite operation. Min and max filters don't commute, so
 * parity needs the OpenFX order: positive axes before negative ones, X before Y.
 *
 * The filters run over one float buffer covering the render window plus the halo, clipped to
 * the output's region of definition, as the OpenFX plug-ins do: outside the source image the
 * buffer reads as zero, and the window clamps at the buffer's edge. The host never splits that
 * buffer (FullySafe), the X pass runs over whole rows and the Y pass over whole columns on the
 * global pool, which gives the same pixels at any thread count.
 *
 * The "(Un)premult by" selector starts at None: the host has no per-source automatic default.
 **/
class ErodeDilate
    : public NativeImageEffect {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new ErodeDilate(node);
    }

    explicit ErodeDilate(NodePtr node);

    virtual ~ErodeDilate();

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;

    virtual StatusEnum getRegionOfDefinition(U64 hash,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;

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
     * whose sign selects the operation opposite to this node's own, in pixels rounded up.
     **/
    static void getExpansion(const RenderScale& scale, const ErodeDilateParams& params, bool dilate, int* deltaX, int* deltaY);

protected:
    ErodeDilate(NodePtr node, bool dilate);

    virtual NativePluginDescription getNativePluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void getRegionsOfInterest(double time,
                                      const RenderScale& scale,
                                      const RectD& outputRoD,
                                      const RectD& renderWindow,
                                      ViewIdx view,
                                      RoIMap* ret) OVERRIDE FINAL;

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

private:
    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual bool isIdentityOp(double time,
                              const RenderScale& scale,
                              const RectI& roi,
                              ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    bool _dilate;
    KnobIntWPtr _size;
    KnobBoolWPtr _expandRoD;
};

/**
 * @brief The dilation: ErodeDilate with the roles of the two signs swapped.
 **/
class Dilate
    : public ErodeDilate {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new Dilate(node);
    }

    explicit Dilate(NodePtr node)
        : ErodeDilate(node, true)
    {
    }

    virtual ~Dilate()
    {
    }
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Filter_ErodeDilate_h
