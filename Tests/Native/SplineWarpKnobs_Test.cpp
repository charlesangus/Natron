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

#include <Python.h>

#include "Global/Macros.h"

#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/Knob.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarp.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpInteract.h"
#include "Engine/Project.h"

NATRON_NAMESPACE_USING

namespace {
struct KnobRow {
    const char* name;
    const char* label;
    const char* page;
    // One value per dimension, or a single value for every dimension.
    std::vector<double> defaults;
};

const char* const kControls = "Controls";
const char* const kTransform = "Transform";
const char* const kRender = "Render";

// The audit table of the Nuke port, then the Natron extras it keeps. The mask and Resampler
// knobs are shared with every native node, so their labels are the shared ones.
const std::vector<KnobRow>&
auditRows()
{
    static const std::vector<KnobRow> rows = {
        { kSplineWarpParamCookieCutInput, "Cookie Cut Input", kControls, { 0. } },
        { kSplineWarpParamCropToFormat, "Crop To Format", kControls, { 1. } },
        { kSplineWarpParamBoundaryBBox, "Boundary BBox", kControls, { 1. } },
        { kSplineWarpUIParamOutputWarped, "Output Warped", kControls, { 1. } },
        { kSplineWarpParamAmount, "Amount", kControls, { 1. } },
        { kSplineWarpParamLayerAmount, "Layer Amount", kControls, { 1. } },
        { kSplineWarpParamLinkAmount, "Link Amount", kControls, { 1. } },
        { "translate", "Translate", kTransform, { 0., 0. } },
        { "rotate", "Rotate", kTransform, { 0. } },
        { "scale", "Scale", kTransform, { 1., 1. } },
        { "uniform", "Uniform", kTransform, { 1. } },
        { "skewx", "Skew X", kTransform, { 0. } },
        { "skewy", "Skew Y", kTransform, { 0. } },
        { "skewOrder", "Skew Order", kTransform, { 0. } },
        // Normalized: a new curve's centre is set to the Source format's centre.
        { "center", "Center", kTransform, { 0.5, 0.5 } },
        { "extraMatrix", "Extra Matrix", kTransform, { 1., 0., 0., 0., 1., 0., 0., 0., 1. } },
        { kSplineWarpParamCurveResolution, "Curve Resolution", kRender, { 4. } },
        { kSplineWarpParamBoundaryCurveResolution, "Boundary Curve Resolution", kRender, { 4. } },
        { kSplineWarpParamPreviewResolution, "Preview Resolution", kRender, { 25. } },
        { kSplineWarpParamSolver, "Solver", kRender, { 0. } },
        { kResamplerParamFilterType, "Filter", kRender, { (double)Resampler::eFilterCubic } },
        { kOfxMaskInvertParamName, "Invert Mask", kControls, { 0. } },
        { kOfxMixParamName, "Mix", kControls, { 1. } },

        { kSplineWarpParamInvert, "Invert", kControls, { 0. } },
        { kSplineWarpParamSmoothness, "Smoothness", kRender, { 0. } },
        { kSplineWarpParamFieldResolution, "Field Resolution", kRender, { 8. } },
        { kResamplerParamFilterClamp, "Clamp", kRender, { 0. } },
        { kResamplerParamFilterBlackOutside, kResamplerParamFilterBlackOutsideLabel, kRender, { 1. } },
        { kSplineWarpParamBBox, "BBox", kRender, { 0. } },
        { kSplineWarpParamMotionBlur, "Motion Blur", kRender, { 0. } },
        { kSplineWarpParamShutter, "Shutter", kRender, { 0.5 } },
        { kResamplerParamShutterOffset, "Shutter Offset", kRender, { (double)Resampler::eShutterOffsetStart } },
        { kResamplerParamShutterCustomOffset, kResamplerParamShutterCustomOffsetLabel, kRender, { 0. } },
    };

    return rows;
}

bool
defaultAt(const KnobIPtr& knob,
          int dimension,
          double* value)
{
    if (KnobDoubleBasePtr d = std::dynamic_pointer_cast<KnobDoubleBase>(knob)) {
        *value = d->getDefaultValue(dimension);

        return true;
    }
    if (KnobIntBasePtr i = std::dynamic_pointer_cast<KnobIntBase>(knob)) {
        *value = i->getDefaultValue(dimension);

        return true;
    }
    if (KnobBoolBasePtr b = std::dynamic_pointer_cast<KnobBoolBase>(knob)) {
        *value = b->getDefaultValue(dimension) ? 1. : 0.;

        return true;
    }

    return false;
}
} // anonymous namespace

class SplineWarpKnobsTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }
};

TEST_F(SplineWarpKnobsTest, EveryAuditedKnobHasItsNameLabelDefaultAndPage)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_SPLINEWARP), PLUGIN_MAJOR_NATRON_SPLINEWARP);
    ASSERT_TRUE(bool(node));

    for (const KnobRow& row : auditRows()) {
        KnobIPtr knob = node->getKnobByName(row.name);
        ASSERT_TRUE(bool(knob)) << row.name;
        EXPECT_EQ(std::string(row.label), knob->getLabel()) << row.name;
        KnobPagePtr page = knob->getTopLevelPage();
        ASSERT_TRUE(bool(page)) << row.name;
        EXPECT_EQ(std::string(row.page), page->getLabel()) << row.name;

        const int dims = knob->getDimension();
        if (row.defaults.size() > 1) {
            ASSERT_EQ((int)row.defaults.size(), dims) << row.name;
        }
        for (int d = 0; d < dims; ++d) {
            double value = 0.;
            ASSERT_TRUE(defaultAt(knob, d, &value)) << row.name;
            const double expected = row.defaults.size() > 1 ? row.defaults[d] : row.defaults[0];
            EXPECT_DOUBLE_EQ(expected, value) << row.name << "[" << d << "]";
        }
    }

    KnobDoublePtr center = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName("center"));
    ASSERT_TRUE(bool(center));
    EXPECT_TRUE(center->getDefaultValuesAreNormalized());

    KnobChannelSelectPtr outputMask = std::dynamic_pointer_cast<KnobChannelSelect>(node->getKnobByName(kSplineWarpParamOutputMask));
    ASSERT_TRUE(bool(outputMask));
    EXPECT_EQ(std::string("Output Mask"), outputMask->getLabel());
    KnobPagePtr outputMaskPage = outputMask->getTopLevelPage();
    ASSERT_TRUE(bool(outputMaskPage));
    EXPECT_EQ(std::string(kControls), outputMaskPage->getLabel());
    EXPECT_EQ(std::string(kSplineWarpOutputMaskLayer) + "." + kSplineWarpOutputMaskChannel, outputMask->get());
}
