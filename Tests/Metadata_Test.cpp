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

#include <atomic>
#include <list>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QFile>
#include <QString>
#include <QTemporaryDir>
#include <QThread>

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
    : public BaseTest {
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
const char kMetadataPrintID[] = "org.openfx.examples.metadataPrint";
const char kMetadataTimeCodeID[] = "org.openfx.examples.metadataTimeCode";
const char kMetadataViewID[] = "org.openfx.examples.metadataView";

// the param metadataContribute reads, and the key it publishes that param's value under
const char kContributeNoteParam[] = "note";
const char kContributeNoteKey[] = "org.openfx.examples.metadataContribute.note";

// every key metadataContribute publishes on each call. ofx/framerate is deliberately one of
// them: the host publishes that key for every clip too, so what a node downstream reads back
// for it is what says whose value wins.
const char* const kContributedKeys[] = {
    kContributeNoteKey,
    "org.openfx.examples.metadataContribute.revision",
    "org.openfx.examples.metadataContribute.quality",
    "org.openfx.examples.metadataContribute.tags",
    "org.openfx.examples.metadataContribute.renderRegion",
    "org.openfx.examples.metadataContribute.weights",
    kOfxMetadataKeyFrameRate
};

const double kContributedFrameRate = 30.;

OFX::Host::ImageEffect::ClipInstance*
clipOf(const NodePtr& node,
       const char* clipName)
{
    OfxEffectInstance* ofxEffect = dynamic_cast<OfxEffectInstance*>(node->getEffectInstance().get());

    if (!ofxEffect || !ofxEffect->effectInstance()) {
        return NULL;
    }

    return ofxEffect->effectInstance()->getClip(clipName);
}

void
setContributedNote(const NodePtr& node,
                   const std::string& note)
{
    KnobString* knob = dynamic_cast<KnobString*>(node->getKnobByName(kContributeNoteParam).get());

    ASSERT_TRUE(knob != NULL) << "metadataContribute has no " << kContributeNoteParam << " param";
    knob->setValue(note);
}

// Every key of a clip's metadata at one time, with each key's values flattened to text. Two
// reads that agree as whole maps agree on the key set and on every value, so a lost key and a
// key whose values came back half-written both show up as a plain inequality.
typedef std::map<std::string, std::string> MetadataSnapshot;

MetadataSnapshot
snapshotMetadata(OFX::Host::ImageEffect::ClipInstance* clip,
                 OfxTime time)
{
    MetadataSnapshot snapshot;
    OFX::Host::ImageEffect::MetadataSet* set = clip->getMetadata(time);

    if (!set) {
        return snapshot;
    }

    const OFX::Host::Property::PropertyMap& props = set->getProperties();
    for (OFX::Host::Property::PropertyMap::const_iterator it = props.begin(); it != props.end(); ++it) {
        std::ostringstream value;
        const int dimension = it->second->getDimension();

        for (int i = 0; i < dimension; ++i) {
            if (i > 0) {
                value << ",";
            }
            switch (it->second->getType()) {
            case OFX::Host::Property::eInt:
                value << set->getIntProperty(it->first, i);
                break;
            case OFX::Host::Property::eDouble:
                value << set->getDoubleProperty(it->first, i);
                break;
            case OFX::Host::Property::eString:
                value << set->getStringProperty(it->first, i);
                break;
            default:
                value << "<unreadable>";
                break;
            }
        }
        snapshot[it->first] = value.str();
    }

    set->releaseReference();

    return snapshot;
}

std::string
snapshotValue(const MetadataSnapshot& snapshot,
              const std::string& key)
{
    MetadataSnapshot::const_iterator found = snapshot.find(key);

    return found == snapshot.end() ? std::string("<absent>") : found->second;
}

std::string
describeSnapshot(const MetadataSnapshot& snapshot)
{
    std::ostringstream text;

    for (MetadataSnapshot::const_iterator it = snapshot.begin(); it != snapshot.end(); ++it) {
        if (it != snapshot.begin()) {
            text << ", ";
        }
        text << it->first << "=" << it->second;
    }

    return text.str();
}

// The four fields of an HH:MM:SS:FF timecode. Failing rather than handing back zeroed fields
// keeps a timecode that never arrived from reading as 00:00:00:00 and passing for a real one.
struct TimecodeFields {
    int hours;
    int minutes;
    int seconds;
    int frames;
};

bool
parseTimecodeFields(const std::string& text,
                    TimecodeFields* fields)
{
    std::istringstream stream(text);
    char first = 0, second = 0, third = 0;

    stream >> fields->hours >> first >> fields->minutes >> second >> fields->seconds >> third >> fields->frames;

    return !stream.fail() && (first == ':') && (second == ':') && (third == ':');
}

std::string
describeTimecodeFields(const TimecodeFields& fields)
{
    std::ostringstream text;

    text << fields.hours << ":" << fields.minutes << ":" << fields.seconds << ":" << fields.frames;

    return text.str();
}

// One reader. Nothing it reads can change while it runs, so every read must match the snapshot
// taken of the same clip and time before the render started. gtest's assertions are not used
// off the main thread: a mismatch is recorded and asserted on once the thread has been joined.
class MetadataReaderThread
    : public QThread {
public:
    MetadataReaderThread(const std::vector<OFX::Host::ImageEffect::ClipInstance*>& clips,
                         const std::vector<std::string>& clipLabels,
                         const std::vector<std::vector<MetadataSnapshot>>& expected,
                         int firstFrame,
                         const std::atomic<bool>* stop)
        : _clips(clips)
        , _clipLabels(clipLabels)
        , _expected(expected)
        , _firstFrame(firstFrame)
        , _stop(stop)
        , _reads(0)
        , _failure()
    {
    }

    long long reads() const
    {
        return _reads.load();
    }

    // only safe to call once this thread has been joined
    const std::string& failure() const
    {
        return _failure;
    }

private:
    void recordFailure(const std::string& what)
    {
        if (_failure.empty()) {
            _failure = what;
        }
    }

    virtual void run() OVERRIDE FINAL
    {
        // Asking for more distinct times than a clip's cache holds makes it fill up, be
        // flushed whole and be derived again while the render is deriving times of its own.
        // Without it every read after the first would be a cache hit and the derivation and
        // flush paths -- the ones an invalidation can race -- would never be reached.
        OfxTime churnTime = 1000.;

        while (!_stop->load()) {
            for (std::size_t c = 0; c < _clips.size(); ++c) {
                for (std::size_t f = 0; f < _expected[c].size(); ++f) {
                    const OfxTime time = _firstFrame + (OfxTime)f;
                    const MetadataSnapshot read = snapshotMetadata(_clips[c], time);
                    _reads.fetch_add(1);

                    if (read != _expected[c][f]) {
                        std::ostringstream message;
                        message << _clipLabels[c] << " at frame " << time << " read {" << describeSnapshot(read)
                                << "} but held {" << describeSnapshot(_expected[c][f]) << "} before the render";
                        recordFailure(message.str());
                    }
                }
            }

            for (int i = 0; i < 8; ++i) {
                churnTime += 1.;
                if (churnTime > 4000.) {
                    churnTime = 1000.;
                }

                OFX::Host::ImageEffect::MetadataSet* set = _clips[0]->getMetadata(churnTime);
                _reads.fetch_add(1);

                if (!set) {
                    recordFailure(_clipLabels[0] + " came back with no metadata at all");
                    continue;
                }

                if (set->fetchProperty(kOfxMetadataKeySourceFrame) == NULL) {
                    recordFailure(_clipLabels[0] + " lost " + kOfxMetadataKeySourceFrame + " on a freshly derived set");
                } else if (set->getIntProperty(kOfxMetadataKeySourceFrame) != (int)churnTime) {
                    std::ostringstream message;
                    message << _clipLabels[0] << " derived at " << churnTime << " carries "
                            << kOfxMetadataKeySourceFrame << "=" << set->getIntProperty(kOfxMetadataKeySourceFrame);
                    recordFailure(message.str());
                }

                set->releaseReference();
            }

            yieldCurrentThread();
        }
    } // run

    std::vector<OFX::Host::ImageEffect::ClipInstance*> _clips;
    std::vector<std::string> _clipLabels;
    std::vector<std::vector<MetadataSnapshot>> _expected;
    int _firstFrame;
    const std::atomic<bool>* _stop;
    std::atomic<long long> _reads;
    std::string _failure;
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

    OfxEffectInstance* ofxEffect = dynamic_cast<OfxEffectInstance*>(node->getEffectInstance().get());
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

    NodePtr node = createNode(QString::fromUtf8("net.sf.openfx.ConstantPlugin"));
    ASSERT_TRUE(bool(node)) << "node creation failed for net.sf.openfx.ConstantPlugin";

    OfxEffectInstance* ofxEffect = dynamic_cast<OfxEffectInstance*>(node->getEffectInstance().get());
    ASSERT_TRUE(ofxEffect != NULL) << "node's effect instance is not backed by the OFX host";

    OFX::Host::ImageEffect::ClipInstance* output = ofxEffect->effectInstance()->getClip(kOfxImageEffectOutputClipName);
    ASSERT_TRUE(output != NULL) << "the effect has no output clip";

    OFX::Host::ImageEffect::MetadataSet* metadata = output->getMetadata(1.);
    ASSERT_TRUE(metadata != NULL);

    EXPECT_DOUBLE_EQ(getApp()->getProjectFrameRate(), metadata->getDoubleProperty(kOfxMetadataKeyFrameRate));
    EXPECT_DOUBLE_EQ(formatPar, metadata->getDoubleProperty(kOfxMetadataKeyPixelAspect));
    EXPECT_EQ(formatWidth, metadata->getIntProperty(kOfxMetadataKeyWidth));
    EXPECT_EQ(formatHeight, metadata->getIntProperty(kOfxMetadataKeyHeight));
    EXPECT_EQ(1, metadata->getIntProperty(kOfxMetadataKeySourceFrame));

    metadata->releaseReference();

    // The cache holds a reference of its own, so releasing the caller's must leave the set
    // alive: asking again returns that same set rather than a fresh one.
    OFX::Host::ImageEffect::MetadataSet* again = output->getMetadata(1.);
    ASSERT_TRUE(again != NULL);
    EXPECT_EQ(metadata, again);
    EXPECT_EQ(formatWidth, again->getIntProperty(kOfxMetadataKeyWidth));

    again->releaseReference();
}

// An input clip carries the metadata of the image handed to it, which is what the node
// connected to it puts out. Comparing against the upstream output clip rather than against
// literal values is the point: the two must agree whatever the project happens to be set to.
TEST_F(MetadataPluginFixture, InputClipCarriesUpstreamOutputMetadata)
{
    Format projectFormat;
    getApp()->getProject()->getProjectDefaultFormat(&projectFormat);

    NodePtr constant = createNode(QString::fromUtf8("net.sf.openfx.ConstantPlugin"));
    ASSERT_TRUE(bool(constant)) << "node creation failed for net.sf.openfx.ConstantPlugin";

    NodePtr grade = createNode(QString::fromUtf8("net.sf.openfx.GradePlugin"));
    ASSERT_TRUE(bool(grade)) << "node creation failed for net.sf.openfx.GradePlugin";

    connectNodes(constant, grade, 0, true);

    OfxEffectInstance* constantEffect = dynamic_cast<OfxEffectInstance*>(constant->getEffectInstance().get());
    ASSERT_TRUE(constantEffect != NULL) << "Constant is not backed by the OFX host";

    OfxEffectInstance* gradeEffect = dynamic_cast<OfxEffectInstance*>(grade->getEffectInstance().get());
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
    EXPECT_DOUBLE_EQ(upstream->getDoubleProperty(kOfxMetadataKeyFrameRate), source->getDoubleProperty(kOfxMetadataKeyFrameRate));
    EXPECT_EQ(upstream->getIntProperty(kOfxMetadataKeyWidth), source->getIntProperty(kOfxMetadataKeyWidth));
    EXPECT_EQ(upstream->getIntProperty(kOfxMetadataKeyHeight), source->getIntProperty(kOfxMetadataKeyHeight));

    // Constant generates the project format, so the values that reached Grade are the ones
    // the project implies rather than whatever a default constructed set would carry
    EXPECT_DOUBLE_EQ(getApp()->getProjectFrameRate(), source->getDoubleProperty(kOfxMetadataKeyFrameRate));
    EXPECT_EQ(projectFormat.width(), source->getIntProperty(kOfxMetadataKeyWidth));

    source->releaseReference();
    upstream->releaseReference();
}

// With nothing connected there is no upstream clip to copy from, and a plug-in asking its
// input for metadata must still get the host's answer rather than an empty set or a crash.
TEST_F(MetadataPluginFixture, DisconnectedInputClipFallsBackToHostDerivedMetadata)
{
    Format projectFormat;
    getApp()->getProject()->getProjectDefaultFormat(&projectFormat);

    NodePtr constant = createNode(QString::fromUtf8("net.sf.openfx.ConstantPlugin"));
    ASSERT_TRUE(bool(constant)) << "node creation failed for net.sf.openfx.ConstantPlugin";

    NodePtr grade = createNode(QString::fromUtf8("net.sf.openfx.GradePlugin"));
    ASSERT_TRUE(bool(grade)) << "node creation failed for net.sf.openfx.GradePlugin";

    connectNodes(constant, grade, 0, true);
    disconnectNodes(constant, grade, true);

    OfxEffectInstance* gradeEffect = dynamic_cast<OfxEffectInstance*>(grade->getEffectInstance().get());
    ASSERT_TRUE(gradeEffect != NULL) << "Grade is not backed by the OFX host";

    OFX::Host::ImageEffect::ClipInstance* gradeSource = gradeEffect->effectInstance()->getClip(kOfxImageEffectSimpleSourceClipName);
    ASSERT_TRUE(gradeSource != NULL) << "Grade has no " << kOfxImageEffectSimpleSourceClipName << " clip";

    OFX::Host::ImageEffect::MetadataSet* source = gradeSource->getMetadata(1.);
    ASSERT_TRUE(source != NULL);

    EXPECT_DOUBLE_EQ(getApp()->getProjectFrameRate(), source->getDoubleProperty(kOfxMetadataKeyFrameRate));
    EXPECT_EQ(projectFormat.width(), source->getIntProperty(kOfxMetadataKeyWidth));
    EXPECT_EQ(projectFormat.height(), source->getIntProperty(kOfxMetadataKeyHeight));
    EXPECT_EQ(1, source->getIntProperty(kOfxMetadataKeySourceFrame));

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
    NodePtr generator = createNode(QString::fromUtf8(PLUGINID_OFX_CONSTANT));
    NodePtr writer = createNode(_writeOIIOPluginID);
    ASSERT_TRUE(bool(generator) && bool(writer));

    connectNodes(generator, writer, 0, true);

    KnobChoice* bitDepth = dynamic_cast<KnobChoice*>(writer->getKnobByName("bitDepth").get());
    ASSERT_TRUE(bitDepth != NULL);
    bitDepth->setValueFromID("32f", 0);

    KnobChoice* compression = dynamic_cast<KnobChoice*>(writer->getKnobByName("compression").get());
    ASSERT_TRUE(compression != NULL);
    compression->setValueFromID("none", 0);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::string pattern = (tmp.path() + QLatin1String("/readerMetadata.####.exr")).toStdString();
    writer->setOutputFilesForWriter(pattern);

    const int firstFrame = 1;
    const int lastFrame = 2;

    OutputEffectInstance* writerEffect = dynamic_cast<OutputEffectInstance*>(writer->getEffectInstance().get());
    ASSERT_TRUE(writerEffect != NULL);

    std::list<AppInstance::RenderWork> works;
    works.push_back(AppInstance::RenderWork(writerEffect, firstFrame, lastFrame, 1, false));
    getApp()->startWritersRendering(false, works);

    const std::vector<std::string>& viewNames = getApp()->getProject()->getProjectViewNames();
    std::vector<std::string> renderedPaths;
    for (int frame = firstFrame; frame <= lastFrame; ++frame) {
        const std::string path = SequenceParsing::generateFileNameFromPattern(pattern, viewNames, frame, 0);
        ASSERT_TRUE(QFile::exists(QString::fromStdString(path))) << "frame " << frame << " was not rendered: " << path;
        renderedPaths.push_back(path);
    }

    CreateNodeArgs args(_readOIIOPluginID.toStdString(), getApp()->getProject());
    args.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, pattern);
    NodePtr reader = getApp()->createNode(args);
    ASSERT_TRUE(bool(reader)) << "node creation failed for " << _readOIIOPluginID.toStdString();

    // a bundled reader is created as a Read container holding the decoder, and the clip that
    // carries the metadata belongs to the decoder
    ReadNode* readNode = dynamic_cast<ReadNode*>(reader->getEffectInstance().get());
    ASSERT_TRUE(readNode != NULL) << "the reader is not backed by a Read container";

    NodePtr decoder = readNode->getEmbeddedReader();
    ASSERT_TRUE(bool(decoder)) << "no decoder was created for the rendered sequence";

    OfxEffectInstance* ofxEffect = dynamic_cast<OfxEffectInstance*>(decoder->getEffectInstance().get());
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

    EXPECT_GT(first->getDoubleProperty(kOfxMetadataKeyFileSize), 0.);
    EXPECT_GT(second->getDoubleProperty(kOfxMetadataKeyFileSize), 0.);
    EXPECT_GT(first->getDoubleProperty(kOfxMetadataKeyMTime), 0.);

    second->releaseReference();
    first->releaseReference();

    for (std::size_t i = 0; i < renderedPaths.size(); ++i) {
        QFile::remove(QString::fromStdString(renderedPaths[i]));
    }
} // TEST_F(MetadataPluginFixture, ReaderOutputClipCarriesPerFrameFileMetadata)

