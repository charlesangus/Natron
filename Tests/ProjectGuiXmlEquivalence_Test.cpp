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

#include <cstddef>
#include <iterator>
#include <list>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "Serialization/BasicSerialization.h"
#include "Serialization/KnobSerialization.h"
#include "Serialization/ProjectGuiSerialization.h"
#include "Serialization/BoostXml/BasicTypesXml.h"
#include "Serialization/BoostXml/KnobXml.h"
#include "Serialization/BoostXml/ProjectGuiXml.h"

NATRON_NAMESPACE_USING

namespace {

template<class T>
std::string
saveXml(const char* name,
        const T& object)
{
    std::stringstream buffer;
    {
        boost::archive::xml_oarchive archive(buffer);
        archive << boost::serialization::make_nvp(name, object);
    }

    return buffer.str();
}

template<class T>
void
loadXml(const std::string& xml,
        const char* name,
        T* object)
{
    std::stringstream buffer(xml);
    boost::archive::xml_iarchive archive(buffer);

    archive >> boost::serialization::make_nvp(name, *object);
}

Serialization::KnobSerialization
makeStringKnob(const std::string& name,
               const std::string& value)
{
    Serialization::KnobSerialization knob;

    knob.name = name;
    knob.typeName = "String";
    knob.dimension = 1;
    knob.dimensions.resize(1);
    knob.dimensions[0].value = Serialization::KnobValueSerialization( value );
    knob.dimensions[0].defaultValue = Serialization::KnobValueSerialization( std::string() );
    knob.stringAnimation = std::map<int, std::string>();

    return knob;
}

Serialization::KnobSerialization
makeIntKnob(const std::string& name,
            int value)
{
    Serialization::KnobSerialization knob;

    knob.name = name;
    knob.typeName = "Int";
    knob.dimension = 1;
    knob.dimensions.resize(1);
    knob.dimensions[0].value = Serialization::KnobValueSerialization(value);
    knob.dimensions[0].defaultValue = Serialization::KnobValueSerialization(0);

    return knob;
}

// colorWasFound is set as loading any file saved by the current version sets it, so a loaded
// node can be compared with the one it was saved from.
Serialization::NodeGuiSerializationPtr
makeNodeGui(const std::string& nodeName,
            double positionX,
            double positionY,
            float colorR,
            float colorG,
            float colorB)
{
    Serialization::NodeGuiSerializationPtr node = std::make_shared<Serialization::NodeGuiSerialization>();

    node->nodeName = nodeName;
    node->positionX = positionX;
    node->positionY = positionY;
    node->width = 104.;
    node->height = 56.;
    node->previewEnabled = false;
    node->colorR = colorR;
    node->colorG = colorG;
    node->colorB = colorB;
    node->colorWasFound = true;

    return node;
}

std::shared_ptr<Serialization::PaneLayoutSerialization>
makePane(const std::string& name,
         const std::list<std::string>& tabs,
         int currentIndex,
         bool isAnchor)
{
    std::shared_ptr<Serialization::PaneLayoutSerialization> pane = std::make_shared<Serialization::PaneLayoutSerialization>();

    pane->name = name;
    pane->tabs = tabs;
    pane->currentIndex = currentIndex;
    pane->isAnchor = isAnchor;

    return pane;
}

std::shared_ptr<Serialization::SplitterSerialization>
makeSplitter(const std::string& sizes,
             int orientation,
             const Serialization::SplitterChildSerialization& firstChild,
             const Serialization::SplitterChildSerialization& secondChild)
{
    std::shared_ptr<Serialization::SplitterSerialization> splitter = std::make_shared<Serialization::SplitterSerialization>();

    splitter->sizes = sizes;
    splitter->orientation = orientation;
    splitter->children.push_back(firstChild);
    splitter->children.push_back(secondChild);

    return splitter;
}

Serialization::SplitterChildSerialization
paneChild(const std::shared_ptr<Serialization::PaneLayoutSerialization>& pane)
{
    Serialization::SplitterChildSerialization child;

    child.pane = pane;

    return child;
}

Serialization::SplitterChildSerialization
splitterChild(const std::shared_ptr<Serialization::SplitterSerialization>& splitter)
{
    Serialization::SplitterChildSerialization child;

    child.splitter = splitter;

    return child;
}

Serialization::ApplicationWindowSerialization
makeWindow(bool isMainWindow,
           int positionX,
           int positionY,
           int width,
           int height)
{
    Serialization::ApplicationWindowSerialization window;

    window.isMainWindow = isMainWindow;
    window.positionX = positionX;
    window.positionY = positionY;
    window.width = width;
    window.height = height;

    return window;
}

// Most fields differ from their defaults, and the two viewers differ from each other.
Serialization::ViewerDataSerialization
makeViewerData(int seed)
{
    Serialization::ViewerDataSerialization viewer;

    viewer.zoomLeft = -12.5 + seed;
    viewer.zoomBottom = 3.25 + seed;
    viewer.zoomFactor = 0.75 + seed;
    viewer.userRoIEnabled = true;
    viewer.userRoI.x1 = 1. + seed;
    viewer.userRoI.y1 = 2. + seed;
    viewer.userRoI.x2 = 300.5 + seed;
    viewer.userRoI.y2 = 400.25 + seed;
    viewer.isClippedToProject = false;
    viewer.isFullFrameProcessEnabled = true;
    viewer.autoContrastEnabled = true;
    viewer.gain = 2.5 + seed;
    viewer.gamma = 1.75 + seed;
    viewer.renderScaleActivated = true;
    viewer.mipmapLevel = 2 + seed;
    viewer.display = "sRGB - Display";
    viewer.view = "ACES 1.0 - SDR Video";
    viewer.look = seed ? std::string() : std::string("ACES 1.3 Reference Gamut Compression");
    viewer.channels = seed ? "Alpha" : "Luminance";
    viewer.layerName = "Color";
    viewer.alphaLayerName = "Color.A";
    viewer.zoomOrPanSinceLastFit = true;
    viewer.wipeCompositingOperator = 3;
    viewer.leftToolbarVisible = false;
    viewer.rightToolbarVisible = true;
    viewer.topToolbarVisible = false;
    viewer.playerVisible = false;
    viewer.infobarVisible = true;
    viewer.timelineVisible = false;
    viewer.checkerboardEnabled = true;
    viewer.isInputAPaused = true;
    viewer.isInputBPaused = false;
    viewer.fps = 29.97;
    viewer.fpsLocked = false;
    viewer.leftBound = 5 + seed;
    viewer.rightBound = 50 + seed;
    viewer.activeInputA = 1;
    viewer.activeInputB = 2 + seed;

    return viewer;
}

// A GUI section with every kind of content the format can hold: top-level nodes and a group with
// a nested group, overlay colours on some nodes, a main window split three ways, floating windows
// holding a pane, a splitter and settings panels, two viewers, histograms, opened panels in every
// state and two Python panels.
Serialization::ProjectGuiSerialization
makePopulatedProjectGui()
{
    Serialization::ProjectGuiSerialization projectGui;

    {
        Serialization::NodeGuiSerializationPtr reader = makeNodeGui("Read1", -120.5, 40.25, 0.7f, 0.7f, 0.7f);
        reader->previewEnabled = true;
        projectGui.nodes.push_back(*reader);

        Serialization::NodeGuiSerializationPtr group = makeNodeGui("Group1", 210., -35.5, 0.25f, 0.5f, 0.125f);
        group->selected = true;
        group->hasOverlayColor = true;
        group->overlayColorR = 0.25;
        group->overlayColorG = 0.5;
        group->overlayColorB = 0.75;

        Serialization::NodeGuiSerializationPtr blur = makeNodeGui("Group1.Blur1", 10., 20., 0.8f, 0.5f, 0.3f);
        blur->hasOverlayColor = true;
        blur->overlayColorR = 1.;
        blur->overlayColorG = 0.;
        blur->overlayColorB = 0.5;
        blur->hasUserColor = true;
        blur->userColorR = 0.25f;
        blur->userColorG = 0.5f;
        blur->userColorB = 0.75f;
        group->children.push_back(blur);

        Serialization::NodeGuiSerializationPtr nestedGroup = makeNodeGui("Group1.Group2", 60., 80., 0.4f, 0.4f, 0.4f);
        nestedGroup->width = 640.;
        nestedGroup->height = 480.;
        nestedGroup->children.push_back( makeNodeGui("Group1.Group2.Dot1", -5., -6., 0.1f, 0.2f, 0.3f) );
        group->children.push_back(nestedGroup);
        projectGui.nodes.push_back(*group);

        Serialization::NodeGuiSerializationPtr viewer = makeNodeGui("Viewer1", 0., 300., 0.6f, 0.6f, 0.6f);
        projectGui.nodes.push_back(*viewer);
    }

    {
        Serialization::ApplicationWindowSerialization mainWindow = makeWindow(true, 0, 25, 1920, 1055);
        std::list<std::string> viewerTabs;
        viewerTabs.push_back("Viewer1");
        viewerTabs.push_back("histogram1");
        std::list<std::string> workshopTabs;
        workshopTabs.push_back("nodeGraph");
        workshopTabs.push_back("curveEditor");
        workshopTabs.push_back("dopeSheetEditor");
        std::list<std::string> propertiesTabs;
        propertiesTabs.push_back("properties");
        std::shared_ptr<Serialization::SplitterSerialization> workshopSplitter =
            makeSplitter( "600 400", 1, paneChild( makePane("pane2", workshopTabs, 1, false) ), paneChild( makePane("pane3", propertiesTabs, 0, false) ) );
        mainWindow.childSplitter = makeSplitter( "1200 720", 0, paneChild( makePane("pane1", viewerTabs, 0, true) ), splitterChild(workshopSplitter) );
        projectGui.layout.windows.push_back(mainWindow);

        Serialization::ApplicationWindowSerialization floatingPaneWindow = makeWindow(false, 200, 300, 640, 480);
        std::list<std::string> scriptTabs;
        scriptTabs.push_back("scriptEditor");
        floatingPaneWindow.childPane = makePane("pane4", scriptTabs, 0, false);
        projectGui.layout.windows.push_back(floatingPaneWindow);

        Serialization::ApplicationWindowSerialization floatingSplitterWindow = makeWindow(false, -40, 60, 800, 600);
        std::list<std::string> secondViewerTabs;
        secondViewerTabs.push_back("Viewer2");
        std::list<std::string> histogramTabs;
        histogramTabs.push_back("histogram2");
        floatingSplitterWindow.childSplitter =
            makeSplitter( "400 400", 0, paneChild( makePane("pane5", secondViewerTabs, 0, false) ), paneChild( makePane("pane6", histogramTabs, 0, false) ) );
        projectGui.layout.windows.push_back(floatingSplitterWindow);

        Serialization::ApplicationWindowSerialization floatingNodePanelWindow = makeWindow(false, 900, 100, 400, 700);
        floatingNodePanelWindow.childDockablePanel = "Blur1";
        projectGui.layout.windows.push_back(floatingNodePanelWindow);

        Serialization::ApplicationWindowSerialization floatingProjectPanelWindow = makeWindow(false, 950, 150, 420, 720);
        floatingProjectPanelWindow.childDockablePanel = NATRON_SERIALIZATION_PROJECT_SETTINGS_PANEL_NAME;
        projectGui.layout.windows.push_back(floatingProjectPanelWindow);
    }

    projectGui.viewers["Viewer1"] = makeViewerData(0);
    projectGui.viewers["Viewer2"] = makeViewerData(1);

    projectGui.histograms.push_back("histogram1");
    projectGui.histograms.push_back("histogram2");

    projectGui.openedPanels.push_back("Group1.Blur1");
    projectGui.openedPanels.push_back(NATRON_SERIALIZATION_PROJECT_SETTINGS_PANEL_NAME);
    projectGui.openedPanels.push_back("Read1");
    projectGui.openedPanelsMinimized.push_back(NATRON_SERIALIZATION_PROJECT_SETTINGS_PANEL_NAME);
    projectGui.openedPanelsMinimized.push_back("Read1");
    projectGui.openedPanelsHideUnmodified.push_back("Group1.Blur1");

    projectGui.scriptEditorInput = "print('hello')\nprint(\"<a & b>\")\n";

    {
        Serialization::PythonPanelSerialization panel;
        panel.name = "My Panel";
        panel.pythonFunction = "createMyPanel";
        panel.knobs.push_back( makeStringKnob("note", "some <text>") );
        panel.knobs.push_back( makeIntKnob("count", 3) );
        panel.userData = "user data & more";
        projectGui.pythonPanels.push_back(panel);

        Serialization::PythonPanelSerialization emptyPanel;
        emptyPanel.name = "Empty Panel";
        emptyPanel.pythonFunction = "createEmptyPanel";
        projectGui.pythonPanels.push_back(emptyPanel);
    }

    return projectGui;
}

void
expectSameKnob(const Serialization::KnobSerialization& expected,
               const Serialization::KnobSerialization& actual)
{
    EXPECT_EQ(expected.name, actual.name);
    EXPECT_EQ(expected.typeName, actual.typeName);
    EXPECT_EQ(expected.dimension, actual.dimension);
    ASSERT_EQ( expected.dimensions.size(), actual.dimensions.size() );
    for (std::size_t i = 0; i < actual.dimensions.size(); ++i) {
        EXPECT_TRUE(expected.dimensions[i].value == actual.dimensions[i].value);
    }
}

void
expectSameNodeGui(const Serialization::NodeGuiSerialization& expected,
                  const Serialization::NodeGuiSerialization& actual)
{
    EXPECT_EQ(expected.version, actual.version);
    EXPECT_EQ(expected.nodeName, actual.nodeName);
    EXPECT_EQ(expected.positionX, actual.positionX);
    EXPECT_EQ(expected.positionY, actual.positionY);
    EXPECT_EQ(expected.width, actual.width);
    EXPECT_EQ(expected.height, actual.height);
    EXPECT_EQ(expected.previewEnabled, actual.previewEnabled);
    EXPECT_EQ(expected.colorR, actual.colorR);
    EXPECT_EQ(expected.colorG, actual.colorG);
    EXPECT_EQ(expected.colorB, actual.colorB);
    EXPECT_EQ(expected.colorWasFound, actual.colorWasFound);
    EXPECT_EQ(expected.selected, actual.selected);
    EXPECT_EQ(expected.hasOverlayColor, actual.hasOverlayColor);
    if (expected.hasOverlayColor && actual.hasOverlayColor) {
        EXPECT_EQ(expected.overlayColorR, actual.overlayColorR);
        EXPECT_EQ(expected.overlayColorG, actual.overlayColorG);
        EXPECT_EQ(expected.overlayColorB, actual.overlayColorB);
    }
    EXPECT_EQ(expected.hasUserColor, actual.hasUserColor);
    if (expected.hasUserColor && actual.hasUserColor) {
        EXPECT_EQ(expected.userColorR, actual.userColorR);
        EXPECT_EQ(expected.userColorG, actual.userColorG);
        EXPECT_EQ(expected.userColorB, actual.userColorB);
    }

    ASSERT_EQ( expected.children.size(), actual.children.size() );
    std::list<Serialization::NodeGuiSerializationPtr>::const_iterator expectedChild = expected.children.begin();
    for (std::list<Serialization::NodeGuiSerializationPtr>::const_iterator child = actual.children.begin(); child != actual.children.end(); ++child, ++expectedChild) {
        expectSameNodeGui(**expectedChild, **child);
    }
}

void
expectSamePane(const Serialization::PaneLayoutSerialization& expected,
               const Serialization::PaneLayoutSerialization& actual)
{
    EXPECT_EQ(expected.tabs, actual.tabs);
    EXPECT_EQ(expected.currentIndex, actual.currentIndex);
    EXPECT_EQ(expected.isAnchor, actual.isAnchor);
    EXPECT_EQ(expected.name, actual.name);
}

void
expectSameSplitter(const Serialization::SplitterSerialization& expected,
                   const Serialization::SplitterSerialization& actual)
{
    EXPECT_EQ(expected.sizes, actual.sizes);
    EXPECT_EQ(expected.orientation, actual.orientation);
    ASSERT_EQ( expected.children.size(), actual.children.size() );
    for (std::size_t i = 0; i < actual.children.size(); ++i) {
        const Serialization::SplitterChildSerialization& expectedChild = expected.children[i];
        const Serialization::SplitterChildSerialization& child = actual.children[i];
        ASSERT_EQ( (bool)expectedChild.splitter, (bool)child.splitter );
        if (child.splitter) {
            expectSameSplitter(*expectedChild.splitter, *child.splitter);
        } else {
            ASSERT_TRUE( (bool)expectedChild.pane );
            ASSERT_TRUE( (bool)child.pane );
            expectSamePane(*expectedChild.pane, *child.pane);
        }
    }
}

void
expectSameWindow(const Serialization::ApplicationWindowSerialization& expected,
                 const Serialization::ApplicationWindowSerialization& actual)
{
    ASSERT_EQ( (bool)expected.childSplitter, (bool)actual.childSplitter );
    ASSERT_EQ( (bool)expected.childPane, (bool)actual.childPane );
    if (actual.childSplitter) {
        expectSameSplitter(*expected.childSplitter, *actual.childSplitter);
    }
    if (actual.childPane) {
        expectSamePane(*expected.childPane, *actual.childPane);
    }
    EXPECT_EQ(expected.childDockablePanel, actual.childDockablePanel);
    EXPECT_EQ(expected.isMainWindow, actual.isMainWindow);
    EXPECT_EQ(expected.positionX, actual.positionX);
    EXPECT_EQ(expected.positionY, actual.positionY);
    EXPECT_EQ(expected.width, actual.width);
    EXPECT_EQ(expected.height, actual.height);
}

void
expectSameLayout(const Serialization::GuiLayoutSerialization& expected,
                 const Serialization::GuiLayoutSerialization& actual)
{
    ASSERT_EQ( expected.windows.size(), actual.windows.size() );
    std::list<Serialization::ApplicationWindowSerialization>::const_iterator expectedWindow = expected.windows.begin();
    for (std::list<Serialization::ApplicationWindowSerialization>::const_iterator window = actual.windows.begin(); window != actual.windows.end(); ++window, ++expectedWindow) {
        expectSameWindow(*expectedWindow, *window);
    }
    ASSERT_EQ( expected.legacyPanes.size(), actual.legacyPanes.size() );
    for (std::map<std::string, Serialization::PaneLayoutSerialization>::const_iterator pane = actual.legacyPanes.begin(); pane != actual.legacyPanes.end(); ++pane) {
        std::map<std::string, Serialization::PaneLayoutSerialization>::const_iterator expectedPane = expected.legacyPanes.find(pane->first);
        ASSERT_TRUE( expectedPane != expected.legacyPanes.end() );
        expectSamePane(expectedPane->second, pane->second);
    }
    EXPECT_EQ(expected.legacySplitterStates, actual.legacySplitterStates);
}

void
expectSameViewerData(const Serialization::ViewerDataSerialization& expected,
                     const Serialization::ViewerDataSerialization& actual)
{
    EXPECT_EQ(expected.version, actual.version);
    EXPECT_EQ(expected.zoomLeft, actual.zoomLeft);
    EXPECT_EQ(expected.zoomBottom, actual.zoomBottom);
    EXPECT_EQ(expected.zoomFactor, actual.zoomFactor);
    EXPECT_EQ(expected.userRoIEnabled, actual.userRoIEnabled);
    EXPECT_EQ(expected.userRoI.x1, actual.userRoI.x1);
    EXPECT_EQ(expected.userRoI.y1, actual.userRoI.y1);
    EXPECT_EQ(expected.userRoI.x2, actual.userRoI.x2);
    EXPECT_EQ(expected.userRoI.y2, actual.userRoI.y2);
    EXPECT_EQ(expected.isClippedToProject, actual.isClippedToProject);
    EXPECT_EQ(expected.isFullFrameProcessEnabled, actual.isFullFrameProcessEnabled);
    EXPECT_EQ(expected.autoContrastEnabled, actual.autoContrastEnabled);
    EXPECT_EQ(expected.gain, actual.gain);
    EXPECT_EQ(expected.gamma, actual.gamma);
    EXPECT_EQ(expected.renderScaleActivated, actual.renderScaleActivated);
    EXPECT_EQ(expected.mipmapLevel, actual.mipmapLevel);
    EXPECT_EQ(expected.display, actual.display);
    EXPECT_EQ(expected.view, actual.view);
    EXPECT_EQ(expected.look, actual.look);
    EXPECT_EQ(expected.channels, actual.channels);
    EXPECT_EQ(expected.layerName, actual.layerName);
    EXPECT_EQ(expected.alphaLayerName, actual.alphaLayerName);
    EXPECT_EQ(expected.zoomOrPanSinceLastFit, actual.zoomOrPanSinceLastFit);
    EXPECT_EQ(expected.wipeCompositingOperator, actual.wipeCompositingOperator);
    EXPECT_EQ(expected.leftToolbarVisible, actual.leftToolbarVisible);
    EXPECT_EQ(expected.rightToolbarVisible, actual.rightToolbarVisible);
    EXPECT_EQ(expected.topToolbarVisible, actual.topToolbarVisible);
    EXPECT_EQ(expected.playerVisible, actual.playerVisible);
    EXPECT_EQ(expected.infobarVisible, actual.infobarVisible);
    EXPECT_EQ(expected.timelineVisible, actual.timelineVisible);
    EXPECT_EQ(expected.checkerboardEnabled, actual.checkerboardEnabled);
    EXPECT_EQ(expected.isInputAPaused, actual.isInputAPaused);
    EXPECT_EQ(expected.isInputBPaused, actual.isInputBPaused);
    EXPECT_EQ(expected.fps, actual.fps);
    EXPECT_EQ(expected.fpsLocked, actual.fpsLocked);
    EXPECT_EQ(expected.leftBound, actual.leftBound);
    EXPECT_EQ(expected.rightBound, actual.rightBound);
    EXPECT_EQ(expected.activeInputA, actual.activeInputA);
    EXPECT_EQ(expected.activeInputB, actual.activeInputB);
}

void
expectSameViewers(const std::map<std::string, Serialization::ViewerDataSerialization>& expected,
                  const std::map<std::string, Serialization::ViewerDataSerialization>& actual)
{
    ASSERT_EQ( expected.size(), actual.size() );
    for (std::map<std::string, Serialization::ViewerDataSerialization>::const_iterator viewer = actual.begin(); viewer != actual.end(); ++viewer) {
        std::map<std::string, Serialization::ViewerDataSerialization>::const_iterator expectedViewer = expected.find(viewer->first);
        ASSERT_TRUE( expectedViewer != expected.end() );
        expectSameViewerData(expectedViewer->second, viewer->second);
    }
}

void
expectSameBackdrop(const Serialization::NodeBackdropSerialization& expected,
                   const Serialization::NodeBackdropSerialization& actual)
{
    EXPECT_EQ(expected.positionX, actual.positionX);
    EXPECT_EQ(expected.positionY, actual.positionY);
    EXPECT_EQ(expected.width, actual.width);
    EXPECT_EQ(expected.height, actual.height);
    EXPECT_EQ(expected.name, actual.name);
    EXPECT_EQ(expected.masterBackdropName, actual.masterBackdropName);
    expectSameKnob(expected.label, actual.label);
    EXPECT_EQ(expected.colorR, actual.colorR);
    EXPECT_EQ(expected.colorG, actual.colorG);
    EXPECT_EQ(expected.colorB, actual.colorB);
    EXPECT_EQ(expected.selected, actual.selected);
}

void
expectSamePythonPanel(const Serialization::PythonPanelSerialization& expected,
                      const Serialization::PythonPanelSerialization& actual)
{
    EXPECT_EQ(expected.name, actual.name);
    EXPECT_EQ(expected.pythonFunction, actual.pythonFunction);
    EXPECT_EQ(expected.userData, actual.userData);
    ASSERT_EQ( expected.knobs.size(), actual.knobs.size() );
    std::list<Serialization::KnobSerialization>::const_iterator expectedKnob = expected.knobs.begin();
    for (std::list<Serialization::KnobSerialization>::const_iterator knob = actual.knobs.begin(); knob != actual.knobs.end(); ++knob, ++expectedKnob) {
        expectSameKnob(*expectedKnob, *knob);
    }
}

void
expectSameProjectGui(const Serialization::ProjectGuiSerialization& expected,
                     const Serialization::ProjectGuiSerialization& actual)
{
    EXPECT_EQ(expected.version, actual.version);

    ASSERT_EQ( expected.nodes.size(), actual.nodes.size() );
    std::list<Serialization::NodeGuiSerialization>::const_iterator expectedNode = expected.nodes.begin();
    for (std::list<Serialization::NodeGuiSerialization>::const_iterator node = actual.nodes.begin(); node != actual.nodes.end(); ++node, ++expectedNode) {
        expectSameNodeGui(*expectedNode, *node);
    }

    expectSameLayout(expected.layout, actual.layout);
    expectSameViewers(expected.viewers, actual.viewers);

    EXPECT_EQ(expected.histograms, actual.histograms);

    ASSERT_EQ( expected.legacyBackdrops.size(), actual.legacyBackdrops.size() );
    std::list<Serialization::NodeBackdropSerialization>::const_iterator expectedBackdrop = expected.legacyBackdrops.begin();
    for (std::list<Serialization::NodeBackdropSerialization>::const_iterator backdrop = actual.legacyBackdrops.begin(); backdrop != actual.legacyBackdrops.end(); ++backdrop, ++expectedBackdrop) {
        expectSameBackdrop(*expectedBackdrop, *backdrop);
    }

    EXPECT_EQ(expected.openedPanels, actual.openedPanels);
    EXPECT_EQ(expected.openedPanelsMinimized, actual.openedPanelsMinimized);
    EXPECT_EQ(expected.openedPanelsHideUnmodified, actual.openedPanelsHideUnmodified);
    EXPECT_EQ(expected.scriptEditorInput, actual.scriptEditorInput);

    ASSERT_EQ( expected.pythonPanels.size(), actual.pythonPanels.size() );
    std::list<Serialization::PythonPanelSerialization>::const_iterator expectedPythonPanel = expected.pythonPanels.begin();
    for (std::list<Serialization::PythonPanelSerialization>::const_iterator pythonPanel = actual.pythonPanels.begin(); pythonPanel != actual.pythonPanels.end(); ++pythonPanel, ++expectedPythonPanel) {
        expectSamePythonPanel(*expectedPythonPanel, *pythonPanel);
    }
}

// Old-format writers: each archives the NVPs a class wrote at the version given to it below, so
// the load branches for files saved by older versions can be exercised.

struct NodeGuiVersion3Writer
{
    std::string name;
    double positionX;
    double positionY;
    bool previewEnabled;
    float colorR;
    float colorG;
    float colorB;
    bool selected;

