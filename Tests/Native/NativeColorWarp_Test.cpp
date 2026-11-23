/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-3.0.html>
 * ***** END LICENSE BLOCK ***** */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cmath>
#include <cstring>
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "BaseTest.h"
#include "NativeParity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobColorWarpPins.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/ColorWarp/ColorWarp.h"
#include "Engine/Nodes/Color/ColorWarp/ColorWarpField.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/ColorMath.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;
const int kSize = 16;

bool
renderColor(const NodePtr& node,
            float rgb[3])
{
    std::list<ImageLayerDesc> present;
    node->getEffectInstance()->getPresentLayers(kTime, ViewIdx(0), -1, &present);
    std::list<ImageLayerDesc> layers;
    for (std::list<ImageLayerDesc>::const_iterator it = present.begin(); it != present.end(); ++it) {
        if (it->isColorLayer()) {
            layers.push_back(*it);
            break;
        }
    }
    EXPECT_EQ(1u, layers.size());
    std::vector<RenderedPlane> planes;
    std::string error;
    const RectI window(4, 4, 5, 5);
    const bool ok = renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, window, layers, &planes, &error);
    EXPECT_TRUE(ok) << error;
    if (!ok || (planes.size() != 1) || (planes[0].channels.size() < 3)) {
        return false;
    }
    for (int c = 0; c < 3; ++c) {
        rgb[c] = planes[0].pixels[c];
    }

    return true;
}

ColorWarpPinData
makePin(int id,
        const double source[3],
        const double target[3],
        double strength,
        double falloff)
{
    ColorWarpPinData pin;
    pin.id = id;
    pin.name = "pin" + std::to_string(id);
    for (int c = 0; c < 3; ++c) {
        pin.source[c] = source[c];
        pin.target[c].value = target[c];
    }
    pin.strength.value = strength;
    pin.falloff.value = falloff;

    return pin;
}

} // namespace

class NativeColorWarpTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        ProjectPtr project = getApp()->getProject();
        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kSize, kSize, "colorWarp16", 1.));
        appPTR->clearAllCaches();
    }

    virtual void TearDown() OVERRIDE
    {
        appPTR->clearAllCaches();
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    NodePtr makeWarp(const std::vector<double>& rgba,
                     NodePtr* constantOut = NULL)
    {
        NodePtr constant = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT), PLUGIN_MAJOR_NATRON_CONSTANT);
        NodePtr warp = createNode(QString::fromUtf8(PLUGINID_NATRON_COLORWARP), PLUGIN_MAJOR_NATRON_COLORWARP);
        EXPECT_TRUE(bool(constant) && bool(warp));
        if (!constant || !warp) {
            return NodePtr();
        }
        EXPECT_TRUE(setKnobValues(constant, kConstantParamColor, rgba));
        connectNodes(constant, warp, 0, true);
        if (constantOut) {
            *constantOut = constant;
        }

        return warp;
    }

    static KnobColorWarpPinsPtr pinsKnob(const NodePtr& node)
    {
        return node ? std::dynamic_pointer_cast<KnobColorWarpPins>(node->getKnobByName(kColorWarpParamPins)) : KnobColorWarpPinsPtr();
    }

    void expectFieldResult(const NodePtr& warp,
                           const std::vector<ColorWarpPin>& pins,
                           const double rgb[3])
    {
        double workingToXYZ[9];
        ColorMath::workingToXYZ(warp->getEffectInstance().get(), workingToXYZ);
        ColorWarpField field;
        field.prepare(pins, eColorWarpSpaceOklabLog, ColorWarpSpaceBasis(workingToXYZ), eColorWarpKernelGaussian);
        double expected[3];
        field.apply(rgb, expected);
        float rendered[3];
        ASSERT_TRUE(renderColor(warp, rendered));
        for (int c = 0; c < 3; ++c) {
            EXPECT_NEAR(expected[c], rendered[c], 1e-5) << "channel " << c;
        }
    }
};

TEST_F(NativeColorWarpTest, OnePinMapsItsSourceToItsTarget)
{
    NodePtr warp = makeWarp({ 0.4, 0.2, 0.1, 1. });
    KnobColorWarpPinsPtr knob = pinsKnob(warp);
    ASSERT_TRUE(knob != nullptr);

    const double source[3] = { 0.4, 0.2, 0.1 };
    const double target[3] = { 0.2, 0.3, 0.5 };
    ColorWarpPinSet pins;
    pins.pins.push_back(makePin(1, source, target, 1., 1.));
    pins.nextId = 2;
    knob->setPinSet(pins);

    float rendered[3];
    ASSERT_TRUE(renderColor(warp, rendered));
    for (int c = 0; c < 3; ++c) {
        EXPECT_NEAR(target[c], rendered[c], 1e-5) << "channel " << c;
    }
}

