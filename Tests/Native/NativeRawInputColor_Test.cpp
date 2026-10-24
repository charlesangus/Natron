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

#include <array>
#include <cmath>
#include <string>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "NativePluginIdentity.h"
#include "RawTestSource.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/Raw/RawColorMath.h"
#include "Engine/Nodes/Raw/RawDemosaic.h"
#include "Engine/Nodes/Raw/RawInputColor.h"
#include "Engine/Project.h"
#include "Engine/ProjectColorManagement.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
const int kWidth = 32;
const int kHeight = 24;
const char* const kLinearRec709Space = "Linear Rec.709 (sRGB)";

const std::array<double, 9> kCamToXyz = { 0.6722, 0.1636, 0.1194,
                                          0.2654, 0.8410, -0.1064,
                                          0.0214, -0.1563, 1.1250 };
} // namespace

class NativeRawInputColorTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        registerRawTestSourceOnce();
        BaseTest::SetUp();
    }

    // A mosaic whose every photosite holds value(colour) through Demosaic and RawInputColor.
    NodePtr createChain(const std::array<float, 3>& cameraRgb)
    {
        NodePtr source = createNode(QString::fromUtf8(kRawTestSourceID));
        NodePtr demosaic = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWDEMOSAIC));
        NodePtr inputColor = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWINPUTCOLOR));

        EXPECT_TRUE(source && demosaic && inputColor);
        if (!source || !demosaic || !inputColor) {
            return NodePtr();
        }
        ImageMetadata metadata = rawTestMetadata("RGGB", 0, 0, eRawStagePrepared);
        RawMetadata::setCamToXyz(&metadata, kCamToXyz);
        const RawLayout layout = *RawLayout::fromPattern("RGGB");
        dynamic_cast<RawTestSource*>(source->getEffectInstance().get())->setRaw(RectI(0, 0, kWidth, kHeight), 1, [cameraRgb, layout](int x, int y, int) { return cameraRgb[layout.colourAt(x, y)]; }, metadata);
        connectNodes(source, demosaic, 0, true);
        connectNodes(demosaic, inputColor, 0, true);

        return inputColor;
    }

    ProjectPtr project() const
    {
        return getApp()->getProject();
    }

    void setWorkingSpace(const std::string& name)
    {
        KnobChoicePtr workingSpace = project()->getKnobByNameAndType<KnobChoice>("workingSpace");
        ASSERT_TRUE(bool(workingSpace));
        workingSpace->setValueFromID(name, 0);
        ASSERT_EQ(name, project()->getWorkingColorSpace());
    }

    // Camera RGB through the white-normalised camera matrix and the working space's inverse.
    std::array<double, 3> expectedWorking(const std::array<float, 3>& cameraRgb)
    {
        double workingToXyz[9];
        RawColorMath::Mat3 toXyz;
        EXPECT_TRUE(project()->getColorManagement()->getWorkingToXYZ(workingToXyz));
        for (int i = 0; i < 9; ++i) {
            toXyz[i] = workingToXyz[i];
        }
        const RawColorMath::Mat3 total = RawColorMath::multiply(*RawColorMath::invert(toXyz), *RawColorMath::whiteNormalisedCameraToXyz(kCamToXyz));

        return RawColorMath::apply(total, { cameraRgb[0], cameraRgb[1], cameraRgb[2] });
    }
};

TEST_F(NativeRawInputColorTest, Identity)
{
    expectNativeIdentity(PLUGINID_NATRON_RAWINPUTCOLOR, PLUGIN_MAJOR_NATRON_RAWINPUTCOLOR, "RawInputColor");

    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWINPUTCOLOR));
    ASSERT_TRUE(bool(node));
    EXPECT_EQ(eDataKindImage, node->getEffectInstance()->getInputDataKind(0));
    EXPECT_EQ(eDataKindImage, node->getEffectInstance()->getOutputDataKind());
}