    template<class Archive>
    void serialize(Archive & ar,
                   const unsigned int /*version*/)
    {
        ar & ::boost::serialization::make_nvp("Name", name);
        ar & ::boost::serialization::make_nvp("X_position", positionX);
        ar & ::boost::serialization::make_nvp("Y_position", positionY);
        ar & ::boost::serialization::make_nvp("Preview_enabled", previewEnabled);
        ar & ::boost::serialization::make_nvp("r", colorR);
        ar & ::boost::serialization::make_nvp("g", colorG);
        ar & ::boost::serialization::make_nvp("b", colorB);
        ar & ::boost::serialization::make_nvp("Selected", selected);
    }
};

struct BackdropVersion1Writer
{
    double positionX;
    double positionY;
    int width;
    int height;
    std::string name;
    std::string masterName;
    Serialization::KnobSerialization label;
    float colorR;
    float colorG;
    float colorB;

    template<class Archive>
    void serialize(Archive & ar,
                   const unsigned int /*version*/)
    {
        ar & ::boost::serialization::make_nvp("X_position", positionX);
        ar & ::boost::serialization::make_nvp("Y_position", positionY);
        ar & ::boost::serialization::make_nvp("Width", width);
        ar & ::boost::serialization::make_nvp("Height", height);
        ar & ::boost::serialization::make_nvp("Name", name);
        ar & ::boost::serialization::make_nvp("MasterName", masterName);
        ar & ::boost::serialization::make_nvp("Label", label);
        ar & ::boost::serialization::make_nvp("r", colorR);
        ar & ::boost::serialization::make_nvp("g", colorG);
        ar & ::boost::serialization::make_nvp("b", colorB);
    }
};

struct PaneLayoutVersion3Writer
{
    bool floating;
    std::list<bool> splits;
    std::string parentName;
    std::list<std::string> splitsNames;
    std::list<std::string> tabs;
    int currentIndex;
    int positionX;
    int positionY;
    int width;
    int height;

