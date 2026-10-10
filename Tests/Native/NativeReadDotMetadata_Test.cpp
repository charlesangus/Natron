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

#include <optional>

#include <QString>

#include <gtest/gtest.h>

#include "BaseTest.h"
#include "NativeMetadataTestEffect.h"

#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/Nodes/Metadata/OfxMetadataBridge.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/OfxEffectInstance.h"
#include "Engine/OfxImageEffectInstance.h"
#include "Engine/ViewIdx.h"

#include <ofxImageEffect.h>
#include <ofxMetadata.h>
#include <ofxhClip.h>
#include <ofxhPropertySuite.h>

NATRON_NAMESPACE_USING

namespace {
const char kMetadataViewID[] = "org.openfx.examples.metadataView";

NativeEffectBase*
nativeEffectOf(const NodePtr& node)
{
    return dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());
}

void
setTag(const NodePtr& source,
       int value)
{
    KnobInt* tag = dynamic_cast<KnobInt*>(source->getKnobByName("tag").get());

    ASSERT_TRUE(tag != NULL);
    tag->setValue(value, ViewSpec::all(), 0);
}

class MetadataRef {
public:
    MetadataRef(OFX::Host::ImageEffect::ClipInstance* clip,
                OfxTime time)
        : _set(clip ? clip->getMetadata(time) : NULL)
    {
    }

    ~MetadataRef()
    {
        if (_set) {
            _set->releaseReference();
        }
    }

    OFX::Host::ImageEffect::MetadataSet* get() const
    {
        return _set;
    }

    OFX::Host::ImageEffect::MetadataSet* operator->() const
    {
        return _set;
    }

private:
    MetadataRef(const MetadataRef&);
    MetadataRef& operator=(const MetadataRef&);

    OFX::Host::ImageEffect::MetadataSet* _set;
};

OFX::Host::ImageEffect::ClipInstance*
sourceClipOf(const NodePtr& node)
{
    OfxEffectInstance* ofxEffect = dynamic_cast<OfxEffectInstance*>(node->getEffectInstance().get());

    if (!ofxEffect || !ofxEffect->effectInstance()) {
        return NULL;
    }

    return ofxEffect->effectInstance()->getClip(kOfxImageEffectSimpleSourceClipName);
}

// The keys the OpenFX input clip holds, read back the way a plug-in sees them.
ImageMetadata
receivedBy(const NodePtr& view,
           double time)
{
    MetadataRef received(sourceClipOf(view), time);

    EXPECT_TRUE(received.get() != NULL);

    return received.get() ? OfxMetadataBridge::fromOfxPropertySet(*received.get()) : ImageMetadata();
}
} // namespace

TEST_F(BaseTest, DotMetadataReachesNativeNodeDownstream)
{
    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDMetadataSource));
    NodePtr dot = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
    NodePtr grade = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));

    ASSERT_TRUE(source && dot && grade);

    connectNodes(source, dot, 0, true);
    connectNodes(dot, grade, 0, true);

    NativeEffectBase* gradeEffect = nativeEffectOf(grade);
    NativeEffectBase* sourceEffect = nativeEffectOf(source);

    ASSERT_TRUE(gradeEffect != NULL);
    ASSERT_TRUE(sourceEffect != NULL);

    const ImageMetadata expected = sourceEffect->getOutputMetadata(3., ViewIdx(0));

    ASSERT_FALSE(expected.empty());
    EXPECT_EQ(expected, gradeEffect->getOutputMetadata(3., ViewIdx(0)));
}

TEST_F(BaseTest, DotChainMetadataReachesNativeNodeDownstream)
{
    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDMetadataSource));
    NodePtr dotA = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
    NodePtr dotB = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
    NodePtr grade = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));

    ASSERT_TRUE(source && dotA && dotB && grade);

    connectNodes(source, dotA, 0, true);
    connectNodes(dotA, dotB, 0, true);
    connectNodes(dotB, grade, 0, true);

    NativeEffectBase* gradeEffect = nativeEffectOf(grade);
    NativeEffectBase* sourceEffect = nativeEffectOf(source);

    ASSERT_TRUE(gradeEffect != NULL);
    ASSERT_TRUE(sourceEffect != NULL);

    EXPECT_EQ(sourceEffect->getOutputMetadata(5., ViewIdx(0)), gradeEffect->getOutputMetadata(5., ViewIdx(0)));
}

