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

#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/Raw/RawColorMath.h"
#include "Engine/Nodes/Raw/RawDemosaic.h"
#include "Engine/Nodes/Raw/RawTemperature.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
const int kWidth = 40;
const int kHeight = 36;

// A camera RGB to XYZ (D65) matrix of the kind a raw file carries: invertible, not sRGB.
const std::array<double, 9> kCamToXyz = { 0.6722, 0.1636, 0.1194,
                                          0.2654, 0.8410, -0.1064,
                                          0.0214, -0.1563, 1.1250 };

float
rampValue(int x,
          int y,
          int /*c*/)
{
    return 0.05f + 0.01f * (float)x + 0.005f * (float)y;
}
} // namespace

class NativeRawTemperatureTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        registerRawTestSourceOnce();
        BaseTest::SetUp();
    }

    NodePtr createSource(int nComps,
                         const RawTestSource::ValueFn& value,
                         const ImageMetadata& metadata)
    {
        NodePtr source = createNode(QString::fromUtf8(kRawTestSourceID));

        EXPECT_TRUE(bool(source));
        if (source) {
            dynamic_cast<RawTestSource*>(source->getEffectInstance().get())->setRaw(RectI(0, 0, kWidth, kHeight), nComps, value, metadata);
        }

        return source;
    }

    NodePtr createAfter(const char* pluginID,
                        const NodePtr& input)
    {
        NodePtr node = createNode(QString::fromUtf8(pluginID));

        EXPECT_TRUE(bool(node));
        if (node && input) {
            connectNodes(input, node, 0, true);
        }

        return node;
    }

    static ImageMetadata outputMetadata(const NodePtr& node)
    {
        NativeEffectBase* effect = dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());

        return effect ? effect->getOutputMetadata(1., ViewIdx(0)) : ImageMetadata();
    }

    // Every photosite of the RGGB ramp mosaic is multiplied by the coefficient of its colour.
    void expectMultiplied(const NodePtr& node,
                          const std::array<double, 3>& coeffs)
    {
        const RawLayout layout = *RawLayout::fromPattern("RGGB");
        RenderedPlane plane;
        std::string error;

        ASSERT_TRUE(renderRawTestPlane(node, RectI(0, 0, kWidth, kHeight), &plane, &error)) << error;
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                const float coeff = (float)coeffs[layout.colourAt(x, y)];
                ASSERT_EQ(rampValue(x, y, 0) * coeff, rawTestPlaneValue(plane, x, y, 0)) << x << "," << y;
            }
        }
    }
};

TEST_F(NativeRawTemperatureTest, Identity)
{
    expectNativeIdentity(PLUGINID_NATRON_RAWTEMPERATURE, PLUGIN_MAJOR_NATRON_RAWTEMPERATURE, "RawTemperature");

    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWTEMPERATURE));
    ASSERT_TRUE(bool(node));
    EXPECT_EQ(eDataKindRaw, node->getEffectInstance()->getInputDataKind(0));
    EXPECT_EQ(eDataKindRaw, node->getEffectInstance()->getOutputDataKind());
}

TEST_F(NativeRawTemperatureTest, AsShotReproducesTheFileCoefficients)
{
    ImageMetadata metadata = rawTestMetadata("RGGB", 0, 0, eRawStagePrepared);
    RawMetadata::setWbCoeffs(&metadata, { 2.4, 1.2, 1.8 });
    NodePtr source = createSource(1, rampValue, metadata);
    NodePtr node = createAfter(PLUGINID_NATRON_RAWTEMPERATURE, source);
    ASSERT_TRUE(source && node);

    const std::array<double, 3> expected = { 2., 1., 1.5 };
    expectMultiplied(node, expected);
    const ImageMetadata out = outputMetadata(node);
    EXPECT_EQ(expected, RawMetadata::getWbApplied(out).value_or(std::array<double, 3> { 0., 0., 0. }));
    EXPECT_EQ(expected, RawMetadata::getProcessedMaximum(out).value_or(std::array<double, 3> { 0., 0., 0. }));
}

TEST_F(NativeRawTemperatureTest, CameraReferenceComesFromTheMatrix)
{
    ImageMetadata metadata = rawTestMetadata("RGGB", 0, 0, eRawStagePrepared);
    RawMetadata::setWbCoeffs(&metadata, { 2.4, 1.2, 1.8 });
    RawMetadata::setCamToXyz(&metadata, kCamToXyz);
    NodePtr source = createSource(1, rampValue, metadata);
    NodePtr node = createAfter(PLUGINID_NATRON_RAWTEMPERATURE, source);
    ASSERT_TRUE(source && node);

    node->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawTemperatureParamMode)->setValue((int)eRawTemperatureModeCameraReference);
    const std::optional<RawColorMath::Vec3> reference = RawColorMath::cameraReferenceMultipliers(*RawColorMath::invert(kCamToXyz));
    ASSERT_TRUE(bool(reference));
    const std::array<double, 3> expected = { (double)(float)(*reference)[0], 1., (double)(float)(*reference)[2] };
    expectMultiplied(node, expected);
}

