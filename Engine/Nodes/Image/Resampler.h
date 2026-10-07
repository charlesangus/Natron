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

#ifndef Engine_Nodes_Image_Resampler_h
#define Engine_Nodes_Image_Resampler_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <vector>

#include "Engine/Nodes/Image/TransformMath.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"

/*
 * Knob script names, labels and hints shared by every resampling node, as openfx-misc's
 * ofxsFilter.h and ofxsShutter.h declare them.
 */
#define kResamplerParamFilterType "filter"
#define kResamplerParamFilterTypeLabel "Filter"
#define kResamplerParamFilterTypeHint "Filtering algorithm - some filters may produce values outside of the initial range (*) or modify the values even if there is no movement (+)."
#define kResamplerParamFilterClamp "clamp"
#define kResamplerParamFilterClampLabel "Clamp"
#define kResamplerParamFilterClampHint "Clamp filter output within the original range - useful to avoid negative values in mattes"
#define kResamplerParamFilterBlackOutside "black_outside"
#define kResamplerParamFilterBlackOutsideLabel "Black outside"
#define kResamplerParamFilterBlackOutsideHint "Fill the area outside the source image with black"

#define kResamplerParamShutterOffset "shutterOffset"
#define kResamplerParamShutterOffsetLabel "Shutter Offset"
#define kResamplerParamShutterOffsetHint "Controls when the shutter should be open/closed. Ignored if there is no motion blur (i.e. shutter=0 or motionBlur=0)."
#define kResamplerParamShutterCustomOffset "shutterCustomOffset"
#define kResamplerParamShutterCustomOffsetLabel "Custom Offset"
#define kResamplerParamShutterCustomOffsetHint "When custom is selected, the shutter is open at current time plus this offset (in frames). Ignored if there is no motion blur (i.e. shutter=0 or motionBlur=0)."

NATRON_NAMESPACE_ENTER

/**
 * @brief The resampling core of the native spatial nodes (Transform, TransformMasked, Reformat):
 * a port of openfx-misc's ofxsFilter.h filters and supersampling, the Transform3x3 processor's
 * per-pixel loop (including motion blur) and its region helpers, with the same formulas,
 * evaluation order and precision (double maths, float accumulation where OFX accumulates in
 * float), so a native node reproduces the OFX output to rounding.
 *
 * Coordinates are pixel coordinates at the render scale: the centre of pixel (i, j) is
 * (i + 0.5, j + 0.5). Everything here is stateless and thread-safe; nothing spawns threads,
 * so a node parallelises by calling resampleRow() for disjoint rows from its own bands.
 *
 * Typical render of a Transform-like node, per plane:
 *  1. on the calling thread, read the knobs and the source image's bounds and pointer into a
 *     SourceImage (never read Image bounds inside band threads);
 *  2. buildSamplingTransforms() with a CanonicalTransformFn for the node's matrix, then
 *     concatenateInputTransform() if the host handed back an input transform;
 *  3. makeResampleParams(), choosing eFilterImpulse in draft render quality;
 *  4. for each row y of the window: resampleRow() into a float row, then apply mask x mix
 *     against the source pixel at (x, y) the way NativeImageEffect does.
 * And for the region actions: getRegionOfDefinition() / getRegionOfInterest() with a
 * RegionParams filled from the same knobs.
 **/
