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

/*
 * The filters in this file are ports of CImg's CImg<float>::deriche(), CImg<float>::vanvliet()
 * and CImg<float>::boxfilter() (CImg 2.9.9, as bundled with openfx-misc), by David Tschumperle
 * and contributors, <http://cimg.eu>. CImg is distributed under the CeCILL-C licence, which is
 * compatible with the GNU GPL.
 */

#ifndef Engine_Nodes_Filter_BlurKernels_h
#define Engine_Nodes_Filter_BlurKernels_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <vector>

#ifdef NATRON_HAVE_GPU
namespace gpu {
struct GpuLineFilter;
}
#endif

NATRON_NAMESPACE_ENTER

/**
 * @brief Separable 1-D blur and derivative filters over a strided float line. The IIR and box
 * filters are ports of the CImg functions named above; the FIR Gaussian has no CImg counterpart.
 *
 * Each filter runs in place over `n` samples at `data[0]`, `data[stride]`, ...,
 * `data[(n - 1) * stride]`. A row of an interleaved image is `stride = nComps`, a column is
 * `stride = rowElements`. The result depends on the whole line (IIR filters, and the
 * boundary condition at both ends), so callers must filter whole lines, never tiles of them.
 *
 * `neumann` is CImg's `boundary_conditions` bool: false is Dirichlet (black outside the line),
 * true is Neumann (the end samples repeat).
 **/
namespace BlurKernels {
/// The order of CImgBlur's `filter` choice.
enum Filter {
    eFilterQuasiGaussian = 0,
    eFilterGaussian,
    eFilterBox,
    eFilterTriangle,
    eFilterQuadratic,
    eFilterFIRGaussian
};

/// Per-thread working memory for LineFilter::apply. Grown on demand and reusable across lines.
struct LineScratch {
    std::vector<double> recursive;
    std::vector<float> window;
    std::vector<float> padded;
};

/**
 * @brief One immutable line filter: coefficients are computed once at construction, then
 * apply() may be called concurrently from any number of threads, each with its own scratch.
 **/
class LineFilter {
public:
    /// The identity filter.
    LineFilter();

    /**
     * @brief CImg<float>::deriche(sigma, order, axis, neumann). Orders 0, 1, 2. sigma is in
     * pixels; negative values are treated as 0. Order 0 with sigma < 0.1 is the identity, and
     * an order above 2 (where CImg throws) gives the identity.
     **/
    static LineFilter deriche(float sigma, unsigned int order, bool neumann);

    /**
     * @brief CImg<float>::vanvliet(sigma, order, axis, neumann), the Young / van Vliet
     * recursive Gaussian with Triggs boundary conditions. Orders 0 to 3. Below sigma 0.5 it
     * is deriche(sigma, order, neumann), as in CImg.
     **/
    static LineFilter vanVliet(float sigma, unsigned int order, bool neumann);

    /**
     * @brief CImg<float>::boxfilter(boxSize, order, axis, neumann, iterations): a box of
     * (possibly fractional) width boxSize applied `iterations` times (1 box, 2 triangle,
     * 3 quadratic), then a centred difference of the given order (0, 1 or 2). boxSize is in
     * pixels; negative values are treated as 0. boxSize 0, or boxSize <= 1 with order 0, is
     * the identity.
     **/
    static LineFilter box(float boxSize, int order, bool neumann, unsigned int iterations);

    /**
     * @brief A Gaussian with finite support: weights exp(-k^2 / (2 sigma^2)) for |k| <= radius,
     * radius = firGaussianRadius(sigma), computed in double and normalised to sum 1, then stored
     * and accumulated in float. Outside the line, samples are the end sample (neumann) or zero.
     * sigma below 0.1 gives no smoothing. An order of 1 or 2 then takes the centred difference
     * box() takes; a higher order gives the identity.
     **/
    static LineFilter firGaussian(double sigma, unsigned int order, bool neumann);

    /// ceil(3 sigma), or 0 when sigma < 0.1: the number of samples firGaussian reads on each side.
    static int firGaussianRadius(double sigma);

    /**
     * @brief The weights firGaussian(sigma, ...) applies to offsets 0 to firGaussianRadius(sigma),
     * the kernel being symmetric: {1} when sigma gives no smoothing.
     **/
    static std::vector<float> firGaussianWeights(double sigma);

