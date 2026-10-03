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

#include <boost/archive/binary_iarchive.hpp>
#include <boost/archive/binary_oarchive.hpp>

#include "Engine/RectDSerialization.h"
#include "Gui/ProjectGuiSerialization.h"

NATRON_NAMESPACE_USING

namespace {

// The ViewerData layout of the version before the OCIO display fields, which stored a viewer
// colourspace where Display, View and Look now are.
struct LegacyViewerData {
    double zoomLeft = 12.;
    double zoomBottom = -3.;
    double zoomFactor = 0.5;
    bool userRoIenabled = true;
    RectD userRoI = RectD(1., 2., 30., 40.);
    bool isClippedToProject = false;
    bool autoContrastEnabled = true;
    double gain = 2.;
    double gamma = 1.5;
    std::string colorSpace = "Linear(None)";
    std::string layerName = "Color";
    std::string alphaLayerName = "Color.A";
    std::string channels = "Luminance";
    bool renderScaleActivated = true;
    unsigned int mipmapLevel = 2;
    bool zoomOrPanSinceLastFit = true;
    int wipeCompositingOp = 3;
    int leftBound = 5;
    int rightBound = 50;
    bool leftToolbarVisible = false;
    bool rightToolbarVisible = true;
    bool topToolbarVisible = false;
    bool playerVisible = true;
    bool timelineVisible = false;
    bool infobarVisible = true;
    bool isInputAPaused = true;
    bool isInputBPaused = false;
    bool checkerboardEnabled = true;
    double fps = 30.;
    bool fpsLocked = false;
    int aChoice = 1;
    int bChoice = 2;
    bool fullFrame = true;

