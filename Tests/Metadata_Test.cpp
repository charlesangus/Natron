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

#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include <SequenceParsing.h>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/OfxEffectInstance.h"
#include "Engine/OfxHost.h"
#include "Engine/OfxImageEffectInstance.h"
#include "Engine/OutputEffectInstance.h"
#include "Engine/Plugin.h"
#include "Engine/Project.h"
#include "Engine/ReadNode.h"

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

const char kMetadataContributeID[] = "org.openfx.examples.metadataContribute";
const char kMetadataViewID[] = "org.openfx.examples.metadataView";

// the param metadataContribute reads, and the key it publishes that param's value under
const char kContributeNoteParam[] = "note";
const char kContributeNoteKey[] = "org.openfx.examples.metadataContribute.note";

OFX::Host::ImageEffect::ClipInstance*
clipOf(const NodePtr& node,
       const char* clipName)
{
    OfxEffectInstance* ofxEffect = dynamic_cast<OfxEffectInstance*>( node->getEffectInstance().get() );

    if ( !ofxEffect || !ofxEffect->effectInstance() ) {
        return NULL;
    }

    return ofxEffect->effectInstance()->getClip(clipName);
}

void
setContributedNote(const NodePtr& node,
                   const std::string& note)
{
    KnobString* knob = dynamic_cast<KnobString*>( node->getKnobByName(kContributeNoteParam).get() );

    ASSERT_TRUE(knob != NULL) << "metadataContribute has no " << kContributeNoteParam << " param";
    knob->setValue(note);
}
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

// ofx/filepath is the one standard key whose value has to change from frame to frame, and a
// reader is the only node that knows it. Rendering a real two-frame sequence and reading it
// back is what makes that testable: a host that published the unexpanded sequence pattern, or
// cached one metadata set for the whole clip, would hand both frames the same path.
//
// The project format is deliberately left alone. setOrAddProjectFormat() only takes effect the
// first time it is called in a process and the app instance is shared across the suite, so a
// case that set its own format and asserted against it would pass alone and fail in a full run.
// Nothing here depends on the image size.
TEST_F(MetadataPluginFixture, ReaderOutputClipCarriesPerFrameFileMetadata)
{
    NodePtr generator = createNode( QString::fromUtf8(PLUGINID_OFX_CONSTANT) );
    NodePtr writer = createNode(_writeOIIOPluginID);
    ASSERT_TRUE( bool(generator) && bool(writer) );

    connectNodes(generator, writer, 0, true);

    KnobChoice* bitDepth = dynamic_cast<KnobChoice*>( writer->getKnobByName("bitDepth").get() );
    ASSERT_TRUE(bitDepth != NULL);
    bitDepth->setValueFromID("32f", 0);

    KnobChoice* compression = dynamic_cast<KnobChoice*>( writer->getKnobByName("compression").get() );
    ASSERT_TRUE(compression != NULL);
    compression->setValueFromID("none", 0);

    QTemporaryDir tmp;
    ASSERT_TRUE( tmp.isValid() );
    const std::string pattern = ( tmp.path() + QLatin1String("/readerMetadata.####.exr") ).toStdString();
    writer->setOutputFilesForWriter(pattern);

    const int firstFrame = 1;
    const int lastFrame = 2;

    OutputEffectInstance* writerEffect = dynamic_cast<OutputEffectInstance*>( writer->getEffectInstance().get() );
    ASSERT_TRUE(writerEffect != NULL);

    std::list<AppInstance::RenderWork> works;
    works.push_back( AppInstance::RenderWork(writerEffect, firstFrame, lastFrame, 1, false) );
    getApp()->startWritersRendering(false, works);

    const std::vector<std::string>& viewNames = getApp()->getProject()->getProjectViewNames();
    std::vector<std::string> renderedPaths;
    for (int frame = firstFrame; frame <= lastFrame; ++frame) {
        const std::string path = SequenceParsing::generateFileNameFromPattern(pattern, viewNames, frame, 0);
        ASSERT_TRUE( QFile::exists( QString::fromStdString(path) ) ) << "frame " << frame << " was not rendered: " << path;
        renderedPaths.push_back(path);
    }

    CreateNodeArgs args( _readOIIOPluginID.toStdString(), getApp()->getProject() );
    args.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, pattern);
    NodePtr reader = getApp()->createNode(args);
    ASSERT_TRUE( bool(reader) ) << "node creation failed for " << _readOIIOPluginID.toStdString();

    // a bundled reader is created as a Read container holding the decoder, and the clip that
    // carries the metadata belongs to the decoder
    ReadNode* readNode = dynamic_cast<ReadNode*>( reader->getEffectInstance().get() );
    ASSERT_TRUE(readNode != NULL) << "the reader is not backed by a Read container";

    NodePtr decoder = readNode->getEmbeddedReader();
    ASSERT_TRUE( bool(decoder) ) << "no decoder was created for the rendered sequence";

    OfxEffectInstance* ofxEffect = dynamic_cast<OfxEffectInstance*>( decoder->getEffectInstance().get() );
    ASSERT_TRUE(ofxEffect != NULL) << "the decoder is not backed by the OFX host";

    OFX::Host::ImageEffect::ClipInstance* output = ofxEffect->effectInstance()->getClip(kOfxImageEffectOutputClipName);
    ASSERT_TRUE(output != NULL) << "the decoder has no output clip";

    OFX::Host::ImageEffect::MetadataSet* first = output->getMetadata(firstFrame);
    ASSERT_TRUE(first != NULL);

    OFX::Host::ImageEffect::MetadataSet* second = output->getMetadata(lastFrame);
    ASSERT_TRUE(second != NULL);

    ASSERT_TRUE(first->fetchProperty(kOfxMetadataKeyFilePath) != NULL) << "the reader published no " << kOfxMetadataKeyFilePath;
    ASSERT_TRUE(second->fetchProperty(kOfxMetadataKeyFilePath) != NULL) << "the reader published no " << kOfxMetadataKeyFilePath;

    const std::string firstPath = first->getStringProperty(kOfxMetadataKeyFilePath);
    const std::string secondPath = second->getStringProperty(kOfxMetadataKeyFilePath);

    EXPECT_EQ(renderedPaths[0], firstPath);
    EXPECT_EQ(renderedPaths[1], secondPath);
    EXPECT_NE(firstPath, secondPath) << "both frames claim the same source file: " << firstPath;

    EXPECT_GT( first->getDoubleProperty(kOfxMetadataKeyFileSize), 0. );
    EXPECT_GT( second->getDoubleProperty(kOfxMetadataKeyFileSize), 0. );
    EXPECT_GT( first->getDoubleProperty(kOfxMetadataKeyMTime), 0. );

    second->releaseReference();
    first->releaseReference();

    for (std::size_t i = 0; i < renderedPaths.size(); ++i) {
        QFile::remove( QString::fromStdString(renderedPaths[i]) );
    }
} // TEST_F(MetadataPluginFixture, ReaderOutputClipCarriesPerFrameFileMetadata)