// A node downstream of a change used to keep serving the metadata it had derived beforehand,
// because nothing dropped the per clip cache. Here B is read, the contribution A makes is
// changed, and B is read again -- B's own params and inputs are never touched in between, so
// only an invalidation that reached B from A can make the new value show up.
TEST_F(MetadataPluginFixture, DownstreamMetadataFollowsAnUpstreamParamChange)
{
    NodePtr contribute = createNode(QString::fromUtf8(kMetadataContributeID));
    NodePtr view = createNode(QString::fromUtf8(kMetadataViewID));
    ASSERT_TRUE(bool(contribute)) << "node creation failed for " << kMetadataContributeID;
    ASSERT_TRUE(bool(view)) << "node creation failed for " << kMetadataViewID;

    connectNodes(contribute, view, 0, true);

    OFX::Host::ImageEffect::ClipInstance* viewOutput = clipOf(view, kOfxImageEffectOutputClipName);
    ASSERT_TRUE(viewOutput != NULL) << "the downstream node has no output clip";

    setContributedNote(contribute, "before");

    OFX::Host::ImageEffect::MetadataSet* first = viewOutput->getMetadata(1.);
    ASSERT_TRUE(first != NULL);
    ASSERT_TRUE(first->fetchProperty(kContributeNoteKey) != NULL) << "the upstream contribution never reached the downstream output clip";
    EXPECT_EQ(std::string("before"), first->getStringProperty(kContributeNoteKey));
    first->releaseReference();

    setContributedNote(contribute, "after");

    OFX::Host::ImageEffect::MetadataSet* second = viewOutput->getMetadata(1.);
    ASSERT_TRUE(second != NULL);
    EXPECT_EQ(std::string("after"), second->getStringProperty(kContributeNoteKey));

    second->releaseReference();
}