namespace Resampler {
/// The filter choice, in the OFX option order (the knob's option index).
enum FilterEnum {
    eFilterImpulse = 0,
    eFilterBox,
    eFilterBilinear,
    eFilterCubic,
    eFilterKeys,
    eFilterSimon,
    eFilterRifman,
    eFilterMitchell,
    eFilterParzen,
    eFilterNotch,
};

/// The shutterOffset choice, in the OFX option order.
enum ShutterOffsetEnum {
    eShutterOffsetCentered = 0,
    eShutterOffsetStart,
    eShutterOffsetEnd,
    eShutterOffsetCustom,
};

/// One option of a choice knob: label, hint and the option ID scripts use.
struct ChoiceOption {
    const char* label;
    const char* hint;
    const char* id;
};

/// The ten filter options, indexed by FilterEnum; the knob's default is eFilterCubic.
const std::vector<ChoiceOption>& filterOptions();

/// The four shutterOffset options, indexed by ShutterOffsetEnum.
const std::vector<ChoiceOption>& shutterOffsetOptions();

/// Source pixels each 1-D interpolation reads: 1 (impulse, box), 2 (bilinear, cubic) or 4.
int filterTaps(FilterEnum filter);

/**
 * @brief Whether the clamp knob affects the filter: only Keys, Simon, Rifman and Mitchell clamp
 * (to the range of the two centre samples). Cubic is never clamped, as in OFX's dispatch, and
 * Parzen and Notch never overshoot.
 **/
bool filterHonoursClamp(FilterEnum filter);

/**
 * @brief How far, in source pixels, a filter reads beyond the back-transformed window:
 * 0 (impulse, box), 0.5 (bilinear, cubic) or 1.5 (the 4-tap filters).
 **/
double filterRoIExpansion(FilterEnum filter);

///////////////////////////////////////////////////////////////////////////////
// 1-D filters. Ip, Ic, In, Ia are the samples at offsets -1, 0, +1, +2 from the
// sample at or left of the point, d in [0, 1] its distance from Ic.

inline double
clampValue(double I,
           double Ic,
           double In)
{
    double Imin = (std::min)(Ic, In);
    if (I < Imin) {
        return Imin;
    }
    double Imax = (std::max)(Ic, In);
    if (I > Imax) {
        return Imax;
    }
    return I;
}

inline double
filterLinear(double Ic,
             double In,
             double d)
{
    return Ic + d * (In - Ic);
}

inline double
filterCubic(double Ic,
            double In,
            double d,
            bool clamp)
{
    double I = Ic + d * d * ((-3 * Ic + 3 * In) + d * (2 * Ic - 2 * In));
    return clamp ? clampValue(I, Ic, In) : I;
}

/// Catmull-Rom (Mitchell-Netravali B = 0, C = 0.5).
inline double
filterKeys(double Ip,
           double Ic,
           double In,
           double Ia,
           double d,
           bool clamp)
{
    double I = Ic + d * ((-Ip + In) + d * ((2 * Ip - 5 * Ic + 4 * In - Ia) + d * (-Ip + 3 * Ic - 3 * In + Ia))) / 2;
    return clamp ? clampValue(I, Ic, In) : I;
}

/// B = 0, C = 0.75.
inline double
filterSimon(double Ip,
            double Ic,
            double In,
            double Ia,
            double d,
            bool clamp)
{
    double I = Ic + d * ((-3 * Ip + 3 * In) + d * ((6 * Ip - 9 * Ic + 6 * In - 3 * Ia) + d * (-3 * Ip + 5 * Ic - 5 * In + 3 * Ia))) / 4;
    return clamp ? clampValue(I, Ic, In) : I;
}

/// B = 0, C = 1.
inline double
filterRifman(double Ip,
             double Ic,
             double In,
             double Ia,
             double d,
             bool clamp)
{
    double I = Ic + d * ((-Ip + In) + d * ((2 * Ip - 2 * Ic + In - Ia) + d * (-Ip + Ic - In + Ia)));
    return clamp ? clampValue(I, Ic, In) : I;
}

/// B = C = 1/3.
inline double
filterMitchell(double Ip,
               double Ic,
               double In,
               double Ia,
               double d,
               bool clamp)
{
    double I = (Ip + 16 * Ic + In + d * ((-9 * Ip + 9 * In) + d * ((15 * Ip - 36 * Ic + 27 * In - 6 * Ia) + d * (-7 * Ip + 21 * Ic - 21 * In + 7 * Ia)))) / 18;
    return clamp ? clampValue(I, Ic, In) : I;
}

/// Cubic B-spline (B = 1, C = 0); never overshoots, so it takes no clamp.
inline double
filterParzen(double Ip,
             double Ic,
             double In,
             double Ia,
             double d)
{
    return (Ip + 4 * Ic + In + d * ((-3 * Ip + 3 * In) + d * ((3 * Ip - 6 * Ic + 3 * In) + d * (-Ip + 3 * Ic - 3 * In + Ia)))) / 6;
}

/// B = 1.5, C = -0.25; never overshoots, so it takes no clamp.
inline double
filterNotch(double Ip,
            double Ic,
            double In,
            double Ia,
            double d)
{
    return (Ip + 2 * Ic + In + d * ((-2 * Ip + 2 * In) + d * ((Ip - Ic - In + Ia)))) / 4;
}

/**
 * @brief The 1-D interpolation of the given filter between (Ip, Ic, In, Ia) at distance d from
 * Ic, applying clamp only where filterHonoursClamp() says so. Impulse picks Ic below d = 0.5
 * and In from it on, which is what the 2-D sampler's floor(x) choice amounts to; box, which
 * only differs from impulse when minifying, does the same.
 **/
double filter1D(FilterEnum filter, double Ip, double Ic, double In, double Ia, double d, bool clamp);

/**
 * @brief The weights filter1D() gives the four samples at d (clamp off): w[0..3] for Ip, Ic,
 * In, Ia. They sum to 1 for every filter.
 **/
void filterWeights(FilterEnum filter, double d, double w[4]);

///////////////////////////////////////////////////////////////////////////////
// 2-D sampling

/**
 * @brief A read-only float image as the resampler sees it: `data` points at pixel
 * (bounds.x1, bounds.y1), rows go upwards `rowStride` floats apart, and each pixel holds
 * nComps (1 to 4) interleaved floats. This is the layout of a float Natron Image, whose
 * rowStride is bounds.width() * nComps. A null data or empty bounds is a missing source, which
 * samples as zero everywhere.
 **/
struct SourceImage {
    const float* data;
    RectI bounds;
    int nComps;
    std::size_t rowStride;

