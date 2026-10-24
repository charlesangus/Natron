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
#include <bitset>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "RawGpuTestSupport.h"

#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/Raw/RawColorCalibration.h"
#include "Engine/Nodes/Raw/RawDemosaic.h"
#include "Engine/Nodes/Raw/RawInputColor.h"

NATRON_NAMESPACE_USING

namespace {
// Vulkan lets a device divide within 2.5 ulp and contract multiply-adds, and the error of this
// chain follows the size of its intermediates rather than of the output: NVIDIA lands 1e-6 to
// 2.6e-6 from the CPU for outputs anywhere from 0.13 (over a hundred ulp there) to 2.4, so a
// bound in ulps of the output cannot hold and arithmetic()'s 1e-6 sits below it. 1e-5 leaves
// nearly four times the worst measured, and stays a hundred times under a wrong matrix entry.
const ParityTolerance kCalibrationGpuTolerance = ParityTolerance::make(0., 1e-5);

const char* const kLinearRec709Space = "Linear Rec.709 (sRGB)";

const int kWidth = 16;
const int kHeight = 4;
const RectI kWindow(0, 0, kWidth, kHeight);

// Working RGB from black to above white, saturated, with negative components and a NaN, which
// the clip turns into zero and the unclipped path into -FLT_MAX.
const std::array<std::array<float, 3>, kWidth> kColours = { {
    { 0.f, 0.f, 0.f },
    { 0.18f, 0.18f, 0.18f },
    { 0.6f, 0.3f, 0.1f },
    { 0.05f, 0.4f, 0.2f },
    { 0.2f, 0.25f, 0.7f },
    { 0.9f, 0.8f, 0.75f },
    { 1.6f, 1.2f, 0.9f },
    { 0.02f, 0.5f, 0.9f },
    { -0.05f, 0.3f, 0.6f },
    { 0.8f, -0.02f, 0.1f },
    { std::numeric_limits<float>::quiet_NaN(), 0.4f, 0.2f },
    { 0.95f, 0.02f, 0.01f },
    { 0.01f, 0.02f, 0.95f },
    { 1e-4f, 2e-4f, 1e-4f },
    { 0.5f, 0.5f, 0.5f },
    { 0.3f, 0.9f, 0.05f },
} };

const float kRowScales[kHeight] = { 1.f, 0.5f, 0.25f, 0.1f };

const char* const kAdaptations[] = { "linearBradford", "cat16", "nonLinearBradford", "xyz", "none" };
const char* const kVersions[] = { "v1", "v2", "v3" };
const double kGamuts[] = { 0., 1., 2.5 };
} // namespace

