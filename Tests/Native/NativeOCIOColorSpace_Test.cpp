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
#include "Engine/Nodes/IO/IoColorSpace.h"
#include "Engine/Nodes/IO/OcioSpaceKnobs.h"
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
        ParityPair pair = makeParityPair(getApp(), PLUGINID_NATRON_OCIOCOLORSPACE, PLUGIN_MAJOR_NATRON_OCIOCOLORSPACE);

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

TEST_F(NativeOCIOColorSpaceTest, ProjectDefaultIsTheWorkingSpace)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));

    const std::string working = IoColorSpace::nameInConfig(config(getApp()), getApp()->getProject()->getWorkingColorSpace());
    ASSERT_FALSE(working.empty());

    // A default node converts the working space to itself: no change.
    EXPECT_EQ(std::string(OcioSpaceKnobs::projectDefaultID()), choiceID(pair.native, kOCIOColorSpaceParamInputSpace));
    EXPECT_EQ(std::string(OcioSpaceKnobs::projectDefaultID()), choiceID(pair.native, kOCIOColorSpaceParamOutputSpace));
    expectProcessed(pair.native, pair.source, OCIO_NAMESPACE::ConstCPUProcessorRcPtr(), "default");

    KnobChoicePtr in = std::dynamic_pointer_cast<KnobChoice>(pair.native->getKnobByName(kOCIOColorSpaceParamInputSpace));
    ASSERT_TRUE(bool(in));
    EXPECT_EQ("Project default (" + working + ")", in->getEntry(0).label);
    EXPECT_EQ(std::string("Input Colorspace"), in->getLabel());
    KnobChoicePtr out = std::dynamic_pointer_cast<KnobChoice>(pair.native->getKnobByName(kOCIOColorSpaceParamOutputSpace));
    ASSERT_TRUE(bool(out));
    EXPECT_EQ(std::string("Output Colorspace"), out->getLabel());

    ASSERT_TRUE(setKnobValue(pair.native, kOCIOColorSpaceParamOutputSpace, kSRGB));
    expectConverts(pair, working, kSRGB, "working to sRGB");
}

TEST_F(NativeOCIOColorSpaceTest, LogPresetsSetTheSpacesOfTheRoles)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(config(getApp())->hasRole(OCIO_NAMESPACE::ROLE_COMPOSITING_LOG));
    const std::string log = IoColorSpace::nameInConfig(config(getApp()), OCIO_NAMESPACE::ROLE_COMPOSITING_LOG);
    const std::string linear = IoColorSpace::nameInConfig(config(getApp()), OCIO_NAMESPACE::ROLE_SCENE_LINEAR);
    ASSERT_FALSE(log.empty());
    ASSERT_FALSE(linear.empty());

    ASSERT_TRUE(setKnobValue(pair.native, kOCIOColorSpaceParamPreset, kOCIOColorSpacePresetLogToLin));
    EXPECT_EQ(log, choiceID(pair.native, kOCIOColorSpaceParamInputSpace));
    EXPECT_EQ(linear, choiceID(pair.native, kOCIOColorSpaceParamOutputSpace));
    EXPECT_EQ(std::string(kOCIOColorSpacePresetLogToLin), choiceID(pair.native, kOCIOColorSpaceParamPreset));
    expectConverts(pair, OCIO_NAMESPACE::ROLE_COMPOSITING_LOG, OCIO_NAMESPACE::ROLE_SCENE_LINEAR, "log to lin");

    ASSERT_TRUE(setKnobValue(pair.native, kOCIOColorSpaceParamPreset, kOCIOColorSpacePresetLinToLog));
    EXPECT_EQ(linear, choiceID(pair.native, kOCIOColorSpaceParamInputSpace));
    EXPECT_EQ(log, choiceID(pair.native, kOCIOColorSpaceParamOutputSpace));
    EXPECT_EQ(std::string(kOCIOColorSpacePresetLinToLog), choiceID(pair.native, kOCIOColorSpaceParamPreset));
    expectConverts(pair, OCIO_NAMESPACE::ROLE_SCENE_LINEAR, OCIO_NAMESPACE::ROLE_COMPOSITING_LOG, "lin to log");
}

TEST_F(NativeOCIOColorSpaceTest, EditingASpaceReturnsThePresetToCustom)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));
    const std::string log = IoColorSpace::nameInConfig(config(getApp()), OCIO_NAMESPACE::ROLE_COMPOSITING_LOG);
    ASSERT_FALSE(log.empty());

    ASSERT_TRUE(setKnobValue(pair.native, kOCIOColorSpaceParamPreset, kOCIOColorSpacePresetLogToLin));
    KnobChoicePtr preset = std::dynamic_pointer_cast<KnobChoice>(pair.native->getKnobByName(kOCIOColorSpaceParamPreset));
    KnobChoicePtr out = std::dynamic_pointer_cast<KnobChoice>(pair.native->getKnobByName(kOCIOColorSpaceParamOutputSpace));
    ASSERT_TRUE(bool(preset));
    ASSERT_TRUE(bool(out));
    EXPECT_EQ(std::string(kOCIOColorSpacePresetLogToLin), preset->getActiveEntry().id);

    const std::vector<ChoiceOption> entries = out->getEntries_mt_safe();
    int srgb = -1;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].id == kSRGB) {
            srgb = (int)i;
        }
    }
    ASSERT_GE(srgb, 0);
    out->setValue(srgb, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);
    EXPECT_EQ(std::string(kOCIOColorSpacePresetCustom), preset->getActiveEntry().id);
    EXPECT_EQ(log, choiceID(pair.native, kOCIOColorSpaceParamInputSpace));
    expectConverts(pair, OCIO_NAMESPACE::ROLE_COMPOSITING_LOG, kSRGB, "log to sRGB");
}
