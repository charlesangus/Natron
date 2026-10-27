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

#include <cstdlib>

#include "BaseTest.h"

#include <QFile>
#include <QThread>
#include <QThreadPool>

// ofxhPropertySuite.h:565:37: warning: 'this' pointer cannot be null in well-defined C++ code; comparison may be assumed to always evaluate to true [-Wtautological-undefined-compare]
// clang-format off
CLANG_DIAG_OFF(unknown-pragmas)
CLANG_DIAG_OFF(tautological-undefined-compare) // appeared in clang 3.5
#include <ofxhPluginCache.h>
//#include <ofxhPluginAPICache.h>
//#include <ofxhImageEffect.h>
#include <ofxhImageEffectAPI.h>
//#include <ofxOpenGLRender.h>
//#include <ofxhHost.h>
CLANG_DIAG_ON(tautological-undefined-compare)
CLANG_DIAG_ON(unknown-pragmas)
// clang-format on

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CLArgs.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/Curve.h"
#include "Engine/EffectInstance.h"
#include "Engine/GpuCostModel.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/IO/NativeWrite.h"
#include "Engine/Plugin.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

#include "CacheMemoryPressureGuard.h"
#include "CountingTestEffect.h"
#include "RenderBothWays.h"
#include "TestOfxPluginPath.h"

NATRON_NAMESPACE_USING

BaseTest::BaseTest()
    : testing::Test()
    , _app()
{
}

BaseTest::~BaseTest()
{
}

void
BaseTest::registerTestPlugins()
{
    _allTestPluginIDs.clear();

    _generatorPluginID = QString::fromUtf8(PLUGINID_OFX_SENOISE);
    _allTestPluginIDs.push_back(_generatorPluginID);

    _readPluginID = QString::fromUtf8(PLUGINID_NATRON_READ);

    _writePluginID = QString::fromUtf8(PLUGINID_NATRON_WRITE);

    _textArenaPluginID = QString::fromUtf8("net.fxarena.openfx.Text");
    _allTestPluginIDs.push_back(_textArenaPluginID);

    for (unsigned int i = 0; i < _allTestPluginIDs.size(); ++i) {
        ///make sure the generic test plugin is present
        LibraryBinary* bin = NULL;
        try {
            Plugin* p = appPTR->getPluginBinary(_allTestPluginIDs[i], -1, -1, false);
            if (p) {
                bin = p->getLibraryBinary();
            }
        } catch (const std::exception & e) {
            std::cout << e.what() << std::endl;
        }

        ASSERT_TRUE(bin != NULL);
    }
}

void
BaseTest::SetUp()
{
    const QString userPath = userOfxPluginPath();
    if (!userPath.isEmpty()) {
        std::cout << "Warning: Ignoring standard plugin path, OFX_PLUGIN_PATH=" << userPath.toStdString() << std::endl;
        OFX::Host::PluginCache::useStdOFXPluginsLocation(false);
    }

    _app = appPTR->getTopLevelInstance();

    // The cases of a suite share one process, so each starts from what a fresh process has: an
    // empty project, empty caches and an unrefined cost model. Identical nodes share cache
    // entries by hash, and a project reset leaves the destroyed nodes' entries in place, so a
    // test node whose output depends on state outside its hash would otherwise be served an
    // earlier case's render, and a Read served from the cache would not set the project format.
    AppInstancePtr app = getApp();
    if (app) {
        app->getProject()->reset(false, true);
    }
    appPTR->clearAllCaches();
    GpuCostModel::session().reset();

    registerTestPlugins();
}

void
BaseTest::TearDown()
{
    appPTR->setNumberOfThreads(0);
    ///Caches may have launched some threads to delete images, wait for them to be done
    QThreadPool::globalInstance()->waitForDone();
}

NodePtr
BaseTest::createNode(const QString & pluginID,
                     int majorVersion,
                     int minorVersion)
{
    CreateNodeArgs args( pluginID.toStdString(), getApp()->getProject() );
    
    args.setProperty<int>(kCreateNodeArgsPropPluginVersion, majorVersion, 0);
    args.setProperty<int>(kCreateNodeArgsPropPluginVersion, minorVersion, 1);

    NodePtr ret =  getApp()->createNode(args);


    EXPECT_NE(ret.get(), (Node*)NULL);

    return ret;
}