    SourceImage()
        : data(0)
        , bounds()
        , nComps(0)
        , rowStride(0)
    {
    }

    SourceImage(const float* data_,
                const RectI& bounds_,
                int nComps_)
        : data(data_)
        , bounds(bounds_)
        , nComps(nComps_)
        , rowStride((std::size_t)bounds_.width() * (std::size_t)nComps_)
    {
    }

    bool isValid() const
    {
        return data && nComps > 0 && bounds.x2 > bounds.x1 && bounds.y2 > bounds.y1;
    }

    /// The pixel at (x, y), or null outside the bounds.
    const float* pixel(int x,
                       int y) const
    {
        if ((x < bounds.x1) || (x >= bounds.x2) || (y < bounds.y1) || (y >= bounds.y2)) {
            return 0;
        }
        return data + (std::size_t)(y - bounds.y1) * rowStride + (std::size_t)(x - bounds.x1) * (std::size_t)nComps;
    }
};

/**
 * @brief Samples src at (fx, fy) with the filter, no supersampling, writing src.nComps floats
 * to out. With blackOutside, taps outside the bounds read 0; otherwise their coordinates are
 * clamped into the bounds. Returns false (and writes zeros) when every tap is outside, i.e.
 * the point is in the black surround. clamp only acts on the filters filterHonoursClamp() names.
 **/
bool interpolate(FilterEnum filter, bool clamp, double fx, double fy, const SourceImage& src, bool blackOutside, float* out);

/**
 * @brief Whether interpolateSuper() supersamples for a back-transform with Jacobian
 * (Jxx = dfx/dx, Jxy = dfx/dy, Jyx = dfy/dx, Jyy = dfy/dy) and, if so, the number of
 * levels per axis: the samples per axis are 3^level, level in 0..4. Supersampling happens only
 * when the squared norm of a Jacobian column exceeds 1 (minification); a scale factor of
 * sqrt(3) or more starts level 1, and level 4 (81 samples per axis) is the maximum.
 **/
bool supersampleLevels(double Jxx, double Jxy, double Jyx, double Jyy, int* levelX, int* levelY);

/**
 * @brief Samples src at (fx, fy) for a destination pixel whose back-transform has the given
 * Jacobian, supersampling when minifying (supersampleLevels()): the centre is sampled with the
 * filter and the other supersamples bilinearly, unclamped, and all are averaged. A zero
 * Jacobian samples like interpolate(). The box filter instead integrates src over the bounding
 * box of the back-transformed pixel.
 **/
void interpolateSuper(FilterEnum filter, bool clamp, double fx, double fy, double Jxx, double Jxy, double Jyx, double Jyy, const SourceImage& src, bool blackOutside, float* out);

///////////////////////////////////////////////////////////////////////////////
// Motion blur

/// The number of matrices sampled over the shutter (or the directional blur's amount range).
const int kMotionBlurTransformCount = 1000;

/// The most samples per pixel: int(motionBlur * 40).
inline int
motionBlurMaxIterations(double motionBlur)
{
    return (int)(motionBlur * 40);
}

/// The samples always taken per pixel, evenly stratified: max(13, maxIterations / 3).
inline int
motionBlurMinSamples(double motionBlur)
{
    return (std::max)(13, motionBlurMaxIterations(motionBlur) / 3);
}

/// The standard error of the mean at which sampling stops: motionBlur / 1000 (float images).
inline double
motionBlurMaxError(double motionBlur)
{
    return motionBlur / 1000.;
}

/// The seed-th element of the base-2 van der Corput sequence, in [0, 1).
double vanDerCorput2(unsigned int seed);

/// The integer hash seeding a pixel's van der Corput sequence.
unsigned int motionBlurHash(unsigned int a);

/// The shutter interval [*tMin, *tMax] at time for the shutterOffset choice.
void shutterRange(double time, double shutter, ShutterOffsetEnum offset, double customOffset, double* tMin, double* tMax);

/**
 * @brief The node's canonical matrix at (time, amount): with invert false the inverse
 * (destination to source) matrix the render samples with, with invert true the forward one.
 * amount is the motion- or directional-blur blend factor, 1 for a plain render. Returns false
 * when no matrix exists at that time. A Transform node implements it with
 * TransformMath::inverseTransformCanonical(params at time, amount, invert).
 **/
typedef std::function<bool(double time, double amount, bool invert, TransformMath::Mat3* matrix)> CanonicalTransformFn;

/**
 * @brief The blur knobs of a Transform-like node. motionBlur is the quality knob (0 disables
 * every blur); shutter, shutterOffset and shutterCustomOffset the shutter; directionalBlur
 * blurs along the amount range [amountFrom, amountTo] instead of over time. fading is
 * DirBlur's fade exponent, 0 (no fading) for Transform. A node without blur knobs (Reformat)
 * keeps the defaults.
 **/
struct BlurSettings {
    double motionBlur;
    bool directionalBlur;
    double shutter;
    ShutterOffsetEnum shutterOffset;
    double shutterCustomOffset;
    double amountFrom;
    double amountTo;
    double fading;