    template<class Archive>
    void serialize(Archive & ar,
                   const unsigned int /*version*/)
    {
        ar & ::boost::serialization::make_nvp("Floating", floating);
        ar & ::boost::serialization::make_nvp("Splits", splits);
        ar & ::boost::serialization::make_nvp("ParentName", parentName);
        ar & ::boost::serialization::make_nvp("SplitsNames", splitsNames);
        ar & ::boost::serialization::make_nvp("Tabs", tabs);
        ar & ::boost::serialization::make_nvp("Index", currentIndex);
        if (floating) {
            ar & ::boost::serialization::make_nvp("x", positionX);
            ar & ::boost::serialization::make_nvp("y", positionY);
            ar & ::boost::serialization::make_nvp("w", width);
            ar & ::boost::serialization::make_nvp("h", height);
        }
    }
};

struct ViewerDataVersion3Writer
{
    double zoomLeft;
    double zoomBottom;
    double zoomFactor;
    double zoomPixelAspectRatio;
    bool userRoIEnabled;
    Serialization::RectDSerialization userRoI;
    bool isClippedToProject;
    bool autoContrastEnabled;
    double gain;
    std::string colorSpace;
    std::string channels;
    bool renderScaleActivated;
    unsigned int mipmapLevel;
    bool zoomOrPanSinceLastFit;
    int wipeCompositingOperator;
    bool frameRangeLocked;

