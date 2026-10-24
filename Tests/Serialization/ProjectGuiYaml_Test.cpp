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
using Serialization::PaneLayoutSerialization;
using Serialization::SplitterChildSerialization;
using Serialization::SplitterSerialization;
using Serialization::YamlError;
using Serialization::YamlWarnings;

namespace {

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
}

void
expectDecodeError(const std::string& text,
                  const std::string& expectedFragment)
{
    try {
        Serialization::decodeGuiLayoutYaml(text);
        ADD_FAILURE() << "decoding did not throw";
    } catch (const YamlError& e) {
        EXPECT_NE(std::string(e.what()).find(expectedFragment), std::string::npos) << e.what();
    }
}

} // anonymous namespace

TEST(ProjectGuiYaml, LayoutHasItsOwnDocument)
{
    const GuiLayoutSerialization layout = makeLayout();
    const std::string text = Serialization::encodeGuiLayoutYaml(layout);
    EXPECT_EQ(0u, text.find("format: natron-layout\nformatVersion: 1\nwindows:\n"));
    expectLayoutsEqual(layout, Serialization::decodeGuiLayoutYaml(text));

    try {
        Serialization::decodeGuiLayoutYaml("format: natron-project\nformatVersion: 1\n");
        ADD_FAILURE() << "a project document was read as a layout";
    } catch (const YamlError& e) {
        EXPECT_NE(std::string(e.what()).find("is not a Natron layout"), std::string::npos) << e.what();
    }
}

TEST(ProjectGuiYaml, MalformedOrNewerDocumentsAreRefused)
{
    expectDecodeError("format: natron-layout\nformatVersion: 2\n", "the layout was saved by a newer Natron");
    expectDecodeError("format: natron-project\nformatVersion: 1\n", "the layout is not a Natron layout");
    expectDecodeError("format: natron-layout\nformatVersion: 1\nwindows:\n  - {pos: [0, 0], size: [1]}\n", "layout.windows[0].size: expected a list of 2 integers");

    YamlWarnings warnings;
    Serialization::decodeGuiLayoutYaml("format: natron-layout\nformatVersion: 1\nwindows: []\nglow: true\n", &warnings);
    ASSERT_EQ(1u, warnings.size());
    EXPECT_EQ("layout: unknown key \"glow\" ignored", warnings[0]);
}