TEST_F(NativeColorWarpTest, TwoPinsAndAHoldPinMatchTheField)
{
    const double rgb[3] = { 0.3, 0.35, 0.2 };
    NodePtr warp = makeWarp({ rgb[0], rgb[1], rgb[2], 1. });
    KnobColorWarpPinsPtr knob = pinsKnob(warp);
    ASSERT_TRUE(knob != nullptr);

    const double s1[3] = { 0.4, 0.2, 0.1 };
    const double t1[3] = { 0.2, 0.3, 0.5 };
    const double s2[3] = { 0.1, 0.4, 0.15 };
    const double t2[3] = { 0.15, 0.45, 0.05 };
    const double s3[3] = { 0.3, 0.3, 0.3 };
    ColorWarpPinSet pins;
    pins.pins.push_back(makePin(1, s1, t1, 0.8, 1.5));
    pins.pins.push_back(makePin(2, s2, t2, 1., 1.));
    pins.pins.push_back(makePin(3, s3, s3, 0., 0.7));
    pins.nextId = 4;
    knob->setPinSet(pins);

    const std::vector<ColorWarpPin> evaluated = knob->evaluate(kTime);
    ASSERT_EQ(3u, evaluated.size());
    expectFieldResult(warp, evaluated, rgb);
}

TEST_F(NativeColorWarpTest, KeyedTargetIsEvaluatedAtTheRenderTime)
{
    const double rgb[3] = { 0.4, 0.2, 0.1 };
    NodePtr warp = makeWarp({ rgb[0], rgb[1], rgb[2], 1. });
    KnobColorWarpPinsPtr knob = pinsKnob(warp);
    ASSERT_TRUE(knob != nullptr);

    ColorWarpPinSet pins;
    pins.pins.push_back(makePin(1, rgb, rgb, 1., 1.));
    pins.nextId = 2;
    pins.pins[0].target[1].setKey(0., 0.2);
    pins.pins[0].target[1].setKey(2., 0.6);
    knob->setPinSet(pins);

    const std::vector<ColorWarpPin> evaluated = knob->evaluate(kTime);
    ASSERT_EQ(1u, evaluated.size());
    EXPECT_GT(evaluated[0].target[1], 0.2);
    EXPECT_LT(evaluated[0].target[1], 0.6);
    expectFieldResult(warp, evaluated, rgb);
}

TEST_F(NativeColorWarpTest, EncodeDecodeRoundTripsPinsAndKeys)
{
    const double s1[3] = { 0.4, -0.2, 3.5 };
    const double t1[3] = { 0.2, 0.3, 0.5 };
    ColorWarpPinSet pins;
    pins.pins.push_back(makePin(4, s1, t1, 0.25, 2.));
    pins.pins[0].enabled = false;
    pins.pins[0].name = "skin \"warm\"";
    pins.pins[0].position[0] = 12.5;
    pins.pins[0].position[1] = -3.;
    pins.pins[0].frame = 7.;
    pins.pins[0].target[2].setKey(1., 0.5);
    pins.pins[0].target[2].setKey(10., 0.9);
    pins.pins[0].strength.setKey(3., 0.1);
    pins.pins[0].falloff.setKey(5., 1.25);
    pins.pins[0].falloff.setKey(6., 1.75);
    pins.pins.push_back(makePin(9, t1, s1, 1., 1.));
    pins.nextId = 10;

    const ColorWarpPinSet decoded = KnobColorWarpPins::decode(KnobColorWarpPins::encode(pins));
    EXPECT_TRUE(decoded == pins);
    EXPECT_EQ(10, decoded.nextId);
    ASSERT_EQ(2u, decoded.pins.size());
    EXPECT_EQ(std::string("skin \"warm\""), decoded.pins[0].name);
    EXPECT_FALSE(decoded.pins[0].enabled);
    EXPECT_TRUE(decoded.pins[0].target[2].isAnimated());
    EXPECT_DOUBLE_EQ(pins.pins[0].target[2].valueAt(4.), decoded.pins[0].target[2].valueAt(4.));
}

TEST_F(NativeColorWarpTest, PinsSurviveTheKnobValue)
{
    NodePtr warp = makeWarp({ 0.5, 0.5, 0.5, 1. });
    KnobColorWarpPinsPtr knob = pinsKnob(warp);
    ASSERT_TRUE(knob != nullptr);

    const double source[3] = { 0.5, 0.5, 0.5 };
    ColorWarpPinSet pins;
    (void)pins.addPin(source, 3., 4., 2.);
    pins.pins[0].strength.setKey(1., 0.5);
    pins.pins[0].strength.setKey(3., 1.);
    knob->setPinSet(pins);

    const std::string raw = knob->getValue();
    EXPECT_TRUE(KnobColorWarpPins::decode(raw) == pins);
    knob->setPinSet(ColorWarpPinSet());
    EXPECT_TRUE(knob->getPinSet().pins.empty());
    knob->setValue(raw, ViewSpec::all(), 0, eValueChangedReasonNatronInternalEdited, 0);
    EXPECT_TRUE(knob->getPinSet() == pins);
}