    template<class Archive>
    void serialize(Archive & ar,
                   const unsigned int /*version*/)
    {
        ar & ::boost::serialization::make_nvp("zoomLeft", zoomLeft);
        ar & ::boost::serialization::make_nvp("zoomBottom", zoomBottom);
        ar & ::boost::serialization::make_nvp("zoomFactor", zoomFactor);
        ar & ::boost::serialization::make_nvp("zoomPAR", zoomPixelAspectRatio);
        ar & ::boost::serialization::make_nvp("UserRoIEnabled", userRoIEnabled);
        ar & ::boost::serialization::make_nvp("UserRoI", userRoI);
        ar & ::boost::serialization::make_nvp("ClippedToProject", isClippedToProject);
        ar & ::boost::serialization::make_nvp("AutoContrast", autoContrastEnabled);
        ar & ::boost::serialization::make_nvp("Gain", gain);
        ar & ::boost::serialization::make_nvp("ColorSpace", colorSpace);
        ar & ::boost::serialization::make_nvp("Channels", channels);
        ar & ::boost::serialization::make_nvp("RenderScaleActivated", renderScaleActivated);
        ar & ::boost::serialization::make_nvp("MipMapLevel", mipmapLevel);
        ar & ::boost::serialization::make_nvp("ZoomOrPanSinceFit", zoomOrPanSinceLastFit);
        ar & ::boost::serialization::make_nvp("CompositingOP", wipeCompositingOperator);
        ar & ::boost::serialization::make_nvp("FrameRangeLocked", frameRangeLocked);
    }
};

struct ProjectGuiVersion2Writer
{
    std::list<NodeGuiVersion3Writer> nodes;
    std::map<std::string, PaneLayoutVersion3Writer> panes;
    std::map<std::string, std::string> splitterStates;
    std::map<std::string, ViewerDataVersion3Writer> viewers;
    bool previewsTurnedOffGlobally;
    std::list<std::string> histograms;
    std::list<BackdropVersion1Writer> backdrops;