    BlurSettings()
        : motionBlur(0.)
        , directionalBlur(false)
        , shutter(0.)
        , shutterOffset(eShutterOffsetStart)
        , shutterCustomOffset(0.)
        , amountFrom(0.)
        , amountTo(1.)
        , fading(0.)
    {
    }
};

/**
 * @brief The pixel-space inverse matrices a render samples, with their weights.
 * invTransforms map destination pixels to source pixels; alphas is empty for uniform weights
 * (motion blur) or holds one weight per matrix (directional blur). motionBlur is the quality to
 * render with: the knob value, or 0 when there is only one distinct matrix.
 **/
struct SamplingTransforms {
    std::vector<TransformMath::Mat3> invTransforms;
    std::vector<double> alphas;
    double motionBlur;

    SamplingTransforms()
        : invTransforms()
        , alphas()
        , motionBlur(0.)
    {
    }
};

/**
 * @brief Builds the render's matrices the way the OFX Transform3x3 render does when its source
 * is connected:
 *  - shutter != 0 and motionBlur != 0 (and not directional): kMotionBlurTransformCount matrices
 *    evenly over the shutter range, collapsed to one when they are all equal;
 *  - directionalBlur (whatever motionBlur is): kMotionBlurTransformCount matrices at amounts
 *    amountFrom + (amountTo - amountFrom) * (1 - (i + 1) / count), weighted by
 *    (1 - |amount| / amountTo)^fading (1 when fading <= 0);
 *  - otherwise the single matrix at amount 1, or a matrix sampling nothing if fn fails.
 * Pass shutter 0 for a directional blur, as the OFX node ignores the shutter then. Matrices are
 * converted with TransformMath::inverseToPixel() at render scale (sx, sy).
 **/
void buildSamplingTransforms(const CanonicalTransformFn& fn,
                             double time,
                             bool invert,
                             const BlurSettings& blur,
                             double sx,
                             double sy,
                             bool fielded,
                             double srcPar,
                             double dstPar,
                             SamplingTransforms* out);

/**
 * @brief Folds in the transform of a concatenated input (source to destination, pixel
 * coordinates, as getImage() hands it back): every inverse matrix is premultiplied by its
 * inverse. A singular input transform is ignored.
 **/
void concatenateInputTransform(const TransformMath::Mat3& inputTransformPixel, SamplingTransforms* transforms);

/**
 * @brief What resampleRow() needs, all borrowed: the matrices and weights must outlive it.
 **/
struct ResampleParams {
    const TransformMath::Mat3* invTransforms;
    const double* alphas;
    std::size_t count;
    FilterEnum filter;
    bool clamp;
    bool blackOutside;
    double motionBlur;

