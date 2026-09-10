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
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Format.h"
#include "Engine/Node.h"
#include "Engine/OfxEffectInstance.h"
#include "Engine/OfxHost.h"
#include "Engine/OfxImageEffectInstance.h"
#include "Engine/Plugin.h"
#include "Engine/Project.h"

#include <ofxImageEffect.h>
#include <ofxMetadata.h>
#include <ofxProperty.h>

NATRON_NAMESPACE_USING

namespace {
// Node creation alone doesn't prove the OFX host can resolve the plugin by ID; a plugin
// that failed cache registration but still had a stale Node subclass around could pass
// createNode() and fail everywhere else. Checking both catches that gap.
struct MetadataPluginFixture
    : public BaseTest
{
    void assertPluginInstantiates(const char* pluginID)
    {
        const QString id = QString::fromUtf8(pluginID);

        Plugin* binary = appPTR->getPluginBinary(id, -1, -1, false);
        EXPECT_TRUE(binary != NULL) << "plugin cache lookup failed for " << pluginID;

        NodePtr node = createNode(id);
        ASSERT_TRUE(bool(node)) << "node creation failed for " << pluginID;
    }
};
} // namespace

// Each of the metadataView/Contribute/TimeCode example plugins loads via OFX_PLUGIN_PATH
// and is known to be enumerated by the plugin cache (see tools/ci/smoke_test.py). Neither
// fact implies the host can actually build a working Node around it -- describe/create
// action failures, missing clip declarations, or a param layout the host can't build knobs
// for would all show up only at node-creation time. This is that stronger check.
TEST_F(MetadataPluginFixture, MetadataViewInstantiatesAsNode)
{
    assertPluginInstantiates("org.openfx.examples.metadataView");
}

TEST_F(MetadataPluginFixture, MetadataContributeInstantiatesAsNode)
{
    assertPluginInstantiates("org.openfx.examples.metadataContribute");
}

TEST_F(MetadataPluginFixture, MetadataTimeCodeInstantiatesAsNode)
{
    assertPluginInstantiates("org.openfx.examples.metadataTimeCode");
}

// Support/Library/ofxsMetadata.cpp fetches the metadata suite optionally and quietly no-ops
// when it is absent, so every other test in this file would keep passing even if the host
// never actually vended the suite. This is the one case that would catch that.
TEST_F(MetadataPluginFixture, MetadataAndPropertySuiteAreFetchableFromOfxHost)
{
    const QString id = QString::fromUtf8("org.openfx.examples.metadataView");
    NodePtr node = createNode(id);
    ASSERT_TRUE(bool(node)) << "node creation failed for " << id.toStdString();

    OfxEffectInstance* ofxEffect = dynamic_cast<OfxEffectInstance*>( node->getEffectInstance().get() );
    ASSERT_TRUE(ofxEffect != NULL) << "node's effect instance is not backed by the OFX host";

    // ImageEffectPlugin/PluginHandle keep no back-pointer to the OfxHost that instantiated
    // them, so the host is reached via Natron's AppManager singleton rather than through the
    // node's plugin handle. AppManager only hands out a const OfxHost*, but fetchSuite() does
    // nothing but look suites up, so the const_cast is safe here.
    const Natron::OfxHost* constHost = appPTR->getOFXHost();
    ASSERT_TRUE(constHost != NULL);
    Natron::OfxHost* host = const_cast<Natron::OfxHost*>(constHost);

    EXPECT_TRUE(host->fetchSuite(kOfxPropertySuite, 1) != NULL) << "property suite v1 not vended";
    EXPECT_TRUE(host->fetchSuite(kOfxPropertySuite, 2) != NULL) << "property suite v2 not vended";
    EXPECT_TRUE(host->fetchSuite(kOfxMetadataSuite, 1) != NULL) << "metadata suite not vended";
}

