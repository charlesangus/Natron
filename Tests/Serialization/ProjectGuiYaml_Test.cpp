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

#include <list>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "Serialization/ProjectGuiSerialization.h"
#include "Serialization/Yaml/ProjectGuiYaml.h"
#include "YamlTestHelpers.h"

NATRON_NAMESPACE_USING
using Serialization::ApplicationWindowSerialization;
using Serialization::GuiLayoutSerialization;
using Serialization::NodeGuiSerialization;
using Serialization::NodeGuiSerializationPtr;
using Serialization::PaneLayoutSerialization;
using Serialization::ProjectGuiSerialization;
using Serialization::SplitterChildSerialization;
using Serialization::SplitterSerialization;
using Serialization::ViewerDataSerialization;
using Serialization::YamlError;
using Serialization::YamlWarnings;

namespace {

NodeGuiSerialization
makeNodeGui(const std::string& name,
            double x,
            double y,
            int seed)
{
    NodeGuiSerialization node;
    node.nodeName = name;
    node.positionX = x;
    node.positionY = y;
    node.width = 80. + seed;
    node.height = 30.25;
    node.previewEnabled = (seed % 2) == 0;
    node.colorR = 0.1f * seed;
    node.colorG = 0.7f;
    node.colorB = 1.f / 3.f;
    node.colorWasFound = true;
    node.selected = (seed % 3) == 0;
    node.hasOverlayColor = (seed % 2) == 1;
    if (node.hasOverlayColor) {
        node.overlayColorR = 0.2;
        node.overlayColorG = 0.30000000000000004;
        node.overlayColorB = 1e-300;
    }
    node.hasUserColor = seed == 1;
    if (node.hasUserColor) {
        node.userColorR = 0.9f;
        node.userColorG = 0.f;
        node.userColorB = 1e-40f;
    }

    return node;
}

std::shared_ptr<PaneLayoutSerialization>
makePane(const std::string& name,
         std::list<std::string> tabs,
         int current,
         bool anchor)
{
    std::shared_ptr<PaneLayoutSerialization> pane = std::make_shared<PaneLayoutSerialization>();
    pane->name = name;
    pane->tabs = tabs;
    pane->currentIndex = current;
    pane->isAnchor = anchor;

    return pane;
}

GuiLayoutSerialization
makeLayout()
{
    std::shared_ptr<SplitterSerialization> inner = std::make_shared<SplitterSerialization>();
    inner->orientation = 1;
    inner->sizes = "640 480";
    SplitterChildSerialization viewerPane;
    viewerPane.pane = makePane("pane1", { "Viewer1", "Viewer2" }, 1, true);
    SplitterChildSerialization graphPane;
    graphPane.pane = makePane("pane2", { "nodeGraph", "curveEditor", "dopeSheet" }, 0, false);
    inner->children = { viewerPane, graphPane };

    std::shared_ptr<SplitterSerialization> root = std::make_shared<SplitterSerialization>();
    root->orientation = 0;
    root->sizes = "1200 400";
    SplitterChildSerialization nested;
    nested.splitter = inner;
    SplitterChildSerialization properties;
    properties.pane = makePane("pane3", { "properties" }, 0, false);
    root->children = { nested, properties };

    GuiLayoutSerialization layout;
    ApplicationWindowSerialization main;
    main.isMainWindow = true;
    main.positionX = -12;
    main.positionY = 40;
    main.width = 1920;
    main.height = 1080;
    main.childSplitter = root;
    layout.windows.push_back(main);

    ApplicationWindowSerialization floatingPanel;
    floatingPanel.positionX = 100;
    floatingPanel.positionY = 200;
    floatingPanel.width = 400;
    floatingPanel.height = 600;
    floatingPanel.childDockablePanel = NATRON_SERIALIZATION_PROJECT_SETTINGS_PANEL_NAME;
    layout.windows.push_back(floatingPanel);

    ApplicationWindowSerialization floatingPane;
    floatingPane.positionX = 0;
    floatingPane.positionY = 0;
    floatingPane.width = 300;
    floatingPane.height = 300;
    floatingPane.childPane = makePane("pane4", { "scriptEditor" }, 0, false);
    layout.windows.push_back(floatingPane);

    return layout;
}

ViewerDataSerialization
makeViewer(int seed)
{
    ViewerDataSerialization viewer;
    viewer.zoomLeft = -12.5 * seed;
    viewer.zoomBottom = 3.25;
    viewer.zoomFactor = 0.1 + seed;
    viewer.userRoIEnabled = true;
    viewer.userRoI.x1 = 10.;
    viewer.userRoI.y1 = 20.;
    viewer.userRoI.x2 = 1910.5;
    viewer.userRoI.y2 = 1070.;
    viewer.isClippedToProject = false;
    viewer.isFullFrameProcessEnabled = true;
    viewer.autoContrastEnabled = true;
    viewer.gain = 2.5;
    viewer.gamma = 0.4545;
    viewer.renderScaleActivated = true;
    viewer.mipmapLevel = 2;
    viewer.display = "sRGB - Display";
    viewer.view = "ACES 1.0 - SDR Video";
    viewer.look = seed ? "Filmic" : "";
    viewer.channels = "Luminance";
    viewer.layerName = "diffuse";
    viewer.alphaLayerName = "rgba";
    viewer.zoomOrPanSinceLastFit = true;
    viewer.wipeCompositingOperator = 3;
    viewer.leftToolbarVisible = false;
    viewer.rightToolbarVisible = true;
    viewer.topToolbarVisible = false;
    viewer.playerVisible = true;
    viewer.infobarVisible = false;
    viewer.timelineVisible = true;
    viewer.checkerboardEnabled = true;
    viewer.isInputAPaused = true;
    viewer.isInputBPaused = false;
    viewer.fps = 23.976;
    viewer.fpsLocked = false;
    viewer.leftBound = -5;
    viewer.rightBound = 240;
    viewer.activeInputA = 0;
    viewer.activeInputB = seed ? 2 : -1;

    return viewer;
}

ProjectGuiSerialization
makeGui()
{
    ProjectGuiSerialization gui;
    NodeGuiSerialization group = makeNodeGui("Group1", 100., -50.5, 0);
    NodeGuiSerializationPtr innerBlur = std::make_shared<NodeGuiSerialization>(makeNodeGui("Group1.Blur2", 0., 0., 1));
    NodeGuiSerializationPtr innerGroup = std::make_shared<NodeGuiSerialization>(makeNodeGui("Group1.Inner", 10., 10., 2));
    innerGroup->children.push_back(std::make_shared<NodeGuiSerialization>(makeNodeGui("Group1.Inner.Grade1", 1e-310, -0., 3)));
    group.children = { innerBlur, innerGroup };
    gui.nodes.push_back(makeNodeGui("Read1", -200., 0., 5));
    gui.nodes.push_back(group);
    gui.nodes.push_back(makeNodeGui("Backdrop1", -300., -300., 4));

    gui.layout = makeLayout();
    gui.viewers["Viewer1"] = makeViewer(0);
    gui.viewers["Group1.Viewer2"] = makeViewer(1);
    gui.histograms = { "Histogram1", "Histogram2" };
    gui.openedPanels = { "Group1.Blur2", NATRON_SERIALIZATION_PROJECT_SETTINGS_PANEL_NAME };
    gui.openedPanelsMinimized = { NATRON_SERIALIZATION_PROJECT_SETTINGS_PANEL_NAME };
    gui.openedPanelsHideUnmodified = { "Group1.Blur2" };
    gui.scriptEditorInput = "app.Read1.filename.get()\nprint(\"done\")";

    Serialization::PythonPanelSerialization panel;
    panel.name = "My panel";
    panel.pythonFunction = "myPanels.createPanel";
    Serialization::KnobSerialization knob;
    knob.name = "threshold";
    knob.typeName = "Double";
    knob.dimension = 1;
    knob.dimensions.resize(1);
    knob.dimensions[0].value = Serialization::KnobValueSerialization(std::in_place_type<double>, 0.75);
    knob.dimensions[0].defaultValue = Serialization::KnobValueSerialization(std::in_place_type<double>, 0.5);
    knob.isUserKnob = true;
    knob.label = "Threshold";
    knob.isPersistent = true;
    knob.extraData = Serialization::KnobValueRangeSerialization();
    panel.knobs.push_back(knob);
    panel.userData = "{\"key\": 1}";
    gui.pythonPanels.push_back(panel);

    return gui;
}

void
expectNodeGuisEqual(const NodeGuiSerialization& e,
                    const NodeGuiSerialization& a)
{
    SCOPED_TRACE("node " + e.nodeName);
    EXPECT_EQ(e.nodeName, a.nodeName);
    EXPECT_SAME_DOUBLE(e.positionX, a.positionX);
    EXPECT_SAME_DOUBLE(e.positionY, a.positionY);
    EXPECT_SAME_DOUBLE(e.width, a.width);
    EXPECT_SAME_DOUBLE(e.height, a.height);
    EXPECT_EQ(e.previewEnabled, a.previewEnabled);
    EXPECT_EQ(YamlTest::bitsOfFloat(e.colorR), YamlTest::bitsOfFloat(a.colorR));
    EXPECT_EQ(YamlTest::bitsOfFloat(e.colorG), YamlTest::bitsOfFloat(a.colorG));
    EXPECT_EQ(YamlTest::bitsOfFloat(e.colorB), YamlTest::bitsOfFloat(a.colorB));
    EXPECT_EQ(e.colorWasFound, a.colorWasFound);
    EXPECT_EQ(e.selected, a.selected);
    EXPECT_SAME_DOUBLE(e.overlayColorR, a.overlayColorR);
    EXPECT_SAME_DOUBLE(e.overlayColorG, a.overlayColorG);
    EXPECT_SAME_DOUBLE(e.overlayColorB, a.overlayColorB);
    EXPECT_EQ(e.hasOverlayColor, a.hasOverlayColor);
    EXPECT_EQ(e.hasUserColor, a.hasUserColor);
    EXPECT_EQ(YamlTest::bitsOfFloat(e.userColorR), YamlTest::bitsOfFloat(a.userColorR));
    EXPECT_EQ(YamlTest::bitsOfFloat(e.userColorG), YamlTest::bitsOfFloat(a.userColorG));
    EXPECT_EQ(YamlTest::bitsOfFloat(e.userColorB), YamlTest::bitsOfFloat(a.userColorB));
    ASSERT_EQ(e.children.size(), a.children.size());
    auto child = a.children.begin();
    for (const NodeGuiSerializationPtr& expectedChild : e.children) {
        expectNodeGuisEqual(*expectedChild, **child);
        ++child;
    }
}

void
expectPanesEqual(const std::shared_ptr<PaneLayoutSerialization>& e,
                 const std::shared_ptr<PaneLayoutSerialization>& a)
{
    ASSERT_EQ(bool(e), bool(a));
    if (!e) {
        return;
    }
    EXPECT_EQ(e->tabs, a->tabs);
    EXPECT_EQ(e->currentIndex, a->currentIndex);
    EXPECT_EQ(e->isAnchor, a->isAnchor);
    EXPECT_EQ(e->name, a->name);
}

void
expectSplittersEqual(const std::shared_ptr<SplitterSerialization>& e,
                     const std::shared_ptr<SplitterSerialization>& a)
{
    ASSERT_EQ(bool(e), bool(a));
    if (!e) {
        return;
    }
    EXPECT_EQ(e->sizes, a->sizes);
    EXPECT_EQ(e->orientation, a->orientation);
    ASSERT_EQ(e->children.size(), a->children.size());
    for (std::size_t i = 0; i < e->children.size(); ++i) {
        expectSplittersEqual(e->children[i].splitter, a->children[i].splitter);
        expectPanesEqual(e->children[i].pane, a->children[i].pane);
    }
}

void
expectLayoutsEqual(const GuiLayoutSerialization& e,
                   const GuiLayoutSerialization& a)
{
    ASSERT_EQ(e.windows.size(), a.windows.size());
    auto window = a.windows.begin();
    for (const ApplicationWindowSerialization& expected : e.windows) {
        expectSplittersEqual(expected.childSplitter, window->childSplitter);
        expectPanesEqual(expected.childPane, window->childPane);
        EXPECT_EQ(expected.childDockablePanel, window->childDockablePanel);
        EXPECT_EQ(expected.isMainWindow, window->isMainWindow);
        EXPECT_EQ(expected.positionX, window->positionX);
        EXPECT_EQ(expected.positionY, window->positionY);
        EXPECT_EQ(expected.width, window->width);
        EXPECT_EQ(expected.height, window->height);
        ++window;
    }
    EXPECT_TRUE(a.legacyPanes.empty());
    EXPECT_TRUE(a.legacySplitterStates.empty());
}

void
expectViewersEqual(const ViewerDataSerialization& e,
                   const ViewerDataSerialization& a)
{
    EXPECT_SAME_DOUBLE(e.zoomLeft, a.zoomLeft);
    EXPECT_SAME_DOUBLE(e.zoomBottom, a.zoomBottom);
    EXPECT_SAME_DOUBLE(e.zoomFactor, a.zoomFactor);
    EXPECT_EQ(e.userRoIEnabled, a.userRoIEnabled);
    EXPECT_SAME_DOUBLE(e.userRoI.x1, a.userRoI.x1);
    EXPECT_SAME_DOUBLE(e.userRoI.y1, a.userRoI.y1);
    EXPECT_SAME_DOUBLE(e.userRoI.x2, a.userRoI.x2);
    EXPECT_SAME_DOUBLE(e.userRoI.y2, a.userRoI.y2);
    EXPECT_EQ(e.isClippedToProject, a.isClippedToProject);
    EXPECT_EQ(e.isFullFrameProcessEnabled, a.isFullFrameProcessEnabled);
    EXPECT_EQ(e.autoContrastEnabled, a.autoContrastEnabled);
    EXPECT_SAME_DOUBLE(e.gain, a.gain);
    EXPECT_SAME_DOUBLE(e.gamma, a.gamma);
    EXPECT_EQ(e.renderScaleActivated, a.renderScaleActivated);
    EXPECT_EQ(e.mipmapLevel, a.mipmapLevel);
    EXPECT_EQ(e.display, a.display);
    EXPECT_EQ(e.view, a.view);
    EXPECT_EQ(e.look, a.look);
    EXPECT_EQ(e.channels, a.channels);
    EXPECT_EQ(e.layerName, a.layerName);
    EXPECT_EQ(e.alphaLayerName, a.alphaLayerName);
    EXPECT_EQ(e.zoomOrPanSinceLastFit, a.zoomOrPanSinceLastFit);
    EXPECT_EQ(e.wipeCompositingOperator, a.wipeCompositingOperator);
    EXPECT_EQ(e.leftToolbarVisible, a.leftToolbarVisible);
    EXPECT_EQ(e.rightToolbarVisible, a.rightToolbarVisible);
    EXPECT_EQ(e.topToolbarVisible, a.topToolbarVisible);
    EXPECT_EQ(e.playerVisible, a.playerVisible);
    EXPECT_EQ(e.infobarVisible, a.infobarVisible);
    EXPECT_EQ(e.timelineVisible, a.timelineVisible);
    EXPECT_EQ(e.checkerboardEnabled, a.checkerboardEnabled);
    EXPECT_EQ(e.isInputAPaused, a.isInputAPaused);
    EXPECT_EQ(e.isInputBPaused, a.isInputBPaused);
    EXPECT_SAME_DOUBLE(e.fps, a.fps);
    EXPECT_EQ(e.fpsLocked, a.fpsLocked);
    EXPECT_EQ(e.leftBound, a.leftBound);
    EXPECT_EQ(e.rightBound, a.rightBound);
    EXPECT_EQ(e.activeInputA, a.activeInputA);
    EXPECT_EQ(e.activeInputB, a.activeInputB);
}

void
expectDecodeError(const std::string& text,
                  const std::string& expectedFragment)
{
    try {
        Serialization::decodeProjectGuiYaml(text);
        ADD_FAILURE() << "decoding did not throw";
    } catch (const YamlError& e) {
        EXPECT_NE(std::string(e.what()).find(expectedFragment), std::string::npos) << e.what();
    }
}

} // anonymous namespace

