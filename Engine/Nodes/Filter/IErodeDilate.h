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

// Registrable classes: IErode (plugin ID IErode) and IDilate (plugin ID IDilate).

#ifndef Engine_Nodes_Filter_IErodeDilate_h
#define Engine_Nodes_Filter_IErodeDilate_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <list>
#include <memory>
#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Filter/ErodeDilate.h"
#include "Engine/Nodes/Filter/MapInput.h"
#include "Engine/Nodes/Image/PassExecutor.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/RectI.h"

#define PLUGINID_NATRON_IERODE "IErode"
#define PLUGIN_MAJOR_NATRON_IERODE 1
#define PLUGINID_NATRON_IDILATE "IDilate"
#define PLUGIN_MAJOR_NATRON_IDILATE 1

#define kIErodeDilateParamSize "size"
#define kIErodeDilateParamShape "shape"
#define kIErodeDilateParamShapeSquare "square"
#define kIErodeDilateParamShapeDisc "disc"
#define kIErodeDilateParamExpandRoD "expandRoD"

// Input indices.
#define kIErodeDilateInputSource 0
#define kIErodeDilateInputMask 1
#define kIErodeDilateInputMap 2

NATRON_NAMESPACE_ENTER

/**
 * @brief The variable-radius minimum and maximum filters of IErode and IDilate.
 *
 * A pixel of radii (rx, ry) takes the minimum (or the maximum) of the pixels of its structuring
 * element: the (2 rx + 1) x (2 ry + 1) rectangle, or the ellipse of those half axes, rows dy with
 * |dy| <= ry each spanning |dx| <= ellipseHalfWidth(rx, ry, dy). A fractional radius blends the
 * results of its floor and its ceiling, bilinearly over the two axes, so that the result moves
 * smoothly with the radius; whole radii take the element's extremum exactly. The element is clipped
 * to the plane, as ErodeDilateKernels::filterLine() clamps its window.
 *
 * Each row of a plane has a sparse table: level k holds the extremum of the 2^k values from each
 * position, so that any span of a row is the extremum of two overlapping level entries, and a
 * pixel of radius ry costs O(ry). A plane's tables hold levels 1 to levels - 1, level k of row y
 * at tables + ((k - 1) * height + y) * width; level 0 is the plane itself.
 **/
namespace IErodeDilateKernels {
/**
 * @brief The level count of tables answering spans of up to 2 * maxHalfWidth + 1 values.
 **/
int levelCount(int maxHalfWidth) WARN_UNUSED_RETURN;

/**
 * @brief Levels 1 to levels - 1 of the table of one row: tableRow + (k - 1) * levelStride holds
 * level k, written where a whole span of 2^k values starts.
 **/
void buildRowTable(const float* row,
                   int width,
                   int levels,
                   bool takeMax,
                   float* tableRow,
                   std::size_t levelStride);

/**
 * @brief The extremum of row[x1] to row[x2], both included, from the row and its table.
 **/
float rowQuery(const float* row,
               const float* tableRow,
               std::size_t levelStride,
               int x1,
               int x2,
               bool takeMax) WARN_UNUSED_RETURN;

/**
 * @brief The largest |dx| with (dx / rx)^2 + (dy / ry)^2 <= 1, computed exactly in integers; rx
 * when ry is 0 and dy is too, -1 when |dy| > ry.
 **/
int ellipseHalfWidth(int rx, int ry, int dy) WARN_UNUSED_RETURN;

/**
 * @brief Pixels x1 to x2 - 1 of row y of the filtered plane into out (indexed from x1), from the
 * plane, its tables and the radii of those pixels (also indexed from x1). The tables cover at
 * least the levels the radii's ceilings need.
 **/
void filterRow(const float* plane,
               const float* tables,
               int width,
               int height,
               int y,
               int x1,
               int x2,
               const float* rx,
               const float* ry,
               bool disc,
               bool takeMax,
               float* out);

/**
 * @brief The whole filter of one width x height plane, single-threaded, with the per-pixel radii
 * rx and ry (width x height each), from src into dst. The reference the node's threaded passes
 * and the GPU kernels are tested against.
 **/
void filterPlane(const float* src,
                 int width,
                 int height,
                 const float* rx,
                 const float* ry,
                 bool disc,
                 bool takeMax,
                 float* dst);
} // namespace IErodeDilateKernels

/**
 * @brief The radius of one axis as a function of the Map value, in pixels at the render scale,
 * computed in float as the GPU computes it: black at a Map of 0, white at 1, and in between
 * black + m (white - black), clamped between the two so that it is monotonic in the Map value and
 * a tile's largest radius is at one end of its Map range.
 **/
struct IErodeDilateAxis {
    float black = 0.f;
    float white = 0.f;

    float at(float map) const WARN_UNUSED_RETURN;

    float largest(const MapRange& range) const WARN_UNUSED_RETURN;
};

/**
 * @brief The knob values one IErode or IDilate evaluation reads: the size at white, in pixels
 * at full scale like Erode's, the shape, and the Map knobs with the size at black.
 **/
struct IErodeDilateParams {
    double sizeX = 1.;
    double sizeY = 1.;
    bool disc = false;
    bool expandRoD = true;
    MapInputParams map;

