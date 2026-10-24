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

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "RawColorReference.h"
#include "RawTestSource.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/Raw/RawDemosaic.h"
#include "Engine/Nodes/Raw/RawInputColor.h"
#include "Engine/Project.h"
#include "Engine/ProjectColorManagement.h"

NATRON_NAMESPACE_USING

namespace Ref = RawColorReference;

namespace {
const int kWidth = 8;
const int kHeight = 2;

const Ref::Xy kSrgbR = { 0.64, 0.33 };
const Ref::Xy kSrgbG = { 0.30, 0.60 };
const Ref::Xy kSrgbB = { 0.15, 0.06 };

Ref::Mat3
srgbToXyz()
{
    return Ref::rgbToXyz(kSrgbR, kSrgbG, kSrgbB, Ref::kD65);
}

Ref::Mat3
adobeToXyz()
{
    return Ref::rgbToXyz(kSrgbR, { 0.21, 0.71 }, kSrgbB, Ref::kD65);
}

Ref::Mat3
rec2020ToXyz()
{
    return Ref::rgbToXyz({ 0.708, 0.292 }, { 0.170, 0.797 }, { 0.131, 0.046 }, Ref::kD65);
}

struct Gamut {
    const char* id;
    Ref::Mat3 toXyz;
};

std::vector<Gamut>
gamuts()
{
    return {
        { kRawInputColorProfileSrgb, srgbToXyz() },
        { kRawInputColorProfileAdobeRgb, adobeToXyz() },
        { kRawInputColorProfileLinearRec709, srgbToXyz() },
        { kRawInputColorProfileLinearRec2020, rec2020ToXyz() },
    };
}

double
clamp01(double v)
{
    return std::min(1., std::max(0., v));
}
} // namespace

class NativeRawInputColorClippingTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        registerRawTestSourceOnce();
        BaseTest::SetUp();
    }

    // A linear Rec2020 RGB source whose pixel (x, 0) is pixels[x], through RawInputColor.
    NodePtr createChain(const std::vector<Ref::Vec3>& pixels)
    {
        NodePtr source = createNode(QString::fromUtf8(kRawTestSourceID));
        NodePtr demosaic = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWDEMOSAIC));
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWINPUTCOLOR));

        EXPECT_TRUE(source && demosaic && node);
        if (!source || !demosaic || !node) {
            return NodePtr();
        }
        const std::vector<Ref::Vec3> values = pixels;
        dynamic_cast<RawTestSource*>(source->getEffectInstance().get())->setRaw(RectI(0, 0, kWidth, kHeight), 3, [values](int x, int, int c) { return (float)values[std::min<std::size_t>(x, values.size() - 1)][c]; }, rawTestMetadata(kRawCfaPatternNone, 0, 0, eRawStagePrepared));
        connectNodes(source, demosaic, 0, true);
        connectNodes(demosaic, node, 0, true);
        EffectInstancePtr effect = node->getEffectInstance();
        effect->getKnobByNameAndType<KnobBool>(kRawInputColorParamProfileOverride)->setValue(true);
        effect->getKnobByNameAndType<KnobChoice>(kRawInputColorParamProfile)->setValueFromID(kRawInputColorProfileLinearRec2020, 0);

        return node;
    }

    static void setChoice(const NodePtr& node,
                          const char* knob,
                          const char* id)
    {
        node->getEffectInstance()->getKnobByNameAndType<KnobChoice>(knob)->setValueFromID(id, 0);
    }

    Ref::Mat3 xyzToWorking()
    {
        double workingToXyz[9];
        Ref::Mat3 m;
        EXPECT_TRUE(getApp()->getProject()->getColorManagement()->getWorkingToXYZ(workingToXyz));
        for (int i = 0; i < 9; ++i) {
            m[i] = workingToXyz[i];
        }

        return Ref::invert(m);
    }

    static std::vector<Ref::Vec3> render(const NodePtr& node,
                                         std::size_t count)
    {
        RenderedPlane plane;
        std::string error;
        std::vector<Ref::Vec3> out;
        EXPECT_TRUE(renderRawTestPlane(node, RectI(0, 0, kWidth, kHeight), &plane, &error)) << error;
        if (!error.empty()) {
            return out;
        }
        for (std::size_t x = 0; x < count; ++x) {
            out.push_back({ rawTestPlaneValue(plane, (int)x, 0, 0), rawTestPlaneValue(plane, (int)x, 0, 1), rawTestPlaneValue(plane, (int)x, 0, 2) });
        }

        return out;
    }

    static void expectNear(const std::vector<Ref::Vec3>& expected,
                           const std::vector<Ref::Vec3>& actual,
                           const std::string& what)
    {
        ASSERT_EQ(expected.size(), actual.size()) << what;
        for (std::size_t x = 0; x < expected.size(); ++x) {
            for (int c = 0; c < 3; ++c) {
                EXPECT_NEAR(expected[x][c], actual[x][c], 2e-4 + 5e-4 * std::fabs(expected[x][c])) << what << ", pixel " << x << ", channel " << c;
            }
        }
    }
};