TEST(ProjectGuiYaml, FullyPopulatedGuiRoundTrips)
{
    const ProjectGuiSerialization gui = makeGui();
    const std::string text = Serialization::encodeProjectGuiYaml(gui);
    YamlWarnings warnings;
    const ProjectGuiSerialization decoded = Serialization::decodeProjectGuiYaml(text, &warnings);
    EXPECT_TRUE(warnings.empty());

    ASSERT_EQ(gui.nodes.size(), decoded.nodes.size());
    auto node = decoded.nodes.begin();
    for (const NodeGuiSerialization& expected : gui.nodes) {
        expectNodeGuisEqual(expected, *node);
        ++node;
    }
    expectLayoutsEqual(gui.layout, decoded.layout);
    ASSERT_EQ(gui.viewers.size(), decoded.viewers.size());
    for (const auto& viewer : gui.viewers) {
        SCOPED_TRACE("viewer " + viewer.first);
        ASSERT_EQ(1u, decoded.viewers.count(viewer.first));
        expectViewersEqual(viewer.second, decoded.viewers.at(viewer.first));
    }
    EXPECT_EQ(gui.histograms, decoded.histograms);
    EXPECT_TRUE(decoded.legacyBackdrops.empty());
    EXPECT_EQ(gui.openedPanels, decoded.openedPanels);
    EXPECT_EQ(gui.openedPanelsMinimized, decoded.openedPanelsMinimized);
    EXPECT_EQ(gui.openedPanelsHideUnmodified, decoded.openedPanelsHideUnmodified);
    EXPECT_EQ(gui.scriptEditorInput, decoded.scriptEditorInput);
    ASSERT_EQ(1u, decoded.pythonPanels.size());
    EXPECT_EQ(gui.pythonPanels.front().name, decoded.pythonPanels.front().name);
    EXPECT_EQ(gui.pythonPanels.front().pythonFunction, decoded.pythonPanels.front().pythonFunction);
    EXPECT_EQ(gui.pythonPanels.front().userData, decoded.pythonPanels.front().userData);
    expectExpectedKnobs(gui.pythonPanels.front().knobs, decoded.pythonPanels.front().knobs);

    EXPECT_EQ(text, Serialization::encodeProjectGuiYaml(decoded));
}

