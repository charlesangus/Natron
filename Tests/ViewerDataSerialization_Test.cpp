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

#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include "Engine/RectDSerialization.h"
#include "Gui/ProjectGuiSerialization.h"

NATRON_NAMESPACE_USING

TEST(ViewerDataSerialization, RoundTripsTheDisplayTransform)
{
    ViewerData original = ViewerData();
    original.zoomFactor = 1.;
    original.gain = 1.;
    original.gamma = 1.;
    original.fps = 24.;
    original.aChoice = original.bChoice = -1;
    original.display = "sRGB - Display";
    original.view = "Un-tone-mapped";
    original.look = "ACES 1.3 Reference Gamut Compression";

    std::stringstream stream;
    {
        boost::archive::xml_oarchive oArchive(stream);
        oArchive << boost::serialization::make_nvp("ViewerData", original);
    }

    ViewerData restored = ViewerData();
    {
        boost::archive::xml_iarchive iArchive(stream);
        iArchive >> boost::serialization::make_nvp("ViewerData", restored);
    }

    EXPECT_EQ((unsigned int)VIEWER_DATA_INTRODUCES_OCIO_DISPLAY, restored.version);
    EXPECT_EQ(original.display, restored.display);
    EXPECT_EQ(original.view, restored.view);
    EXPECT_EQ(original.look, restored.look);
    EXPECT_EQ(std::string::npos, stream.str().find("ColorSpace"));
}