NodePtr
BaseTest::createNativeWriter(const std::string& filename)
{
    NodePtr writer = createNode(_writePluginID, PLUGIN_MAJOR_NATRON_WRITE);
    if (!writer) {
        return writer;
    }
    // The default output space comes from OCIO file rules and would convert the pixels the tests compare.
    KnobStringBase* outputSpace = dynamic_cast<KnobStringBase*>(writer->getKnobByName("ocioOutputSpace").get());
    KnobBool* outputSpaceSet = dynamic_cast<KnobBool*>(writer->getKnobByName("ocioOutputSpaceSet").get());
    EXPECT_TRUE(outputSpace != NULL);
    EXPECT_TRUE(outputSpaceSet != NULL);
    if (outputSpace && outputSpaceSet) {
        outputSpace->setValue(getApp()->getProject()->getWorkingColorSpace());
        outputSpaceSet->setValue(true);
    }
    if (!filename.empty()) {
        writer->setOutputFilesForWriter(filename);
    }

    return writer;
}

NodePtr
BaseTest::createWorkingSpaceRead(const std::string& filePath)
{
    CreateNodeArgs args(_readPluginID.toStdString(), getApp()->getProject());

    args.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, filePath);
    NodePtr reader = getApp()->createNode(args);
    EXPECT_TRUE(bool(reader)) << "node creation failed for " << _readPluginID.toStdString() << " on " << filePath;
    if (reader) {
        readInWorkingSpace(reader);
    }

    return reader;
}

void
BaseTest::readInWorkingSpace(const NodePtr& reader)
{
    KnobStringBase* inputSpace = dynamic_cast<KnobStringBase*>(reader->getKnobByName("ocioInputSpace").get());
    KnobBool* inputSpaceSet = dynamic_cast<KnobBool*>(reader->getKnobByName("ocioInputSpaceSet").get());

    ASSERT_TRUE(inputSpace != NULL) << reader->getScriptName_mt_safe();
    ASSERT_TRUE(inputSpaceSet != NULL) << reader->getScriptName_mt_safe();
    inputSpace->setValue(getApp()->getProject()->getWorkingColorSpace());
    // Marked as chosen by hand: otherwise a reload or a new file re-guesses the space from the
    // file and the pixels the tests compare get converted after all.
    inputSpaceSet->setValue(true);
    reader->getEffectInstance()->dropCachedMetadata();
}

void
BaseTest::connectNodes(NodePtr input,
                       NodePtr output,
                       int inputNumber,
                       bool expectedReturnValue)
{
    if (expectedReturnValue) {
        ///check that the connections are internally all set as "expected"

        EXPECT_EQ( (Node*)NULL, output->getInput(inputNumber).get() );
        EXPECT_FALSE( output->isInputConnected(inputNumber) );
    } else {
        ///the call can only fail for those 2 reasons
        EXPECT_TRUE(inputNumber > output->getNInputs() || //< inputNumber is greater than the maximum input number
                    output->getInput(inputNumber).get() != (Node*)NULL); //< input slot is already filled with another node
    }


    bool ret = getApp()->getProject()->connectNodes(inputNumber, input, output);
    EXPECT_EQ(expectedReturnValue, ret);

    if (expectedReturnValue) {
        EXPECT_TRUE( input->hasOutputConnected() );
        EXPECT_EQ(output->getInput(inputNumber), input);
        EXPECT_TRUE( output->isInputConnected(inputNumber) );
    }
}

