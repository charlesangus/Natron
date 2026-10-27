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
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <vector>

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
 * The element's value is defined row by row: each row's span is reduced by the row's sparse table
 * (level k holds the extremum of the 2^k values from each position, so that any span is the
 * extremum of two overlapping level entries), and the rows' values are reduced from the top. Every
 * reduction keeps its first operand unless the second is strictly beyond it, so NaNs and zeros of
 * either sign give one defined result, which the GPU repeats. filterPlane() computes that
 * definition directly; filterTile() and the node's passes compute it bit for bit with bounded
 * memory (TilePlan).
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
 * plane, its tables (level k of row y at tables + ((k - 1) * height + y) * width) and the radii of
 * those pixels (also indexed from x1). The tables cover at least the levels the radii's ceilings
 * need.
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
 * rx and ry (width x height each), from src into dst: the definition, with the tables of every
 * row of the plane at once, which filterTile() and the GPU kernels are tested against.
 **/
void filterPlane(const float* src,
                 int width,
                 int height,
                 const float* rx,
                 const float* ry,
                 bool disc,
                 bool takeMax,
                 float* dst);

/**
 * @brief The up to four elements of a pixel of radii (rx, ry): element e has the horizontal radius
 * x[e & 1] and the vertical one y[e >> 1], the floors at 0 and the ceilings at 1, blended by tx
 * and ty. Only the elements whose radii differ from element 0's are computed.
 **/
struct PixelElements {
    int x[2] = { 0, 0 };
    int y[2] = { 0, 0 };
    float tx = 0.f;
    float ty = 0.f;

    bool has(int e) const WARN_UNUSED_RETURN
    {
        return (!(e & 1) || (x[1] != x[0])) && (!(e & 2) || (y[1] != y[0]));
    }

    int radiusX(int e) const WARN_UNUSED_RETURN
    {
        return x[e & 1];
    }

    int radiusY(int e) const WARN_UNUSED_RETURN
    {
        return y[e >> 1];
    }
};

PixelElements pixelElements(float rx, float ry) WARN_UNUSED_RETURN;

/**
 * @brief The pixel's value from the values of its elements, v[e] read only where p.has(e).
 **/
float blendElements(const PixelElements& p, const float v[4]) WARN_UNUSED_RETURN;

/**
 * @brief How a tile computes its elements; both give filterPlane()'s values bit for bit.
 *
 * Rows: the window is split into bands of bandRows rows. For each band, the tables of the plane
 * rows its elements reach are built batchRows rows at a time, and every pixel of the band reduces
 * the batch's rows into its elements, in the definition's order, before the next batch; a band's
 * last batch done, its pixels blend their elements. A pixel costs O(ry) per element, and memory
 * is O(batchRows x width x log rx) for the tables plus O(bandRows x window width) per fractional
 * element. Any element shape.
 *
 * Columns, for the square only: a slot is a window column and a horizontal radius r that some
 * element of the column has. The span extremum over [x - r, x + r] of every plane row is written
 * down the slot's column, and a sparse table down that column, which skips NaNs as the
 * definition's top-down reduction does, answers each of the column's elements in O(1) whatever its
 * radius. The window's columns are taken in batches whose slots fit the budget, a batch building
 * the tables of the plane rows only over its own columns and halo. Memory is O(slots x height x
 * log ry) per batch.
 **/
enum class TileStrategy {
    Rows,
    Columns,
};

/**
 * @brief Fills rx and ry with the radii of the window.width() pixels of window row r.
 **/
using TileRadiiFn = std::function<void(int r, float* rx, float* ry)>;

/**
 * @brief The plan of one tile: its window, absolute, the radii of its pixels, and how planTile()
 * split the work so that no buffer exceeds its budget. The plane the tile reads is at most the
 * window grown by haloX and haloY; it may be smaller, clipped to an image.
 **/
struct TilePlan {
    RectI window;
    bool disc = false;
    bool takeMax = false;
    TileRadiiFn radii;
    int haloX = 0;
    int haloY = 0;
    // Of the row tables, and of the Columns strategy's column tables.
    int levels = 1;
    int columnLevels = 1;
    // The elements any pixel computes; element 0 always.
    bool element[4] = { true, false, false, false };
    TileStrategy strategy = TileStrategy::Rows;

    // Rows: band b covers window rows b * bandRows onwards; batch k of a band holds the tables of
    // its plane rows k * batchRows onwards.
    int bandRows = 1;
    int batchRows = 1;

    // Columns: the slots of window column c are slotStart[c] to slotStart[c + 1] - 1, slot s of
    // horizontal radius slotRadius[s] (increasing within a column); batch b covers window columns
    // columnBatch[b] to columnBatch[b + 1] - 1.
    std::vector<int> slotStart;
    std::vector<int> slotRadius;
    std::vector<int> columnBatch;
    // The most slots of any batch.
    int batchSlots = 0;
    // Whether the Columns strategy fits the budget at all; when not, its batches are empty.
    bool columnsFit = false;

    int bandCount() const WARN_UNUSED_RETURN;

    // The batches of band b, Rows.
    int batchCount(int band) const WARN_UNUSED_RETURN;

    // The batches of the Columns strategy.
    int columnBatchCount() const WARN_UNUSED_RETURN
    {
        return columnBatch.empty() ? 0 : (int)columnBatch.size() - 1;
    }

