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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <gtest/gtest.h>

#include "Engine/GpuWeight.h"
#include "Engine/Nodes/Filter/ErodeDilate.h"

NATRON_NAMESPACE_USING

namespace {
GpuWeight
erodeWeightAt(int size,
              unsigned int mipmapLevel)
{
    ErodeDilateParams params;

    params.sizeX = size;
    params.sizeY = size;

    PassContext context;
    context.scale = RenderScale::fromMipmapLevel(mipmapLevel);

    const std::optional<GpuWeight> weight = ErodeDilateOp<ErodeDilateMode::Erode>().gpuWeight(params, context);
    EXPECT_TRUE(weight.has_value());

    return weight.value_or(GpuWeight::point());
}
} // namespace

TEST(GpuWeight, DefaultFollowsOpKind)
{
    for (const OpKind kind : { OpKind::Point, OpKind::Composite, OpKind::Route }) {
        const GpuWeight weight = defaultGpuWeight(kind);
        EXPECT_EQ(weight.weightClass, GpuWeight::Class::Point);
        EXPECT_EQ(weight.weight, kGpuPointWeight);
    }
    for (const OpKind kind : { OpKind::Passes, OpKind::Resample }) {
        const GpuWeight weight = defaultGpuWeight(kind);
        EXPECT_EQ(weight.weightClass, GpuWeight::Class::Filter);
        EXPECT_EQ(weight.weight, kGpuFilterWeight);
    }
    EXPECT_TRUE(defaultGpuWeight(OpKind::Custom).isHeavy());
}

TEST(GpuWeight, ErodeWeightGrowsWithSize)
{
    const GpuWeight small = erodeWeightAt(8, 0);
    const GpuWeight large = erodeWeightAt(24, 0);

    EXPECT_EQ(small.weightClass, GpuWeight::Class::Filter);
    EXPECT_EQ(large.weightClass, GpuWeight::Class::Filter);
    EXPECT_EQ(small.weight, kGpuFilterWeight);
    EXPECT_GT(large.weight, small.weight);
}

TEST(GpuWeight, ErodeWeightFollowsRenderScale)
{
    EXPECT_LT(erodeWeightAt(24, 2).weight, erodeWeightAt(24, 0).weight);
}

TEST(GpuWeight, SizeScaledWeightIsCapped)
{
    EXPECT_EQ(gpuSizeScaledWeight(1e6).weight, kGpuFilterWeightCap);
    EXPECT_EQ(gpuSizeScaledWeight(0.).weight, kGpuFilterWeight);
}
