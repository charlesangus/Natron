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

#include <string>

#include <gtest/gtest.h>

#include "Engine/AppManager.h"
#include "Engine/Plugin.h"

NATRON_NAMESPACE_USING

static void
expectSingleVersion(const char* pluginID,
                    int expectedMajor)
{
    const PluginsMap& plugins = appPTR->getPluginsList();
    PluginsMap::const_iterator it = plugins.find(pluginID);

    ASSERT_TRUE(it != plugins.end()) << pluginID;
    ASSERT_EQ(1u, it->second.size()) << pluginID;
    EXPECT_EQ(expectedMajor, (*it->second.begin())->getMajorVersion()) << pluginID;
}

TEST(PluginIdentity, HueCorrectVersionsHaveDistinctIDs)
{
    expectSingleVersion("net.sf.openfx.HueCorrect", 2);
    expectSingleVersion("net.sf.openfx.HueCorrect1", 1);
}

TEST(PluginIdentity, TextVersionsHaveDistinctIDs)
{
    expectSingleVersion("net.fxarena.openfx.Text", 6);
    expectSingleVersion("net.fxarena.openfx.MagickText", 5);
}