// The base ClipInstance::fetchMetadata() adds nothing of the host's own, so without
// Natron's override an output clip's metadata carries only whatever the plug-in
// contributed -- and Constant contributes nothing. Every key checked here therefore comes
// from the host, and the values are the ones the project and its format imply.
TEST_F(MetadataPluginFixture, OutputClipCarriesHostDerivedMetadata)
{
    const int formatWidth = 320;
    const int formatHeight = 240;
    const double formatPar = 1.;

    Format f(0, 0, formatWidth, formatHeight, "metadataHostKeysFormat", formatPar);
    getApp()->getProject()->setOrAddProjectFormat(f);

    NodePtr node = createNode( QString::fromUtf8("net.sf.openfx.ConstantPlugin") );
    ASSERT_TRUE( bool(node) ) << "node creation failed for net.sf.openfx.ConstantPlugin";

    OfxEffectInstance* ofxEffect = dynamic_cast<OfxEffectInstance*>( node->getEffectInstance().get() );
    ASSERT_TRUE(ofxEffect != NULL) << "node's effect instance is not backed by the OFX host";

    OFX::Host::ImageEffect::ClipInstance* output = ofxEffect->effectInstance()->getClip(kOfxImageEffectOutputClipName);
    ASSERT_TRUE(output != NULL) << "the effect has no output clip";

    OFX::Host::ImageEffect::MetadataSet* metadata = output->getMetadata(1.);
    ASSERT_TRUE(metadata != NULL);

    EXPECT_DOUBLE_EQ( getApp()->getProjectFrameRate(), metadata->getDoubleProperty(kOfxMetadataKeyFrameRate) );
    EXPECT_DOUBLE_EQ( formatPar, metadata->getDoubleProperty(kOfxMetadataKeyPixelAspect) );
    EXPECT_EQ( formatWidth, metadata->getIntProperty(kOfxMetadataKeyWidth) );
    EXPECT_EQ( formatHeight, metadata->getIntProperty(kOfxMetadataKeyHeight) );
    EXPECT_EQ( 1, metadata->getIntProperty(kOfxMetadataKeySourceFrame) );

    metadata->releaseReference();

    // The cache holds a reference of its own, so releasing the caller's must leave the set
    // alive: asking again returns that same set rather than a fresh one.
    OFX::Host::ImageEffect::MetadataSet* again = output->getMetadata(1.);
    ASSERT_TRUE(again != NULL);
    EXPECT_EQ(metadata, again);
    EXPECT_EQ( formatWidth, again->getIntProperty(kOfxMetadataKeyWidth) );

    again->releaseReference();
}

