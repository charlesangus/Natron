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
#include <list>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

// The old classes are archived here too, so their BOOST_CLASS_VERSION declarations (in these
// headers) and RectD's serialize definition must be visible.
#include "Engine/RectDSerialization.h"
#include "Gui/NodeBackdropSerialization.h"
#include "Gui/NodeGuiSerialization.h"
#include "Gui/ProjectGuiSerialization.h"

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

NATRON_NAMESPACE::PaneLayout*
toOldPane(const Serialization::PaneLayoutSerialization& pane)
{
    NATRON_NAMESPACE::PaneLayout* oldPane = new NATRON_NAMESPACE::PaneLayout;

    oldPane->tabs = pane.tabs;
    oldPane->currentIndex = pane.currentIndex;
    oldPane->isAnchor = pane.isAnchor;
    oldPane->name = pane.name;

    return oldPane;
}

NATRON_NAMESPACE::SplitterSerialization*
toOldSplitter(const Serialization::SplitterSerialization& splitter)
{
    NATRON_NAMESPACE::SplitterSerialization* oldSplitter = new NATRON_NAMESPACE::SplitterSerialization;

    oldSplitter->sizes = splitter.sizes;
    oldSplitter->orientation = splitter.orientation;
    for (std::size_t i = 0; i < splitter.children.size(); ++i) {
        NATRON_NAMESPACE::SplitterSerialization::Child* oldChild = new NATRON_NAMESPACE::SplitterSerialization::Child;
        if (splitter.children[i].splitter) {
            oldChild->child_asSplitter = toOldSplitter(*splitter.children[i].splitter);
        } else {
            oldChild->child_asPane = toOldPane(*splitter.children[i].pane);
        }
        oldSplitter->children.push_back(oldChild);
    }

    return oldSplitter;
}

NATRON_NAMESPACE::ApplicationWindowSerialization*
toOldWindow(const Serialization::ApplicationWindowSerialization& window)
{
    NATRON_NAMESPACE::ApplicationWindowSerialization* oldWindow = new NATRON_NAMESPACE::ApplicationWindowSerialization;

    if (window.childSplitter) {
        oldWindow->child_asSplitter = toOldSplitter(*window.childSplitter);
    } else if (window.childPane) {
        oldWindow->child_asPane = toOldPane(*window.childPane);
    }
    oldWindow->child_asDockablePanel = window.childDockablePanel;
    oldWindow->isMainWindow = window.isMainWindow;
    oldWindow->x = window.positionX;
    oldWindow->y = window.positionY;
    oldWindow->w = window.width;
    oldWindow->h = window.height;

    return oldWindow;
}

NATRON_NAMESPACE::ViewerData
toOldViewerData(const Serialization::ViewerDataSerialization& viewer)
{
    NATRON_NAMESPACE::ViewerData oldViewer = NATRON_NAMESPACE::ViewerData();

    oldViewer.zoomLeft = viewer.zoomLeft;
    oldViewer.zoomBottom = viewer.zoomBottom;
    oldViewer.zoomFactor = viewer.zoomFactor;
    oldViewer.userRoIenabled = viewer.userRoIEnabled;
    oldViewer.userRoI = RectD(viewer.userRoI.x1, viewer.userRoI.y1, viewer.userRoI.x2, viewer.userRoI.y2);
    oldViewer.isClippedToProject = viewer.isClippedToProject;
    oldViewer.isFullFrameProcessEnabled = viewer.isFullFrameProcessEnabled;
    oldViewer.autoContrastEnabled = viewer.autoContrastEnabled;
    oldViewer.gain = viewer.gain;
    oldViewer.gamma = viewer.gamma;
    oldViewer.renderScaleActivated = viewer.renderScaleActivated;
    oldViewer.mipmapLevel = viewer.mipmapLevel;
    oldViewer.display = viewer.display;
    oldViewer.view = viewer.view;
    oldViewer.look = viewer.look;
    oldViewer.channels = viewer.channels;
    oldViewer.layerName = viewer.layerName;
    oldViewer.alphaLayerName = viewer.alphaLayerName;
    oldViewer.zoomOrPanSinceLastFit = viewer.zoomOrPanSinceLastFit;
    oldViewer.wipeCompositingOp = viewer.wipeCompositingOperator;
    oldViewer.leftToolbarVisible = viewer.leftToolbarVisible;
    oldViewer.rightToolbarVisible = viewer.rightToolbarVisible;
    oldViewer.topToolbarVisible = viewer.topToolbarVisible;
    oldViewer.playerVisible = viewer.playerVisible;
    oldViewer.infobarVisible = viewer.infobarVisible;
    oldViewer.timelineVisible = viewer.timelineVisible;
    oldViewer.checkerboardEnabled = viewer.checkerboardEnabled;
    oldViewer.isPauseEnabled[0] = viewer.isInputAPaused;
    oldViewer.isPauseEnabled[1] = viewer.isInputBPaused;
    oldViewer.fps = viewer.fps;
    oldViewer.fpsLocked = viewer.fpsLocked;
    oldViewer.leftBound = viewer.leftBound;
    oldViewer.rightBound = viewer.rightBound;
    oldViewer.aChoice = viewer.activeInputA;
    oldViewer.bChoice = viewer.activeInputB;
    oldViewer.version = viewer.version;

    return oldViewer;
}