// A grey card lit at 4500 K reads, through the camera, the camera's response to that light; the
// multipliers the temperature gives make it neutral again, so the demosaiced patch is grey.
TEST_F(NativeRawTemperatureTest, ACustomTemperatureNeutralisesAGreyPatchLitAtIt)
{
    const RawColorMath::Mat3 xyzToCam = *RawColorMath::invert(kCamToXyz);
    const RawColorMath::Vec3 white = RawColorMath::temperatureToXyz(4500.);
    const RawColorMath::Vec3 camera = RawColorMath::apply(xyzToCam, white);
    const RawLayout layout = *RawLayout::fromPattern("RGGB");
    const RawTestSource::ValueFn grey = [camera, layout](int x, int y, int) {
        return (float)(0.25 * camera[layout.colourAt(x, y)]);
    };
    ImageMetadata metadata = rawTestMetadata("RGGB", 0, 0, eRawStagePrepared);
    RawMetadata::setCamToXyz(&metadata, kCamToXyz);
    NodePtr source = createSource(1, grey, metadata);
    NodePtr temperature = createAfter(PLUGINID_NATRON_RAWTEMPERATURE, source);
    NodePtr demosaic = createAfter(PLUGINID_NATRON_RAWDEMOSAIC, temperature);
    ASSERT_TRUE(source && temperature && demosaic);

    EffectInstancePtr effect = temperature->getEffectInstance();
    effect->getKnobByNameAndType<KnobDouble>(kRawTemperatureParamTemperature)->setValue(4500.);
    EXPECT_EQ((int)eRawTemperatureModeCustom, effect->getKnobByNameAndType<KnobChoice>(kRawTemperatureParamMode)->getValue());

    RenderedPlane plane;
    std::string error;
    ASSERT_TRUE(renderRawTestPlane(demosaic, RectI(0, 0, kWidth, kHeight), &plane, &error)) << error;
    ASSERT_EQ(4u, plane.channels.size());
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const float g = rawTestPlaneValue(plane, x, y, 1);
            ASSERT_GT(g, 0.f);
            EXPECT_NEAR(1.f, rawTestPlaneValue(plane, x, y, 0) / g, 1e-4f) << x << "," << y;
            EXPECT_NEAR(1.f, rawTestPlaneValue(plane, x, y, 2) / g, 1e-4f) << x << "," << y;
            EXPECT_EQ(1.f, rawTestPlaneValue(plane, x, y, 3)) << x << "," << y;
        }
    }

    // Editing the multipliers moves the temperature to the one they neutralise.
    const char* const names[3] = { kRawTemperatureParamRed, kRawTemperatureParamGreen, kRawTemperatureParamBlue };
    double at4500[3];
    for (int c = 0; c < 3; ++c) {
        at4500[c] = effect->getKnobByNameAndType<KnobDouble>(names[c])->getValue();
    }
    effect->getKnobByNameAndType<KnobDouble>(kRawTemperatureParamTemperature)->setValue(6500.);
    EXPECT_NE(at4500[0], effect->getKnobByNameAndType<KnobDouble>(kRawTemperatureParamRed)->getValue());
    for (int c = 0; c < 3; ++c) {
        effect->getKnobByNameAndType<KnobDouble>(names[c])->setValue(at4500[c]);
    }
    EXPECT_NEAR(4500., effect->getKnobByNameAndType<KnobDouble>(kRawTemperatureParamTemperature)->getValue(), 2.);
}

TEST_F(NativeRawTemperatureTest, LinearDataIsBalancedPerChannel)
{
    ImageMetadata metadata = rawTestMetadata(kRawCfaPatternNone, 0, 0, eRawStagePrepared);
    RawMetadata::setWbCoeffs(&metadata, { 1.5, 1., 2. });
    NodePtr source = createSource(3, rampValue, metadata);
    NodePtr node = createAfter(PLUGINID_NATRON_RAWTEMPERATURE, source);
    ASSERT_TRUE(source && node);

    RenderedPlane plane;
    std::string error;
    ASSERT_TRUE(renderRawTestPlane(node, RectI(0, 0, kWidth, kHeight), &plane, &error)) << error;
    const float coeffs[3] = { 1.5f, 1.f, 2.f };
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            for (int c = 0; c < 3; ++c) {
                ASSERT_EQ(rampValue(x, y, c) * coeffs[c], rawTestPlaneValue(plane, x, y, c)) << x << "," << y;
            }
        }
    }
}

TEST_F(NativeRawTemperatureTest, UnpreparedDataRaisesTheStageError)
{
    const ImageMetadata metadata = rawTestMetadata("RGGB", 0, 0, eRawStageRaw);
    NodePtr source = createSource(1, rampValue, metadata);
    NodePtr node = createAfter(PLUGINID_NATRON_RAWTEMPERATURE, source);
    ASSERT_TRUE(source && node);

    const std::optional<std::string> expected = RawMetadata::requireStage(metadata, eRawStagePrepared, node->getLabel());
    ASSERT_TRUE(bool(expected));
    RenderedPlane plane;
    std::string error;
    EXPECT_FALSE(renderRawTestPlane(node, RectI(0, 0, kWidth, kHeight), &plane, &error));
    ASSERT_TRUE(node->hasPersistentMessage());
    QString message;
    int type = 0;
    node->getPersistentMessage(&message, &type, false);
    EXPECT_EQ(QString::fromUtf8(expected->c_str()), message);
    EXPECT_EQ((int)eMessageTypeError, type);
}
