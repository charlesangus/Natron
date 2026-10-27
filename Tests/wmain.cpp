#include <stdio.h>

#include <list>
#include <map>
#include <string>

#include "gtest/gtest.h"

#include "Engine/AppManager.h"
#include "Engine/CLArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/LibraryBinary.h"
#include "Engine/Plugin.h"
#include "Engine/PluginActionShortcut.h"
#include "Engine/ViewerInstance.h"

#include "CacheStressTestEffect.h"
#include "ContextTestEffects.h"
#include "CountingTestEffect.h"
#include "CurveHostTestEffect.h"
#include "DataKindTestEffect.h"
#include "DeepRenderTestEffect.h"
#include "IdentityHopTestEffect.h"
#include "InputChangedFetchTestEffect.h"
#include "MultiplanarTestEffect.h"
#include "Native/GlHostWrapTestEffect.h"
#include "NativeMetadataTestEffect.h"
#include "NativeParity.h"
#include "PassThroughRoDTestEffect.h"
#include "PerProcessUserDirs.h"
#include "SceneTestEffect.h"
#include "TestBuiltInPlugins.h"
#include "TestOfxPluginPath.h"
#include "TestPlaneChoiceTakeOvers.h"
#include "TestRetiredOfxIds.h"

using namespace NATRON_NAMESPACE;