TEST_F(BaseTest, DotMetadataFollowsKnobChangeOnTheSource)
{
    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDMetadataSource));
    NodePtr dot = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
    NodePtr grade = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));

    ASSERT_TRUE(source && dot && grade);

    connectNodes(source, dot, 0, true);
    connectNodes(dot, grade, 0, true);

    NativeEffectBase* gradeEffect = nativeEffectOf(grade);

    ASSERT_TRUE(gradeEffect != NULL);

    EXPECT_EQ(std::optional<int>(1), gradeEffect->getOutputMetadata(1., ViewIdx(0)).getInt("exr/tag"));

    setTag(source, 42);

    EXPECT_EQ(std::optional<int>(42), gradeEffect->getOutputMetadata(1., ViewIdx(0)).getInt("exr/tag"));
}

TEST_F(BaseTest, DotMetadataFollowsConnectionChangeUpstreamOfTheDot)
{
    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDMetadataSource));
    NodePtr dot = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
    NodePtr grade = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));

    ASSERT_TRUE(source && dot && grade);

    connectNodes(dot, grade, 0, true);

    NativeEffectBase* gradeEffect = nativeEffectOf(grade);

    ASSERT_TRUE(gradeEffect != NULL);
    EXPECT_TRUE(gradeEffect->getOutputMetadata(1., ViewIdx(0)).empty());

    connectNodes(source, dot, 0, true);

    EXPECT_EQ(std::optional<int>(1), gradeEffect->getOutputMetadata(1., ViewIdx(0)).getInt("exr/tag"));

    disconnectNodes(source, dot, true);

    EXPECT_TRUE(gradeEffect->getOutputMetadata(1., ViewIdx(0)).empty());
}

TEST_F(BaseTest, DisconnectedDotGivesNoMetadataToNativeNode)
{
    NodePtr dot = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
    NodePtr grade = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));

    ASSERT_TRUE(dot && grade);

    connectNodes(dot, grade, 0, true);

    NativeEffectBase* gradeEffect = nativeEffectOf(grade);

    ASSERT_TRUE(gradeEffect != NULL);
    EXPECT_TRUE(gradeEffect->getOutputMetadata(1., ViewIdx(0)).empty());
}

TEST_F(BaseTest, DotChainMetadataReachesOfxInputClipExactly)
{
    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDMetadataSource));
    NodePtr dotA = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
    NodePtr dotB = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
    NodePtr view = createNode(QString::fromUtf8(kMetadataViewID));

    ASSERT_TRUE(source && dotA && dotB);
    ASSERT_TRUE(bool(view)) << "node creation failed for " << kMetadataViewID;

    NativeEffectBase* sourceEffect = nativeEffectOf(source);

    ASSERT_TRUE(sourceEffect != NULL);

    connectNodes(source, dotA, 0, true);
    connectNodes(dotA, dotB, 0, true);
    connectNodes(dotB, view, 0, true);

    const double time = 3.;
    const ImageMetadata expected = sourceEffect->getOutputMetadata(time, ViewIdx(0));

    ASSERT_EQ(std::optional<int>(static_cast<int>(time)), expected.getInt(kOfxMetadataKeySourceFrame));
    EXPECT_EQ(expected, receivedBy(view, time));
}

TEST_F(BaseTest, DotMetadataReachesOfxInputClipAfterKnobChangeOnTheSource)
{
    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDMetadataSource));
    NodePtr dot = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
    NodePtr view = createNode(QString::fromUtf8(kMetadataViewID));

    ASSERT_TRUE(source && dot);
    ASSERT_TRUE(bool(view)) << "node creation failed for " << kMetadataViewID;

    connectNodes(source, dot, 0, true);
    connectNodes(dot, view, 0, true);

    EXPECT_EQ(std::optional<int>(1), receivedBy(view, 1.).getInt("exr/tag"));

    setTag(source, 42);

    EXPECT_EQ(std::optional<int>(42), receivedBy(view, 1.).getInt("exr/tag"));
}

TEST_F(BaseTest, DisconnectedDotKeepsTheHostFallbackOnOfxInputClip)
{
    NodePtr dot = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
    NodePtr view = createNode(QString::fromUtf8(kMetadataViewID));

    ASSERT_TRUE(bool(dot));
    ASSERT_TRUE(bool(view)) << "node creation failed for " << kMetadataViewID;

    connectNodes(dot, view, 0, true);

    MetadataRef received(sourceClipOf(view), 4.);

    ASSERT_TRUE(received.get() != NULL);
    EXPECT_TRUE(received->fetchProperty(kOfxMetadataKeyFrameRate) != NULL);
    EXPECT_TRUE(received->fetchProperty("exr/tag") == NULL);
}