    /**
     * @brief The radius functions of the two axes at scale.
     **/
    void axes(const RenderScale& scale,
              IErodeDilateAxis* x,
              IErodeDilateAxis* y) const;
};

/**
 * @brief Erode (or, with MODE Dilate, dilate) by a structuring element whose size varies per pixel
 * with a Map input: Size where the Map is white, Size At Black where it is black, in between
 * elsewhere. The element is a rectangle, as Erode's and Dilate's, or an ellipse. A pixel whose
 * size is 0 is left exactly as it is, and at a Map of 1 and a whole Size the square shape gives
 * Erode's and Dilate's result bit for bit. A disconnected Map filters at Size everywhere, and the
 * node says so in a warning.
 *
 * Its passes: the sparse tables of every row of each plane, then each output pixel's queries into
 * the second slot, the pixels of the scratch outside the render window copied there. The region of
 * interest grows by the larger radius of the two sizes, since a tile's Map is not known when it is
 * asked, and IDilate's region of definition by as much unless Expand RoD is off; a tile whose Map
 * gives a radius of 0 throughout is a pass-through copy. The Map is read over the render window
 * only, since only the output pixels' radii matter.
 *
 * It is a Passes pipeline driven by its own render, as IBlur is, because its passes read the Map,
 * an Aux input the host never blends. On the GPU (Kernels/ierodedilate.slang) the tables live in
 * device memory and the queries match the CPU's bit for bit; a radius above the GPU's cap stays on
 * the CPU.
 **/
template <ErodeDilateMode MODE>
class IErodeDilateNode
    : public StagedImageEffect {
public:
    static constexpr bool kDilate = (MODE == ErodeDilateMode::Dilate);

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new IErodeDilateNode(node);
    }

    explicit IErodeDilateNode(NodePtr node);

    virtual ~IErodeDilateNode();

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual const ImagePipelineSpec& imagePipelineSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual std::string getInputHint(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;

    IErodeDilateParams getParams(double time, ViewIdx view) const WARN_UNUSED_RETURN;

    /**
     * @brief The passes for params at scale over the render window roi, whose Map values lie in
     * range, reading map; with no map, the passes have no work and only carry the halo and growth,
     * as the region actions need. No pass at all when the range gives a radius of 0 throughout.
     **/
    static PassList passes(const IErodeDilateParams& params,
                           const RenderScale& scale,
                           const MapRange& range,
                           const std::shared_ptr<const MapPlane>& map,
                           const RectI& roi) WARN_UNUSED_RETURN;

    /**
     * @brief The text of the warning a disconnected Map posts.
     **/
    static std::string mapNotConnectedMessage() WARN_UNUSED_RETURN;

    /**
     * @brief The Map is read only while its channel resolves; otherwise its Map Channel reads 0
     * everywhere, as outside its image.
     **/
    virtual bool readsGpuInput(int inputNb,
                               const ImageLayerDesc& plane,
                               double time,
                               ViewIdx view) const OVERRIDE FINAL WARN_UNUSED_RETURN;

protected:
    /**
     * @brief Without Expand RoD, the source's region, unexpanded, as Dilate's.
     **/
    virtual bool clipRegionOfDefinition(double time,
                                        const RenderScale& scale,
                                        ViewIdx view,
                                        const EffectInstancePtr& source,
                                        const RectI& srcRoD,
                                        RectI* rod) OVERRIDE FINAL;

private:
    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual void onEffectCreated(bool mayCreateFileDialog,
                                 const CreateNodeArgs& args) OVERRIDE FINAL;

    virtual void onInputChanged(int inputNo) OVERRIDE FINAL;

    // An identity when both sizes give a radius of 0.
    virtual bool isOpIdentity(double time,
                              const RenderScale& scale,
                              const RectI& roi,
                              ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum renderOp(const RenderActionArgs& args,
                                const StagePlan& plan) OVERRIDE FINAL WARN_UNUSED_RETURN;

    // The passes over the full range of the Map, without CPU work; with a GPU recorder while no
    // radius exceeds the GPU's cap.
    virtual PassList opPasses(double time,
                              ViewIdx view,
                              const RenderScale& scale) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    // The Map in the layer its channel resolves in.
    virtual bool opPassesInputLayer(int inputNb,
                                    const ImageLayerDesc& plane,
                                    double time,
                                    ViewIdx view,
                                    ImageLayerDesc* layer) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    // The GPU recorder of opPasses(): the filter for params at scale, reading the Map's channel at
    // time and view from io.
    StatusEnum recordGpu(const PassGpuIO& io,
                         double time,
                         ViewIdx view,
                         const IErodeDilateParams& params,
                         const RenderScale& scale) const WARN_UNUSED_RETURN;

    // Posts the warning while the Map is disconnected, and takes back its own once connected.
    void refreshMapWarning();

    MapInput _map;
    KnobDoubleWPtr _size;
    KnobChoiceWPtr _shape;
    KnobBoolWPtr _expandRoD;
};

extern template class IErodeDilateNode<ErodeDilateMode::Erode>;
extern template class IErodeDilateNode<ErodeDilateMode::Dilate>;

using IErode = IErodeDilateNode<ErodeDilateMode::Erode>;
using IDilate = IErodeDilateNode<ErodeDilateMode::Dilate>;

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Filter_IErodeDilate_h