// A node downstream of a change used to keep serving the metadata it had derived beforehand,
// because nothing dropped the per clip cache. Here B is read, the contribution A makes is
// changed, and B is read again -- B's own params and inputs are never touched in between, so
// only an invalidation that reached B from A can make the new value show up.
TEST_F(MetadataPluginFixture, DownstreamMetadataFollowsAnUpstreamParamChange)
{
    NodePtr contribute = createNode( QString::fromUtf8(kMetadataContributeID) );
    NodePtr view = createNode( QString::fromUtf8(kMetadataViewID) );
    ASSERT_TRUE( bool(contribute) ) << "node creation failed for " << kMetadataContributeID;
    ASSERT_TRUE( bool(view) ) << "node creation failed for " << kMetadataViewID;

    connectNodes(contribute, view, 0, true);

    OFX::Host::ImageEffect::ClipInstance* viewOutput = clipOf(view, kOfxImageEffectOutputClipName);
    ASSERT_TRUE(viewOutput != NULL) << "the downstream node has no output clip";

    setContributedNote(contribute, "before");

    OFX::Host::ImageEffect::MetadataSet* first = viewOutput->getMetadata(1.);
    ASSERT_TRUE(first != NULL);
    ASSERT_TRUE(first->fetchProperty(kContributeNoteKey) != NULL) << "the upstream contribution never reached the downstream output clip";
    EXPECT_EQ( std::string("before"), first->getStringProperty(kContributeNoteKey) );
    first->releaseReference();

    setContributedNote(contribute, "after");

    OFX::Host::ImageEffect::MetadataSet* second = viewOutput->getMetadata(1.);
    ASSERT_TRUE(second != NULL);
    EXPECT_EQ( std::string("after"), second->getStringProperty(kContributeNoteKey) );

    second->releaseReference();
}