    template<class Archive>
    void serialize(Archive & ar,
                   const unsigned int /*version*/)
    {
        ar & ::boost::serialization::make_nvp("NodesGui", nodes);
        ar & ::boost::serialization::make_nvp("Gui_Layout", panes);
        ar & ::boost::serialization::make_nvp("Splitters_states", splitterStates);
        ar & ::boost::serialization::make_nvp("ViewersData", viewers);
        ar & ::boost::serialization::make_nvp("PreviewsTurnedOffGlobaly", previewsTurnedOffGlobally);
        ar & ::boost::serialization::make_nvp("Histograms", histograms);
        ar & ::boost::serialization::make_nvp("Backdrops", backdrops);
    }
};

struct GuiLayoutVersion1Writer
{
    std::map<std::string, PaneLayoutVersion3Writer> panes;
    std::map<std::string, std::string> splitterStates;

    template<class Archive>
    void serialize(Archive & ar,
                   const unsigned int /*version*/)
    {
        ar & ::boost::serialization::make_nvp("Gui_Layout", panes);
        ar & ::boost::serialization::make_nvp("Splitters_states", splitterStates);
    }
};

struct ProjectGuiVersion9Writer
{
    std::list<Serialization::NodeGuiSerialization> nodes;
    GuiLayoutVersion1Writer layout;
    std::map<std::string, Serialization::ViewerDataSerialization> viewers;
    std::list<std::string> histograms;
    std::list<Serialization::NodeBackdropSerialization> backdrops;
    std::list<std::string> openedPanels;
    std::string scriptEditorInput;