TEST(ProjectGuiYaml, NodesAreKeyedByFullyQualifiedName)
{
    const std::string text = Serialization::encodeProjectGuiYaml(makeGui());
    const std::size_t nodes = text.find("\nnodes:\n");
    ASSERT_NE(std::string::npos, nodes);
    EXPECT_NE(std::string::npos, text.find("\n  Read1: {pos: [-200, 0], size: [85, 30.25], color: [0.5, 0.7, 0.33333334], overlay: [0.2, 0.30000000000000004, 1e-300]}\n", nodes)) << text;
    EXPECT_NE(std::string::npos, text.find("\n  Group1.Inner.Grade1: {pos: [1e-310, -0]", nodes)) << text;
    EXPECT_LT(text.find("\n  Group1:"), text.find("\n  Group1.Blur2:"));
    EXPECT_LT(text.find("\n  Group1.Inner.Grade1:"), text.find("\n  Backdrop1:"));
}

TEST(ProjectGuiYaml, LayoutHasItsOwnDocument)
{
    const GuiLayoutSerialization layout = makeLayout();
    const std::string text = Serialization::encodeGuiLayoutYaml(layout);
    EXPECT_EQ(0u, text.find("format: natron-layout\nformatVersion: 1\nwindows:\n"));
    expectLayoutsEqual(layout, Serialization::decodeGuiLayoutYaml(text));

    const std::string guiText = Serialization::encodeProjectGuiYaml(makeGui());
    const std::string windows = text.substr(text.find("windows:\n") + 9);
    EXPECT_NE(std::string::npos, guiText.find("layout:\n" + windows)) << guiText;

    try {
        Serialization::decodeGuiLayoutYaml(guiText);
        ADD_FAILURE() << "a gui.yaml was read as a layout";
    } catch (const YamlError& e) {
        EXPECT_NE(std::string(e.what()).find("is not a Natron layout"), std::string::npos) << e.what();
    }
}

TEST(ProjectGuiYaml, MalformedOrNewerDocumentsAreRefused)
{
    expectDecodeError("format: natron-gui\nformatVersion: 2\n", "gui.yaml was saved by a newer Natron");
    expectDecodeError("format: natron-project\nformatVersion: 1\n", "gui.yaml is not a Natron GUI state");
    expectDecodeError("format: natron-gui\nformatVersion: 1\nnodes:\n  Read1: {size: [1, 2]}\n", "gui.nodes.Read1.pos: expected a list of 2 numbers");
    expectDecodeError("format: natron-gui\nformatVersion: 1\nlayout:\n  - {pos: [0, 0], size: [1]}\n", "gui.layout[0].size: expected a list of 2 integers");

    YamlWarnings warnings;
    Serialization::decodeProjectGuiYaml("format: natron-gui\nformatVersion: 1\nnodes:\n  Read1: {pos: [1, 2], glow: true}\n", &warnings);
    ASSERT_EQ(1u, warnings.size());
    EXPECT_EQ("gui.nodes.Read1: unknown key \"glow\" ignored", warnings[0]);
}
