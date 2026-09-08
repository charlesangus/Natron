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

#include "DataKindTestEffect.h"

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
    int retval = RUN_ALL_TESTS();
    return retval;
}