void
expectSameKnob(const Serialization::KnobSerialization& oldKnob,
               const Serialization::KnobSerialization& knob)
{
    EXPECT_EQ(oldKnob.name, knob.name);
    EXPECT_EQ(oldKnob.typeName, knob.typeName);
    EXPECT_EQ(oldKnob.dimension, knob.dimension);
    ASSERT_EQ( oldKnob.dimensions.size(), knob.dimensions.size() );
    for (std::size_t i = 0; i < knob.dimensions.size(); ++i) {
        EXPECT_TRUE(oldKnob.dimensions[i].value == knob.dimensions[i].value);
    }
}

void
expectSameNodeGui(const NATRON_NAMESPACE::NodeGuiSerialization& oldNode,
                  const Serialization::NodeGuiSerialization& node)
{
    EXPECT_EQ(oldNode.getFullySpecifiedName(), node.nodeName);
    EXPECT_EQ(oldNode.getX(), node.positionX);
    EXPECT_EQ(oldNode.getY(), node.positionY);
    double oldWidth = -1.;
    double oldHeight = -1.;
    oldNode.getSize(&oldWidth, &oldHeight);
    EXPECT_EQ(oldWidth, node.width);
    EXPECT_EQ(oldHeight, node.height);
    EXPECT_EQ(oldNode.isPreviewEnabled(), node.previewEnabled);
    float oldColorR = -1.f;
    float oldColorG = -1.f;
    float oldColorB = -1.f;
    oldNode.getColor(&oldColorR, &oldColorG, &oldColorB);
    EXPECT_EQ(oldColorR, node.colorR);
    EXPECT_EQ(oldColorG, node.colorG);
    EXPECT_EQ(oldColorB, node.colorB);
    EXPECT_EQ(oldNode.colorWasFound(), node.colorWasFound);
    EXPECT_EQ(oldNode.isSelected(), node.selected);
    double oldOverlayR = -1.;
    double oldOverlayG = -1.;
    double oldOverlayB = -1.;
    const bool oldHasOverlayColor = oldNode.getOverlayColor(&oldOverlayR, &oldOverlayG, &oldOverlayB);
    EXPECT_EQ(oldHasOverlayColor, node.hasOverlayColor);
    if (oldHasOverlayColor && node.hasOverlayColor) {
        EXPECT_EQ(oldOverlayR, node.overlayColorR);
        EXPECT_EQ(oldOverlayG, node.overlayColorG);
        EXPECT_EQ(oldOverlayB, node.overlayColorB);
    }

    const std::list<NodeGuiSerializationPtr>& oldChildren = oldNode.getChildren();
    ASSERT_EQ( oldChildren.size(), node.children.size() );
    std::list<NodeGuiSerializationPtr>::const_iterator oldChild = oldChildren.begin();
    for (std::list<Serialization::NodeGuiSerializationPtr>::const_iterator child = node.children.begin(); child != node.children.end(); ++child, ++oldChild) {
        expectSameNodeGui(**oldChild, **child);
    }
}

void
expectSamePane(const NATRON_NAMESPACE::PaneLayout& oldPane,
               const Serialization::PaneLayoutSerialization& pane)
{
    EXPECT_EQ(oldPane.tabs, pane.tabs);
    EXPECT_EQ(oldPane.currentIndex, pane.currentIndex);
    EXPECT_EQ(oldPane.isAnchor, pane.isAnchor);
    EXPECT_EQ(oldPane.name, pane.name);
}