namespace {
// Mirrors AppManager::registerBuiltInPlugin<PLUGIN>(), which cannot be called from here directly:
// it is a protected member template whose definition lives in AppManager.cpp, so it is never
// instantiated for a type this file names. registerPlugin() itself is public and ordinary, so
// calling it the same way with a hand-built LibraryBinary reaches the same registry.
template <typename PLUGIN>
void
registerTestBuiltInPlugin()
{
    EffectInstancePtr node(PLUGIN::BuildEffect(NodePtr()));
    std::map<std::string, void (*)()> functions;

    testRegisteredPluginIds().insert(node->getPluginID());
    functions.insert(std::make_pair("BuildEffect", (void (*)())&PLUGIN::BuildEffect));
    LibraryBinary* binary = new LibraryBinary(functions);

    std::list<std::string> grouping;
    node->getPluginGrouping(&grouping);
    QStringList qgrouping;
    for (std::list<std::string>::iterator it = grouping.begin(); it != grouping.end(); ++it) {
        qgrouping.push_back(QString::fromUtf8(it->c_str()));
    }

    Plugin* p = appPTR->registerPlugin(QString(), qgrouping, QString::fromUtf8(node->getPluginID().c_str()), QString::fromUtf8(node->getPluginLabel().c_str()),
                                       QString::fromUtf8(""), QStringList(), node->isReader(), node->isWriter(), binary, node->renderThreadSafety() == eRenderSafetyUnsafe, node->getMajorVersion(), node->getMinorVersion(), false);
    // Registration happens after AppManager::load(), i.e. after onAllPluginsLoaded() has already
    // assigned every plugin its label-without-suffix. Without this these plugins keep an empty one,
    // and every node built from them ends up with an empty script name.
    p->setLabelWithoutSuffix(Plugin::makeLabelWithoutSuffix(p->getPluginLabel()));

    std::list<PluginActionShortcut> shortcuts;
    node->getPluginShortcuts(&shortcuts);
    p->setShorcuts(shortcuts);
    p->setOpenGLRenderSupport(node->supportsOpenGLRender());
}

void
registerDataKindTestPlugins()
{
    // Registered purely so DataKind_Test.cpp can exercise Node::canConnectInput() against
    // concrete non-image data kinds: no shipped plugin declares one.
    registerTestBuiltInPlugin<DataKindTestDeepSource>();
    registerTestBuiltInPlugin<DataKindTestImageSink>();
    registerTestBuiltInPlugin<DataKindTestDeepSink>();
    registerTestBuiltInPlugin<DataKindTestPolyTwoInputs>();
    registerTestBuiltInPlugin<DataKindTestPolyAndImageInput>();
    registerTestBuiltInPlugin<DataKindTestScenePolicy>();
    registerTestBuiltInPlugin<DataKindTestSelectFirstInputPolicy>();
    registerTestBuiltInPlugin<DataKindTestConsumerMirrorPolicy>();
    registerTestBuiltInPlugin<DataKindTestAdapterSink>();
    registerTestBuiltInPlugin<DataKindTestSceneAdapterSink>();
    registerTestBuiltInPlugin<DataKindTestRawSource>();
    registerTestBuiltInPlugin<DataKindTestRawSink>();
    registerTestBuiltInPlugin<DataKindTestRawAdapterSink>();
    // The background AppManager does not register the Viewer, and the Viewer is the one image input
    // that takes raw, so ViewerRaw_Test.cpp needs it registered by hand.
    registerTestBuiltInPlugin<ViewerInstance>();
}

void
registerDeepRenderTestPlugins()
{
    // Two deep-capable stubs chained together, so DeepRenderPipeline_Test.cpp can drive a real
    // renderDeepRoI() pull through more than one node; a deep source serving hand-built sample
    // layouts and an image generator, so DeepNodes_Test.cpp can feed the merge and conversion
    // nodes exactly what it wants to reason about.
    registerTestBuiltInPlugin<DeepRenderTestSource>();
    registerTestBuiltInPlugin<DeepRenderTestGain>();
    registerTestBuiltInPlugin<DeepRenderTestReshape>();
    registerTestBuiltInPlugin<DeepSyntheticSource>();
    registerTestBuiltInPlugin<DeepLayersStub>();
    registerTestBuiltInPlugin<ImageRenderTestSource>();
}

void
registerMultiplanarTestPlugins()
{
    // Registered so LayerKnobsRender_Test.cpp can exercise a multiplanar effect that never
    // produces Color, which no shipped plugin does today.
    registerTestBuiltInPlugin<MultiplanarDiffuseOnlyTestEffect>();
}

void
registerGraphScalingTestPlugins()
{
    // Registered so GraphScaling_Test.cpp can pull an input from inside onInputChanged, which no
    // shipped native node does.
    registerTestBuiltInPlugin<InputChangedFetchTestEffect>();
}

void
registerSceneTestPlugins()
{
    // A scene source and a scene op, so Scene/SceneAction_Test.cpp can pull scenes through the
    // engine's action before any shipped scene node exists, and a geometry stand-in for the
    // Material input until shipped primitives exist.
    registerTestBuiltInPlugin<SceneTestSource>();
    registerTestBuiltInPlugin<SceneTestOp>();
    registerTestBuiltInPlugin<SceneTestGeometry>();
}

void
registerRenderSchedulerTestPlugins()
{
    // Registered to count the render calls of every task, which no shipped plugin exposes.
    registerTestBuiltInPlugin<CountingTestEffect>();
    registerTestBuiltInPlugin<CountingMergeTestEffect>();
    // Registered to be an identity over part of its window only, which no shipped native node is.
    registerTestBuiltInPlugin<TiledIdentityTestEffect>();
    // Registered to request inputs under render contexts and read them, which no shipped plugin does yet.
    registerTestBuiltInPlugin<ContextReaderTestEffect>();
    registerTestBuiltInPlugin<ContextFanOutTestEffect>();
    registerTestBuiltInPlugin<ContextChainTestEffect>();
    registerTestBuiltInPlugin<ContextFormatTestEffect>();
    registerTestBuiltInPlugin<ContextRegionTestEffect>();
    registerTestBuiltInPlugin<ContextUnionTestEffect>();
    registerTestBuiltInPlugin<ContextSetTestEffect>();
}

void
registerIdentityHopTestPlugins()
{
    // Registered so IdentityHop_Test.cpp can chain identities that shift time and view and read
    // back the time and view the source was pulled at, which no shipped node reports.
    registerTestBuiltInPlugin<FrameValueTestEffect>();
    registerTestBuiltInPlugin<ViewSelfIdentityTestEffect>();
    registerTestBuiltInPlugin<TimeShiftIdentityTestEffect>();
    registerTestBuiltInPlugin<DeepFrameValueTestEffect>();
    registerTestBuiltInPlugin<DeepViewSelfIdentityTestEffect>();
    registerTestBuiltInPlugin<DeepTimeShiftIdentityTestEffect>();
}

void
registerFrameGraphBuildTestPlugins()
{
    // Registered so FrameGraphBuild_Test.cpp can build a chain thousands of nodes deep whose RoD queries recurse
    // upstream like OpenFX filters' do, without the cost of creating that many OpenFX nodes.
    registerTestBuiltInPlugin<PassThroughRoDTestEffect>();
}

void
registerCurveHostTestPlugins()
{
    // Registered so CurveHost_Test.cpp can edit curves on a geometry curve host, which no shipped
    // node is yet.
    registerTestBuiltInPlugin<CurveHostTestEffect>();
}

void
registerNativeMetadataTestPlugins()
{
    // Registered so NativeMetadata_Test.cpp can inject metadata at the head of a native chain,
    // which no shipped node does.
    registerTestBuiltInPlugin<MetadataSourceTestEffect>();
    registerTestBuiltInPlugin<MetadataFilterTestEffect>();
}

void
registerNativeParityTestPlugins()
{
    // Registered so the native-node parity tests feed the OFX and native versions of a node the
    // same deterministic multiplanar picture, with negatives, super-whites and exact alpha bands.
    registerTestBuiltInPlugin<ParitySourceTestEffect>();
    registerTestBuiltInPlugin<GlHostWrapTestEffect>();
}

void
registerCacheStressTestPlugins()
{
    // A source whose render remembers a knob value outside every engine cache, so the cache
    // stress self-test can plant a hole the harness must catch.
    registerTestBuiltInPlugin<StaleMemoTestEffect>();
    // A filter whose knob edits queue under a render, for the queued-edit tests.
    registerTestBuiltInPlugin<QueuedEditTestEffect>();
    // A filter that stores an actions-cache answer from onInputChanged().
    registerTestBuiltInPlugin<InputChangedTestEffect>();
}
}