void
BaseTest::disconnectNodes(NodePtr input,
                          NodePtr output,
                          bool expectedReturnvalue)
{
    if (expectedReturnvalue) {
        ///check that the connections are internally all set as "expected"

        ///the input must have in its output the node 'output'
        EXPECT_TRUE( input->hasOutputConnected() );
        const NodesWList & outputs = input->getGuiOutputs();
        bool foundOutput = false;
        for (NodesWList::const_iterator it = outputs.begin(); it != outputs.end(); ++it) {
            if (it->lock() == output) {
                foundOutput = true;
                break;
            }
        }

        ///the output must have in its inputs the node 'input'
        const std::vector<NodeWPtr> & inputs = output->getGuiInputs();
        int inputIndex = 0;
        bool foundInput = false;
        for (U32 i = 0; i < inputs.size(); ++i) {
            if (inputs[i].lock() == input) {
                foundInput = true;
                break;
            }
            ++inputIndex;
        }

        EXPECT_TRUE(foundInput);
        EXPECT_TRUE(foundOutput);
        EXPECT_EQ(output->getInput(inputIndex), input);
        EXPECT_TRUE( output->isInputConnected(inputIndex) );
    }

    ///call disconnect
    bool ret = getApp()->getProject()->disconnectNodes(input, output);
    EXPECT_EQ(expectedReturnvalue, ret);

    if (expectedReturnvalue) {
        ///check that the disconnection went OK

        const NodesWList & outputs = input->getGuiOutputs();
        bool foundOutput = false;
        for (NodesWList::const_iterator it = outputs.begin(); it != outputs.end(); ++it) {
            if (it->lock() == output) {
                foundOutput = true;
                break;
            }
        }

        ///the output must have in its inputs the node 'input'
        const std::vector<NodeWPtr> & inputs = output->getGuiInputs();
        int inputIndex = 0;
        bool foundInput = false;
        for (U32 i = 0; i < inputs.size(); ++i) {
            if (inputs[i].lock() == input) {
                foundInput = true;
                break;
            }
            ++inputIndex;
        }

        EXPECT_FALSE(foundOutput);
        EXPECT_FALSE(foundInput);
        EXPECT_EQ( (Node*)NULL, output->getInput(inputIndex).get() );
        EXPECT_FALSE( output->isInputConnected(inputIndex) );
    }
} // disconnectNodes

///High level test: render 1 frame of dot generator
TEST_F(BaseTest, GenerateDot)
{
    ///create the generator
    NodePtr generator = createNode(_generatorPluginID);

    ///create the writer and set its output filename
    NodePtr writer = createNativeWriter();

    ASSERT_TRUE( bool(generator) && bool(writer) );

    KnobIPtr frameRange = generator->getApp()->getProject()->getKnobByName("frameRange");
    ASSERT_TRUE( bool(frameRange) );
    KnobInt* knob = dynamic_cast<KnobInt*>( frameRange.get() );
    ASSERT_TRUE(knob);
    knob->setValue(1, ViewSpec::all(), 0);
    knob->setValue(1, ViewSpec::all(), 1);

    Format f(0, 0, 200, 200, "toto", 1.);
    generator->getApp()->getProject()->setOrAddProjectFormat(f);

    const QString& binPath = appPTR->getApplicationBinaryPath();
    QString filePath = binPath + QString::fromUtf8("/test_dot_generator.jpg");
    writer->setOutputFilesForWriter( filePath.toStdString() );

    ///attempt to connect the 2 nodes together
    connectNodes(generator, writer, 0, true);

    ///and start rendering. This call is blocking.
    std::list<AppInstance::RenderWork> works;
    AppInstance::RenderWork w;
    w.writer = dynamic_cast<OutputEffectInstance*>( writer->getEffectInstance().get() );
    assert(w.writer);
    w.firstFrame = INT_MIN;
    w.lastFrame = INT_MAX;
    w.frameStep = INT_MIN;
    w.useRenderStats = false;
    works.push_back(w);
    getApp()->startWritersRendering(eRenderModeBlocking, works);

    EXPECT_TRUE( QFile::exists(filePath) );
    QFile::remove(filePath);
}

TEST_F(BaseTest, SetValues)
{
    NodePtr generator = createNode(_generatorPluginID);

    assert(generator);
    KnobIPtr knob = generator->getKnobByName("noiseZSlope");
    KnobDouble* radius = dynamic_cast<KnobDouble*>( knob.get() );
    EXPECT_TRUE(radius != 0);
    if (!radius) {
        return;
    }
    radius->setValue(0.5);
    EXPECT_TRUE(radius->getValue() == 0.5);

    //Check that linear interpolation is working as intended
    KeyFrame kf;
    radius->setInterpolationAtTime(eCurveChangeReasonInternal, ViewSpec::all(),  0, 0, eKeyframeTypeLinear, &kf);
    radius->setValueAtTime(0, 0., ViewSpec::all(), 0);
    radius->setValueAtTime(100, 1., ViewSpec::all(), 0);
    for (int i = 0; i <= 100; ++i) {
        double v = radius->getValueAtTime(i);
        EXPECT_TRUE(std::abs(v - i / 100.) < 1e-6);
    }
}