    ResampleParams()
        : invTransforms(0)
        , alphas(0)
        , count(0)
        , filter(eFilterCubic)
        , clamp(false)
        , blackOutside(true)
        , motionBlur(0.)
    {
    }
};

ResampleParams makeResampleParams(const SamplingTransforms& transforms, FilterEnum filter, bool clamp, bool blackOutside);

/**
 * @brief Resamples destination pixels [x1, x2) of row y into dst, src.nComps floats per pixel,
 * without mask or mix. Each pixel centre (x + 0.5, y + 0.5) is mapped through the inverse
 * matrix; a point at or behind infinity (z <= 0) is 0. Impulse samples the nearest pixel,
 * every other filter goes through interpolateSuper() with the Jacobian of the matrix at that
 * point (zeroed along an axis whose sample falls outside the source, or along both when
 * blackOutside and either does). With motionBlur != 0 and several matrices, each pixel is a
 * Monte Carlo average over the matrices: at least motionBlurMinSamples() stratified samples,
 * then more until the standard error falls under motionBlurMaxError() or
 * motionBlurMaxIterations() is reached, the sequence seeded by (x, y, motionBlur) so the result
 * does not depend on the row order or the thread.
 **/
void resampleRow(const ResampleParams& params, const SourceImage& src, int y, int x1, int x2, float* dst);

/**
 * @brief One motion-blurred pixel, as resampleRow() computes it, also reporting how many
 * samples it took. params.motionBlur must be non-zero.
 **/
void motionBlurPixel(const ResampleParams& params, const SourceImage& src, int x, int y, float* out, int* samplesTaken);

///////////////////////////////////////////////////////////////////////////////
// Regions. Rectangles are canonical; kOfxFlagInfiniteMin/Max mark infinite edges.

/**
 * @brief The bounding box of the quadrilateral the matrix makes of rect, ignoring corners at or
 * behind infinity; where an edge crosses z = 0 the box extends to infinity in that direction.
 * Empty (all zeros) when every corner is behind.
 **/
void transformRegionFromRect(const RectD& rect, const TransformMath::Mat3& transform, RectD* out);

/**
 * @brief The region rectFrom maps to through fn(·, ·, invert): one matrix at time, or, with
 * motion blur (shutter != 0 or directionalBlur, and motionBlur != 0), the union over the
 * shutter at every quarter frame (or over 9 amounts for a directional blur), grown by the
 * largest corner step between consecutive positions. isIdentity without motion blur returns
 * rectFrom unchanged; fn failing gives an infinite region.
 **/
void transformRegion(const CanonicalTransformFn& fn,
                     const RectD& rectFrom,
                     double time,
                     bool invert,
                     const BlurSettings& blur,
                     bool isIdentity,
                     RectD* rectTo);

/**
 * @brief Grows a non-empty RoD by one output pixel on each finite side when blackOutside, so
 * the edge fades to black. (sx, sy) is the render scale and par the output's.
 **/
void expandRoD(double par, double sx, double sy, bool blackOutside, RectD* rod);

/**
 * @brief Grows srcRoI, the back-transformed roi, by filterRoIExpansion() source pixels on each
 * finite side, then unions it with roi when masking or mixing (mix != 1), which read the
 * source under the output pixel. An empty roi gives srcRoI = roi.
 **/
void expandRoI(const RectD& roi, double par, double sx, double sy, FilterEnum filter, bool doMasking, double mix, RectD* srcRoI);

/**
 * @brief The knob state the region actions depend on. invert is the node's invert knob;
 * isIdentity what the node's own identity test says at that time (it stops the RoD from
 * growing, so an identity node keeps its source's RoD); doMasking whether a mask is connected
 * and applied; mix is only read when doMasking.
 **/
struct RegionParams {
    bool invert;
    BlurSettings blur;
    FilterEnum filter;
    bool blackOutside;
    bool isIdentity;
    bool doMasking;
    double mix;