// The node upstream here is native, so the input clip has no upstream OFX output clip to copy
// and derives its keys from the host instead. The change made is one no OFX param change can
// stand in for: the project's own frame rate, which reaches the node without any hash moving.
TEST_F(MetadataPluginFixture, NativeUpstreamNodeChangeReachesTheOfxInputClip)
{
    NodePtr dot = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
    NodePtr view = createNode(QString::fromUtf8(kMetadataViewID));
    ASSERT_TRUE(bool(dot)) << "node creation failed for " << PLUGINID_NATRON_DOT;
    ASSERT_TRUE(bool(view)) << "node creation failed for " << kMetadataViewID;
    ASSERT_TRUE(dynamic_cast<OfxEffectInstance*>(dot->getEffectInstance().get()) == NULL) << "the upstream node is not native";

    connectNodes(dot, view, 0, true);

    OFX::Host::ImageEffect::ClipInstance* source = clipOf(view, kOfxImageEffectSimpleSourceClipName);
    ASSERT_TRUE(source != NULL) << "the downstream node has no " << kOfxImageEffectSimpleSourceClipName << " clip";

    KnobDouble* projectFrameRate = dynamic_cast<KnobDouble*>(getApp()->getProject()->getKnobByName("frameRate").get());
    ASSERT_TRUE(projectFrameRate != NULL) << "the project has no frame rate param";

    const double originalFrameRate = getApp()->getProjectFrameRate();

    Format projectFormat;
    getApp()->getProject()->getProjectDefaultFormat(&projectFormat);

    OFX::Host::ImageEffect::MetadataSet* before = source->getMetadata(1.);
    ASSERT_TRUE(before != NULL);
    EXPECT_DOUBLE_EQ(originalFrameRate, before->getDoubleProperty(kOfxMetadataKeyFrameRate));
    EXPECT_EQ(projectFormat.width(), before->getIntProperty(kOfxMetadataKeyWidth));
    EXPECT_DOUBLE_EQ(projectFormat.getPixelAspectRatio(), before->getDoubleProperty(kOfxMetadataKeyPixelAspect));
    before->releaseReference();

    const double changedFrameRate = originalFrameRate + 12.;
    projectFrameRate->setValue(changedFrameRate);
    ASSERT_DOUBLE_EQ(changedFrameRate, getApp()->getProjectFrameRate());

    OFX::Host::ImageEffect::MetadataSet* after = source->getMetadata(1.);
    ASSERT_TRUE(after != NULL);
    EXPECT_DOUBLE_EQ(changedFrameRate, after->getDoubleProperty(kOfxMetadataKeyFrameRate));

    // read live rather than asserted against literals: setOrAddProjectFormat() is a no-op
    // after the first call in a process and the app instance is shared by the whole suite
    Format liveFormat;
    getApp()->getProject()->getProjectDefaultFormat(&liveFormat);
    EXPECT_EQ(liveFormat.width(), after->getIntProperty(kOfxMetadataKeyWidth));
    EXPECT_DOUBLE_EQ(liveFormat.getPixelAspectRatio(), after->getDoubleProperty(kOfxMetadataKeyPixelAspect));
    after->releaseReference();

    // the project outlives this case, so it is left as it was found
    projectFrameRate->setValue(originalFrameRate);
}

