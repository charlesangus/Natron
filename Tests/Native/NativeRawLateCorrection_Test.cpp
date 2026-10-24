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
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "RawTestSource.h"

#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/Raw/RawColorMath.h"
#include "Engine/Nodes/Raw/RawDemosaic.h"
#include "Engine/Nodes/Raw/RawHighlights.h"
#include "Engine/Nodes/Raw/RawInputColor.h"
#include "Engine/Nodes/Raw/RawTemperature.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
const int kWidth = 24;
const int kHeight = 18;

// A camera RGB to XYZ (D65) matrix of the kind a raw file carries: invertible, not sRGB.
const std::array<double, 9> kCamToXyz = { 0.6722, 0.1636, 0.1194,
                                          0.2654, 0.8410, -0.1064,
                                          0.0214, -0.1563, 1.1250 };
const std::array<double, 3> kAsShot = { 2.4, 1.2, 1.8 };
// Low enough that no multiplier takes a photosite near the clipping threshold.
const std::array<float, 3> kCameraRgb = { 0.2f, 0.3f, 0.25f };

const RawTemperatureModeEnum kModes[] = {
    eRawTemperatureModeAsShot,
    eRawTemperatureModeCameraReference,
    eRawTemperatureModeCustom,
    eRawTemperatureModeCameraPreset,
    eRawTemperatureModeFromImageArea,
    eRawTemperatureModeAsShotToReference,
};
} // namespace

class NativeRawLateCorrectionTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        registerRawTestSourceOnce();
        BaseTest::SetUp();
    }

    struct Chain {
        NodePtr temperature;
        NodePtr highlights;
        NodePtr demosaic;
        NodePtr inputColor;
    };

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

    // A flat grey card in camera RGB, with the file's as-shot multipliers and matrix, through
    // RawTemperature, RawHighlights, RawDemosaic and RawInputColor.
    Chain createChain()
    {
        Chain chain;
        NodePtr source = createNode(QString::fromUtf8(kRawTestSourceID));

        EXPECT_TRUE(bool(source));
        if (!source) {
            return chain;
        }
        ImageMetadata metadata = rawTestMetadata("RGGB", 0, 0, eRawStagePrepared);
        RawMetadata::setCamToXyz(&metadata, kCamToXyz);
        RawMetadata::setWbCoeffs(&metadata, kAsShot);
        const RawLayout layout = *RawLayout::fromPattern("RGGB");
        dynamic_cast<RawTestSource*>(source->getEffectInstance().get())->setRaw(RectI(0, 0, kWidth, kHeight), 1, [layout](int x, int y, int) { return kCameraRgb[layout.colourAt(x, y)]; }, metadata);
        chain.temperature = createAfter(PLUGINID_NATRON_RAWTEMPERATURE, source);
        chain.highlights = createAfter(PLUGINID_NATRON_RAWHIGHLIGHTS, chain.temperature);
        chain.demosaic = createAfter(PLUGINID_NATRON_RAWDEMOSAIC, chain.highlights);
        chain.inputColor = createAfter(PLUGINID_NATRON_RAWINPUTCOLOR, chain.demosaic);

        return chain;
    }

    static void setMode(const Chain& chain,
                        RawTemperatureModeEnum mode)
    {
        chain.temperature->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawTemperatureParamMode)->setValue((int)mode);
    }

    static ImageMetadata outputMetadata(const NodePtr& node)
    {
        NativeEffectBase* effect = dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());

        return effect ? effect->getOutputMetadata(1., ViewIdx(0)) : ImageMetadata();
    }

    static RawColorMath::Vec3 cameraReference()
    {
        return *RawColorMath::cameraReferenceMultipliers(*RawColorMath::invert(kCamToXyz));
    }
};

