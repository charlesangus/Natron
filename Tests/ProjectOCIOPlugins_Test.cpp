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

#include <string>

#include <gtest/gtest.h>

#include <ofxColour.h>
#include <ofxNatron.h>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/OfxEffectInstance.h"
#include "Engine/OfxImageEffectInstance.h"
#include "Engine/Project.h"
#include "Engine/WriteNode.h"

NATRON_NAMESPACE_USING

namespace {
const char* const kStudioURI = "ocio://studio-config-v4.0.0_aces-v2.0_ocio-v2.5";
const char* const kCGURI = "ocio://cg-config-v4.0.0_aces-v2.0_ocio-v2.5";

ProjectPtr
project()
{
    return appPTR->getTopLevelInstance()->getProject();
}
} // namespace

class ProjectOCIOPluginsTest
    : public testing::Test {
protected:
    virtual void SetUp()
    {
        project()->reset(false, true);
    }

    virtual void TearDown()
    {
        project()->reset(false, true);
    }

    OFX::Host::ImageEffect::Instance* createWriter()
    {
        CreateNodeArgs args(PLUGINID_OFX_WRITEOIIO, project());
        _node = appPTR->getTopLevelInstance()->createNode(args);
        if (!_node) {
            return NULL;
        }
        WriteNode* writeNode = dynamic_cast<WriteNode*>(_node->getEffectInstance().get());
        NodePtr encoder = writeNode ? writeNode->getEmbeddedWriter() : NodePtr();
        if (!encoder) {
            return NULL;
        }
        OfxEffectInstance* effect = dynamic_cast<OfxEffectInstance*>(encoder->getEffectInstance().get());

        return effect ? effect->effectInstance() : NULL;
    }

    NodePtr _node;
};

TEST_F(ProjectOCIOPluginsTest, InstancePropsReturnTheProjectConfigWorkingSpaceAndFileDefaults)
{
    OFX::Host::ImageEffect::Instance* instance = createWriter();

    ASSERT_TRUE(instance != NULL);
    const OFX::Host::Property::Set& props = instance->getProps();

    EXPECT_EQ(std::string(kStudioURI), props.getStringProperty(kOfxImageEffectPropOCIOConfig, 0));
    EXPECT_EQ(std::string("ACEScg"), props.getStringProperty(NatronOfxImageEffectPropOCIOWorkingColourspace, 0));
    ASSERT_EQ(4, props.getDimension(NatronOfxImageEffectPropOCIOFileColourspaces));
    EXPECT_EQ(project()->getFileColorSpace(eFileColorCategory8Bit), props.getStringProperty(NatronOfxImageEffectPropOCIOFileColourspaces, 0));
    EXPECT_EQ(project()->getFileColorSpace(eFileColorCategory16Bit), props.getStringProperty(NatronOfxImageEffectPropOCIOFileColourspaces, 1));
    EXPECT_EQ(project()->getFileColorSpace(eFileColorCategoryLog), props.getStringProperty(NatronOfxImageEffectPropOCIOFileColourspaces, 2));
    EXPECT_EQ(project()->getFileColorSpace(eFileColorCategoryFloat), props.getStringProperty(NatronOfxImageEffectPropOCIOFileColourspaces, 3));
    EXPECT_EQ(std::string("sRGB Encoded Rec.709 (sRGB)"), props.getStringProperty(NatronOfxImageEffectPropOCIOFileColourspaces, 0));
    EXPECT_EQ(std::string("sRGB Encoded Rec.709 (sRGB)"), props.getStringProperty(NatronOfxImageEffectPropOCIOFileColourspaces, 1));
    EXPECT_EQ(std::string("ACEScct"), props.getStringProperty(NatronOfxImageEffectPropOCIOFileColourspaces, 2));
    EXPECT_EQ(std::string("ACEScg"), props.getStringProperty(NatronOfxImageEffectPropOCIOFileColourspaces, 3));
}

TEST_F(ProjectOCIOPluginsTest, SwitchingTheProjectConfigIsSeenByTheSameInstance)
{
    OFX::Host::ImageEffect::Instance* instance = createWriter();

    ASSERT_TRUE(instance != NULL);
    const OFX::Host::Property::Set& props = instance->getProps();
    ASSERT_EQ(std::string(kStudioURI), props.getStringProperty(kOfxImageEffectPropOCIOConfig, 0));

    KnobChoicePtr config = project()->getKnobByNameAndType<KnobChoice>("ocioConfig");
    ASSERT_TRUE(bool(config));
    config->setValueFromID(kCGURI, 0);

    EXPECT_EQ(std::string(kCGURI), props.getStringProperty(kOfxImageEffectPropOCIOConfig, 0));
    EXPECT_EQ(project()->getWorkingColorSpace(), props.getStringProperty(NatronOfxImageEffectPropOCIOWorkingColourspace, 0));
    EXPECT_EQ(project()->getFileColorSpace(eFileColorCategoryFloat), props.getStringProperty(NatronOfxImageEffectPropOCIOFileColourspaces, 3));
}
