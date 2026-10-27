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

#ifndef Engine_GpuWeight_h
#define Engine_GpuWeight_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <algorithm>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/ImagePipelineSpec.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The weight of a Point node, the unit the other weights are measured in.
 **/
constexpr double kGpuPointWeight = 1.;

/**
 * @brief The weight of a Filter node whose cost does not depend on a size: about four point ops,
 * from lone Blur and Transform regions breaking even or winning on the GPU.
 **/
constexpr double kGpuFilterWeight = 4.;

/**
 * @brief The size in pixels, at the rendered scale, up to which a size-scaled Filter node weighs
 * kGpuFilterWeight; beyond it the weight grows in proportion to the size.
 **/
constexpr double kGpuFilterWeightRefSize = 8.;

/**
 * @brief The largest weight a size-scaled Filter node gets.
 **/
constexpr double kGpuFilterWeightCap = 32.;

/**
 * @brief The summed weight from which a region with no Heavy member renders on the GPU. Measured on a
 * Ryzen 7 9800X3D against a Radeon AI PRO R9700 (release): source plus two Grades (3) ran ~10% slower
 * on the GPU, three Grades (4) broke even, six won by 20%.
 **/
constexpr double kGpuRegionMinWeight = 4.;

/**
 * @brief The pixel count, at the rendered scale, below which a region stays on the CPU whatever
 * it weighs: the GPU's fixed costs outweigh what so few pixels save.
 **/
constexpr double kGpuRegionMinPixels = 256. * 256.;

/**
 * @brief How much a node weighs when GPU placement decides where a region of nodes renders. A
 * Point node is kGpuPointWeight. A Filter node carries a weight, kGpuFilterWeight unless its
 * cost follows a size. A Heavy node sends the region it belongs to to the GPU whatever the other
 * members weigh.
 **/
struct GpuWeight {
    enum class Class : unsigned char {
        Point,
        Filter,
        Heavy
    };

    Class weightClass = Class::Point;
    // What the node adds to the sum over its region. Unused by Heavy.
    double weight = kGpuPointWeight;

    static GpuWeight point()
    {
        return GpuWeight { Class::Point, kGpuPointWeight };
    }

    static GpuWeight filter(double weight = kGpuFilterWeight)
    {
        return GpuWeight { Class::Filter, weight };
    }

    static GpuWeight heavy()
    {
        return GpuWeight { Class::Heavy, 0. };
    }

    bool isHeavy() const
    {
        return weightClass == Class::Heavy;
    }
};

/**
 * @brief The Filter weight of a node whose cost grows with a size of sizePixels at the rendered
 * scale: kGpuFilterWeight up to kGpuFilterWeightRefSize, proportionally more beyond, capped at
 * kGpuFilterWeightCap.
 **/
inline GpuWeight
gpuSizeScaledWeight(double sizePixels)
{
    const double scaled = kGpuFilterWeight * std::max(1., sizePixels / kGpuFilterWeightRefSize);

    return GpuWeight::filter(std::min(kGpuFilterWeightCap, scaled));
}

/**
 * @brief The weight of a node whose image op is of kind, when the node declares none of its own.
 **/
inline GpuWeight
defaultGpuWeight(OpKind kind)
{
    switch (kind) {
    case OpKind::Point:
    case OpKind::Composite:
    case OpKind::Route:
        return GpuWeight::point();
    case OpKind::Passes:
    case OpKind::Resample:
        return GpuWeight::filter();
    case OpKind::Custom:
        break;
    }

    return GpuWeight::heavy();
}

NATRON_NAMESPACE_EXIT

#endif // Engine_GpuWeight_h