    template<class Archive>
    void serialize(Archive & ar,
                   const unsigned int /*version*/)
    {
        ar & ::boost::serialization::make_nvp("NodesGui", nodes);
        ar & ::boost::serialization::make_nvp("Gui_Layout", layout);
        ar & ::boost::serialization::make_nvp("ViewersData", viewers);
        ar & ::boost::serialization::make_nvp("Histograms", histograms);
        ar & ::boost::serialization::make_nvp("Backdrops", backdrops);
        ar & ::boost::serialization::make_nvp("OpenedPanels", openedPanels);
        ar & ::boost::serialization::make_nvp("ScriptEditorInput", scriptEditorInput);
    }
};

NodeGuiVersion3Writer
makeNodeGuiVersion3(const std::string& name,
                    double positionX,
                    bool selected)
{
    NodeGuiVersion3Writer node;

    node.name = name;
    node.positionX = positionX;
    node.positionY = -positionX;
    node.previewEnabled = true;
    node.colorR = 0.5f;
    node.colorG = 0.25f;
    node.colorB = 0.125f;
    node.selected = selected;

    return node;
}

BackdropVersion1Writer
makeBackdropVersion1(const std::string& name,
                     const std::string& masterName)
{
    BackdropVersion1Writer backdrop;

    backdrop.positionX = 15.5;
    backdrop.positionY = -25.25;
    backdrop.width = 320;
    backdrop.height = 240;
    backdrop.name = name;
    backdrop.masterName = masterName;
    backdrop.label = makeStringKnob("Label", "Backdrop note");
    backdrop.colorR = 0.3f;
    backdrop.colorG = 0.6f;
    backdrop.colorB = 0.9f;

    return backdrop;
}

PaneLayoutVersion3Writer
makePaneLayoutVersion3(bool floating)
{
    PaneLayoutVersion3Writer pane;

    pane.floating = floating;
    pane.splits.push_back(true);
    pane.splits.push_back(false);
    pane.parentName = floating ? std::string() : std::string("Pane1");
    pane.splitsNames.push_back("Pane2");
    pane.tabs.push_back("CurveEditor");
    pane.tabs.push_back("NodeGraph");
    pane.tabs.push_back("Properties");
    pane.tabs.push_back("Viewer1");
    pane.currentIndex = 2;
    pane.positionX = 100;
    pane.positionY = 120;
    pane.width = 500;
    pane.height = 400;

    return pane;
}

ViewerDataVersion3Writer
makeViewerDataVersion3()
{
    ViewerDataVersion3Writer viewer;

    viewer.zoomLeft = -7.5;
    viewer.zoomBottom = 2.25;
    viewer.zoomFactor = 0.5;
    viewer.zoomPixelAspectRatio = 2.;
    viewer.userRoIEnabled = true;
    viewer.userRoI.x1 = 1.;
    viewer.userRoI.y1 = 2.;
    viewer.userRoI.x2 = 30.;
    viewer.userRoI.y2 = 40.;
    viewer.isClippedToProject = false;
    viewer.autoContrastEnabled = true;
    viewer.gain = 3.;
    viewer.colorSpace = "Linear(None)";
    viewer.channels = "RGB";
    viewer.renderScaleActivated = true;
    viewer.mipmapLevel = 1;
    viewer.zoomOrPanSinceLastFit = true;
    viewer.wipeCompositingOperator = 2;
    viewer.frameRangeLocked = true;

    return viewer;
}

// The tabs of makePaneLayoutVersion3() once loading has given them their script names.
std::list<std::string>
legacyPaneTabsAsScriptNames()
{
    std::list<std::string> tabs;

    tabs.push_back(NATRON_SERIALIZATION_CURVE_EDITOR_TAB_NAME);
    tabs.push_back(NATRON_SERIALIZATION_NODE_GRAPH_TAB_NAME);
    tabs.push_back("properties");
    tabs.push_back("Viewer1");

    return tabs;
}

// Loads xml through the adapter, checks that saving what was read, loading that back and saving
// it again writes the same XML, and returns what was read.
Serialization::ProjectGuiSerialization
loadAndCheckResave(const std::string& xml)
{
    Serialization::ProjectGuiSerialization loaded;

    EXPECT_NO_THROW( loadXml(xml, "ProjectGui", &loaded) );

    const std::string savedXml = saveXml("ProjectGui", loaded);
    Serialization::ProjectGuiSerialization reloaded;
    EXPECT_NO_THROW( loadXml(savedXml, "ProjectGui", &reloaded) );
    EXPECT_EQ( savedXml, saveXml("ProjectGui", reloaded) );

    return loaded;
}

} // anonymous namespace

BOOST_CLASS_VERSION(NodeGuiVersion3Writer, NATRON_SERIALIZATION_NODE_GUI_INTRODUCES_SELECTED)
BOOST_CLASS_VERSION(BackdropVersion1Writer, 1)
BOOST_CLASS_VERSION(PaneLayoutVersion3Writer, NATRON_SERIALIZATION_PANE_INTRODUCES_SIZE)
BOOST_CLASS_VERSION(ViewerDataVersion3Writer, NATRON_SERIALIZATION_VIEWER_DATA_INTRODUCES_FRAME_RANGE)
BOOST_CLASS_VERSION(ProjectGuiVersion2Writer, NATRON_SERIALIZATION_PROJECT_GUI_INTRODUCES_BACKDROPS)
BOOST_CLASS_VERSION(GuiLayoutVersion1Writer, 1)
BOOST_CLASS_VERSION(ProjectGuiVersion9Writer, NATRON_SERIALIZATION_PROJECT_GUI_SCRIPT_EDITOR)

TEST(ProjectGuiXmlEquivalence, APopulatedGuiSectionRoundTripsThroughTheAdapter)
{
    const Serialization::ProjectGuiSerialization source = makePopulatedProjectGui();
    const std::string xml = saveXml("ProjectGui", source);

    Serialization::ProjectGuiSerialization reloaded;
    ASSERT_NO_THROW( loadXml(xml, "ProjectGui", &reloaded) );
    expectSameProjectGui(source, reloaded);
    EXPECT_EQ( xml, saveXml("ProjectGui", reloaded) );

    EXPECT_EQ( (unsigned int)NATRON_SERIALIZATION_PROJECT_GUI_VERSION, reloaded.version );
    ASSERT_EQ( (std::size_t)3, reloaded.nodes.size() );
    const Serialization::NodeGuiSerialization& group = *std::next( reloaded.nodes.begin() );
    EXPECT_EQ( std::string("Group1"), group.nodeName );
    EXPECT_TRUE(group.selected);
    EXPECT_TRUE(group.hasOverlayColor);
    EXPECT_EQ(0.75, group.overlayColorB);
    ASSERT_EQ( (std::size_t)2, group.children.size() );
    const Serialization::NodeGuiSerialization& nestedGroup = *group.children.back();
    EXPECT_EQ(640., nestedGroup.width);
    ASSERT_EQ( (std::size_t)1, nestedGroup.children.size() );
    EXPECT_EQ( std::string("Group1.Group2.Dot1"), nestedGroup.children.front()->nodeName );

    ASSERT_EQ( (std::size_t)5, reloaded.layout.windows.size() );
    const Serialization::ApplicationWindowSerialization& mainWindow = reloaded.layout.windows.front();
    EXPECT_TRUE(mainWindow.isMainWindow);
    ASSERT_TRUE( (bool)mainWindow.childSplitter );
    ASSERT_EQ( (std::size_t)2, mainWindow.childSplitter->children.size() );
    ASSERT_TRUE( (bool)mainWindow.childSplitter->children[0].pane );
    EXPECT_TRUE(mainWindow.childSplitter->children[0].pane->isAnchor);
    ASSERT_TRUE( (bool)mainWindow.childSplitter->children[1].splitter );
    EXPECT_EQ( std::string("600 400"), mainWindow.childSplitter->children[1].splitter->sizes );
    EXPECT_EQ( std::string(NATRON_SERIALIZATION_PROJECT_SETTINGS_PANEL_NAME), reloaded.layout.windows.back().childDockablePanel );
    EXPECT_FALSE( (bool)reloaded.layout.windows.back().childSplitter );
    EXPECT_FALSE( (bool)reloaded.layout.windows.back().childPane );

    ASSERT_EQ( (std::size_t)2, reloaded.viewers.size() );
    EXPECT_EQ( std::string("Alpha"), reloaded.viewers.find("Viewer2")->second.channels );
    EXPECT_EQ( std::string("ACES 1.3 Reference Gamut Compression"), reloaded.viewers.find("Viewer1")->second.look );
    EXPECT_EQ( std::string("print('hello')\nprint(\"<a & b>\")\n"), reloaded.scriptEditorInput );
    ASSERT_EQ( (std::size_t)2, reloaded.pythonPanels.size() );
    EXPECT_EQ( (std::size_t)2, reloaded.pythonPanels.front().knobs.size() );
    EXPECT_TRUE( reloaded.pythonPanels.back().knobs.empty() );

    // Splitters and panes are tracked, so the file numbers every one of them.
    EXPECT_NE( std::string::npos, xml.find("object_id=") );
}