    /**
     * @brief The line filter for `filter`: deriche for QuasiGaussian, vanVliet for Gaussian,
     * box with 1/2/3 iterations for Box/Triangle/Quadratic, firGaussian for FIRGaussian. `size`
     * is sigma for the Gaussians and the box width for the others, both in pixels.
     **/
    static LineFilter forFilter(Filter filter, float size, unsigned int order, bool neumann);

    enum Kind {
        eKindIdentity,
        eKindDeriche,
        eKindVanVliet,
        eKindBox,
        eKindFIRGaussian
    };

    /**
     * @brief What apply() runs, for a device that runs the same recursion: the kind, the order,
     * the boundary condition, and the coefficients apply() reads, as computed at construction.
     **/
    Kind kind() const { return _kind; }
    unsigned int order() const { return _order; }
    bool neumann() const { return _neumann; }

    /// Deriche: a0, a1, a2, a3, b1, b2, coefp, coefn.
    void dericheCoefficients(double out[8]) const;

    /// Van Vliet: [B, -b1, -b2, -b3], then the 3x3 Triggs matrix row by row.
    void vanVlietCoefficients(double filter[4], double triggs[9]) const;

    /// Box: the width and the number of box passes.
    float boxSize() const { return _boxSize; }
    unsigned int boxIterations() const { return _iterations; }

    /// The column count applyColumns() works on at once.
    static const int kColumnBlock = 64;

    /// True when apply() leaves every line unchanged.
    bool isIdentity() const { return _kind == eKindIdentity; }

    /// Filters one line in place. n <= 0 is a no-op.
    void apply(float* data, int n, std::ptrdiff_t stride, LineScratch& scratch) const;

    /// apply() with a scratch allocated for this call only.
    void apply(float* data, int n, std::ptrdiff_t stride) const;

    /**
     * @brief apply() over the `count` lines starting at data, data + 1, ..., data + count - 1,
     * each of n samples `stride` apart: adjacent columns of a row-major plane. Equal to calling
     * apply() on each, but the FIR Gaussian walks the columns together, kColumnBlock at a time,
     * reading each row once per block instead of once per column.
     **/
    void applyColumns(float* data, int n, std::ptrdiff_t stride, int count, LineScratch& scratch) const;

private:
    void applyDeriche(float* data, int n, std::ptrdiff_t stride, LineScratch& scratch) const;
    void applyVanVliet(float* data, int n, std::ptrdiff_t stride) const;
    void applyBox(float* data, int n, std::ptrdiff_t stride, LineScratch& scratch) const;
    void applyFIRGaussian(float* data, int n, std::ptrdiff_t stride, LineScratch& scratch) const;
    void applyFIRGaussianColumns(float* data, int n, std::ptrdiff_t stride, int count, LineScratch& scratch) const;

    Kind _kind;
    unsigned int _order;
    bool _neumann;

    double _a0, _a1, _a2, _a3, _b1, _b2, _coefp, _coefn;

    // Van Vliet: [B, -b1, -b2, -b3] and the 3x3 Triggs boundary matrix.
    double _filter[4];
    double _triggs[9];

    float _boxSize;
    unsigned int _iterations;

    // FIR Gaussian: weights for offsets 0 to _radius; the kernel is symmetric.
    std::vector<float> _weights;
    int _radius;
};

#ifdef NATRON_HAVE_GPU
/**
 * @brief The device form of `filter`, with its order and boundary condition. The FIR Gaussian has
 * none and gives the identity; gpu::recordLineBlur refuses an order above 1.
 **/
gpu::GpuLineFilter toGpuLineFilter(const LineFilter& filter);
#endif

/// One-shot wrappers: build the filter and apply it to a single line.
void deriche(float* data, int n, std::ptrdiff_t stride, float sigma, unsigned int order, bool neumann);
void vanVliet(float* data, int n, std::ptrdiff_t stride, float sigma, unsigned int order, bool neumann);
void boxFilter(float* data,
               int n,
               std::ptrdiff_t stride,
               float boxSize,
               int order,
               bool neumann,
               unsigned int iterations);
} // namespace BlurKernels

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Filter_BlurKernels_h
