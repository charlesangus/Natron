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

#include <set>
#include <string>

#include <gtest/gtest.h>

#include <QString>

#include "Engine/AppManager.h"
#include "Engine/Nodes/Channel/Shuffle.h"
#include "Engine/Plugin.h"

NATRON_NAMESPACE_USING

// The OFX plugin registry is populated once, at process start, from OFX_PLUGIN_PATH -- so this
// only actually exercises the openfx-misc build when the test run points OFX_PLUGIN_PATH at a
// Misc.ofx.bundle built from a source tree that no longer has a Shuffle directory.
TEST(ShufflePluginList, OfxShuffleIsAbsent)
{
    const PluginsMap& plugins = appPTR->getPluginsList();

    EXPECT_TRUE(plugins.find("net.sf.openfx.ShufflePlugin") == plugins.end());
}

TEST(ShufflePluginList, NativeShuffleAndShuffleCopyAreTheOnlyChannelShuffles)
{
    const PluginsMap& plugins = appPTR->getPluginsList();
    const QString channelGroup = QString::fromUtf8(PLUGIN_GROUP_CHANNEL);
    const QString shuffleSubstring = QString::fromUtf8("Shuffle");

    std::set<std::string> shufflesInChannelGroup;

    for (PluginsMap::const_iterator it = plugins.begin(); it != plugins.end(); ++it) {
        for (PluginVersionsOrdered::const_iterator vit = it->second.begin(); vit != it->second.end(); ++vit) {
            const Plugin* p = *vit;

            if (!p->getGrouping().contains(channelGroup)) {
                continue;
            }
            if (p->getPluginID().contains(shuffleSubstring) || p->getPluginLabel().contains(shuffleSubstring)) {
                shufflesInChannelGroup.insert(it->first);
            }
        }
    }

    std::set<std::string> expected;
    expected.insert(PLUGINID_NATRON_SHUFFLE);
    expected.insert(PLUGINID_NATRON_SHUFFLECOPY);

    EXPECT_EQ(expected, shufflesInChannelGroup);
}