void
expectSameSplitter(const NATRON_NAMESPACE::SplitterSerialization& oldSplitter,
                   const Serialization::SplitterSerialization& splitter)
{
    EXPECT_EQ(oldSplitter.sizes, splitter.sizes);
    EXPECT_EQ(oldSplitter.orientation, splitter.orientation);
    ASSERT_EQ( oldSplitter.children.size(), splitter.children.size() );
    for (std::size_t i = 0; i < splitter.children.size(); ++i) {
        const NATRON_NAMESPACE::SplitterSerialization::Child& oldChild = *oldSplitter.children[i];
        const Serialization::SplitterChildSerialization& child = splitter.children[i];
        ASSERT_EQ( oldChild.child_asSplitter != NULL, (bool)child.splitter );
        if (child.splitter) {
            expectSameSplitter(*oldChild.child_asSplitter, *child.splitter);
        } else {
            ASSERT_TRUE(oldChild.child_asPane != NULL);
            ASSERT_TRUE( (bool)child.pane );
            expectSamePane(*oldChild.child_asPane, *child.pane);
        }
    }
}

void
expectSameWindow(const NATRON_NAMESPACE::ApplicationWindowSerialization& oldWindow,
                 const Serialization::ApplicationWindowSerialization& window)
{
    ASSERT_EQ( oldWindow.child_asSplitter != NULL, (bool)window.childSplitter );
    ASSERT_EQ( oldWindow.child_asPane != NULL, (bool)window.childPane );
    if (window.childSplitter) {
        expectSameSplitter(*oldWindow.child_asSplitter, *window.childSplitter);
    }
    if (window.childPane) {
        expectSamePane(*oldWindow.child_asPane, *window.childPane);
    }
    EXPECT_EQ(oldWindow.child_asDockablePanel, window.childDockablePanel);
    EXPECT_EQ(oldWindow.isMainWindow, window.isMainWindow);
    EXPECT_EQ(oldWindow.x, window.positionX);
    EXPECT_EQ(oldWindow.y, window.positionY);
    EXPECT_EQ(oldWindow.w, window.width);
    EXPECT_EQ(oldWindow.h, window.height);
}

void
expectSameViewerData(const NATRON_NAMESPACE::ViewerData& oldViewer,
                     const Serialization::ViewerDataSerialization& viewer)
{
    EXPECT_EQ(oldViewer.version, viewer.version);
    EXPECT_EQ(oldViewer.zoomLeft, viewer.zoomLeft);
    EXPECT_EQ(oldViewer.zoomBottom, viewer.zoomBottom);
    EXPECT_EQ(oldViewer.zoomFactor, viewer.zoomFactor);
    EXPECT_EQ(oldViewer.userRoIenabled, viewer.userRoIEnabled);
    EXPECT_EQ(oldViewer.userRoI.x1, viewer.userRoI.x1);
    EXPECT_EQ(oldViewer.userRoI.y1, viewer.userRoI.y1);
    EXPECT_EQ(oldViewer.userRoI.x2, viewer.userRoI.x2);
    EXPECT_EQ(oldViewer.userRoI.y2, viewer.userRoI.y2);
    EXPECT_EQ(oldViewer.isClippedToProject, viewer.isClippedToProject);
    EXPECT_EQ(oldViewer.isFullFrameProcessEnabled, viewer.isFullFrameProcessEnabled);
    EXPECT_EQ(oldViewer.autoContrastEnabled, viewer.autoContrastEnabled);
    EXPECT_EQ(oldViewer.gain, viewer.gain);
    EXPECT_EQ(oldViewer.gamma, viewer.gamma);
    EXPECT_EQ(oldViewer.renderScaleActivated, viewer.renderScaleActivated);
    EXPECT_EQ(oldViewer.mipmapLevel, viewer.mipmapLevel);
    EXPECT_EQ(oldViewer.display, viewer.display);
    EXPECT_EQ(oldViewer.view, viewer.view);
    EXPECT_EQ(oldViewer.look, viewer.look);
    EXPECT_EQ(oldViewer.channels, viewer.channels);
    EXPECT_EQ(oldViewer.layerName, viewer.layerName);
    EXPECT_EQ(oldViewer.alphaLayerName, viewer.alphaLayerName);
    EXPECT_EQ(oldViewer.zoomOrPanSinceLastFit, viewer.zoomOrPanSinceLastFit);
    EXPECT_EQ(oldViewer.wipeCompositingOp, viewer.wipeCompositingOperator);
    EXPECT_EQ(oldViewer.leftToolbarVisible, viewer.leftToolbarVisible);
    EXPECT_EQ(oldViewer.rightToolbarVisible, viewer.rightToolbarVisible);
    EXPECT_EQ(oldViewer.topToolbarVisible, viewer.topToolbarVisible);
    EXPECT_EQ(oldViewer.playerVisible, viewer.playerVisible);
    EXPECT_EQ(oldViewer.infobarVisible, viewer.infobarVisible);
    EXPECT_EQ(oldViewer.timelineVisible, viewer.timelineVisible);
    EXPECT_EQ(oldViewer.checkerboardEnabled, viewer.checkerboardEnabled);
    EXPECT_EQ(oldViewer.isPauseEnabled[0], viewer.isInputAPaused);
    EXPECT_EQ(oldViewer.isPauseEnabled[1], viewer.isInputBPaused);
    EXPECT_EQ(oldViewer.fps, viewer.fps);
    EXPECT_EQ(oldViewer.fpsLocked, viewer.fpsLocked);
    EXPECT_EQ(oldViewer.leftBound, viewer.leftBound);
    EXPECT_EQ(oldViewer.rightBound, viewer.rightBound);
    EXPECT_EQ(oldViewer.aChoice, viewer.activeInputA);
    EXPECT_EQ(oldViewer.bChoice, viewer.activeInputB);
}