/// A freshly created node must already carry a computed, non-zero hash, and
/// reading that hash must not itself mutate it.
///
/// The non-zero half is what gives the two tests below their teeth: getHashValue()
/// is documented to return 0 when the hash has never been computed, so if node
/// creation stopped computing it, their EXPECT_NE(before, after) would still pass
/// with `before` merely being 0.
TEST_F(BaseTest, HashComputedAtCreationAndStableAcrossReads)
{
    NodePtr generator = createNode(_generatorPluginID);
    ASSERT_TRUE(bool(generator));

    U64 hash1 = generator->getHashValue();
    EXPECT_NE((U64)0, hash1) << "node creation must compute the hash";

    U64 hash2 = generator->getHashValue();
    EXPECT_EQ(hash1, hash2) << "getHashValue() must be side-effect-free";
}

/// The hash keys the cache by what the node renders, so returning a knob to an
/// earlier value must give the earlier hash back and reuse what was cached then.
TEST_F(BaseTest, HashReturnsWithTheState)
{
    NodePtr generator = createNode(_generatorPluginID);
    ASSERT_TRUE(bool(generator));

    KnobDouble* slope = dynamic_cast<KnobDouble*>(generator->getKnobByName("noiseZSlope").get());
    ASSERT_TRUE(slope != 0);

    const double original = slope->getValue();
    const U64 hashBefore = generator->getHashValue();
    slope->setValue(original + 1.);
    EXPECT_NE(hashBefore, generator->getHashValue());
    slope->setValue(original);
    EXPECT_EQ(hashBefore, generator->getHashValue());
}

/// The same state reached by another edit path, in another project, or under
/// another name hashes the same: none of them changes what the node renders.
TEST_F(BaseTest, HashIgnoresTheEditPathTheProjectAndTheName)
{
    ProjectPtr project = getApp()->getProject();
    project->reset(false, true);

    U64 firstProjectHash = 0;
    qint64 firstCreationTime = 0;
    {
        NodePtr direct = createNode(QString::fromUtf8(kTestPluginIDCounting));
        NodePtr detour = createNode(QString::fromUtf8(kTestPluginIDCounting));
        ASSERT_TRUE(direct && detour);
        KnobDouble* directValue = dynamic_cast<KnobDouble*>(direct->getKnobByName("value").get());
        KnobDouble* detourValue = dynamic_cast<KnobDouble*>(detour->getKnobByName("value").get());
        ASSERT_TRUE(directValue && detourValue);

        directValue->setValue(0.5);
        detourValue->setValue(2.);
        detourValue->setValue(0.75);
        detourValue->setValue(0.5);
        EXPECT_NE(direct->getScriptName(), detour->getScriptName());
        EXPECT_EQ(direct->getHashValue(), detour->getHashValue()) << "the edit path changed the hash";

        firstProjectHash = direct->getHashValue();
        direct->setScriptName("renamedCounting");
        direct->refreshHashNow();
        EXPECT_EQ(std::string("renamedCounting"), direct->getScriptName());
        EXPECT_EQ(firstProjectHash, direct->getHashValue()) << "renaming a node without expressions changed its hash";

        firstCreationTime = project->getProjectCreationTime();
    }

    // The creation time has millisecond resolution; the wait makes the second project's differ.
    QThread::msleep(5);
    project->reset(false, true);
    ASSERT_NE(firstCreationTime, project->getProjectCreationTime());

    NodePtr rebuilt = createNode(QString::fromUtf8(kTestPluginIDCounting));
    ASSERT_TRUE(bool(rebuilt));
    KnobDouble* rebuiltValue = dynamic_cast<KnobDouble*>(rebuilt->getKnobByName("value").get());
    ASSERT_TRUE(rebuiltValue != 0);
    rebuiltValue->setValue(0.5);
    EXPECT_EQ(firstProjectHash, rebuilt->getHashValue()) << "the same graph hashes differently in another project";

    project->reset(false, true);
}