TEST(ProjectGuiXmlEquivalence, TheLayoutViewersAndPythonPanelsRoundTripOnTheirOwn)
{
    const Serialization::ProjectGuiSerialization source = makePopulatedProjectGui();

    // A layout file holds the layout alone, under this name.
    const std::string layoutXml = saveXml("Layout", source.layout);
    Serialization::GuiLayoutSerialization layout;
    ASSERT_NO_THROW( loadXml(layoutXml, "Layout", &layout) );
    expectSameLayout(source.layout, layout);
    EXPECT_EQ( layoutXml, saveXml("Layout", layout) );

    const std::string viewersXml = saveXml("ViewersData", source.viewers);
    std::map<std::string, Serialization::ViewerDataSerialization> viewers;
    ASSERT_NO_THROW( loadXml(viewersXml, "ViewersData", &viewers) );
    expectSameViewers(source.viewers, viewers);
    EXPECT_EQ( viewersXml, saveXml("ViewersData", viewers) );

    const Serialization::PythonPanelSerialization& sourcePythonPanel = source.pythonPanels.front();
    const std::string pythonPanelXml = saveXml("PythonPanel", sourcePythonPanel);
    Serialization::PythonPanelSerialization pythonPanel;
    ASSERT_NO_THROW( loadXml(pythonPanelXml, "PythonPanel", &pythonPanel) );
    expectSamePythonPanel(sourcePythonPanel, pythonPanel);
    EXPECT_EQ( pythonPanelXml, saveXml("PythonPanel", pythonPanel) );
}

TEST(ProjectGuiXmlEquivalence, AVersion1LayoutFileLoadsItsPanesAsLegacyPanes)
{
    GuiLayoutVersion1Writer written;

    written.panes["Pane1"] = makePaneLayoutVersion3(false);
    written.panes["Pane2"] = makePaneLayoutVersion3(true);
    written.splitterStates["Splitter1"] = "100 200";

    Serialization::GuiLayoutSerialization layout;
    ASSERT_NO_THROW( loadXml(saveXml("Layout", written), "Layout", &layout) );

    EXPECT_TRUE(layout.windows.empty());
    ASSERT_EQ( (std::size_t)2, layout.legacyPanes.size() );
    for (std::map<std::string, Serialization::PaneLayoutSerialization>::const_iterator it = layout.legacyPanes.begin(); it != layout.legacyPanes.end(); ++it) {
        EXPECT_EQ( legacyPaneTabsAsScriptNames(), it->second.tabs );
        EXPECT_EQ(2, it->second.currentIndex);
        EXPECT_EQ( std::string(), it->second.name );
        EXPECT_FALSE(it->second.isAnchor);
    }
    ASSERT_EQ( (std::size_t)1, layout.legacySplitterStates.size() );
    EXPECT_EQ( std::string("100 200"), layout.legacySplitterStates.find("Splitter1")->second );
}

