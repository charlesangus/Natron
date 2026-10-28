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
#include <vector>

#include <gtest/gtest.h>

#include <OpenColorIO/OpenColorIO.h>

#include "BaseTest.h"
#include "NativeParity.h"
#include "OcioNodeTestSupport.h"

#include "Engine/AppInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/OCIO/OCIOLookTransform.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING
using namespace OcioNodeTest;

namespace {

OCIO_NAMESPACE::ConstTransformRcPtr
lookTransform(const std::string& src,
              const std::string& dst,
              const std::string& looks)
{
    OCIO_NAMESPACE::LookTransformRcPtr transform = OCIO_NAMESPACE::LookTransform::Create();

    transform->setSrc(src.c_str());
    transform->setDst(dst.c_str());
    transform->setLooks(looks.c_str());

    return transform;
}

} // namespace

class NativeOCIOLookTransformTest
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
        ParityPair pair = makeParityPair(getApp(), PLUGINID_NATRON_OCIOLOOKTRANSFORM, PLUGIN_MAJOR_NATRON_OCIOLOOKTRANSFORM);

        EXPECT_TRUE(bool(pair.native));
        EXPECT_TRUE(pair.native && dynamic_cast<OCIOLookTransform*>(pair.native->getEffectInstance().get()));

        return pair;
    }

    std::string firstLook()
    {
        const OCIO_NAMESPACE::ConstConfigRcPtr c = config(getApp());

        EXPECT_TRUE(bool(c));

        return (c && (c->getNumLooks() > 0)) ? std::string(c->getLookNameByIndex(0)) : std::string();
    }
};

TEST_F(NativeOCIOLookTransformTest, AppliesTheFirstLookBothWays)
{
    const std::string look = firstLook();
    ASSERT_FALSE(look.empty());

    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));

    // The menu lists the config's looks.
    KnobChoicePtr menu = std::dynamic_pointer_cast<KnobChoice>(pair.native->getKnobByName(kOCIOLookTransformParamLook));
    ASSERT_TRUE(bool(menu));
    ASSERT_EQ(config(getApp())->getNumLooks(), (int)menu->getEntries_mt_safe().size());

    ASSERT_TRUE(setKnobValue(pair.native, kOCIOLookTransformParamInputSpace, "ACES2065-1"));
    ASSERT_TRUE(setKnobValue(pair.native, kOCIOLookTransformParamOutputSpace, "ACEScg"));
    ASSERT_TRUE(setKnobValue(pair.native, kOCIOLookTransformParamLook, look));
    expectProcessed(pair.native, pair.source, referenceProcessor(config(getApp()), lookTransform("ACES2065-1", "ACEScg", look)), "forward");

    // The inverse undoes the look from the output back to the input.
    ASSERT_TRUE(setKnobValue(pair.native, kOCIOLookTransformParamDirection, "inverse"));
    expectProcessed(pair.native, pair.source,
                    referenceProcessor(config(getApp()), lookTransform("ACEScg", "ACES2065-1", look), OCIO_NAMESPACE::TRANSFORM_DIR_INVERSE),
                    "inverse");
}

TEST_F(NativeOCIOLookTransformTest, AppendBuildsTheCombination)
{
    const std::string look = firstLook();
    ASSERT_FALSE(look.empty());

    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(setKnobValue(pair.native, kOCIOLookTransformParamLook, look));

    KnobButton* append = dynamic_cast<KnobButton*>(pair.native->getKnobByName(kOCIOLookTransformParamAppend).get());
    ASSERT_TRUE(append != NULL);
    append->trigger();
    EXPECT_EQ(look, stringValue(pair.native, kOCIOLookTransformParamLookCombination));
    append->trigger();
    EXPECT_EQ(look + ", " + look, stringValue(pair.native, kOCIOLookTransformParamLookCombination));

    // The combination renders once Single Look is off: the look forward then inverted is the
    // conversion from the working space to itself.
    ASSERT_TRUE(setKnobValue(pair.native, kOCIOLookTransformParamLookCombination, "+" + look + ", -" + look));
    ASSERT_TRUE(setKnobValues(pair.native, kOCIOLookTransformParamSingleLook, std::vector<double>(1, 0.)));
    const std::string working = getApp()->getProject()->getWorkingColorSpace();
    expectProcessed(pair.native, pair.source,
                    referenceProcessor(config(getApp()), lookTransform(working, working, "+" + look + ", -" + look)), "combination");
}