    // The floats of the tables of one row of a plane of width, Rows.
    std::size_t rowTableFloats(int width) const WARN_UNUSED_RETURN
    {
        return (std::size_t)(levels - 1) * (std::size_t)width;
    }

    // The floats of one slot's column on a plane of height, all its levels, Columns.
    std::size_t columnFloats(int height) const WARN_UNUSED_RETURN
    {
        return (std::size_t)columnLevels * (std::size_t)height;
    }

    // The fractional elements, which Rows accumulates beside the output.
    int extraElements() const WARN_UNUSED_RETURN
    {
        return (int)element[1] + (int)element[2] + (int)element[3];
    }
};

/**
 * @brief The plan of the tile over window, the radii of whose pixels radii gives. budgetFloats
 * bounds, per plane, the batch's tables (Rows takes half of it for them, half for the band's
 * fractional elements) or slot columns (Columns). The strategy is the cheaper one by an estimate
 * of the work; the disc always takes Rows. Both strategies are planned, so that a test may switch
 * a square plan's strategy when columnsFit.
 **/
TilePlan planTile(const RectI& window,
                  const TileRadiiFn& radii,
                  bool disc,
                  bool takeMax,
                  std::size_t budgetFloats) WARN_UNUSED_RETURN;

/**
 * @brief One plane of a tile: the source, width x height with its pixel (0, 0) at absolute
 * (x0, y0), and the window columns c0 to c1 - 1 and rows r0 to r1 - 1 that lie on it, the only
 * ones computed.
 **/
struct TilePlane {
    const float* src = nullptr;
    int width = 0;
    int height = 0;
    int x0 = 0;
    int y0 = 0;
    int c0 = 0;
    int c1 = 0;
    int r0 = 0;
    int r1 = 0;

    // The plane column of window column c, and the plane row of window row r.
    int x(const TilePlan& plan, int c) const WARN_UNUSED_RETURN
    {
        return plan.window.x1 + c - x0;
    }

    int y(const TilePlan& plan, int r) const WARN_UNUSED_RETURN
    {
        return plan.window.y1 + r - y0;
    }
};

TilePlane tilePlane(const TilePlan& plan, const float* src, int width, int height, int x0, int y0) WARN_UNUSED_RETURN;

/**
 * @brief Where the elements of the window's pixels go: element k of window pixel (c, r) at
 * e[k] + (r - r0[k]) * stride[k] + (c - c0[k]). Element 0 is the output plane, which finishRow()
 * blends the others into.
 **/
struct TileElements {
    float* e[4] = { nullptr, nullptr, nullptr, nullptr };
    std::size_t stride[4] = { 0, 0, 0, 0 };
    int c0[4] = { 0, 0, 0, 0 };
    int r0[4] = { 0, 0, 0, 0 };

    float* at(int k,
              int c,
              int r) const WARN_UNUSED_RETURN
    {
        return e[k] + (std::ptrdiff_t)(r - r0[k]) * (std::ptrdiff_t)stride[k] + (c - c0[k]);
    }
};

/**
 * @brief Rows: the window rows of band b on the plane, *ra to *rb - 1 (none when equal).
 **/
void bandRange(const TilePlan& plan, const TilePlane& t, int band, int* ra, int* rb);

/**
 * @brief Rows: the plane rows of batch k of band b, *first to *first + *count - 1, whose tables
 * go at tables + (y - *first) * plan.rowTableFloats(t.width), level stride t.width.
 **/
void batchRange(const TilePlan& plan, const TilePlane& t, int band, int batch, int* first, int* count);

/**
 * @brief Rows: reduces the batch's rows into the elements of window row r, in the definition's
 * order; an element's first row writes it. work is scratch of the caller's.
 **/
void foldRows(const TilePlan& plan,
              const TilePlane& t,
              const float* tables,
              int first,
              int count,
              int r,
              const TileElements& out,
              std::vector<float>& work);

/**
 * @brief Rows: the blended pixels of window row r, over element 0's.
 **/
void finishRow(const TilePlan& plan, const TilePlane& t, int r, const TileElements& out, std::vector<float>& work);

/**
 * @brief Columns: the window columns of batch b on the plane, *ca to *cb - 1, whose slots go at
 * columns + (s - plan.slotStart[*ca]) * plan.columnFloats(t.height); none when *ca == *cb.
 **/
void columnBatchRange(const TilePlan& plan, const TilePlane& t, int b, int* ca, int* cb);

/**
 * @brief Columns: level 0 of the batch's slots at plane row y, the span extrema of the row.
 **/
void fillSpans(const TilePlan& plan,
               const TilePlane& t,
               int ca,
               int cb,
               int y,
               float* columns,
               std::vector<float>& work);

/**
 * @brief Columns: the levels above 0 of slot s's column, whose level 0 is at column.
 **/
void buildColumn(const TilePlan& plan, const TilePlane& t, float* column);

/**
 * @brief Columns: window row r of the batch's columns, blended, into out.
 **/
void answerRow(const TilePlan& plan,
               const TilePlane& t,
               int ca,
               int cb,
               int r,
               const float* columns,
               const TileElements& out,
               std::vector<float>& work);

/**
 * @brief The tile of plan, single-threaded, from the width x height plane src whose pixel (0, 0)
 * is absolute (0, 0), into dst (width x height too), which receives the window's pixels on the
 * plane: what the node's passes compute, in the same calls.
 **/
void filterTile(const TilePlan& plan,
                const float* src,
                int width,
                int height,
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