void
expectSameBackdrop(const NATRON_NAMESPACE::NodeBackdropSerialization& oldBackdrop,
                   const Serialization::NodeBackdropSerialization& backdrop)
{
    double oldPositionX = -1.;
    double oldPositionY = -1.;
    oldBackdrop.getPos(oldPositionX, oldPositionY);
    EXPECT_EQ(oldPositionX, backdrop.positionX);
    EXPECT_EQ(oldPositionY, backdrop.positionY);
    int oldWidth = -1;
    int oldHeight = -1;
    oldBackdrop.getSize(oldWidth, oldHeight);
    EXPECT_EQ(oldWidth, backdrop.width);
    EXPECT_EQ(oldHeight, backdrop.height);
    EXPECT_EQ(oldBackdrop.getFullySpecifiedName(), backdrop.name);
    EXPECT_EQ(oldBackdrop.getMasterBackdropName(), backdrop.masterBackdropName);
    float oldColorR = -1.f;
    float oldColorG = -1.f;
    float oldColorB = -1.f;
    oldBackdrop.getColor(oldColorR, oldColorG, oldColorB);
    EXPECT_EQ(oldColorR, backdrop.colorR);
    EXPECT_EQ(oldColorG, backdrop.colorG);
    EXPECT_EQ(oldColorB, backdrop.colorB);
    EXPECT_EQ(oldBackdrop.isSelected(), backdrop.selected);
}