/// Changing a knob's value must change the node's hash, otherwise a cached
/// render from before the change could be returned for a different render.
TEST_F(BaseTest, HashChangesOnKnobValueChange)
{
    NodePtr generator = createNode(_generatorPluginID);
    ASSERT_TRUE(bool(generator));

    KnobIPtr knob = generator->getKnobByName("noiseZSlope");
    KnobDouble* slope = dynamic_cast<KnobDouble*>(knob.get());
    ASSERT_TRUE(slope != 0);

    U64 hashBefore = generator->getHashValue();
    slope->setValue(slope->getValue() + 1.);
    U64 hashAfter = generator->getHashValue();

    EXPECT_NE(hashBefore, hashAfter);
}

/// Connecting an input must change the downstream node's hash, since the
/// downstream render now depends on a different (or newly-present) upstream tree.
TEST_F(BaseTest, HashChangesOnInputConnected)
{
    NodePtr generator = createNode(_generatorPluginID);
    NodePtr writer = createNativeWriter();
    ASSERT_TRUE(generator && writer);

    U64 hashBefore = writer->getHashValue();
    connectNodes(generator, writer, 0, true);
    U64 hashAfter = writer->getHashValue();

    EXPECT_NE(hashBefore, hashAfter);
}

/// Two nodes in the same state are keyed alike, so the second one is served the
/// image the first one cached and never renders itself.
TEST_F(BaseTest, IdenticalNodesShareOneCacheEntry)
{
    getApp()->getProject()->reset(false, true);
    appPTR->clearAllCaches();
    CountingTestRegistry::reset();

    NodePtr first = createNode(QString::fromUtf8(kTestPluginIDCounting));
    NodePtr second = createNode(QString::fromUtf8(kTestPluginIDCounting));
    NodePtr merge = createNode(QString::fromUtf8(kTestPluginIDCountingMerge));
    ASSERT_TRUE(first && second && merge);
    const NodePtr leaves[2] = { first, second };
    for (int i = 0; i < 2; ++i) {
        KnobDouble* value = dynamic_cast<KnobDouble*>(leaves[i]->getKnobByName("value").get());
        KnobBool* cacheOutput = dynamic_cast<KnobBool*>(leaves[i]->getKnobByName("cacheOutput").get());
        ASSERT_TRUE(value && cacheOutput);
        value->setValue(0.25);
        cacheOutput->setValue(true);
    }
    KnobDouble* mergeValue = dynamic_cast<KnobDouble*>(merge->getKnobByName("value").get());
    ASSERT_TRUE(mergeValue != 0);
    mergeValue->setValue(0.);
    connectNodes(first, merge, 0, true);
    connectNodes(second, merge, 1, true);

    EXPECT_NE(first->getScriptName(), second->getScriptName());
    ASSERT_EQ(first->getHashValue(), second->getHashValue());

    std::vector<RenderedPlane> planes;
    std::string error;
    // Through the cache: bypassing it would make each leaf drop the entry it finds and render again.
    ASSERT_TRUE(renderNodePlanesDirect(merge, 1., ViewIdx(0), 0, RectI(0, 0, kCountingTestSize, kCountingTestSize),
                                       std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()), &planes, &error,
                                       false /*byPassCache*/))
        << error;
    ASSERT_EQ(1u, planes.size());
    ASSERT_FALSE(planes[0].pixels.empty());
    // Each input holds 0.25, so anything but 0.5 means one of them was not rendered.
    EXPECT_FLOAT_EQ(0.5f, planes[0].pixels[0]);
    EXPECT_EQ(1, CountingTestRegistry::renders(first) + CountingTestRegistry::renders(second))
        << "first rendered " << CountingTestRegistry::renders(first) << " times, second " << CountingTestRegistry::renders(second);
    EXPECT_EQ(1, CountingTestRegistry::renders(merge));

    CountingTestRegistry::reset();
    appPTR->clearAllCaches();
    getApp()->getProject()->reset(false, true);
}