// An input clip's metadata is a copy of what the node connected to it puts out, so pointing the
// input at a different node has to drop that copy just as a change in the node itself would.
TEST_F(MetadataPluginFixture, ReconnectingAnInputRepointsTheMetadata)
{
    NodePtr firstUpstream = createNode(QString::fromUtf8(kMetadataContributeID));
    NodePtr secondUpstream = createNode(QString::fromUtf8(kMetadataContributeID));
    NodePtr view = createNode(QString::fromUtf8(kMetadataViewID));
    ASSERT_TRUE(bool(firstUpstream) && bool(secondUpstream)) << "node creation failed for " << kMetadataContributeID;
    ASSERT_TRUE(bool(view)) << "node creation failed for " << kMetadataViewID;

    setContributedNote(firstUpstream, "from the first");
    setContributedNote(secondUpstream, "from the second");

    connectNodes(firstUpstream, view, 0, true);

    OFX::Host::ImageEffect::ClipInstance* source = clipOf(view, kOfxImageEffectSimpleSourceClipName);
    ASSERT_TRUE(source != NULL) << "the downstream node has no " << kOfxImageEffectSimpleSourceClipName << " clip";

    OFX::Host::ImageEffect::MetadataSet* fromFirst = source->getMetadata(1.);
    ASSERT_TRUE(fromFirst != NULL);
    ASSERT_TRUE(fromFirst->fetchProperty(kContributeNoteKey) != NULL) << "the upstream contribution never reached the input clip";
    EXPECT_EQ(std::string("from the first"), fromFirst->getStringProperty(kContributeNoteKey));
    fromFirst->releaseReference();

    disconnectNodes(firstUpstream, view, true);
    connectNodes(secondUpstream, view, 0, true);

    OFX::Host::ImageEffect::MetadataSet* fromSecond = source->getMetadata(1.);
    ASSERT_TRUE(fromSecond != NULL);
    EXPECT_EQ(std::string("from the second"), fromSecond->getStringProperty(kContributeNoteKey));

    fromSecond->releaseReference();
}

