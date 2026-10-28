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

#ifndef Engine_Nodes_Filter_BokehKernel_h
#define Engine_Nodes_Filter_BokehKernel_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <limits>
#include <vector>

NATRON_NAMESPACE_ENTER

/**
 * @brief The bokeh kernels of the defocus filters: a disc, an n-blade aperture or an image, sized
 * to a radius in pixels, and the highlight boost that makes bright out-of-focus pixels bloom into
 * the kernel's shape. Engine-free, so the GPU host side uploads the very weights the CPU uses.
 *
 * A kernel spans [-haloX, haloX] x [-haloY, haloY] pixel offsets, y up. A defocus gathers
 * out(p) = sum over d of weight(d) * in(p - d), so a single bright pixel spreads into the kernel's
 * own shape, not its mirror image.
 **/
enum class BokehShape {
    Disc,
    Blades,
    Image
};

/**
 * @brief A second aperture that clips the kernel, as a lens barrel does off axis: a disc centred at
 * (offsetX, offsetY) of radius radius, in units of the kernel's radii, so that it squeezes with the
 * kernel's aspect. The default, an infinite radius, clips nothing; so does any disc holding the
 * whole shape.
 **/
struct BokehClip {
    double offsetX = 0.;
    double offsetY = 0.;
    double radius = std::numeric_limits<double>::infinity();

    bool operator==(const BokehClip& o) const
    {
        return (offsetX == o.offsetX) && (offsetY == o.offsetY) && (radius == o.radius);
    }

    bool operator!=(const BokehClip& o) const
    {
        return !(*this == o);
    }
};

struct BokehShapeParams {
    BokehShape shape = BokehShape::Disc;
    // Blades only: the aperture's blade count (3 or more), its rotation in degrees counter-clockwise
    // (at 0 a corner points up), and its roundness, 0 for straight blades and 1 for the disc.
    int blades = 6;
    double rotation = 0.;
    double roundness = 0.;
    // Clips Disc, Blades and Image kernels alike; a clip that leaves nothing is ignored.
    BokehClip clip;
    // The textures of Disc and Blades kernels, as fractions of the shape's radius. The inner
    // polygon, the shape scaled by innerSize, has brightness innerBrightness against the rim's 1;
    // innerFeather blends the two outward above 0.5 (to the rim at 1) and inward below it (from
    // the centre at 0). A catadioptric lens has a round hole of catadioptricSize. Spherical
    // aberration in [-1, 1] brightens the centre when negative and the rim when positive.
    // filterShape dissolves from a Gaussian at 0 to the flat shape at 1. The defaults leave the
    // kernel flat.
    double innerSize = 0.8;
    double innerFeather = 1.;
    double innerBrightness = 1.;
    bool catadioptric = false;
    double catadioptricSize = 0.3;
    double sphericalAberration = 0.;
    double filterShape = 1.;

    bool operator==(const BokehShapeParams& o) const
    {
        return (shape == o.shape) && (blades == o.blades) && (rotation == o.rotation) && (roundness == o.roundness)
            && (clip == o.clip) && (innerSize == o.innerSize) && (innerFeather == o.innerFeather)
            && (innerBrightness == o.innerBrightness) && (catadioptric == o.catadioptric)
            && (catadioptricSize == o.catadioptricSize) && (sphericalAberration == o.sphericalAberration)
            && (filterShape == o.filterShape);
    }

    bool operator!=(const BokehShapeParams& o) const
    {
        return !(*this == o);
    }
};

/**
 * @brief One run of equal, non-zero weights along a kernel row: offsets dx0 .. dx0 + length - 1 at
 * offset dy. A defocus sums each run's pixels once and weighs the sum, so a disc costs about one
 * run per row plus its anti-aliased edge.
 **/
struct BokehRun {
    int dy = 0;
    int dx0 = 0;
    int length = 0;
    float weight = 0.f;
};

class BokehKernel {
public:
    /// The single tap that leaves an image unchanged.
    BokehKernel();

    /**
     * @brief How far a kernel of radius radius, in pixels, reaches: the largest offset whose pixel
     * the shape can cover. 0 below half a pixel.
     **/
    static int extent(double radius) WARN_UNUSED_RETURN;