/// A node's results are keyed by its state, so leaving a state does not drop them:
/// going back to it is served from the image cache and the actions cache, with
/// neither a render nor a region of definition call on the node.
TEST_F(BaseTest, ReturningToAnEarlierStateReusesItsCachedResults)
{
    DisableUnreachableRAMPurging noPurging;
    getApp()->getProject()->reset(false, true);
    appPTR->clearAllCaches();
    CountingTestRegistry::reset();

    NodePtr leaf = createNode(QString::fromUtf8(kTestPluginIDCounting));
    NodePtr root = createNode(QString::fromUtf8(kTestPluginIDCounting));
    ASSERT_TRUE(leaf && root);
    KnobDouble* leafValue = dynamic_cast<KnobDouble*>(leaf->getKnobByName("value").get());
    KnobBool* leafCacheOutput = dynamic_cast<KnobBool*>(leaf->getKnobByName("cacheOutput").get());
    KnobDouble* rootValue = dynamic_cast<KnobDouble*>(root->getKnobByName("value").get());
    ASSERT_TRUE(leafValue && leafCacheOutput && rootValue);
    leafCacheOutput->setValue(true);
    leafValue->setValue(1.);
    rootValue->setValue(0.);
    connectNodes(leaf, root, 0, true);

    // The root passes the leaf's value through and does not cache its own output. The renders go
    // through the cache, which they leave as it is between calls.
    const RectI window(0, 0, kCountingTestSize, kCountingTestSize);
    const auto renderRoot = [&](float* out) {
        std::vector<RenderedPlane> planes;
        std::string error;
        ASSERT_TRUE(renderNodePlanesDirect(root, 1., ViewIdx(0), 0, window,
                                           std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()), &planes, &error,
                                           false /*byPassCache*/))
            << error;
        ASSERT_EQ(1u, planes.size());
        ASSERT_FALSE(planes[0].pixels.empty());
        *out = planes[0].pixels[0];
    };

    const U64 firstHash = leaf->getHashValue();
    float corner = 0.f;
    CountingTestRegistry::reset();
    renderRoot(&corner);
    EXPECT_FLOAT_EQ(1.f, corner);
    EXPECT_EQ(1, CountingTestRegistry::renders(leaf));

    // Counted from before the edit, so that a region of definition worked out while the edit is
    // handled counts too.
    CountingTestRegistry::reset();
    leafValue->setValue(2.);
    ASSERT_NE(firstHash, leaf->getHashValue());
    renderRoot(&corner);
    EXPECT_FLOAT_EQ(2.f, corner);
    EXPECT_EQ(1, CountingTestRegistry::renders(leaf)) << "a new state was served another state's image";
    EXPECT_GT(CountingTestRegistry::regionOfDefinitionCalls(leaf), 0) << "a new state was served another state's region of definition";

    CountingTestRegistry::reset();
    leafValue->setValue(1.);
    ASSERT_EQ(firstHash, leaf->getHashValue());
    renderRoot(&corner);
    EXPECT_FLOAT_EQ(1.f, corner);
    EXPECT_EQ(0, CountingTestRegistry::renders(leaf)) << "the image of the earlier state was not reused";
    EXPECT_EQ(0, CountingTestRegistry::regionOfDefinitionCalls(leaf)) << "the actions of the earlier state were not reused";
    EXPECT_EQ(1, CountingTestRegistry::renders(root));

    CountingTestRegistry::reset();
    appPTR->clearAllCaches();
    getApp()->getProject()->reset(false, true);
}

///High level test: simple node connections test
TEST_F(BaseTest, SimpleNodeConnections) {
    ///create the generator
    NodePtr generator = createNode(_generatorPluginID);

    ///create the writer and set its output filename
    NodePtr writer = createNativeWriter();

    ASSERT_TRUE(writer && generator);
    connectNodes(generator, writer, 0, true);
    connectNodes(generator, writer, 0, false); //< expect it to fail
    disconnectNodes(generator, writer, true);
    disconnectNodes(generator, writer, false);
    connectNodes(generator, writer, 0, true);
}