    RegionParams()
        : invert(false)
        , blur()
        , filter(eFilterCubic)
        , blackOutside(true)
        , isIdentity(false)
        , doMasking(false)
        , mix(1.)
    {
    }
};

/**
 * @brief The output RoD from the source RoD, as the OFX Transform3x3 computes it: infinite in,
 * infinite out; empty in, empty out; mix 0 under a mask gives srcRoD; otherwise the forward
 * transformRegion() of srcRoD, expanded by expandRoD() unless identity, and unioned with
 * srcRoD when masking. dstPar is the output's PAR, (sx, sy) the render scale.
 **/
void getRegionOfDefinition(const CanonicalTransformFn& fn,
                           const RectD& srcRoD,
                           double time,
                           double dstPar,
                           double sx,
                           double sy,
                           const RegionParams& params,
                           RectD* rod);

/**
 * @brief The source RoI for an output roi, as the OFX Transform3x3 computes it: roi itself for
 * mix 0 under a mask; otherwise the back-transformed roi (transformRegion() with the render's
 * inverse matrix), expanded by expandRoI(). An infinite result is replaced by its intersection
 * with srcRoD, unioned with the project rectangle projectRect. srcPar is the source's PAR.
 **/
void getRegionOfInterest(const CanonicalTransformFn& fn,
                         const RectD& roi,
                         const RectD& srcRoD,
                         const RectD& projectRect,
                         double time,
                         double srcPar,
                         double sx,
                         double sy,
                         const RegionParams& params,
                         RectD* srcRoI);
} // namespace Resampler

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Image_Resampler_h