// The guard against the opposite failure. getOutputMetadata() allocates a fresh MetadataSet for
// every run of the plug-in's get metadata action and caches that one, and the reference taken
// on the first read keeps it alive, so a second run could not hand its address back. Identical
// pointers across two reads with nothing changed in between therefore say the action ran once.
TEST_F(MetadataPluginFixture, UnchangedStateDoesNotReRunTheGetMetadataAction)
{
    NodePtr contribute = createNode(QString::fromUtf8(kMetadataContributeID));
    NodePtr view = createNode(QString::fromUtf8(kMetadataViewID));
    ASSERT_TRUE(bool(contribute)) << "node creation failed for " << kMetadataContributeID;
    ASSERT_TRUE(bool(view)) << "node creation failed for " << kMetadataViewID;

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
    EXPECT_EQ(std::string("stable"), secondView->getStringProperty(kContributeNoteKey));

    secondView->releaseReference();
    secondSource->releaseReference();
    secondContribute->releaseReference();
    firstView->releaseReference();
    firstSource->releaseReference();
    firstContribute->releaseReference();
}

// The chain contract the reference host's harness holds two nodes to, made to hold here: a node
// downstream of a contributing one has to put out the union of what its source carries and what
// the contributor adds, with the contributed values winning, and has to follow a later change to
// the contributor. Natron's source keys are the host's own rather than a fixture's, so the union
// is checked as each side being contained in what the tail carries rather than as a literal set.
TEST_F(MetadataPluginFixture, MetadataChainCarriesTheContributedKeysDownstream)
{
    NodePtr generator = createNode(QString::fromUtf8(PLUGINID_OFX_CONSTANT));
    NodePtr contribute = createNode(QString::fromUtf8(kMetadataContributeID));
    NodePtr view = createNode(QString::fromUtf8(kMetadataViewID));
    ASSERT_TRUE(bool(generator)) << "node creation failed for " << PLUGINID_OFX_CONSTANT;
    ASSERT_TRUE(bool(contribute)) << "node creation failed for " << kMetadataContributeID;
    ASSERT_TRUE(bool(view)) << "node creation failed for " << kMetadataViewID;

    connectNodes(generator, contribute, 0, true);
    connectNodes(contribute, view, 0, true);

    setContributedNote(contribute, "before the chain");

    // read live: the project is shared by the whole suite. Were it sitting at the contributed
    // rate the frame rate check below would say nothing at all, so it is asserted to differ.
    const double projectFrameRate = getApp()->getProjectFrameRate();
    ASSERT_NE(projectFrameRate, kContributedFrameRate)
        << "the project frame rate is the contributed one, so nothing below could tell them apart";

    OFX::Host::ImageEffect::ClipInstance* sourceOutput = clipOf(generator, kOfxImageEffectOutputClipName);
    OFX::Host::ImageEffect::ClipInstance* viewOutput = clipOf(view, kOfxImageEffectOutputClipName);
    ASSERT_TRUE(sourceOutput != NULL) << "the head of the chain has no output clip";
    ASSERT_TRUE(viewOutput != NULL) << "the tail of the chain has no output clip";

    const int firstFrame = 1;
    const int lastFrame = 4;

    for (int frame = firstFrame; frame <= lastFrame; ++frame) {
        const MetadataSnapshot source = snapshotMetadata(sourceOutput, frame);
        const MetadataSnapshot tail = snapshotMetadata(viewOutput, frame);

        ASSERT_FALSE(source.empty()) << "the chain's source carries no metadata at frame " << frame;
        ASSERT_FALSE(tail.empty()) << "the chain's tail carries no metadata at frame " << frame;

        for (MetadataSnapshot::const_iterator it = source.begin(); it != source.end(); ++it) {
            EXPECT_TRUE(tail.find(it->first) != tail.end())
                << "frame " << frame << ": the source key " << it->first
                << " did not reach the tail, which carries {" << describeSnapshot(tail) << "}";
        }

        for (std::size_t k = 0; k < sizeof(kContributedKeys) / sizeof(kContributedKeys[0]); ++k) {
            EXPECT_NE(std::string("<absent>"), snapshotValue(tail, kContributedKeys[k]))
                << "frame " << frame << ": the contributed key " << kContributedKeys[k]
                << " did not reach the tail, which carries {" << describeSnapshot(tail) << "}";
        }

        OFX::Host::ImageEffect::MetadataSet* sourceSet = sourceOutput->getMetadata(frame);
        OFX::Host::ImageEffect::MetadataSet* tailSet = viewOutput->getMetadata(frame);
        ASSERT_TRUE(sourceSet != NULL);
        ASSERT_TRUE(tailSet != NULL);

        EXPECT_DOUBLE_EQ(projectFrameRate, sourceSet->getDoubleProperty(kOfxMetadataKeyFrameRate))
            << "frame " << frame << ": the source does not carry the project's frame rate, so the "
            << "value read at the tail proves nothing about which of the two won";
        EXPECT_DOUBLE_EQ(kContributedFrameRate, tailSet->getDoubleProperty(kOfxMetadataKeyFrameRate))
            << "frame " << frame << ": the tail carries the inherited frame rate rather than the "
            << "contributed one";

        tailSet->releaseReference();
        sourceSet->releaseReference();
    }

    setContributedNote(contribute, "chained");

    OFX::Host::ImageEffect::MetadataSet* revised = viewOutput->getMetadata(firstFrame);
    ASSERT_TRUE(revised != NULL);
    ASSERT_TRUE(revised->fetchProperty(kContributeNoteKey) != NULL)
        << "the contributed note is no longer at the tail at all";
    EXPECT_EQ(std::string("chained"), revised->getStringProperty(kContributeNoteKey));
    EXPECT_DOUBLE_EQ(kContributedFrameRate, revised->getDoubleProperty(kOfxMetadataKeyFrameRate));

    revised->releaseReference();
} // TEST_F(MetadataPluginFixture, MetadataChainCarriesTheContributedKeysDownstream)

