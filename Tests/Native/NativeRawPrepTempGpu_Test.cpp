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

#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "RawGpuTestSupport.h"

#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Raw/RawPrepare.h"
#include "Engine/Nodes/Raw/RawTemperature.h"

NATRON_NAMESPACE_USING

namespace {
// RawPrepare divides on the GPU, which Vulkan rounds within 2.5 ulp; prepared values lie around
// [0, 1], so that stays under the arithmetic class. RawTemperature multiplies once, as the CPU
// does, and is held to the same class.
const ParityTolerance kPrepareGpuTolerance = ParityTolerance::arithmetic();
const ParityTolerance kTemperatureGpuTolerance = ParityTolerance::arithmetic();

// Windows of the prepared image, one starting on an odd photosite so that every cell of the 2x2
// black level block and of the CFA lands on every position of the window.
const RectI kWindows[] = { RectI(200, 150, 264, 198), RectI(321, 97, 360, 130) };
} // namespace

class NativeRawPrepTempGpuTest
    : public RawGpuTest {
protected:
    void setBool(const NodePtr& node,
                 const char* name,
                 bool value)
    {
        KnobBoolPtr knob = node->getEffectInstance()->getKnobByNameAndType<KnobBool>(name);
        ASSERT_TRUE(bool(knob)) << name;
        knob->setValue(value);
    }

    void setDouble(const NodePtr& node,
                   const char* name,
                   double value,
                   int dimension = 0)
    {
        KnobDoublePtr knob = node->getEffectInstance()->getKnobByNameAndType<KnobDouble>(name);
        ASSERT_TRUE(bool(knob)) << name;
        knob->setValue(value, ViewSpec::all(), dimension);
    }

    void setInt(const NodePtr& node,
                const char* name,
                int value)
    {
        KnobIntPtr knob = node->getEffectInstance()->getKnobByNameAndType<KnobInt>(name);
        ASSERT_TRUE(bool(knob)) << name;
        knob->setValue(value);
    }
};

#ifdef NATRON_HAVE_GPU

// The levels, scale and crop offset of the raw files, on a Bayer mosaic, an X-Trans mosaic and
// three-channel linear data.
TEST_F(NativeRawPrepTempGpuTest, PrepareMatchesTheCpuOnEveryFixture)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    for (const RawFixture& fixture : rawFixtures()) {
        NodePtr read = createRead(fixture);
        NodePtr prepare = createAfter(PLUGINID_NATRON_RAWPREPARE, read);
        ASSERT_TRUE(read && prepare);
        for (const RectI& window : kWindows) {
            expectGpuMatchesCpu(*runtime, prepare, read, window, kPrepareGpuTolerance, std::string("RawPrepare, ") + fixture.camera);
        }
    }
}

// Overridden levels and an odd crop, which moves the black level cells against the source.
TEST_F(NativeRawPrepTempGpuTest, PrepareOverridesMatchTheCpu)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    for (const RawFixture* fixture : { &bayerFixture(), &xtransFixture() }) {
        NodePtr read = createRead(*fixture);
        NodePtr prepare = createAfter(PLUGINID_NATRON_RAWPREPARE, read);
        ASSERT_TRUE(read && prepare);
        setBool(prepare, kRawPrepareParamBlackOverride, true);
        const double black[4] = { 2041., 2049., 2050., 2039. };
        for (int i = 0; i < 4; ++i) {
            setDouble(prepare, kRawPrepareParamBlackLevels, black[i], i);
        }
        setBool(prepare, kRawPrepareParamWhiteOverride, true);
        setDouble(prepare, kRawPrepareParamWhiteLevel, 15000.);
        setBool(prepare, kRawPrepareParamCropOverride, true);
        setInt(prepare, kRawPrepareParamCropLeft, 37);
        setInt(prepare, kRawPrepareParamCropBottom, 21);
        setInt(prepare, kRawPrepareParamCropRight, 4);
        setInt(prepare, kRawPrepareParamCropTop, 9);
        if (HasFatalFailure()) {
            return;
        }
        for (const RectI& window : kWindows) {
            expectGpuMatchesCpu(*runtime, prepare, read, window, kPrepareGpuTolerance, std::string("RawPrepare overridden, ") + fixture->camera);
        }
    }
}

// The white balance of every mode, per CFA colour on the mosaics and per channel on linear data.
TEST_F(NativeRawPrepTempGpuTest, TemperatureMatchesTheCpuOnEveryFixture)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    for (const RawFixture& fixture : rawFixtures()) {
        NodePtr read = createRead(fixture);
        NodePtr prepare = createAfter(PLUGINID_NATRON_RAWPREPARE, read);
        NodePtr temperature = createAfter(PLUGINID_NATRON_RAWTEMPERATURE, prepare);
        ASSERT_TRUE(read && prepare && temperature);
        KnobChoicePtr mode = temperature->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawTemperatureParamMode);
        ASSERT_TRUE(bool(mode));
        for (int m : { (int)eRawTemperatureModeAsShot, (int)eRawTemperatureModeCameraReference, (int)eRawTemperatureModeCustom }) {
            mode->setValue(m);
            if (m == (int)eRawTemperatureModeCustom) {
                setDouble(temperature, kRawTemperatureParamRed, 1.75);
                setDouble(temperature, kRawTemperatureParamGreen, 0.9);
                setDouble(temperature, kRawTemperatureParamBlue, 2.3);
                ASSERT_FALSE(HasFatalFailure());
            }
            for (const RectI& window : kWindows) {
                expectGpuMatchesCpu(*runtime, temperature, prepare, window, kTemperatureGpuTolerance,
                                    std::string("RawTemperature mode ") + std::to_string(m) + ", " + fixture.camera);
            }
        }
    }
}

#endif // NATRON_HAVE_GPU
