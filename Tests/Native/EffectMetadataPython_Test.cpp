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

#include <sstream>
#include <string>
#include <vector>

#include <QString>

#include <gtest/gtest.h>

#include "BaseTest.h"
#include "NativeMetadataTestEffect.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Merge/Switch.h"
#include "Engine/Nodes/Metadata/EffectMetadata.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING
NATRON_PYTHON_NAMESPACE_USING

class EffectMetadataPythonTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        source = createNode(QString::fromUtf8(kTestPluginIDMetadataSource));
        ASSERT_TRUE(bool(source));

        MetadataSourceTestEffect* sourceEffect = dynamic_cast<MetadataSourceTestEffect*>(source->getEffectInstance().get());

        ASSERT_TRUE(sourceEffect != NULL);
        ImageMetadata extra;
        extra.setString("exr/owner", "natron");
        extra.setDouble("exr/gain", 0.5);
        extra.setIntVector("dpx/size", std::vector<int>(2, 4));
        extra.setDoubleVector("exr/chroma", std::vector<double>(2, 0.25));
        sourceEffect->setExtraMetadata(extra);
        nodeVar = getApp()->getAppIDString() + "." + source->getFullyQualifiedName();
    }

    void expectScript(const std::string& body)
    {
        std::string error, output;

        EXPECT_TRUE(interpretPythonScript(body, &error, &output)) << error;
    }

    NodePtr source;
    std::string nodeVar;
};

TEST_F(EffectMetadataPythonTest, HelperReadsANativeNodeAndIgnoresNull)
{
    const ImageMetadata metadata = getEffectOutputMetadata(source->getEffectInstance(), 3., ViewIdx(0));

    EXPECT_EQ(std::string("natron"), metadata.getString("exr/owner").value_or(""));
    EXPECT_EQ(3, metadata.getInt("ofx/frame").value_or(-1));
    EXPECT_TRUE(getEffectOutputMetadata(EffectInstancePtr(), 3., ViewIdx(0)).empty());
}

TEST_F(EffectMetadataPythonTest, MetadataReturnsTypedValues)
{
    expectScript("n = " + nodeVar + "\n" + "assert n.metadata('exr/owner', 1) == 'natron'\n"
                                           "assert type(n.metadata('exr/owner', 1)) is str\n"
                                           "assert type(n.metadata('exr/tag', 1)) is int and n.metadata('exr/tag', 1) == 1\n"
                                           "assert type(n.metadata('exr/gain', 1)) is float and n.metadata('exr/gain', 1) == 0.5\n"
                                           "assert n.metadata('dpx/size', 1) == [4, 4]\n"
                                           "assert n.metadata('exr/chroma', 1) == [0.25, 0.25]\n");
}

TEST_F(EffectMetadataPythonTest, AbsentKeyIsNone)
{
    expectScript("assert " + nodeVar + ".metadata('exr/missing', 1) is None\n");
}

TEST_F(EffectMetadataPythonTest, FrameArgumentSelectsTheFrame)
{
    expectScript("n = " + nodeVar + "\n" + "assert n.metadata('ofx/frame', 2) == 2\n"
                                           "assert n.metadata('ofx/frame', 7, 0) == 7\n");
}

TEST_F(EffectMetadataPythonTest, FrameDefaultsToTheCurrentFrame)
{
    expectScript("n = " + nodeVar + "\n" + "assert n.metadata('ofx/frame') == n.getCurrentTime()\n"
                                           "assert n.metadata('ofx/frame', n.getCurrentTime()) == n.metadata('ofx/frame')\n");
}

TEST_F(EffectMetadataPythonTest, KeysAreSortedAndComplete)
{
    expectScript("n = " + nodeVar + "\n" + "keys = n.metadataKeys(1)\n"
                                           "assert keys == sorted(keys)\n"
                                           "assert keys == n.metadataKeys(1, 0)\n"
                                           "for k in ('dpx/size', 'exr/chroma', 'exr/gain', 'exr/origin', 'exr/owner', 'exr/tag', 'ofx/frame'):\n"
                                           "    assert k in keys, k\n"
                                           "assert 'exr/missing' not in keys\n"
                                           "assert n.metadataKeys() == n.metadataKeys(n.getCurrentTime())\n");
}