TEST_F(NativeColorWarpTest, NoPinsOrAllDisabledIsBitExactPassThrough)
{
    const float rgb[3] = { 0.123456f, -0.5f, 7.25f };
    NodePtr warp = makeWarp({ rgb[0], rgb[1], rgb[2], 1. });
    KnobColorWarpPinsPtr knob = pinsKnob(warp);
    ASSERT_TRUE(knob != nullptr);

    float rendered[3];
    ASSERT_TRUE(renderColor(warp, rendered));
    EXPECT_EQ(0, std::memcmp(rgb, rendered, sizeof(rgb)));

    const double source[3] = { 0.12, -0.5, 7. };
    const double target[3] = { 0.5, 0.5, 0.5 };
    ColorWarpPinSet pins;
    pins.pins.push_back(makePin(1, source, target, 1., 1.));
    pins.pins[0].enabled = false;
    pins.nextId = 2;
    knob->setPinSet(pins);
    EXPECT_TRUE(knob->evaluate(kTime).empty());

    ASSERT_TRUE(renderColor(warp, rendered));
    EXPECT_EQ(0, std::memcmp(rgb, rendered, sizeof(rgb)));
}

TEST_F(NativeColorWarpTest, PinEditsChangeTheHashAndTheRender)
{
    const double rgb[3] = { 0.4, 0.2, 0.1 };
    NodePtr warp = makeWarp({ rgb[0], rgb[1], rgb[2], 1. });
    KnobColorWarpPinsPtr knob = pinsKnob(warp);
    ASSERT_TRUE(knob != nullptr);

    ColorWarpPinSet pins;
    pins.pins.push_back(makePin(1, rgb, rgb, 1., 1.));
    pins.nextId = 2;
    knob->setPinSet(pins);
    const U64 before = knob->getContentHash();
    float first[3];
    ASSERT_TRUE(renderColor(warp, first));

    pins.pins[0].target[0].value = 0.8;
    knob->setPinSet(pins);
    EXPECT_NE(before, knob->getContentHash());
    float second[3];
    ASSERT_TRUE(renderColor(warp, second));
    EXPECT_NEAR(0.8, second[0], 1e-5);
    EXPECT_NE(first[0], second[0]);

    const U64 moved = knob->getContentHash();
    pins.pins[0].strength.setKey(5., 0.5);
    knob->setPinSet(pins);
    EXPECT_NE(moved, knob->getContentHash());
}

TEST_F(NativeColorWarpTest, AddPinAtSamplesTheInputAndUndoesInOneStep)
{
    NodePtr warp = makeWarp({ 0.25, 0.5, 0.75, 1. });
    KnobColorWarpPinsPtr knob = pinsKnob(warp);
    ColorWarp* effect = warp ? dynamic_cast<ColorWarp*>(warp->getEffectInstance().get()) : NULL;
    ASSERT_TRUE(knob != nullptr);
    ASSERT_TRUE(effect != NULL);

    const int id = effect->addPinAt(4.5, 4.5, kTime, ViewIdx(0));
    ASSERT_NE(0, id);
    const ColorWarpPinSet pins = knob->getPinSet();
    ASSERT_EQ(1u, pins.pins.size());
    EXPECT_EQ(id, knob->getSelectedPinId());
    EXPECT_NEAR(0.25, pins.pins[0].source[0], 1e-5);
    EXPECT_NEAR(0.5, pins.pins[0].source[1], 1e-5);
    EXPECT_NEAR(0.75, pins.pins[0].source[2], 1e-5);
    for (int c = 0; c < 3; ++c) {
        EXPECT_DOUBLE_EQ(pins.pins[0].source[c], pins.pins[0].target[c].valueAt(kTime));
    }
    EXPECT_DOUBLE_EQ(4.5, pins.pins[0].position[0]);
    EXPECT_DOUBLE_EQ(kTime, pins.pins[0].frame);

    EXPECT_EQ(0, effect->addPinAt(-100., -100., kTime, ViewIdx(0)));
    EXPECT_EQ(1u, knob->getPinSet().pins.size());

    EXPECT_TRUE(effect->removePin(id));
    EXPECT_TRUE(knob->getPinSet().pins.empty());
    EXPECT_EQ(0, knob->getSelectedPinId());
}