// process_cmatrix_proper(): to the gamut's RGB, clipped to [0, 1], then on to the working space.
TEST_F(NativeRawInputColorClippingTest, EachGamutClipsOnlyWhatIsOutside)
{
    const Ref::Mat3 toWorking = xyzToWorking();
    const Ref::Mat3 fromInput = rec2020ToXyz();
    // In every gamut: sRGB colours, sRGB being inside the other three.
    std::vector<Ref::Vec3> inside;
    for (const Ref::Vec3& srgb : { Ref::Vec3 { 0.18, 0.18, 0.18 }, Ref::Vec3 { 0.3, 0.5, 0.2 }, Ref::Vec3 { 0.8, 0.1, 0.4 } }) {
        inside.push_back(Ref::applyMatrix(Ref::multiply(Ref::invert(fromInput), srgbToXyz()), srgb));
    }
    // Outside every gamut: above 1 and below 0 in Rec2020 itself, and a green above 1.
    const std::vector<Ref::Vec3> outside = { { 1.4, 0.2, -0.1 }, { 0.05, 1.2, 0.05 }, { -0.2, 0.3, 1.6 } };

    for (const Gamut& gamut : gamuts()) {
        SCOPED_TRACE(gamut.id);
        const Ref::Mat3 toGamut = Ref::multiply(Ref::invert(gamut.toXyz), fromInput);
        std::vector<Ref::Vec3> pixels = inside;
        std::vector<Ref::Vec3> expected;
        for (const Ref::Vec3& v : inside) {
            expected.push_back(Ref::applyMatrix(Ref::multiply(toWorking, fromInput), v));
        }
        for (const Ref::Vec3& v : outside) {
            const Ref::Vec3 g = Ref::applyMatrix(toGamut, v);
            ASSERT_TRUE((g[0] < 0.) || (g[1] < 0.) || (g[2] < 0.) || (g[0] > 1.) || (g[1] > 1.) || (g[2] > 1.));
            pixels.push_back(v);
            expected.push_back(Ref::applyMatrix(Ref::multiply(toWorking, gamut.toXyz), { clamp01(g[0]), clamp01(g[1]), clamp01(g[2]) }));
        }
        NodePtr node = createChain(pixels);
        ASSERT_TRUE(bool(node));
        setChoice(node, kRawInputColorParamGamutClipping, gamut.id);
        expectNear(expected, render(node, pixels.size()), gamut.id);
    }
}

TEST_F(NativeRawInputColorClippingTest, OffLeavesValuesOutsideTheGamuts)
{
    const std::vector<Ref::Vec3> pixels = { { 1.4, 0.2, -0.1 }, { -0.2, 0.3, 1.6 } };
    const Ref::Mat3 total = Ref::multiply(xyzToWorking(), rec2020ToXyz());
    std::vector<Ref::Vec3> expected;

    for (const Ref::Vec3& v : pixels) {
        expected.push_back(Ref::applyMatrix(total, v));
    }
    NodePtr node = createChain(pixels);
    ASSERT_TRUE(bool(node));
    setChoice(node, kRawInputColorParamGamutClipping, kRawInputColorGamutOff);
    expectNear(expected, render(node, pixels.size()), "off");
}

// Another working profile moves the output from the project's working space by the matrix
// between the two, then writes the profile's tone curve.
TEST_F(NativeRawInputColorClippingTest, WorkingProfileConvertsTheOutput)
{
    const std::vector<Ref::Vec3> pixels = { { 0.18, 0.18, 0.18 }, { 0.3, 0.5, 0.2 }, { 0.7, 0.1, 0.05 }, { 1.4, 0.2, -0.1 } };
    NodePtr node = createChain(pixels);
    ASSERT_TRUE(bool(node));
    const std::vector<Ref::Vec3> project = render(node, pixels.size());
    ASSERT_EQ(pixels.size(), project.size());
    EXPECT_FALSE(node->hasPersistentMessage());

    const Ref::Mat3 projectToProPhoto = Ref::multiply(Ref::invert(Ref::multiply(Ref::bradford(Ref::xyzOf(Ref::kD50), Ref::kD65Xyz),
                                                                                Ref::rgbToXyz({ 0.734699, 0.265301 }, { 0.159597, 0.840403 }, { 0.036598, 0.000105 }, Ref::kD50))),
                                                      Ref::invert(xyzToWorking()));
    std::vector<Ref::Vec3> expected;
    for (const Ref::Vec3& v : project) {
        expected.push_back(Ref::applyMatrix(projectToProPhoto, v));
    }
    setChoice(node, kRawInputColorParamWorkingProfile, kRawInputColorProfileLinearProPhoto);
    expectNear(expected, render(node, pixels.size()), "linear ProPhoto");
    ASSERT_TRUE(node->hasPersistentMessage());
    QString message;
    int type = 0;
    node->getPersistentMessage(&message, &type, false);
    EXPECT_EQ((int)eMessageTypeInfo, type);
    EXPECT_TRUE(message.contains(QString::fromUtf8("Linear ProPhoto RGB"))) << message.toStdString();

    // Inside [0, 1] only: above 1 the curve is darktable's fitted extrapolation.
    const Ref::Mat3 projectToSrgb = Ref::multiply(Ref::invert(srgbToXyz()), Ref::invert(xyzToWorking()));
    expected.clear();
    for (std::size_t x = 0; x < 3; ++x) {
        const Ref::Vec3 linear = Ref::applyMatrix(projectToSrgb, project[x]);
        expected.push_back({ Ref::srgbEncode(linear[0]), Ref::srgbEncode(linear[1]), Ref::srgbEncode(linear[2]) });
    }
    setChoice(node, kRawInputColorParamWorkingProfile, kRawInputColorProfileSrgb);
    std::vector<Ref::Vec3> srgb = render(node, pixels.size());
    srgb.resize(3);
    expectNear(expected, srgb, "sRGB");

    setChoice(node, kRawInputColorParamWorkingProfile, kRawInputColorWorkingProject);
    expectNear(project, render(node, pixels.size()), "project");
    EXPECT_FALSE(node->hasPersistentMessage());
}