#if defined(_WIN32) && defined(UNICODE)
GTEST_API_ int wmain(int argc, wchar_t **argv)
#else
GTEST_API_ int main(int argc, char **argv)
#endif
{
    printf("Running main() from wmain.cpp\n");
    ::testing::InitGoogleTest(&argc, argv);
    // ctest lists the suites before every run. Every test is registered statically, so the list
    // does not depend on load(), which would cost a cold OFX rescan plus Python and OCIO init.
    if (::testing::GTEST_FLAG(list_tests)) {
        return RUN_ALL_TESTS();
    }
    PerProcessUserDirs userDirs;
    appendTestOfxPluginDir();
    applyTestRetiredOfxIds();
    applyTestPlaneChoiceTakeOvers();
    AppManager manager;

    {
        int argc = 0;
        QStringList args;
#if defined(_WIN32) && defined(UNICODE)
        args << QString::fromWCharArray(argv[0]);
#else
        args << QString::fromUtf8(argv[0]);
#endif
        args << QString::fromUtf8("--clear-cache");
        args << QString::fromUtf8("--clear-openfx-cache");
        args << QString::fromUtf8("--no-settings");
        args << QString::fromUtf8("--settings") << QString::fromUtf8("useStdOFXPluginsLocation=False");
        CLArgs cl(args, true);
        if (!manager.load(argc, 0, cl)) {
            printf("Failed to load AppManager\n");

            return 1;
        }
    }
    registerDataKindTestPlugins();
    registerDeepRenderTestPlugins();
    registerSceneTestPlugins();
    registerMultiplanarTestPlugins();
    registerGraphScalingTestPlugins();
    registerRenderSchedulerTestPlugins();
    registerIdentityHopTestPlugins();
    registerFrameGraphBuildTestPlugins();
    registerNativeParityTestPlugins();
    registerNativeMetadataTestPlugins();
    registerCurveHostTestPlugins();
    registerCacheStressTestPlugins();
    int retval = RUN_ALL_TESTS();
    return retval;
}
