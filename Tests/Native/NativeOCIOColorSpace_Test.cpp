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

#include <string>

#include <gtest/gtest.h>

#include <OpenColorIO/OpenColorIO.h>

#include "BaseTest.h"
#include "NativeParity.h"
#include "OcioNodeTestSupport.h"

#include "Engine/AppInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/OCIO/OCIOColorSpace.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING
using namespace OcioNodeTest;

namespace {

OCIO_NAMESPACE::ConstTransformRcPtr
spaceTransform(const std::string& from,
               const std::string& to)
{
    OCIO_NAMESPACE::ColorSpaceTransformRcPtr transform = OCIO_NAMESPACE::ColorSpaceTransform::Create();

    transform->setSrc(from.c_str());
    transform->setDst(to.c_str());

    return transform;
}

} // namespace

class NativeOCIOColorSpaceTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
        setConfig(getApp(), kStudioURI);
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    ParityPair makePair()
    {
        ParityPair pair = makeParityPair(getApp(), PLUGINID_NATRON_OCIOCOLORSPACE, -1, PLUGIN_MAJOR_NATRON_OCIOCOLORSPACE);

        EXPECT_TRUE(bool(pair.native));
        EXPECT_TRUE(pair.native && dynamic_cast<OCIOColorSpace*>(pair.native->getEffectInstance().get()));

        return pair;
    }

    void expectConverts(const ParityPair& pair,
                        const std::string& from,
                        const std::string& to,
                        const std::string& what)
    {
        expectProcessed(pair.native, pair.source, referenceProcessor(config(getApp()), spaceTransform(from, to)), what);
    }
};

TEST_F(NativeOCIOColorSpaceTest, ConvertsBetweenTwoSpaces)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));

    ASSERT_TRUE(setKnobValue(pair.native, kOCIOColorSpaceParamInputSpace, "ACEScg"));
    ASSERT_TRUE(setKnobValue(pair.native, kOCIOColorSpaceParamOutputSpace, kSRGB));
    expectConverts(pair, "ACEScg", kSRGB, "ACEScg to sRGB");

    ASSERT_TRUE(setKnobValue(pair.native, kOCIOColorSpaceParamInputSpace, "ACES2065-1"));
    ASSERT_TRUE(setKnobValue(pair.native, kOCIOColorSpaceParamOutputSpace, "ACEScct"));
    expectConverts(pair, "ACES2065-1", "ACEScct", "ACES2065-1 to ACEScct");
}

TEST_F(NativeOCIOColorSpaceTest, EmptySpaceIsTheWorkingSpace)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));

    const std::string working = getApp()->getProject()->getWorkingColorSpace();
    ASSERT_FALSE(working.empty());

    // A default node converts the working space to itself: no change.
    expectProcessed(pair.native, pair.source, OCIO_NAMESPACE::ConstCPUProcessorRcPtr(), "default");

    ASSERT_TRUE(setKnobValue(pair.native, kOCIOColorSpaceParamOutputSpace, kSRGB));
    expectConverts(pair, working, kSRGB, "working to sRGB");
}

TEST_F(NativeOCIOColorSpaceTest, LogPresetsSetTheRoles)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(config(getApp())->hasRole(OCIO_NAMESPACE::ROLE_COMPOSITING_LOG));

    ASSERT_TRUE(setKnobValue(pair.native, kOCIOColorSpaceParamPreset, kOCIOColorSpacePresetLogToLin));
    EXPECT_EQ(std::string(OCIO_NAMESPACE::ROLE_COMPOSITING_LOG), stringValue(pair.native, kOCIOColorSpaceParamInputSpace));
    EXPECT_EQ(std::string(OCIO_NAMESPACE::ROLE_SCENE_LINEAR), stringValue(pair.native, kOCIOColorSpaceParamOutputSpace));
    expectConverts(pair, OCIO_NAMESPACE::ROLE_COMPOSITING_LOG, OCIO_NAMESPACE::ROLE_SCENE_LINEAR, "log to lin");

    ASSERT_TRUE(setKnobValue(pair.native, kOCIOColorSpaceParamPreset, kOCIOColorSpacePresetLinToLog));
    EXPECT_EQ(std::string(OCIO_NAMESPACE::ROLE_SCENE_LINEAR), stringValue(pair.native, kOCIOColorSpaceParamInputSpace));
    EXPECT_EQ(std::string(OCIO_NAMESPACE::ROLE_COMPOSITING_LOG), stringValue(pair.native, kOCIOColorSpaceParamOutputSpace));
    expectConverts(pair, OCIO_NAMESPACE::ROLE_SCENE_LINEAR, OCIO_NAMESPACE::ROLE_COMPOSITING_LOG, "lin to log");
}

TEST_F(NativeOCIOColorSpaceTest, EditingASpaceReturnsThePresetToCustom)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));

    ASSERT_TRUE(setKnobValue(pair.native, kOCIOColorSpaceParamPreset, kOCIOColorSpacePresetLogToLin));
    KnobChoicePtr preset = std::dynamic_pointer_cast<KnobChoice>(pair.native->getKnobByName(kOCIOColorSpaceParamPreset));
    KnobStringPtr out = std::dynamic_pointer_cast<KnobString>(pair.native->getKnobByName(kOCIOColorSpaceParamOutputSpace));
    ASSERT_TRUE(bool(preset));
    ASSERT_TRUE(bool(out));
    EXPECT_EQ(std::string(kOCIOColorSpacePresetLogToLin), preset->getActiveEntry().id);

    out->setValue(std::string(kSRGB), ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);
    EXPECT_EQ(std::string(kOCIOColorSpacePresetCustom), preset->getActiveEntry().id);
    EXPECT_EQ(std::string(OCIO_NAMESPACE::ROLE_COMPOSITING_LOG), stringValue(pair.native, kOCIOColorSpaceParamInputSpace));
    expectConverts(pair, OCIO_NAMESPACE::ROLE_COMPOSITING_LOG, kSRGB, "log to sRGB");
}