TEST_F(NativeRawLateCorrectionTest, AsShotToReferenceEndsWhereCameraReferenceDoes)
{
    const Chain chain = createChain();
    ASSERT_TRUE(chain.temperature && chain.highlights && chain.demosaic && chain.inputColor);

    setMode(chain, eRawTemperatureModeCameraReference);
    RenderedPlane reference;
    std::string error;
    ASSERT_TRUE(renderRawTestPlane(chain.inputColor, RectI(0, 0, kWidth, kHeight), &reference, &error)) << error;

    setMode(chain, eRawTemperatureModeAsShotToReference);
    RenderedPlane late;
    ASSERT_TRUE(renderRawTestPlane(chain.inputColor, RectI(0, 0, kWidth, kHeight), &late, &error)) << error;

    ASSERT_EQ(reference.channels.size(), late.channels.size());
    ASSERT_GE(late.channels.size(), 3u);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            for (int c = 0; c < 3; ++c) {
                const float expected = rawTestPlaneValue(reference, x, y, c);
                EXPECT_NEAR(expected, rawTestPlaneValue(late, x, y, c), 1e-5f * (std::fabs(expected) + 1.f)) << x << "," << y << "," << c;
            }
        }
    }
}

TEST_F(NativeRawLateCorrectionTest, TheCorrectionTravelsToRawInputColorInThatModeOnly)
{
    const Chain chain = createChain();
    ASSERT_TRUE(chain.temperature && chain.highlights && chain.demosaic && chain.inputColor);
    const RawColorMath::Vec3 d65 = cameraReference();

    setMode(chain, eRawTemperatureModeAsShotToReference);
    // As Shot applies the as-shot multipliers normalised to green; the correction moves them on
    // to the reference ones.
    const std::optional<std::array<double, 3>> applied = RawMetadata::getWbApplied(outputMetadata(chain.temperature));
    ASSERT_TRUE(applied.has_value());
    EXPECT_FLOAT_EQ(2.f, (float)(*applied)[0]);
    EXPECT_FLOAT_EQ(1.5f, (float)(*applied)[2]);
    for (const NodePtr& node : { chain.temperature, chain.highlights, chain.demosaic }) {
        const std::optional<std::array<double, 3>> correction = RawMetadata::getWbLateCorrection(outputMetadata(node));
        ASSERT_TRUE(correction.has_value()) << node->getScriptName();
        EXPECT_DOUBLE_EQ(d65[0] / 2., (*correction)[0]);
        EXPECT_DOUBLE_EQ(d65[1], (*correction)[1]);
        EXPECT_DOUBLE_EQ(d65[2] / 1.5, (*correction)[2]);
    }
    // After the correction the data carries the reference multipliers.
    const std::optional<std::array<double, 3>> corrected = RawMetadata::getWbApplied(outputMetadata(chain.inputColor));
    ASSERT_TRUE(corrected.has_value());
    for (int c = 0; c < 3; ++c) {
        EXPECT_DOUBLE_EQ(d65[c], (*corrected)[c]) << c;
    }

    setMode(chain, eRawTemperatureModeCameraReference);
    EXPECT_FALSE(RawMetadata::getWbLateCorrection(outputMetadata(chain.temperature)).has_value());
    EXPECT_FALSE(RawMetadata::getWbLateCorrection(outputMetadata(chain.demosaic)).has_value());
}

TEST_F(NativeRawLateCorrectionTest, EveryModePublishesTheCameraReferenceMultipliers)
{
    const Chain chain = createChain();
    ASSERT_TRUE(chain.temperature && chain.demosaic);
    const RawColorMath::Vec3 d65 = cameraReference();

    for (RawTemperatureModeEnum mode : kModes) {
        setMode(chain, mode);
        for (const NodePtr& node : { chain.temperature, chain.demosaic }) {
            const std::optional<std::array<double, 3>> published = RawMetadata::getWbD65Coeffs(outputMetadata(node));
            ASSERT_TRUE(published.has_value()) << (int)mode;
            for (int c = 0; c < 3; ++c) {
                EXPECT_DOUBLE_EQ(d65[c], (*published)[c]) << (int)mode << "," << c;
            }
        }
    }
}