// The per clip metadata cache is reached from whichever thread asks for metadata, and a render
// is the one thing that asks for it from many threads at once. This renders a real frame range
// through a chain whose middle node reads its source clip's metadata inside its render action,
// while separate threads read the same clips, and checks that every read comes back whole: the
// failures it exists to catch are a crash, a hang, and a read that has lost or half-written a
// key. The nodes are raised to fully safe first, because that is the level at which
// EffectInstance::renderRoI takes no serialising lock and makes no render clone -- at anything
// less the render is serialised on the node and barely touches the cache concurrently at all.
TEST_F(MetadataPluginFixture, MetadataConcurrentReadsDuringRenderStayWhole)
{
    // small, so that a run of this is spent on metadata rather than on pixels.
    // setOrAddProjectFormat() only bites the first time it is called in a process and the app
    // instance is shared by the whole suite, so nothing below may depend on it having taken
    Format small(0, 0, 64, 64, "metadataConcurrentFormat", 1.);
    getApp()->getProject()->setOrAddProjectFormat(small);

    NodePtr generator = createNode(QString::fromUtf8(PLUGINID_OFX_CONSTANT));
    NodePtr contribute = createNode(QString::fromUtf8(kMetadataContributeID));
    NodePtr print = createNode(QString::fromUtf8(kMetadataPrintID));
    NodePtr writer = createNode(_writeOIIOPluginID);
    ASSERT_TRUE(bool(generator)) << "node creation failed for " << PLUGINID_OFX_CONSTANT;
    ASSERT_TRUE(bool(contribute)) << "node creation failed for " << kMetadataContributeID;
    ASSERT_TRUE(bool(print)) << "node creation failed for " << kMetadataPrintID;
    ASSERT_TRUE(bool(writer)) << "node creation failed for " << _writeOIIOPluginID.toStdString();

    connectNodes(generator, contribute, 0, true);
    connectNodes(contribute, print, 0, true);
    connectNodes(print, writer, 0, true);

    setContributedNote(contribute, "rendered concurrently");

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::string pattern = (tmp.path() + QLatin1String("/metadataConcurrent.####.exr")).toStdString();
    writer->setOutputFilesForWriter(pattern);

    OutputEffectInstance* writerEffect = dynamic_cast<OutputEffectInstance*>(writer->getEffectInstance().get());
    ASSERT_TRUE(writerEffect != NULL);

    // Raised last, after every input and knob change: Node::refreshDynamicProperties() puts the
    // plug-in's own level back and both of those run it. The example plug-ins never call
    // setRenderThreadSafety(), so they take the OFX default of instance safe, and their render
    // bodies -- a per window pixel copy of images the host hands them, plus a read of metadata
    // the host owns -- meet fully safe. The generator and the writer declare fully safe for
    // themselves and are left alone, so with these two raised nothing in the chain serialises.
    contribute->setRenderThreadSafety(eRenderSafetyFullySafe);
    print->setRenderThreadSafety(eRenderSafetyFullySafe);
    ASSERT_EQ(eRenderSafetyFullySafe, contribute->getCurrentRenderThreadSafety());
    ASSERT_EQ(eRenderSafetyFullySafe, print->getCurrentRenderThreadSafety());

    OFX::Host::ImageEffect::ClipInstance* contributeOutput = clipOf(contribute, kOfxImageEffectOutputClipName);
    // the clip the reading node's own render action asks for metadata on
    OFX::Host::ImageEffect::ClipInstance* printSource = clipOf(print, kOfxImageEffectSimpleSourceClipName);
    OFX::Host::ImageEffect::ClipInstance* printOutput = clipOf(print, kOfxImageEffectOutputClipName);
    ASSERT_TRUE(contributeOutput != NULL) << "the contributing node has no output clip";
    ASSERT_TRUE(printSource != NULL) << "the reading node has no " << kOfxImageEffectSimpleSourceClipName << " clip";
    ASSERT_TRUE(printOutput != NULL) << "the reading node has no output clip";

    std::vector<OFX::Host::ImageEffect::ClipInstance*> clips;
    std::vector<std::string> clipLabels;
    clips.push_back(printSource);
    clipLabels.push_back("the reading node's source clip");
    clips.push_back(printOutput);
    clipLabels.push_back("the reading node's output clip");
    clips.push_back(contributeOutput);
    clipLabels.push_back("the contributing node's output clip");

    const int firstFrame = 1;
    const int lastFrame = 6;

    std::vector<std::vector<MetadataSnapshot>> before(clips.size());
    for (std::size_t c = 0; c < clips.size(); ++c) {
        for (int frame = firstFrame; frame <= lastFrame; ++frame) {
            before[c].push_back(snapshotMetadata(clips[c], frame));
        }
    }

    // An empty answer would satisfy every comparison made against it, so the baseline is checked
    // to carry the chain's keys before it is used as one: the plug-in's contribution, and the
    // host's own keys including the one whose value differs from frame to frame.
    ASSERT_FALSE(before[0][0].empty()) << "the reading node's source clip carries no metadata at all";
    EXPECT_EQ(std::string("rendered concurrently"), snapshotValue(before[0][0], kContributeNoteKey));
    EXPECT_EQ(std::string("rendered concurrently"), snapshotValue(before[1][0], kContributeNoteKey));
    EXPECT_EQ(std::string("rendered concurrently"), snapshotValue(before[2][0], kContributeNoteKey));
    EXPECT_EQ(std::string("1"), snapshotValue(before[0][0], kOfxMetadataKeySourceFrame));
    EXPECT_EQ(std::string("6"), snapshotValue(before[0][lastFrame - firstFrame], kOfxMetadataKeySourceFrame));
    EXPECT_NE(std::string("<absent>"), snapshotValue(before[0][0], kOfxMetadataKeyFrameRate));
    EXPECT_NE(std::string("<absent>"), snapshotValue(before[0][0], kOfxMetadataKeyWidth));
    EXPECT_NE(std::string("<absent>"), snapshotValue(before[0][0], kOfxMetadataKeyHeight));

    std::atomic<bool> stop(false);
    std::vector<MetadataReaderThread*> readers;
    for (int i = 0; i < 4; ++i) {
        readers.push_back(new MetadataReaderThread(clips, clipLabels, before, firstFrame, &stop));
        readers.back()->start();
    }

    long long readsBeforeRender = 0;
    for (std::size_t i = 0; i < readers.size(); ++i) {
        readsBeforeRender += readers[i]->reads();
    }

    std::list<AppInstance::RenderWork> works;
    works.push_back(AppInstance::RenderWork(writerEffect, firstFrame, lastFrame, 1, false));
    getApp()->startWritersRendering(false, works);

    long long readsDuringRender = 0;
    for (std::size_t i = 0; i < readers.size(); ++i) {
        readsDuringRender += readers[i]->reads();
    }
    readsDuringRender -= readsBeforeRender;

    stop.store(true);

    std::string failure;
    long long totalReads = 0;
    for (std::size_t i = 0; i < readers.size(); ++i) {
        readers[i]->wait();
        totalReads += readers[i]->reads();
        if (failure.empty()) {
            failure = readers[i]->failure();
        }
    }
    for (std::size_t i = 0; i < readers.size(); ++i) {
        delete readers[i];
    }
    readers.clear();

    EXPECT_EQ(std::string(), failure);
    EXPECT_GT(readsDuringRender, 0) << "the readers were not running while the render was";
    EXPECT_GT(totalReads, 0);

    // the level has to have held for the whole render, or the lock free path was not the one taken
    EXPECT_EQ(eRenderSafetyFullySafe, contribute->getCurrentRenderThreadSafety());
    EXPECT_EQ(eRenderSafetyFullySafe, print->getCurrentRenderThreadSafety());

    const std::vector<std::string>& viewNames = getApp()->getProject()->getProjectViewNames();
    for (int frame = firstFrame; frame <= lastFrame; ++frame) {
        const std::string path = SequenceParsing::generateFileNameFromPattern(pattern, viewNames, frame, 0);
        EXPECT_TRUE(QFile::exists(QString::fromStdString(path))) << "frame " << frame << " was not rendered: " << path;
        QFile::remove(QString::fromStdString(path));
    }

    NodePtr chain[] = { generator, contribute, print, writer };
    for (std::size_t i = 0; i < sizeof(chain) / sizeof(chain[0]); ++i) {
        QString message;
        int type = 0;
        chain[i]->getPersistentMessage(&message, &type);
        EXPECT_TRUE(message.isEmpty()) << chain[i]->getScriptName() << " reported: " << message.toStdString();
    }

    for (std::size_t c = 0; c < clips.size(); ++c) {
        for (int frame = firstFrame; frame <= lastFrame; ++frame) {
            const MetadataSnapshot after = snapshotMetadata(clips[c], frame);
            EXPECT_TRUE(after == before[c][frame - firstFrame])
                << clipLabels[c] << " at frame " << frame << " reads {" << describeSnapshot(after)
                << "} after the render but held {" << describeSnapshot(before[c][frame - firstFrame]) << "} before it";
        }
    }
} // TEST_F(MetadataPluginFixture, MetadataConcurrentReadsDuringRenderStayWhole)