    template <class Archive>
    void serialize(Archive& ar,
                   const unsigned int /*version*/)
    {
        ar& ::boost::serialization::make_nvp("zoomLeft", zoomLeft);
        ar& ::boost::serialization::make_nvp("zoomBottom", zoomBottom);
        ar& ::boost::serialization::make_nvp("zoomFactor", zoomFactor);
        ar& ::boost::serialization::make_nvp("UserRoIEnabled", userRoIenabled);
        ar& ::boost::serialization::make_nvp("UserRoI", userRoI);
        ar& ::boost::serialization::make_nvp("ClippedToProject", isClippedToProject);
        ar& ::boost::serialization::make_nvp("AutoContrast", autoContrastEnabled);
        ar& ::boost::serialization::make_nvp("Gain", gain);
        ar& ::boost::serialization::make_nvp("Gain", gamma);
        ar& ::boost::serialization::make_nvp("ColorSpace", colorSpace);
        ar& ::boost::serialization::make_nvp("Layer", layerName);
        ar& ::boost::serialization::make_nvp("AlphaLayer", alphaLayerName);
        ar& ::boost::serialization::make_nvp("Channels", channels);
        ar& ::boost::serialization::make_nvp("RenderScaleActivated", renderScaleActivated);
        ar& ::boost::serialization::make_nvp("MipMapLevel", mipmapLevel);
        ar& ::boost::serialization::make_nvp("ZoomOrPanSinceFit", zoomOrPanSinceLastFit);
        ar& ::boost::serialization::make_nvp("CompositingOP", wipeCompositingOp);
        ar& ::boost::serialization::make_nvp("LeftBound", leftBound);
        ar& ::boost::serialization::make_nvp("RightBound", rightBound);
        ar& ::boost::serialization::make_nvp("LeftToolbarVisible", leftToolbarVisible);
        ar& ::boost::serialization::make_nvp("RightToolbarVisible", rightToolbarVisible);
        ar& ::boost::serialization::make_nvp("TopToolbarVisible", topToolbarVisible);
        ar& ::boost::serialization::make_nvp("PlayerVisible", playerVisible);
        ar& ::boost::serialization::make_nvp("TimelineVisible", timelineVisible);
        ar& ::boost::serialization::make_nvp("InfobarVisible", infobarVisible);
        ar& ::boost::serialization::make_nvp("isInputAPaused", isInputAPaused);
        ar& ::boost::serialization::make_nvp("isInputBPaused", isInputBPaused);
        ar& ::boost::serialization::make_nvp("CheckerboardEnabled", checkerboardEnabled);
        ar& ::boost::serialization::make_nvp("Fps", fps);
        ar& ::boost::serialization::make_nvp("FpsLocked", fpsLocked);
        ar& ::boost::serialization::make_nvp("aInput", aChoice);
        ar& ::boost::serialization::make_nvp("bInput", bChoice);
        ar& ::boost::serialization::make_nvp("fullFrame", fullFrame);
    }
};

void
expectLegacyFieldsLoaded(const LegacyViewerData& legacy,
                         const ViewerData& restored)
{
    EXPECT_EQ((unsigned int)VIEWER_DATA_INTRODUCES_FULL_FRAME_PROC, restored.version);
    EXPECT_EQ(std::string(), restored.display);
    EXPECT_EQ(std::string(), restored.view);
    EXPECT_EQ(std::string(), restored.look);
    EXPECT_EQ(legacy.zoomLeft, restored.zoomLeft);
    EXPECT_EQ(legacy.zoomFactor, restored.zoomFactor);
    EXPECT_EQ(legacy.userRoI.x2, restored.userRoI.x2);
    EXPECT_EQ(legacy.gain, restored.gain);
    EXPECT_EQ(legacy.gamma, restored.gamma);
    EXPECT_EQ(legacy.layerName, restored.layerName);
    EXPECT_EQ(legacy.alphaLayerName, restored.alphaLayerName);
    EXPECT_EQ(legacy.channels, restored.channels);
    EXPECT_EQ(legacy.mipmapLevel, restored.mipmapLevel);
    EXPECT_EQ(legacy.wipeCompositingOp, restored.wipeCompositingOp);
    EXPECT_EQ(legacy.rightBound, restored.rightBound);
    EXPECT_EQ(legacy.timelineVisible, restored.timelineVisible);
    EXPECT_EQ(legacy.isInputAPaused, restored.isPauseEnabled[0]);
    EXPECT_EQ(legacy.fps, restored.fps);
    EXPECT_EQ(legacy.fpsLocked, restored.fpsLocked);
    EXPECT_EQ(legacy.aChoice, restored.aChoice);
    EXPECT_EQ(legacy.bChoice, restored.bChoice);
    EXPECT_EQ(legacy.fullFrame, restored.isFullFrameProcessEnabled);
}

} // namespace

BOOST_CLASS_VERSION(LegacyViewerData, VIEWER_DATA_INTRODUCES_FULL_FRAME_PROC)

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

TEST(ViewerDataSerialization, AnXmlArchiveFromBeforeTheDisplayTransformLoadsWithEmptyDisplayFields)
{
    const LegacyViewerData legacy;
    std::stringstream stream;
    {
        boost::archive::xml_oarchive oArchive(stream);
        oArchive << boost::serialization::make_nvp("ViewerData", legacy);
    }
    ASSERT_NE(std::string::npos, stream.str().find("ColorSpace"));

    ViewerData restored = ViewerData();
    restored.display = restored.view = restored.look = "stale";
    {
        boost::archive::xml_iarchive iArchive(stream);
        ASSERT_NO_THROW(iArchive >> boost::serialization::make_nvp("ViewerData", restored));
    }

    expectLegacyFieldsLoaded(legacy, restored);
}

TEST(ViewerDataSerialization, ABinaryArchiveFromBeforeTheDisplayTransformLoadsWithEmptyDisplayFields)
{
    const LegacyViewerData legacy;
    std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
    {
        boost::archive::binary_oarchive oArchive(stream);
        oArchive << legacy;
    }

    ViewerData restored = ViewerData();
    restored.display = restored.view = restored.look = "stale";
    {
        boost::archive::binary_iarchive iArchive(stream);
        ASSERT_NO_THROW(iArchive >> restored);
    }

    expectLegacyFieldsLoaded(legacy, restored);
}