void
expectSameProjectGui(const NATRON_NAMESPACE::ProjectGuiSerialization& oldProjectGui,
                     const Serialization::ProjectGuiSerialization& projectGui)
{
    EXPECT_EQ(oldProjectGui.getVersion(), projectGui.version);

    const std::list<NATRON_NAMESPACE::NodeGuiSerialization>& oldNodes = oldProjectGui.getSerializedNodesGui();
    ASSERT_EQ( oldNodes.size(), projectGui.nodes.size() );
    std::list<NATRON_NAMESPACE::NodeGuiSerialization>::const_iterator oldNode = oldNodes.begin();
    for (std::list<Serialization::NodeGuiSerialization>::const_iterator node = projectGui.nodes.begin(); node != projectGui.nodes.end(); ++node, ++oldNode) {
        expectSameNodeGui(*oldNode, *node);
    }

    const NATRON_NAMESPACE::GuiLayoutSerialization& oldLayout = oldProjectGui.getGuiLayout();
    ASSERT_EQ( oldLayout._windows.size(), projectGui.layout.windows.size() );
    std::list<NATRON_NAMESPACE::ApplicationWindowSerialization*>::const_iterator oldWindow = oldLayout._windows.begin();
    for (std::list<Serialization::ApplicationWindowSerialization>::const_iterator window = projectGui.layout.windows.begin(); window != projectGui.layout.windows.end(); ++window, ++oldWindow) {
        expectSameWindow(**oldWindow, *window);
    }
    ASSERT_EQ( oldLayout._layout.size(), projectGui.layout.legacyPanes.size() );
    for (std::map<std::string, Serialization::PaneLayoutSerialization>::const_iterator pane = projectGui.layout.legacyPanes.begin(); pane != projectGui.layout.legacyPanes.end(); ++pane) {
        std::map<std::string, NATRON_NAMESPACE::PaneLayout>::const_iterator oldPane = oldLayout._layout.find(pane->first);
        ASSERT_TRUE( oldPane != oldLayout._layout.end() );
        expectSamePane(oldPane->second, pane->second);
    }
    EXPECT_EQ(oldLayout._splittersStates, projectGui.layout.legacySplitterStates);

    const std::map<std::string, NATRON_NAMESPACE::ViewerData>& oldViewers = oldProjectGui.getViewersProjections();
    ASSERT_EQ( oldViewers.size(), projectGui.viewers.size() );
    for (std::map<std::string, Serialization::ViewerDataSerialization>::const_iterator viewer = projectGui.viewers.begin(); viewer != projectGui.viewers.end(); ++viewer) {
        std::map<std::string, NATRON_NAMESPACE::ViewerData>::const_iterator oldViewer = oldViewers.find(viewer->first);
        ASSERT_TRUE( oldViewer != oldViewers.end() );
        expectSameViewerData(oldViewer->second, viewer->second);
    }

    EXPECT_EQ(oldProjectGui.getHistograms(), projectGui.histograms);

    const std::list<NATRON_NAMESPACE::NodeBackdropSerialization>& oldBackdrops = oldProjectGui.getBackdrops();
    ASSERT_EQ( oldBackdrops.size(), projectGui.legacyBackdrops.size() );
    std::list<NATRON_NAMESPACE::NodeBackdropSerialization>::const_iterator oldBackdrop = oldBackdrops.begin();
    for (std::list<Serialization::NodeBackdropSerialization>::const_iterator backdrop = projectGui.legacyBackdrops.begin(); backdrop != projectGui.legacyBackdrops.end(); ++backdrop, ++oldBackdrop) {
        expectSameBackdrop(*oldBackdrop, *backdrop);
    }

    EXPECT_EQ(oldProjectGui.getOpenedPanels(), projectGui.openedPanels);
    EXPECT_EQ(oldProjectGui.getOpenedPanelsMinimized(), projectGui.openedPanelsMinimized);
    EXPECT_EQ(oldProjectGui.getOpenedPanelsHideUnmodified(), projectGui.openedPanelsHideUnmodified);
    EXPECT_EQ(oldProjectGui.getInputScript(), projectGui.scriptEditorInput);

    const std::list<PythonPanelSerializationPtr>& oldPythonPanels = oldProjectGui.getPythonPanels();
    ASSERT_EQ( oldPythonPanels.size(), projectGui.pythonPanels.size() );
    std::list<PythonPanelSerializationPtr>::const_iterator oldPythonPanel = oldPythonPanels.begin();
    for (std::list<Serialization::PythonPanelSerialization>::const_iterator pythonPanel = projectGui.pythonPanels.begin(); pythonPanel != projectGui.pythonPanels.end(); ++pythonPanel, ++oldPythonPanel) {
        EXPECT_EQ( (*oldPythonPanel)->name, pythonPanel->name );
        EXPECT_EQ( (*oldPythonPanel)->pythonFunction, pythonPanel->pythonFunction );
        EXPECT_EQ( (*oldPythonPanel)->userData, pythonPanel->userData );
        ASSERT_EQ( (*oldPythonPanel)->knobs.size(), pythonPanel->knobs.size() );
        std::list<Serialization::KnobSerialization>::const_iterator oldKnob = (*oldPythonPanel)->knobs.begin();
        for (std::list<Serialization::KnobSerialization>::const_iterator knob = pythonPanel->knobs.begin(); knob != pythonPanel->knobs.end(); ++knob, ++oldKnob) {
            expectSameKnob(*oldKnob, *knob);
        }
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

// Loads xml with both the old classes and the new adapter, checks they read the same state and
// save it back to the same XML, and returns what the new adapter read.
Serialization::ProjectGuiSerialization
loadWithBothAndCompare(const std::string& xml)
{
    NATRON_NAMESPACE::ProjectGuiSerialization oldProjectGui;
    Serialization::ProjectGuiSerialization projectGui;

    EXPECT_NO_THROW( loadXml(xml, "ProjectGui", &oldProjectGui) );
    EXPECT_NO_THROW( loadXml(xml, "ProjectGui", &projectGui) );
    expectSameProjectGui(oldProjectGui, projectGui);
    EXPECT_EQ( saveXml("ProjectGui", oldProjectGui), saveXml("ProjectGui", projectGui) );

    return projectGui;
}

} // anonymous namespace

BOOST_CLASS_VERSION(NodeGuiVersion3Writer, NATRON_SERIALIZATION_NODE_GUI_INTRODUCES_SELECTED)
BOOST_CLASS_VERSION(BackdropVersion1Writer, 1)
BOOST_CLASS_VERSION(PaneLayoutVersion3Writer, NATRON_SERIALIZATION_PANE_INTRODUCES_SIZE)
BOOST_CLASS_VERSION(ViewerDataVersion3Writer, NATRON_SERIALIZATION_VIEWER_DATA_INTRODUCES_FRAME_RANGE)
BOOST_CLASS_VERSION(ProjectGuiVersion2Writer, NATRON_SERIALIZATION_PROJECT_GUI_INTRODUCES_BACKDROPS)
BOOST_CLASS_VERSION(GuiLayoutVersion1Writer, 1)
BOOST_CLASS_VERSION(ProjectGuiVersion9Writer, NATRON_SERIALIZATION_PROJECT_GUI_SCRIPT_EDITOR)

TEST(ProjectGuiXmlEquivalence, APopulatedGuiSectionSavesTheSameXmlThroughTheOldAndNewClasses)
{
    const Serialization::ProjectGuiSerialization source = makePopulatedProjectGui();
    const std::string newXml = saveXml("ProjectGui", source);

    NATRON_NAMESPACE::ProjectGuiSerialization oldProjectGui;
    ASSERT_NO_THROW( loadXml(newXml, "ProjectGui", &oldProjectGui) );
    expectSameProjectGui(oldProjectGui, source);

    const std::string oldXml = saveXml("ProjectGui", oldProjectGui);
    EXPECT_EQ(oldXml, newXml);

    Serialization::ProjectGuiSerialization reloaded;
    ASSERT_NO_THROW( loadXml(oldXml, "ProjectGui", &reloaded) );
    expectSameProjectGui(oldProjectGui, reloaded);
    EXPECT_EQ( saveXml("ProjectGui", reloaded), oldXml );
}

TEST(ProjectGuiXmlEquivalence, TheLayoutViewersAndPythonPanelsMatchTheOldClassesBuiltFromTheSameValues)
{
    const Serialization::ProjectGuiSerialization source = makePopulatedProjectGui();

    NATRON_NAMESPACE::GuiLayoutSerialization oldLayout;
    for (std::list<Serialization::ApplicationWindowSerialization>::const_iterator it = source.layout.windows.begin(); it != source.layout.windows.end(); ++it) {
        oldLayout._windows.push_back( toOldWindow(*it) );
    }
    EXPECT_EQ( saveXml("Layout", oldLayout), saveXml("Layout", source.layout) );

    std::map<std::string, NATRON_NAMESPACE::ViewerData> oldViewers;
    for (std::map<std::string, Serialization::ViewerDataSerialization>::const_iterator it = source.viewers.begin(); it != source.viewers.end(); ++it) {
        oldViewers.insert( std::make_pair( it->first, toOldViewerData(it->second) ) );
    }
    EXPECT_EQ( saveXml("ViewersData", oldViewers), saveXml("ViewersData", source.viewers) );

    const Serialization::PythonPanelSerialization& pythonPanel = source.pythonPanels.front();
    NATRON_NAMESPACE::PythonPanelSerialization oldPythonPanel;
    oldPythonPanel.knobs = pythonPanel.knobs;
    oldPythonPanel.name = pythonPanel.name;
    oldPythonPanel.pythonFunction = pythonPanel.pythonFunction;
    oldPythonPanel.userData = pythonPanel.userData;
    EXPECT_EQ( saveXml("PythonPanel", oldPythonPanel), saveXml("PythonPanel", pythonPanel) );
}

TEST(ProjectGuiXmlEquivalence, AVersion2GuiSectionLoadsTheSameThroughTheOldAndNewClasses)
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

    const Serialization::ProjectGuiSerialization loaded = loadWithBothAndCompare( saveXml("ProjectGui", written) );

    EXPECT_EQ( (unsigned int)NATRON_SERIALIZATION_PROJECT_GUI_INTRODUCES_BACKDROPS, loaded.version );
    ASSERT_EQ( (std::size_t)2, loaded.nodes.size() );
    EXPECT_EQ( std::string("My_Blur_1"), loaded.nodes.front().nodeName );
    EXPECT_EQ( std::string("p2D_Transform"), loaded.nodes.back().nodeName );
    EXPECT_TRUE(loaded.nodes.front().colorWasFound);
    EXPECT_FALSE(loaded.nodes.front().hasOverlayColor);
    EXPECT_TRUE(loaded.layout.windows.empty());
    ASSERT_EQ( (std::size_t)2, loaded.layout.legacyPanes.size() );
    std::list<std::string> expectedTabs;
    expectedTabs.push_back(NATRON_SERIALIZATION_CURVE_EDITOR_TAB_NAME);
    expectedTabs.push_back(NATRON_SERIALIZATION_NODE_GRAPH_TAB_NAME);
    expectedTabs.push_back("properties");
    expectedTabs.push_back("Viewer1");
    EXPECT_EQ( expectedTabs, loaded.layout.legacyPanes.find("Pane2")->second.tabs );
    EXPECT_EQ( 2, loaded.layout.legacyPanes.find("Pane2")->second.currentIndex );
    EXPECT_TRUE(loaded.layout.legacySplitterStates.empty());
    ASSERT_EQ( (std::size_t)1, loaded.viewers.size() );
    const Serialization::ViewerDataSerialization& viewer = loaded.viewers.find("Viewer1")->second;
    EXPECT_EQ( (unsigned int)NATRON_SERIALIZATION_VIEWER_DATA_INTRODUCES_FRAME_RANGE, viewer.version );
    EXPECT_EQ(1., viewer.gamma);
    EXPECT_EQ(3., viewer.gain);
    EXPECT_EQ(std::string(), viewer.display);
    EXPECT_EQ(2, viewer.wipeCompositingOperator);
    EXPECT_EQ(1, viewer.leftBound);
    EXPECT_EQ(-1, viewer.activeInputA);
    ASSERT_EQ( (std::size_t)2, loaded.legacyBackdrops.size() );
    EXPECT_EQ( std::string("Backdrop1"), loaded.legacyBackdrops.back().masterBackdropName );
    EXPECT_FALSE(loaded.legacyBackdrops.back().selected);
    EXPECT_EQ( std::string("Label"), loaded.legacyBackdrops.back().label.name );
    EXPECT_TRUE(loaded.openedPanels.empty());
    EXPECT_TRUE(loaded.scriptEditorInput.empty());
}