    /**
     * @brief Whether radii radiusX and radiusY give the single tap: both below half a pixel, or
     * either not a finite positive number.
     **/
    static bool isIdentityRadius(double radiusX, double radiusY) WARN_UNUSED_RETURN;

    /**
     * @brief The disc or n-blade kernel of shape (Image reads as the disc) filling an ellipse of
     * radii radiusX and radiusY, in pixels: each weight is the fraction of its pixel the shape and
     * its clip cover, supersampled along their edges and weighted by the shape's texture, and the
     * weights are normalised to sum to 1. A clip that leaves nothing gives the unclipped kernel,
     * and a texture that leaves nothing the flat one.
     **/
    static BokehKernel shaped(const BokehShapeParams& shape,
                              double radiusX,
                              double radiusY) WARN_UNUSED_RETURN;

    /**
     * @brief The kernel of an image of width x height values, row 0 at the bottom, its longer side
     * stretched over the diameter and centred, each weight the area average of the values its
     * pixel covers, then normalised to sum to 1. Negative and non-finite values count as 0. An
     * image that sums to 0, or an identity radius, gives the single tap; *empty is then set to
     * whether the image was to blame. clip then scales each weight by the fraction of its pixel
     * the clip covers before the normalisation, unless that leaves nothing.
     **/
    static BokehKernel fromImage(const float* values,
                                 int width,
                                 int height,
                                 double radiusX,
                                 double radiusY,
                                 bool* empty = nullptr,
                                 const BokehClip& clip = BokehClip()) WARN_UNUSED_RETURN;

    int haloX() const
    {
        return _haloX;
    }

    int haloY() const
    {
        return _haloY;
    }

    int width() const
    {
        return 2 * _haloX + 1;
    }

    int height() const
    {
        return 2 * _haloY + 1;
    }

    bool isIdentity() const WARN_UNUSED_RETURN;

    /// The weight at offset (dx, dy); 0 outside the kernel.
    float at(int dx,
             int dy) const WARN_UNUSED_RETURN;

    /// The weights row by row from dy = -haloY, each row from dx = -haloX.
    const std::vector<float>& weights() const
    {
        return _weights;
    }

    /// The non-zero weights as runs, row by row from dy = -haloY, each row from left to right.
    std::vector<BokehRun> runs() const WARN_UNUSED_RETURN;

private:
    BokehKernel(int haloX, int haloY, std::vector<float> weights);

    // Divides by the sum; false, leaving the kernel as it is, when the sum is not positive.
    bool normalise();

    int _haloX = 0;
    int _haloY = 0;
    std::vector<float> _weights;
};

/**
 * @brief The defocus gather over a plane of width x height floats, row by row. bokehPrefixRow()
 * fills a row's table, width + 1 sums from 0, a NaN or an infinity counting as 0; bokehGatherRow()
 * then writes dst[0 .. x2 - x1) for columns x1 .. x2 - 1 of row y from tables, the planes' row
 * tables one after another, and runs, a pixel outside the plane reading 0. acc holds x2 - x1
 * doubles of scratch.
 **/
void bokehPrefixRow(const float* src,
                    int width,
                    double* table);

void bokehGatherRow(const double* tables,
                    int width,
                    int height,
                    const std::vector<BokehRun>& runs,
                    int y,
                    int x1,
                    int x2,
                    double* acc,
                    float* dst);

/**
 * @brief The highlight boost: values above threshold are pushed away from it by gain, so that the
 * gather spreads them as bright bokeh; unboostHighlight() undoes it after the gather. Values at or
 * below threshold, and a gain of 1, are left as they are.
 **/
struct BokehHighlight {
    float threshold = 1.f;
    float gain = 1.f;

    bool isActive() const
    {
        return gain != 1.f;
    }

    float boost(float v) const
    {
        return (v > threshold) ? threshold + (v - threshold) * gain : v;
    }

    float unboost(float v) const
    {
        return (v > threshold) ? threshold + (v - threshold) / gain : v;
    }
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Filter_BokehKernel_h