TEST(ProjectGuiXmlEquivalence, AVersion2GuiSectionLoadsItsValues)
{
    ProjectGuiVersion2Writer written;

    written.nodes.push_back( makeNodeGuiVersion3("My Blur 1", 12.5, true) );
    written.nodes.push_back( makeNodeGuiVersion3("2D Transform", -40., false) );
    written.panes["Pane1"] = makePaneLayoutVersion3(false);
    written.panes["Pane2"] = makePaneLayoutVersion3(true);
    written.splitterStates["Splitter1"] = "100 200";
    written.viewers["Viewer1"] = makeViewerDataVersion3();
    written.previewsTurnedOffGlobally = true;
    written.histograms.push_back("histogram1");
    written.backdrops.push_back( makeBackdropVersion1("Backdrop1", "") );
    written.backdrops.push_back( makeBackdropVersion1("Backdrop2", "Backdrop1") );

    const Serialization::ProjectGuiSerialization loaded = loadAndCheckResave( saveXml("ProjectGui", written) );

    EXPECT_EQ( (unsigned int)NATRON_SERIALIZATION_PROJECT_GUI_INTRODUCES_BACKDROPS, loaded.version );

    ASSERT_EQ( (std::size_t)2, loaded.nodes.size() );
    const Serialization::NodeGuiSerialization& blur = loaded.nodes.front();
    EXPECT_EQ( (unsigned int)NATRON_SERIALIZATION_NODE_GUI_INTRODUCES_SELECTED, blur.version );
    EXPECT_EQ( std::string("My_Blur_1"), blur.nodeName );
    EXPECT_EQ(12.5, blur.positionX);
    EXPECT_EQ(-12.5, blur.positionY);
    EXPECT_TRUE(blur.previewEnabled);
    EXPECT_EQ(0.5f, blur.colorR);
    EXPECT_EQ(0.25f, blur.colorG);
    EXPECT_EQ(0.125f, blur.colorB);
    EXPECT_TRUE(blur.colorWasFound);
    EXPECT_TRUE(blur.selected);
    EXPECT_EQ(0., blur.width);
    EXPECT_EQ(0., blur.height);
    EXPECT_FALSE(blur.hasOverlayColor);
    EXPECT_TRUE(blur.children.empty());
    const Serialization::NodeGuiSerialization& transform = loaded.nodes.back();
    EXPECT_EQ( std::string("p2D_Transform"), transform.nodeName );
    EXPECT_EQ(-40., transform.positionX);
    EXPECT_FALSE(transform.selected);

    EXPECT_TRUE(loaded.layout.windows.empty());
    ASSERT_EQ( (std::size_t)2, loaded.layout.legacyPanes.size() );
    for (std::map<std::string, Serialization::PaneLayoutSerialization>::const_iterator it = loaded.layout.legacyPanes.begin(); it != loaded.layout.legacyPanes.end(); ++it) {
        EXPECT_EQ( legacyPaneTabsAsScriptNames(), it->second.tabs );
        EXPECT_EQ(2, it->second.currentIndex);
        EXPECT_EQ( std::string(), it->second.name );
        EXPECT_FALSE(it->second.isAnchor);
    }
    // Splitter states saved before NATRON_SERIALIZATION_PROJECT_GUI_CHANGES_SPLITTERS are dropped.
    EXPECT_TRUE(loaded.layout.legacySplitterStates.empty());

    ASSERT_EQ( (std::size_t)1, loaded.viewers.size() );
    const Serialization::ViewerDataSerialization& viewer = loaded.viewers.find("Viewer1")->second;
    EXPECT_EQ( (unsigned int)NATRON_SERIALIZATION_VIEWER_DATA_INTRODUCES_FRAME_RANGE, viewer.version );
    EXPECT_EQ(-7.5, viewer.zoomLeft);
    EXPECT_EQ(2.25, viewer.zoomBottom);
    EXPECT_EQ(0.5, viewer.zoomFactor);
    EXPECT_TRUE(viewer.userRoIEnabled);
    EXPECT_EQ(1., viewer.userRoI.x1);
    EXPECT_EQ(2., viewer.userRoI.y1);
    EXPECT_EQ(30., viewer.userRoI.x2);
    EXPECT_EQ(40., viewer.userRoI.y2);
    EXPECT_FALSE(viewer.isClippedToProject);
    EXPECT_TRUE(viewer.autoContrastEnabled);
    EXPECT_EQ(3., viewer.gain);
    EXPECT_EQ(1., viewer.gamma);
    EXPECT_EQ(std::string(), viewer.display);
    EXPECT_EQ(std::string(), viewer.view);
    EXPECT_EQ(std::string(), viewer.look);
    EXPECT_EQ(std::string(), viewer.layerName);
    EXPECT_EQ(std::string("RGB"), viewer.channels);
    EXPECT_TRUE(viewer.renderScaleActivated);
    EXPECT_EQ(1u, viewer.mipmapLevel);
    EXPECT_TRUE(viewer.zoomOrPanSinceLastFit);
    EXPECT_EQ(2, viewer.wipeCompositingOperator);
    EXPECT_EQ(1, viewer.leftBound);
    EXPECT_EQ(1, viewer.rightBound);
    EXPECT_TRUE(viewer.leftToolbarVisible);
    EXPECT_TRUE(viewer.timelineVisible);
    EXPECT_FALSE(viewer.isInputAPaused);
    EXPECT_FALSE(viewer.checkerboardEnabled);
    EXPECT_EQ(24., viewer.fps);
    EXPECT_TRUE(viewer.fpsLocked);
    EXPECT_EQ(-1, viewer.activeInputA);
    EXPECT_EQ(-1, viewer.activeInputB);
    EXPECT_FALSE(viewer.isFullFrameProcessEnabled);

    ASSERT_EQ( (std::size_t)1, loaded.histograms.size() );
    EXPECT_EQ( std::string("histogram1"), loaded.histograms.front() );

    ASSERT_EQ( (std::size_t)2, loaded.legacyBackdrops.size() );
    const Serialization::NodeBackdropSerialization& firstBackdrop = loaded.legacyBackdrops.front();
    EXPECT_EQ(15.5, firstBackdrop.positionX);
    EXPECT_EQ(-25.25, firstBackdrop.positionY);
    EXPECT_EQ(320, firstBackdrop.width);
    EXPECT_EQ(240, firstBackdrop.height);
    EXPECT_EQ( std::string("Backdrop1"), firstBackdrop.name );
    EXPECT_EQ( std::string(), firstBackdrop.masterBackdropName );
    expectSameKnob(makeStringKnob("Label", "Backdrop note"), firstBackdrop.label);
    EXPECT_EQ(0.3f, firstBackdrop.colorR);
    EXPECT_EQ(0.6f, firstBackdrop.colorG);
    EXPECT_EQ(0.9f, firstBackdrop.colorB);
    EXPECT_FALSE(firstBackdrop.selected);
    EXPECT_EQ( std::string("Backdrop2"), loaded.legacyBackdrops.back().name );
    EXPECT_EQ( std::string("Backdrop1"), loaded.legacyBackdrops.back().masterBackdropName );

    EXPECT_TRUE(loaded.openedPanels.empty());
    EXPECT_TRUE(loaded.openedPanelsMinimized.empty());
    EXPECT_TRUE(loaded.openedPanelsHideUnmodified.empty());
    EXPECT_TRUE(loaded.scriptEditorInput.empty());
    EXPECT_TRUE(loaded.pythonPanels.empty());
}

TEST(ProjectGuiXmlEquivalence, AVersion9GuiSectionLoadsItsValues)
{
    ProjectGuiVersion9Writer written;

    const Serialization::ProjectGuiSerialization populated = makePopulatedProjectGui();
    written.nodes = populated.nodes;
    written.layout.panes["Pane1"] = makePaneLayoutVersion3(false);
    written.layout.splitterStates["Splitter1"] = "100 200";
    written.viewers = populated.viewers;
    written.histograms = populated.histograms;
    Serialization::NodeBackdropSerialization backdrop;
    backdrop.positionX = -3.5;
    backdrop.positionY = 9.75;
    backdrop.width = 200;
    backdrop.height = 100;
    backdrop.name = "Backdrop1";
    backdrop.label = makeStringKnob("Label", "Selected backdrop");
    backdrop.colorR = 0.2f;
    backdrop.colorG = 0.4f;
    backdrop.colorB = 0.6f;
    backdrop.selected = true;
    written.backdrops.push_back(backdrop);
    written.openedPanels = populated.openedPanels;
    written.scriptEditorInput = populated.scriptEditorInput;

    const Serialization::ProjectGuiSerialization loaded = loadAndCheckResave( saveXml("ProjectGui", written) );

    EXPECT_EQ( (unsigned int)NATRON_SERIALIZATION_PROJECT_GUI_SCRIPT_EDITOR, loaded.version );

    ASSERT_EQ( populated.nodes.size(), loaded.nodes.size() );
    std::list<Serialization::NodeGuiSerialization>::const_iterator populatedNode = populated.nodes.begin();
    for (std::list<Serialization::NodeGuiSerialization>::const_iterator node = loaded.nodes.begin(); node != loaded.nodes.end(); ++node, ++populatedNode) {
        expectSameNodeGui(*populatedNode, *node);
    }

    EXPECT_TRUE(loaded.layout.windows.empty());
    ASSERT_EQ( (std::size_t)1, loaded.layout.legacyPanes.size() );
    const Serialization::PaneLayoutSerialization& pane = loaded.layout.legacyPanes.find("Pane1")->second;
    EXPECT_EQ( legacyPaneTabsAsScriptNames(), pane.tabs );
    EXPECT_EQ(2, pane.currentIndex);
    ASSERT_EQ( (std::size_t)1, loaded.layout.legacySplitterStates.size() );
    EXPECT_EQ( std::string("100 200"), loaded.layout.legacySplitterStates.find("Splitter1")->second );

    expectSameViewers(populated.viewers, loaded.viewers);
    EXPECT_EQ(populated.histograms, loaded.histograms);

    ASSERT_EQ( (std::size_t)1, loaded.legacyBackdrops.size() );
    expectSameBackdrop(backdrop, loaded.legacyBackdrops.front());

    EXPECT_EQ(populated.openedPanels, loaded.openedPanels);
    EXPECT_TRUE(loaded.openedPanelsMinimized.empty());
    EXPECT_TRUE(loaded.openedPanelsHideUnmodified.empty());
    EXPECT_EQ(populated.scriptEditorInput, loaded.scriptEditorInput);
    EXPECT_TRUE(loaded.pythonPanels.empty());
}
