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

#include <algorithm>
#include <list>
#include <string>

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

// clang-format off
CLANG_DIAG_OFF(unknown-pragmas)
CLANG_DIAG_OFF(tautological-undefined-compare)
#include <ofxhPluginCache.h>
CLANG_DIAG_ON(tautological-undefined-compare)
CLANG_DIAG_ON(unknown-pragmas)
// clang-format on

#include "OfxTestPlugins/natronTest/NatronTestPluginIDs.h"
#include "TestRetiredOfxIds.h"

#include "Engine/AppManager.h"
#include "Engine/OfxRetired.h"
#include "Engine/Plugin.h"

NATRON_NAMESPACE_USING

namespace {

const char kHookedSuite[] = "OfxRetiredInHookedProcess";

// What the bundles on the plug-in path provide, whether or not the host registered it.
bool
ofxBundleProvides(const std::string& id)
{
    const std::list<OFX::Host::Plugin*>& plugins = OFX::Host::PluginCache::getPluginCache()->getPlugins();

    return std::any_of(plugins.begin(), plugins.end(), [&id](const OFX::Host::Plugin* plugin) { return plugin->getIdentifier() == id; });
}

bool
isRegistered(const std::string& id)
{
    const std::list<std::string> ids = appPTR->getPluginIDs();

    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

} // namespace

TEST(OfxRetiredTest, EveryShippedMarkerNamesAnOfxPluginABundleProvides)
{
    for (const auto& marker : retiredOfxPlugins()) {
        EXPECT_TRUE(ofxBundleProvides(marker.first)) << "Engine/OfxRetired/" << marker.first << " retires an ID no loaded OFX bundle provides";
    }
}

TEST(OfxRetiredTest, NoShippedRetiredIdIsRegisteredAsAnOfxPlugin)
{
    const PluginsMap& plugins = appPTR->getPluginsList();

    for (const auto& marker : retiredOfxPlugins()) {
        const PluginsMap::const_iterator it = plugins.find(marker.first);
        if (it == plugins.end()) {
            continue;
        }
        for (const Plugin* plugin : it->second) {
            EXPECT_FALSE(plugin->getOfxPlugin()) << marker.first << " is retired but still registered as an OFX plug-in";
        }
    }
}

// The retirement itself is shown in a child Tests process that retires one test-bundle plug-in
// through the hook: this process has to keep that plug-in for the tests that use it as a fixture.
TEST(OfxRetiredTest, TheHookSkipsAListedIdAndKeepsTheRestOfItsBundle)
{
    ASSERT_TRUE(isRegistered(kNatronTestGeneratorPluginID)) << "the test bundle must be loaded here for the child's result to mean anything";

    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QString::fromUtf8(NATRON_TEST_RETIRED_OFX_IDS_ENV_VAR), QString::fromUtf8(kNatronTestGeneratorPluginID));

    QProcess child;
    child.setProcessEnvironment(environment);
    child.setProcessChannelMode(QProcess::MergedChannels);
    child.start(QCoreApplication::applicationFilePath(),
                QStringList() << QString::fromUtf8("--gtest_filter=%1.*").arg(QString::fromUtf8(kHookedSuite)));
    ASSERT_TRUE(child.waitForStarted(30000)) << child.errorString().toStdString();
    // The child loads every OFX bundle from a cold cache before it runs anything.
    if (!child.waitForFinished(240000)) {
        child.kill();
        child.waitForFinished(10000);
        const std::string output = QString::fromUtf8(child.readAll()).toStdString();
        FAIL() << "the child Tests process did not finish: " << output;
    }
    const std::string output = QString::fromUtf8(child.readAll()).toStdString();

    EXPECT_EQ(QProcess::NormalExit, child.exitStatus()) << output;
    EXPECT_EQ(0, child.exitCode()) << output;
    const std::string passed = std::string("[       OK ] ") + kHookedSuite + ".";
    EXPECT_NE(std::string::npos, output.find(passed)) << "the child ran no hooked test: " << output;
}

TEST(OfxRetiredInHookedProcess, ListedIdIsSkippedAndUnlistedIdsOfItsBundleLoad)
{
    // Every Tests process runs this case, but only the child started above retires anything.
    if (!testRetiredOfxIds().contains(QString::fromUtf8(kNatronTestGeneratorPluginID))) {
        return;
    }

    ASSERT_TRUE(ofxBundleProvides(kNatronTestGeneratorPluginID)) << "the test bundle was not scanned, so the skip below would prove nothing";
    EXPECT_FALSE(isRegistered(kNatronTestGeneratorPluginID));
    EXPECT_TRUE(isRegistered(kNatronTestTwoInputCopyPluginID));
    EXPECT_TRUE(isRegistered(kNatronTestPointMatrixPluginID));
}
