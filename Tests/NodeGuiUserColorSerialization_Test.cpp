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

#include "Serialization/ProjectGuiSerialization.h"
#include "Serialization/Yaml/ProjectGuiYaml.h"

NATRON_NAMESPACE_USING

namespace {

Serialization::NodeGuiSerialization
roundTrip(const Serialization::NodeGuiSerialization& original)
{
    Serialization::ProjectGuiSerialization gui;
    gui.nodes.push_back(original);
    const Serialization::ProjectGuiSerialization decoded = Serialization::decodeProjectGuiYaml(Serialization::encodeProjectGuiYaml(gui));
    if (decoded.nodes.size() != 1) {
        ADD_FAILURE() << "Expected one node back, got " << decoded.nodes.size();

        return Serialization::NodeGuiSerialization();
    }

    return decoded.nodes.front();
}

Serialization::NodeGuiSerialization
makeNodeGui()
{
    Serialization::NodeGuiSerialization node;
    node.nodeName = "Blur1";
    node.colorWasFound = true;
    node.colorR = 0.8f;
    node.colorG = 0.5f;
    node.colorB = 0.3f;

    return node;
}

} // namespace

TEST(NodeGuiUserColorSerialization, RoundTripsAUserColor)
{
    Serialization::NodeGuiSerialization original = makeNodeGui();
    original.hasUserColor = true;
    original.userColorR = 0.25f;
    original.userColorG = 0.5f;
    original.userColorB = 0.75f;

    Serialization::NodeGuiSerialization restored = roundTrip(original);

    float r = -1.f, g = -1.f, b = -1.f;
    ASSERT_TRUE(Serialization::resolveNodeGuiUserColor(restored, &r, &g, &b));
    EXPECT_FLOAT_EQ(0.25f, r);
    EXPECT_FLOAT_EQ(0.5f, g);
    EXPECT_FLOAT_EQ(0.75f, b);
}

TEST(NodeGuiUserColorSerialization, ANodeWithoutAUserColorNeverGuessesOne)
{
    Serialization::NodeGuiSerialization restored = roundTrip(makeNodeGui());

    float r, g, b;
    EXPECT_FALSE(Serialization::resolveNodeGuiUserColor(restored, &r, &g, &b));
}
