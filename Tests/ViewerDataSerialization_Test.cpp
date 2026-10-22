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

TEST(ViewerDataSerialization, RoundTripsTheDisplayTransform)
{
    Serialization::ViewerDataSerialization original;
    original.display = "sRGB - Display";
    original.view = "Un-tone-mapped";
    original.look = "ACES 1.3 Reference Gamut Compression";

    Serialization::ProjectGuiSerialization gui;
    gui.viewers["Viewer1"] = original;
    const Serialization::ProjectGuiSerialization decoded = Serialization::decodeProjectGuiYaml(Serialization::encodeProjectGuiYaml(gui));

    ASSERT_EQ(1u, decoded.viewers.count("Viewer1"));
    const Serialization::ViewerDataSerialization& restored = decoded.viewers.at("Viewer1");
    EXPECT_EQ(original.display, restored.display);
    EXPECT_EQ(original.view, restored.view);
    EXPECT_EQ(original.look, restored.look);
}