// The node upstream here is native, so the input clip has no upstream OFX output clip to copy
// and derives its keys from the host instead. The change made is one no OFX param change can
// stand in for: the project's own frame rate, which reaches the node without any hash moving.
TEST_F(MetadataPluginFixture, NativeUpstreamNodeChangeReachesTheOfxInputClip)
{
    NodePtr dot = createNode( QString::fromUtf8(PLUGINID_NATRON_DOT) );
    NodePtr view = createNode( QString::fromUtf8(kMetadataViewID) );
    ASSERT_TRUE( bool(dot) ) << "node creation failed for " << PLUGINID_NATRON_DOT;
    ASSERT_TRUE( bool(view) ) << "node creation failed for " << kMetadataViewID;
    ASSERT_TRUE( dynamic_cast<OfxEffectInstance*>( dot->getEffectInstance().get() ) == NULL ) << "the upstream node is not native";

    connectNodes(dot, view, 0, true);

    OFX::Host::ImageEffect::ClipInstance* source = clipOf(view, kOfxImageEffectSimpleSourceClipName);
    ASSERT_TRUE(source != NULL) << "the downstream node has no " << kOfxImageEffectSimpleSourceClipName << " clip";

    KnobDouble* projectFrameRate = dynamic_cast<KnobDouble*>( getApp()->getProject()->getKnobByName("frameRate").get() );
    ASSERT_TRUE(projectFrameRate != NULL) << "the project has no frame rate param";

    const double originalFrameRate = getApp()->getProjectFrameRate();

    Format projectFormat;
    getApp()->getProject()->getProjectDefaultFormat(&projectFormat);

    OFX::Host::ImageEffect::MetadataSet* before = source->getMetadata(1.);
    ASSERT_TRUE(before != NULL);
    EXPECT_DOUBLE_EQ( originalFrameRate, before->getDoubleProperty(kOfxMetadataKeyFrameRate) );
    EXPECT_EQ( projectFormat.width(), before->getIntProperty(kOfxMetadataKeyWidth) );
    EXPECT_DOUBLE_EQ( projectFormat.getPixelAspectRatio(), before->getDoubleProperty(kOfxMetadataKeyPixelAspect) );
    before->releaseReference();

    const double changedFrameRate = originalFrameRate + 12.;
    projectFrameRate->setValue(changedFrameRate);
    ASSERT_DOUBLE_EQ( changedFrameRate, getApp()->getProjectFrameRate() );

    OFX::Host::ImageEffect::MetadataSet* after = source->getMetadata(1.);
    ASSERT_TRUE(after != NULL);
    EXPECT_DOUBLE_EQ( changedFrameRate, after->getDoubleProperty(kOfxMetadataKeyFrameRate) );

    // read live rather than asserted against literals: setOrAddProjectFormat() is a no-op
    // after the first call in a process and the app instance is shared by the whole suite
    Format liveFormat;
    getApp()->getProject()->getProjectDefaultFormat(&liveFormat);
    EXPECT_EQ( liveFormat.width(), after->getIntProperty(kOfxMetadataKeyWidth) );
    EXPECT_DOUBLE_EQ( liveFormat.getPixelAspectRatio(), after->getDoubleProperty(kOfxMetadataKeyPixelAspect) );
    after->releaseReference();

    // the project outlives this case, so it is left as it was found
    projectFrameRate->setValue(originalFrameRate);
}