// A white-balanced grey card is camera RGB (v, v, v); it must come out neutral in the working
// space, whatever the camera matrix.
TEST_F(NativeRawInputColorTest, AGreyCardEndsNeutralInTheWorkingSpace)
{
    NodePtr node = createChain({ 0.18f, 0.18f, 0.18f });
    ASSERT_TRUE(bool(node));

    RenderedPlane plane;
    std::string error;
    ASSERT_TRUE(renderRawTestPlane(node, RectI(0, 0, kWidth, kHeight), &plane, &error)) << error;
    ASSERT_EQ(4u, plane.channels.size());
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const float g = rawTestPlaneValue(plane, x, y, 1);
            ASSERT_GT(g, 0.f);
            EXPECT_NEAR(1.f, rawTestPlaneValue(plane, x, y, 0) / g, 1e-3f) << x << "," << y;
            EXPECT_NEAR(1.f, rawTestPlaneValue(plane, x, y, 2) / g, 1e-3f) << x << "," << y;
            EXPECT_EQ(1.f, rawTestPlaneValue(plane, x, y, 3));
        }
    }
}

TEST_F(NativeRawInputColorTest, ChangingTheWorkingSpaceReRenders)
{
    const std::string original = project()->getWorkingColorSpace();
    const std::array<float, 3> camera = { 0.4f, 0.25f, 0.1f };
    NodePtr node = createChain(camera);
    ASSERT_TRUE(bool(node));

    const RectI window(4, 4, 12, 10);
    RenderedPlane before;
    std::string error;
    ASSERT_TRUE(renderRawTestPlane(node, window, &before, &error)) << error;
    const std::array<double, 3> expectedBefore = expectedWorking(camera);
    for (int c = 0; c < 3; ++c) {
        EXPECT_NEAR(expectedBefore[c], rawTestPlaneValue(before, 6, 6, c), 1e-4) << "channel " << c << " in " << original;
    }
    const U64 hashBefore = node->getHashValue();

    if (original == kLinearRec709Space) {
        setWorkingSpace("ACEScg");
    } else {
        setWorkingSpace(kLinearRec709Space);
    }
    EXPECT_NE(hashBefore, node->getHashValue());
    RenderedPlane after;
    ASSERT_TRUE(renderRawTestPlane(node, window, &after, &error)) << error;
    const std::array<double, 3> expectedAfter = expectedWorking(camera);
    bool changed = false;
    for (int c = 0; c < 3; ++c) {
        EXPECT_NEAR(expectedAfter[c], rawTestPlaneValue(after, 6, 6, c), 1e-4) << "channel " << c << " in " << project()->getWorkingColorSpace();
        changed = changed || (std::fabs(rawTestPlaneValue(after, 6, 6, c) - rawTestPlaneValue(before, 6, 6, c)) > 1e-3f);
    }
    EXPECT_TRUE(changed);

    setWorkingSpace(original);
}

TEST_F(NativeRawInputColorTest, ACustomMatrixIsUsedAsGiven)
{
    const std::array<float, 3> camera = { 0.3f, 0.2f, 0.6f };
    NodePtr node = createChain(camera);
    ASSERT_TRUE(bool(node));
    EffectInstancePtr effect = node->getEffectInstance();
    effect->getKnobByNameAndType<KnobBool>(kRawInputColorParamProfileOverride)->setValue(true);
    effect->getKnobByNameAndType<KnobChoice>(kRawInputColorParamProfile)->setValueFromID(kRawInputColorProfileCustom, 0);
    const RawColorMath::Mat3 custom = RawColorMath::srgbToXyz();
    const char* const rows[3] = { kRawInputColorParamRow0, kRawInputColorParamRow1, kRawInputColorParamRow2 };
    for (int r = 0; r < 3; ++r) {
        KnobDoublePtr row = effect->getKnobByNameAndType<KnobDouble>(rows[r]);
        ASSERT_TRUE(bool(row));
        for (int c = 0; c < 3; ++c) {
            EXPECT_EQ(custom[r * 3 + c], row->getValue(c));
        }
    }

    double workingToXyz[9];
    ASSERT_TRUE(project()->getColorManagement()->getWorkingToXYZ(workingToXyz));
    RawColorMath::Mat3 toXyz;
    for (int i = 0; i < 9; ++i) {
        toXyz[i] = workingToXyz[i];
    }
    const RawColorMath::Vec3 expected = RawColorMath::apply(RawColorMath::multiply(*RawColorMath::invert(toXyz), custom), { camera[0], camera[1], camera[2] });
    RenderedPlane plane;
    std::string error;
    ASSERT_TRUE(renderRawTestPlane(node, RectI(4, 4, 12, 10), &plane, &error)) << error;
    for (int c = 0; c < 3; ++c) {
        EXPECT_NEAR(expected[c], rawTestPlaneValue(plane, 6, 6, c), 1e-4) << "channel " << c;
    }
}
