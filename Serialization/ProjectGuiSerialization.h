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

#ifndef Serialization_ProjectGuiSerialization_h
#define Serialization_ProjectGuiSerialization_h

#include "Global/Macros.h"

#include <list>
#include <memory>
#include <string>
#include <vector>

#include "Serialization/SerializationFwd.h"

#define NATRON_SERIALIZATION_PANE_INTRODUCES_CURRENT_TAB 2
#define NATRON_SERIALIZATION_PANE_INTRODUCES_SIZE 3
#define NATRON_SERIALIZATION_PANE_MAJOR_OVERHAUL 4
#define NATRON_SERIALIZATION_PANE_INTRODUCES_SCRIPT_NAME 5
#define NATRON_SERIALIZATION_PANE_VERSION NATRON_SERIALIZATION_PANE_INTRODUCES_SCRIPT_NAME

#define NATRON_SERIALIZATION_SPLITTER_VERSION 1
#define NATRON_SERIALIZATION_APPLICATION_WINDOW_VERSION 1

#define NATRON_SERIALIZATION_GUI_LAYOUT_MAJOR_OVERHAUL 2
#define NATRON_SERIALIZATION_GUI_LAYOUT_VERSION NATRON_SERIALIZATION_GUI_LAYOUT_MAJOR_OVERHAUL

// The name a layout gives the project settings panel wherever it names a settings panel.
#define NATRON_SERIALIZATION_PROJECT_SETTINGS_PANEL_NAME "Natron_Project_Settings_Panel"

NATRON_NAMESPACE_ENTER
namespace Serialization {

/**
 * @brief One pane: the script names of its tabs, the index of the current tab, its name and
 * whether it is the anchor pane new viewers go to.
 **/
struct PaneLayoutSerialization {
    std::list<std::string> tabs;
    int currentIndex;

    bool isAnchor;
    std::string name;

    PaneLayoutSerialization()
        : tabs()
        , currentIndex(-1)
        , isAnchor(false)
        , name()
    {
    }
};

/**
 * @brief One side of a splitter: a nested splitter when splitter is set, a pane otherwise.
 **/
struct SplitterChildSerialization {
    std::shared_ptr<SplitterSerialization> splitter;
    std::shared_ptr<PaneLayoutSerialization> pane;
};

/**
 * @brief A splitter: the sizes of its two sides as Splitter::serializeNatron() writes them, its
 * orientation as the value of OrientationEnum, and its two sides.
 **/
struct SplitterSerialization {
    std::string sizes;
    int orientation;
    std::vector<SplitterChildSerialization> children;

    SplitterSerialization()
        : sizes()
        , orientation(0)
        , children()
    {
    }
};

/**
 * @brief One window of the application, the main window or a floating one: what it holds (a
 * splitter, a pane, or for a floating window the settings panel named childDockablePanel), and
 * its position and size. At most one of childSplitter, childPane and childDockablePanel is set.
 **/
struct ApplicationWindowSerialization {
    std::shared_ptr<SplitterSerialization> childSplitter;
    std::shared_ptr<PaneLayoutSerialization> childPane;

    // The script name of the node whose settings panel the window holds, or
    // NATRON_SERIALIZATION_PROJECT_SETTINGS_PANEL_NAME.
    std::string childDockablePanel;

    bool isMainWindow;
    int positionX;
    int positionY;
    int width;
    int height;

    ApplicationWindowSerialization()
        : childSplitter()
        , childPane()
        , childDockablePanel()
        , isMainWindow(false)
        , positionX(0)
        , positionY(0)
        , width(0)
        , height(0)
    {
    }
};

/**
 * @brief The layout of the application: its windows, the main window first.
 **/
struct GuiLayoutSerialization {
    std::list<ApplicationWindowSerialization> windows;
};

} // namespace Serialization
NATRON_NAMESPACE_EXIT

#endif // Serialization_ProjectGuiSerialization_h