TEST(ProjectGuiXmlEquivalence, AVersion9GuiSectionLoadsTheSameThroughTheOldAndNewClasses)
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

    const Serialization::ProjectGuiSerialization loaded = loadWithBothAndCompare( saveXml("ProjectGui", written) );

    EXPECT_EQ( (unsigned int)NATRON_SERIALIZATION_PROJECT_GUI_SCRIPT_EDITOR, loaded.version );
    EXPECT_EQ( populated.nodes.size(), loaded.nodes.size() );
    EXPECT_TRUE(loaded.layout.windows.empty());
    EXPECT_EQ( (std::size_t)1, loaded.layout.legacyPanes.size() );
    EXPECT_EQ( std::string("100 200"), loaded.layout.legacySplitterStates.find("Splitter1")->second );
    EXPECT_EQ( populated.viewers.size(), loaded.viewers.size() );
    ASSERT_EQ( (std::size_t)1, loaded.legacyBackdrops.size() );
    EXPECT_TRUE(loaded.legacyBackdrops.front().selected);
    EXPECT_EQ(populated.openedPanels, loaded.openedPanels);
    EXPECT_TRUE(loaded.openedPanelsMinimized.empty());
    EXPECT_EQ(populated.scriptEditorInput, loaded.scriptEditorInput);
    EXPECT_TRUE(loaded.pythonPanels.empty());
}