// An input clip carries the metadata of the image handed to it, which is what the node
// connected to it puts out. Comparing against the upstream output clip rather than against
// literal values is the point: the two must agree whatever the project happens to be set to.
TEST_F(MetadataPluginFixture, InputClipCarriesUpstreamOutputMetadata)
{
    Format projectFormat;
    getApp()->getProject()->getProjectDefaultFormat(&projectFormat);

    NodePtr constant = createNode( QString::fromUtf8("net.sf.openfx.ConstantPlugin") );
    ASSERT_TRUE( bool(constant) ) << "node creation failed for net.sf.openfx.ConstantPlugin";

    NodePtr grade = createNode( QString::fromUtf8("net.sf.openfx.GradePlugin") );
    ASSERT_TRUE( bool(grade) ) << "node creation failed for net.sf.openfx.GradePlugin";

    connectNodes(constant, grade, 0, true);

    OfxEffectInstance* constantEffect = dynamic_cast<OfxEffectInstance*>( constant->getEffectInstance().get() );
    ASSERT_TRUE(constantEffect != NULL) << "Constant is not backed by the OFX host";

    OfxEffectInstance* gradeEffect = dynamic_cast<OfxEffectInstance*>( grade->getEffectInstance().get() );
    ASSERT_TRUE(gradeEffect != NULL) << "Grade is not backed by the OFX host";

    OFX::Host::ImageEffect::ClipInstance* constantOutput = constantEffect->effectInstance()->getClip(kOfxImageEffectOutputClipName);
    ASSERT_TRUE(constantOutput != NULL) << "Constant has no output clip";

    OFX::Host::ImageEffect::ClipInstance* gradeSource = gradeEffect->effectInstance()->getClip(kOfxImageEffectSimpleSourceClipName);
    ASSERT_TRUE(gradeSource != NULL) << "Grade has no " << kOfxImageEffectSimpleSourceClipName << " clip";

    OFX::Host::ImageEffect::MetadataSet* upstream = constantOutput->getMetadata(1.);
    ASSERT_TRUE(upstream != NULL);

    OFX::Host::ImageEffect::MetadataSet* source = gradeSource->getMetadata(1.);
    ASSERT_TRUE(source != NULL);

    // the keys are copied out of the upstream set, not aliased to it, so the two clips hold
    // sets of their own that happen to carry equal values
    EXPECT_NE(upstream, source);
    EXPECT_DOUBLE_EQ( upstream->getDoubleProperty(kOfxMetadataKeyFrameRate), source->getDoubleProperty(kOfxMetadataKeyFrameRate) );
    EXPECT_EQ( upstream->getIntProperty(kOfxMetadataKeyWidth), source->getIntProperty(kOfxMetadataKeyWidth) );
    EXPECT_EQ( upstream->getIntProperty(kOfxMetadataKeyHeight), source->getIntProperty(kOfxMetadataKeyHeight) );

    // Constant generates the project format, so the values that reached Grade are the ones
    // the project implies rather than whatever a default constructed set would carry
    EXPECT_DOUBLE_EQ( getApp()->getProjectFrameRate(), source->getDoubleProperty(kOfxMetadataKeyFrameRate) );
    EXPECT_EQ( projectFormat.width(), source->getIntProperty(kOfxMetadataKeyWidth) );

    source->releaseReference();
    upstream->releaseReference();
}

// With nothing connected there is no upstream clip to copy from, and a plug-in asking its
// input for metadata must still get the host's answer rather than an empty set or a crash.
TEST_F(MetadataPluginFixture, DisconnectedInputClipFallsBackToHostDerivedMetadata)
{
    Format projectFormat;
    getApp()->getProject()->getProjectDefaultFormat(&projectFormat);

    NodePtr constant = createNode( QString::fromUtf8("net.sf.openfx.ConstantPlugin") );
    ASSERT_TRUE( bool(constant) ) << "node creation failed for net.sf.openfx.ConstantPlugin";

    NodePtr grade = createNode( QString::fromUtf8("net.sf.openfx.GradePlugin") );
    ASSERT_TRUE( bool(grade) ) << "node creation failed for net.sf.openfx.GradePlugin";

    connectNodes(constant, grade, 0, true);
    disconnectNodes(constant, grade, true);

    OfxEffectInstance* gradeEffect = dynamic_cast<OfxEffectInstance*>( grade->getEffectInstance().get() );
    ASSERT_TRUE(gradeEffect != NULL) << "Grade is not backed by the OFX host";

    OFX::Host::ImageEffect::ClipInstance* gradeSource = gradeEffect->effectInstance()->getClip(kOfxImageEffectSimpleSourceClipName);
    ASSERT_TRUE(gradeSource != NULL) << "Grade has no " << kOfxImageEffectSimpleSourceClipName << " clip";

    OFX::Host::ImageEffect::MetadataSet* source = gradeSource->getMetadata(1.);
    ASSERT_TRUE(source != NULL);

    EXPECT_DOUBLE_EQ( getApp()->getProjectFrameRate(), source->getDoubleProperty(kOfxMetadataKeyFrameRate) );
    EXPECT_EQ( projectFormat.width(), source->getIntProperty(kOfxMetadataKeyWidth) );
    EXPECT_EQ( projectFormat.height(), source->getIntProperty(kOfxMetadataKeyHeight) );
    EXPECT_EQ( 1, source->getIntProperty(kOfxMetadataKeySourceFrame) );

    source->releaseReference();
}