// Metadata that changes with time rather than only with the clip. MetadataTimeCode synthesises
// its timecode -- it counts frames on from a start code rather than reading one off a file --
// which is what makes this testable at all, since nothing in the tree carries a file derived
// timecode. It counts at the rate it reads off its source clip's ofx/framerate, so the rate the
// host publishes for that clip is what decides both the value reported and where the frames
// field rolls over, and the rate param is deliberately set well away from the project's so that
// a count at the project's rate cannot also be explained by the param.
TEST_F(MetadataPluginFixture, MetadataTimeCodeAdvancesFrameByFrameAtTheHostFrameRate)
{
    NodePtr generator = createNode(QString::fromUtf8(PLUGINID_OFX_CONSTANT));
    NodePtr timecode = createNode(QString::fromUtf8(kMetadataTimeCodeID));
    ASSERT_TRUE(bool(generator)) << "node creation failed for " << PLUGINID_OFX_CONSTANT;
    ASSERT_TRUE(bool(timecode)) << "node creation failed for " << kMetadataTimeCodeID;

    connectNodes(generator, timecode, 0, true);

    KnobString* startTimecode = dynamic_cast<KnobString*>(timecode->getKnobByName("startTimecode").get());
    KnobDouble* rate = dynamic_cast<KnobDouble*>(timecode->getKnobByName("rate").get());
    KnobBool* rateFromMetadata = dynamic_cast<KnobBool*>(timecode->getKnobByName("rateFromMetadata").get());
    KnobBool* useStartFrame = dynamic_cast<KnobBool*>(timecode->getKnobByName("useStartFrame").get());
    ASSERT_TRUE(startTimecode != NULL) << "the time code node has no startTimecode param";
    ASSERT_TRUE(rate != NULL) << "the time code node has no rate param";
    ASSERT_TRUE(rateFromMetadata != NULL) << "the time code node has no rateFromMetadata param";
    ASSERT_TRUE(useStartFrame != NULL) << "the time code node has no useStartFrame param";

    // read live rather than asserted against a literal: the app instance, and so the project, is
    // shared by the whole suite
    const double projectFrameRate = getApp()->getProjectFrameRate();
    const int countedRate = (int)(projectFrameRate + 0.5);
    ASSERT_GE(countedRate, 3) << "the project frame rate is too low for a second boundary to be crossed within a frame or two of it";

    const double paramRate = projectFrameRate + 36.;

    startTimecode->setValue(std::string("01:00:00:00"));
    useStartFrame->setValue(false);
    rate->setValue(paramRate);
    rateFromMetadata->setValue(true);

    OFX::Host::ImageEffect::ClipInstance* output = clipOf(timecode, kOfxImageEffectOutputClipName);
    ASSERT_TRUE(output != NULL) << "the time code node has no output clip";

    // With the start code landing on frame 1, frame t carries t-1 frames counted at the rate, so
    // the frames field rolls into the seconds field at frame countedRate+1. Reading either side
    // of it is what makes the rollover exercised rather than assumed.
    const int rolloverTime = countedRate + 1;
    const int firstTime = rolloverTime - 2;
    const int lastTime = rolloverTime + 1;

    TimecodeFields previous;
    bool havePrevious = false;

    for (int time = firstTime; time <= lastTime; ++time) {
        OFX::Host::ImageEffect::MetadataSet* set = output->getMetadata(time);
        ASSERT_TRUE(set != NULL) << "frame " << time << ": no metadata at all";
        ASSERT_TRUE(set->fetchProperty(kOfxMetadataKeyTimecode) != NULL)
            << "frame " << time << ": the plug-in's timecode never reached its output clip";

        const std::string text = set->getStringProperty(kOfxMetadataKeyTimecode);
        const double reportedRate = set->getDoubleProperty(kOfxMetadataKeyFrameRate);
        set->releaseReference();

        EXPECT_DOUBLE_EQ(projectFrameRate, reportedRate)
            << "frame " << time << ": the rate reported alongside the timecode is neither the host's "
            << projectFrameRate << " nor, as it happens, anything the param's " << paramRate << " implies";

        TimecodeFields fields;
        ASSERT_TRUE(parseTimecodeFields(text, &fields))
            << "frame " << time << ": '" << text << "' is not an HH:MM:SS:FF timecode";

        EXPECT_EQ(1, fields.hours) << "frame " << time << ": read " << text;
        EXPECT_EQ(0, fields.minutes) << "frame " << time << ": read " << text;
        EXPECT_EQ((time - 1) / countedRate, fields.seconds)
            << "frame " << time << ": read " << text << ", counting at " << countedRate;
        EXPECT_EQ((time - 1) % countedRate, fields.frames)
            << "frame " << time << ": read " << text << ", counting at " << countedRate;

        if (havePrevious) {
            if (fields.seconds == previous.seconds) {
                EXPECT_EQ(previous.frames + 1, fields.frames)
                    << "the frames field did not advance by one from frame " << (time - 1) << " ("
                    << describeTimecodeFields(previous) << ") to frame " << time << " (" << text << ")";
            } else {
                EXPECT_EQ(previous.seconds + 1, fields.seconds)
                    << "frame " << time << " (" << text << ") did not follow frame " << (time - 1)
                    << " (" << describeTimecodeFields(previous) << ") by one second";
                EXPECT_EQ(0, fields.frames) << "the frames field did not restart at zero across the second boundary: " << text;
                EXPECT_EQ(countedRate - 1, previous.frames)
                    << "the frames field rolled over at " << describeTimecodeFields(previous)
                    << " rather than after counting to " << countedRate;
            }
        }

        previous = fields;
        havePrevious = true;
    }

    ASSERT_TRUE(havePrevious);

    // rateFromMetadata is the param that chooses between the two rates, so turning it off has to
    // make the same frame count at the rate param instead. That is what says the count above came
    // from the rate this host publishes rather than from anything the plug-in fell back on.
    rateFromMetadata->setValue(false);

    OFX::Host::ImageEffect::MetadataSet* fromParam = output->getMetadata(rolloverTime);
    ASSERT_TRUE(fromParam != NULL) << "frame " << rolloverTime << ": no metadata at all";
    ASSERT_TRUE(fromParam->fetchProperty(kOfxMetadataKeyTimecode) != NULL)
        << "frame " << rolloverTime << ": the plug-in's timecode never reached its output clip";

    const std::string paramText = fromParam->getStringProperty(kOfxMetadataKeyTimecode);
    const double paramReportedRate = fromParam->getDoubleProperty(kOfxMetadataKeyFrameRate);
    fromParam->releaseReference();

    EXPECT_DOUBLE_EQ(paramRate, paramReportedRate)
        << "with rateFromMetadata off the reported rate is not the param's";

    TimecodeFields paramFields;
    ASSERT_TRUE(parseTimecodeFields(paramText, &paramFields))
        << "frame " << rolloverTime << ": '" << paramText << "' is not an HH:MM:SS:FF timecode";

    EXPECT_EQ(0, paramFields.seconds)
        << "frame " << rolloverTime << " read " << paramText << ": counted at the param's " << paramRate
        << " it is still short of a whole second, so it must not have rolled over";
    EXPECT_EQ(rolloverTime - 1, paramFields.frames)
        << "frame " << rolloverTime << " read " << paramText << " rather than counting to "
        << (rolloverTime - 1) << " at the param's rate";
} // TEST_F(MetadataPluginFixture, MetadataTimeCodeAdvancesFrameByFrameAtTheHostFrameRate)