class NativeRawColorCalibrationGpuTest
    : public RawGpuTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        RawGpuTest::SetUp();
        KnobChoicePtr workingSpace = getApp()->getProject()->getKnobByNameAndType<KnobChoice>("workingSpace");
        ASSERT_TRUE(bool(workingSpace));
        workingSpace->setValueFromID(kLinearRec709Space, 0);
        ASSERT_EQ(std::string(kLinearRec709Space), getApp()->getProject()->getWorkingColorSpace());
    }

    struct Chain {
        NodePtr source;
        NodePtr inputColor;
        NodePtr calibration;
    };

    // kColours, row y scaled by kRowScales[y], through Demosaic and RawInputColor's Linear Rec709
    // profile into a RawColorCalibration that mixes, saturates and lightens under a 3500 K
    // illuminant of its own.
    Chain createChain()
    {
        Chain c;

        c.source = createNode(QString::fromUtf8(kRawTestSourceID));
        EXPECT_TRUE(bool(c.source));
        if (!c.source) {
            return c;
        }
        dynamic_cast<RawTestSource*>(c.source->getEffectInstance().get())->setRaw(kWindow, 3, [](int x, int y, int ch) { return kColours[x][ch] * kRowScales[y]; }, rawTestMetadata(kRawCfaPatternNone, 0, 0, eRawStagePrepared));
        NodePtr demosaic = createAfter(PLUGINID_NATRON_RAWDEMOSAIC, c.source);
        c.inputColor = createAfter(PLUGINID_NATRON_RAWINPUTCOLOR, demosaic);
        c.calibration = createAfter(PLUGINID_NATRON_RAWCOLORCALIBRATION, c.inputColor);
        if (!c.inputColor || !c.calibration) {
            return c;
        }
        c.inputColor->getEffectInstance()->getKnobByNameAndType<KnobBool>(kRawInputColorParamProfileOverride)->setValue(true);
        c.inputColor->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawInputColorParamProfile)->setValueFromID(kRawInputColorProfileLinearRec709, 0);

        boolKnob(c.calibration, kRawColorCalibrationParamCatOverride)->setValue(true);
        choiceKnob(c.calibration, kRawColorCalibrationParamIlluminant)->setValueFromID("d", 0);
        doubleKnob(c.calibration, kRawColorCalibrationParamTemperature)->setValue(3500.);
        setRow(c.calibration, eRawColorCalibrationRowRed, { 0.9, 0.2, -0.05 });
        setRow(c.calibration, eRawColorCalibrationRowGreen, { 0.1, 0.85, 0.1 });
        setRow(c.calibration, eRawColorCalibrationRowBlue, { -0.05, 0.15, 1.1 });
        setRow(c.calibration, eRawColorCalibrationRowSaturation, { 0.3, -0.2, 0.15 });
        setRow(c.calibration, eRawColorCalibrationRowLightness, { 0.1, -0.05, 0.2 });

        return c;
    }

    // The query a render of node's colour plane makes, with the channels its layer knob processes.
    static GpuQuery colourQueryFor(const NodePtr& node)
    {
        RenderedPlane cpu;
        renderCpu(node, kWindow, &cpu);
        const int nComps = (int)cpu.channels.size();
        GpuQuery query = queryFor(nComps);
        EffectInstancePtr effect = node->getEffectInstance();
        const ImageLayerDesc& layer = (nComps == 3) ? ImageLayerDesc::getRGBComponents() : ImageLayerDesc::getRGBAComponents();
        const std::bitset<4> channels = effect->getProcessChannelsForPlane(effect->getRenderHash(), kTime, ViewIdx(0), layer);

        for (int i = 0; i < 4; ++i) {
            query.processChannels[i] = channels[i];
        }

        return query;
    }

    static KnobBoolPtr boolKnob(const NodePtr& node,
                                const std::string& name)
    {
        return node->getEffectInstance()->getKnobByNameAndType<KnobBool>(name);
    }

    static KnobChoicePtr choiceKnob(const NodePtr& node,
                                    const std::string& name)
    {
        return node->getEffectInstance()->getKnobByNameAndType<KnobChoice>(name);
    }

    static KnobDoublePtr doubleKnob(const NodePtr& node,
                                    const std::string& name)
    {
        return node->getEffectInstance()->getKnobByNameAndType<KnobDouble>(name);
    }

    static void setRow(const NodePtr& node,
                       RawColorCalibrationRowEnum row,
                       const std::array<double, 3>& values)
    {
        for (int c = 0; c < 3; ++c) {
            doubleKnob(node, rawColorCalibrationRowKnobName(row, c))->setValue(values[c]);
        }
    }
};

#ifdef NATRON_HAVE_GPU

// Every adaptation, saturation algorithm, clip setting and a gamut compression of none, the
// default and a strong one, against the CPU.
TEST_F(NativeRawColorCalibrationGpuTest, EveryAdaptationVersionClipAndGamutMatchesTheCpu)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    const Chain c = createChain();
    ASSERT_TRUE(c.source && c.inputColor && c.calibration);

    for (const char* adaptation : kAdaptations) {
        choiceKnob(c.calibration, kRawColorCalibrationParamAdaptation)->setValueFromID(adaptation, 0);
        for (const char* version : kVersions) {
            choiceKnob(c.calibration, kRawColorCalibrationParamVersion)->setValueFromID(version, 0);
            for (bool clip : { true, false }) {
                boolKnob(c.calibration, kRawColorCalibrationParamClip)->setValue(clip);
                for (double gamut : kGamuts) {
                    doubleKnob(c.calibration, kRawColorCalibrationParamGamut)->setValue(gamut);
                    const std::string what = std::string(adaptation) + ", " + version + (clip ? ", clip" : ", no clip") + ", gamut " + std::to_string(gamut);
                    const GpuQuery query = colourQueryFor(c.calibration);
                    expectGpuMatchesCpu(*runtime, c.calibration, c.inputColor, kWindow, kCalibrationGpuTolerance, what, &query);
                }
            }
        }
    }
}

// A grey row replaces the colour with its weighted sum, left in the adaptation's space.
TEST_F(NativeRawColorCalibrationGpuTest, GreyRowMatchesTheCpu)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    const Chain c = createChain();
    ASSERT_TRUE(c.source && c.inputColor && c.calibration);
    setRow(c.calibration, eRawColorCalibrationRowGrey, { 0.3, 0.6, 0.1 });

    for (const char* adaptation : kAdaptations) {
        choiceKnob(c.calibration, kRawColorCalibrationParamAdaptation)->setValueFromID(adaptation, 0);
        for (bool clip : { true, false }) {
            boolKnob(c.calibration, kRawColorCalibrationParamClip)->setValue(clip);
            const std::string what = std::string("grey, ") + adaptation + (clip ? ", clip" : ", no clip");
            const GpuQuery query = colourQueryFor(c.calibration);
            expectGpuMatchesCpu(*runtime, c.calibration, c.inputColor, kWindow, kCalibrationGpuTolerance, what, &query);
        }
    }
}

#endif // NATRON_HAVE_GPU