// An input clip's metadata is a copy of what the node connected to it puts out, so pointing the
// input at a different node has to drop that copy just as a change in the node itself would.
TEST_F(MetadataPluginFixture, ReconnectingAnInputRepointsTheMetadata)
{
    NodePtr firstUpstream = createNode( QString::fromUtf8(kMetadataContributeID) );
    NodePtr secondUpstream = createNode( QString::fromUtf8(kMetadataContributeID) );
    NodePtr view = createNode( QString::fromUtf8(kMetadataViewID) );
    ASSERT_TRUE( bool(firstUpstream) && bool(secondUpstream) ) << "node creation failed for " << kMetadataContributeID;
    ASSERT_TRUE( bool(view) ) << "node creation failed for " << kMetadataViewID;

    setContributedNote(firstUpstream, "from the first");
    setContributedNote(secondUpstream, "from the second");

    connectNodes(firstUpstream, view, 0, true);

    OFX::Host::ImageEffect::ClipInstance* source = clipOf(view, kOfxImageEffectSimpleSourceClipName);
    ASSERT_TRUE(source != NULL) << "the downstream node has no " << kOfxImageEffectSimpleSourceClipName << " clip";

    OFX::Host::ImageEffect::MetadataSet* fromFirst = source->getMetadata(1.);
    ASSERT_TRUE(fromFirst != NULL);
    ASSERT_TRUE(fromFirst->fetchProperty(kContributeNoteKey) != NULL) << "the upstream contribution never reached the input clip";
    EXPECT_EQ( std::string("from the first"), fromFirst->getStringProperty(kContributeNoteKey) );
    fromFirst->releaseReference();

    disconnectNodes(firstUpstream, view, true);
    connectNodes(secondUpstream, view, 0, true);

    OFX::Host::ImageEffect::MetadataSet* fromSecond = source->getMetadata(1.);
    ASSERT_TRUE(fromSecond != NULL);
    EXPECT_EQ( std::string("from the second"), fromSecond->getStringProperty(kContributeNoteKey) );

    fromSecond->releaseReference();
}

// The guard against the opposite failure. getOutputMetadata() allocates a fresh MetadataSet for
// every run of the plug-in's get metadata action and caches that one, and the reference taken
// on the first read keeps it alive, so a second run could not hand its address back. Identical
// pointers across two reads with nothing changed in between therefore say the action ran once.
TEST_F(MetadataPluginFixture, UnchangedStateDoesNotReRunTheGetMetadataAction)
{
    NodePtr contribute = createNode( QString::fromUtf8(kMetadataContributeID) );
    NodePtr view = createNode( QString::fromUtf8(kMetadataViewID) );
    ASSERT_TRUE( bool(contribute) ) << "node creation failed for " << kMetadataContributeID;
    ASSERT_TRUE( bool(view) ) << "node creation failed for " << kMetadataViewID;

    connectNodes(contribute, view, 0, true);
    setContributedNote(contribute, "stable");

    OFX::Host::ImageEffect::ClipInstance* contributeOutput = clipOf(contribute, kOfxImageEffectOutputClipName);
    OFX::Host::ImageEffect::ClipInstance* viewSource = clipOf(view, kOfxImageEffectSimpleSourceClipName);
    OFX::Host::ImageEffect::ClipInstance* viewOutput = clipOf(view, kOfxImageEffectOutputClipName);
    ASSERT_TRUE(contributeOutput != NULL);
    ASSERT_TRUE(viewSource != NULL);
    ASSERT_TRUE(viewOutput != NULL);

    OFX::Host::ImageEffect::MetadataSet* firstContribute = contributeOutput->getMetadata(1.);
    OFX::Host::ImageEffect::MetadataSet* firstSource = viewSource->getMetadata(1.);
    OFX::Host::ImageEffect::MetadataSet* firstView = viewOutput->getMetadata(1.);
    ASSERT_TRUE(firstContribute != NULL);
    ASSERT_TRUE(firstSource != NULL);
    ASSERT_TRUE(firstView != NULL);

    OFX::Host::ImageEffect::MetadataSet* secondContribute = contributeOutput->getMetadata(1.);
    OFX::Host::ImageEffect::MetadataSet* secondSource = viewSource->getMetadata(1.);
    OFX::Host::ImageEffect::MetadataSet* secondView = viewOutput->getMetadata(1.);
    ASSERT_TRUE(secondContribute != NULL);
    ASSERT_TRUE(secondSource != NULL);
    ASSERT_TRUE(secondView != NULL);

    EXPECT_EQ(firstContribute, secondContribute) << "the upstream get metadata action ran a second time";
    EXPECT_EQ(firstSource, secondSource) << "the input clip's cached copy was dropped for nothing";
    EXPECT_EQ(firstView, secondView) << "the downstream get metadata action ran a second time";
    EXPECT_EQ( std::string("stable"), secondView->getStringProperty(kContributeNoteKey) );

    secondView->releaseReference();
    secondSource->releaseReference();
    secondContribute->releaseReference();
    firstView->releaseReference();
    firstSource->releaseReference();
    firstContribute->releaseReference();
}