TEST_F(EffectMetadataPythonTest, StreamGettersFollowTheFrameAndView)
{
    NodePtr sourceA = createNode(QString::fromUtf8(kTestPluginIDMetadataSource));
    NodePtr sourceB = createNode(QString::fromUtf8(kTestPluginIDMetadataSource));
    NodePtr switchNode = createNode(QString::fromUtf8(PLUGINID_NATRON_SWITCH));
    ASSERT_TRUE(sourceA && sourceB && switchNode);
    connectNodes(sourceA, switchNode, 0, true);
    connectNodes(sourceB, switchNode, 1, true);
    MetadataSourceTestEffect* effectA = dynamic_cast<MetadataSourceTestEffect*>(sourceA->getEffectInstance().get());
    MetadataSourceTestEffect* effectB = dynamic_cast<MetadataSourceTestEffect*>(sourceB->getEffectInstance().get());
    ASSERT_TRUE(effectA != NULL && effectB != NULL);
    // Set after connecting: the connection check compares the inputs' aspect ratios.
    effectA->getFormatKnob()->setValue(40, ViewSpec::all(), 0);
    effectA->getFormatKnob()->setValue(30, ViewSpec::all(), 1);
    effectA->getBitDepthKnob()->setValue(static_cast<int>(eImageBitDepthByte), ViewSpec::all(), 0);
    effectB->getFormatKnob()->setValue(80, ViewSpec::all(), 0);
    effectB->getFormatKnob()->setValue(60, ViewSpec::all(), 1);
    effectB->getParKnob()->setValue(2., ViewSpec::all(), 0);
    KnobIntPtr which = std::dynamic_pointer_cast<KnobInt>(switchNode->getKnobByName(kSwitchParamWhich));
    ASSERT_TRUE(bool(which));
    which->setValueAtTime(1, 0, ViewSpec::all(), 0);
    which->setValueAtTime(2, 1, ViewSpec::all(), 0);

    const NodeMetadataConstPtr atF1 = switchNode->getEffectInstance()->getMetadata(1., ViewIdx(0));
    const NodeMetadataConstPtr atF2 = switchNode->getEffectInstance()->getMetadata(2., ViewIdx(0));
    ASSERT_NE(nullptr, atF1);
    ASSERT_NE(nullptr, atF2);
    ASSERT_EQ(RectI(0, 0, 40, 30), atF1->getOutputFormat());
    ASSERT_EQ(RectI(0, 0, 80, 60), atF2->getOutputFormat());
    ASSERT_EQ(1., atF1->getPixelAspectRatio(-1));
    ASSERT_EQ(2., atF2->getPixelAspectRatio(-1));
    ASSERT_NE(atF1->getBitDepth(-1), atF2->getBitDepth(-1));

    std::stringstream script;
    script << "import NatronEngine\n"
           << "n = " << getApp()->getAppIDString() << "." << switchNode->getFullyQualifiedName() << "\n"
           << "f1 = n.getOutputFormat(1, 0)\n"
           << "f2 = n.getOutputFormat(2, 0)\n"
           << "assert (f1.width(), f1.height()) == (40, 30), (f1.width(), f1.height())\n"
           << "assert (f2.width(), f2.height()) == (80, 60), (f2.width(), f2.height())\n"
           << "assert n.getPixelAspectRatio(1, 0) == 1.0\n"
           << "assert n.getPixelAspectRatio(2, 0) == 2.0\n"
           << "assert n.getBitDepth(1, 0) == NatronEngine.Natron.ImageBitDepthEnum.eImageBitDepthByte\n"
           << "assert n.getBitDepth(2, 0) == NatronEngine.Natron.ImageBitDepthEnum.eImageBitDepthFloat\n"
           << "assert n.getFrameRate(1, 0) == " << atF1->getOutputFrameRate() << "\n"
           << "assert n.getFrameRate(2, 0) == " << atF2->getOutputFrameRate() << "\n"
           // The stream getters have no frame default: a node's stream can change from one frame to the next.
           << "for name in ('getOutputFormat', 'getFrameRate', 'getPixelAspectRatio', 'getBitDepth'):\n"
           << "    for args in ((), (1,)):\n"
           << "        try:\n"
           << "            getattr(n, name)(*args)\n"
           << "        except TypeError:\n"
           << "            continue\n"
           << "        raise AssertionError(name + str(args) + ' accepted')\n";
    expectScript(script.str());
}
