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

#ifndef NATRON_TESTS_NATIVEPLUGINIDENTITY_H
#define NATRON_TESTS_NATIVEPLUGINIDENTITY_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>

#include <gtest/gtest.h>

#include <QString>

#include "Engine/AppManager.h"
#include "Engine/Plugin.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Expects the plug-in registered under id to be the only version of it, at major
 * version major and labelled label. Meant for a native node's own test, so the identity of
 * each node is asserted next to the node rather than in a shared table.
 **/
inline void
expectNativeIdentity(const std::string& id,
                     int major,
                     const std::string& label)
{
    const PluginsMap& plugins = appPTR->getPluginsList();
    const PluginsMap::const_iterator it = plugins.find(id);

    ASSERT_TRUE(it != plugins.end()) << id << " is not registered";
    ASSERT_EQ(1u, it->second.size()) << id << " must have a single registered version";

    const Plugin* plugin = *it->second.begin();

    EXPECT_EQ(major, plugin->getMajorVersion()) << id;
    EXPECT_EQ(label, plugin->getPluginLabel().toStdString()) << id;
}

NATRON_NAMESPACE_EXIT

#endif // NATRON_TESTS_NATIVEPLUGINIDENTITY_H
