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
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "Serialization/BasicSerialization.h"
#include "Serialization/KnobSerialization.h"
#include "Serialization/SerializationFwd.h"

// The class versions the XML writer stamps on the GUI section it writes.
#define NATRON_SERIALIZATION_NODE_GUI_INTRODUCES_COLOR 2
#define NATRON_SERIALIZATION_NODE_GUI_INTRODUCES_SELECTED 3
#define NATRON_SERIALIZATION_NODE_GUI_MERGE_BACKDROP 4
#define NATRON_SERIALIZATION_NODE_GUI_INTRODUCES_OVERLAY_COLOR 5
#define NATRON_SERIALIZATION_NODE_GUI_INTRODUCES_CHILDREN 6
#define NATRON_SERIALIZATION_NODE_GUI_INTRODUCES_USER_COLOR 7
#define NATRON_SERIALIZATION_NODE_GUI_VERSION NATRON_SERIALIZATION_NODE_GUI_INTRODUCES_USER_COLOR

#define NATRON_SERIALIZATION_NODE_BACKDROP_INTRODUCES_SELECTED 2
#define NATRON_SERIALIZATION_NODE_BACKDROP_VERSION NATRON_SERIALIZATION_NODE_BACKDROP_INTRODUCES_SELECTED

#define NATRON_SERIALIZATION_PYTHON_PANEL_VERSION 1

#define NATRON_SERIALIZATION_VIEWER_DATA_INTRODUCES_WIPE_COMPOSITING 2
#define NATRON_SERIALIZATION_VIEWER_DATA_INTRODUCES_FRAME_RANGE 3
#define NATRON_SERIALIZATION_VIEWER_DATA_INTRODUCES_TOOLBARS_VISIBLITY 4
#define NATRON_SERIALIZATION_VIEWER_DATA_INTRODUCES_CHECKERBOARD 5
#define NATRON_SERIALIZATION_VIEWER_DATA_INTRODUCES_FPS 6
#define NATRON_SERIALIZATION_VIEWER_DATA_REMOVES_ASPECT_RATIO 7
#define NATRON_SERIALIZATION_VIEWER_DATA_INTRODUCES_FPS_LOCK 8
#define NATRON_SERIALIZATION_VIEWER_DATA_REMOVES_FRAME_RANGE_LOCK 9
#define NATRON_SERIALIZATION_VIEWER_DATA_INTRODUCES_GAMMA 10
#define NATRON_SERIALIZATION_VIEWER_DATA_INTRODUCES_ACTIVE_INPUTS 11
#define NATRON_SERIALIZATION_VIEWER_DATA_INTRODUCES_PAUSE_VIEWER 12
#define NATRON_SERIALIZATION_VIEWER_DATA_INTRODUCES_LAYER 13
#define NATRON_SERIALIZATION_VIEWER_DATA_INTRODUCES_FULL_FRAME_PROC 14
#define NATRON_SERIALIZATION_VIEWER_DATA_INTRODUCES_OCIO_DISPLAY 15
#define NATRON_SERIALIZATION_VIEWER_DATA_VERSION NATRON_SERIALIZATION_VIEWER_DATA_INTRODUCES_OCIO_DISPLAY

#define NATRON_SERIALIZATION_PROJECT_GUI_INTRODUCES_BACKDROPS 2
#define NATRON_SERIALIZATION_PROJECT_GUI_REMOVES_ALL_NODE_PREVIEW_TOGGLED 3
#define NATRON_SERIALIZATION_PROJECT_GUI_INTRODUCES_PANELS 4
#define NATRON_SERIALIZATION_PROJECT_GUI_CHANGES_SPLITTERS 5
#define NATRON_SERIALIZATION_PROJECT_GUI_EXTERNALISE_GUI_LAYOUT 6
#define NATRON_SERIALIZATION_PROJECT_GUI_MAJOR_OVERHAUL 7
#define NATRON_SERIALIZATION_PROJECT_GUI_NODEGRAPH_ZOOM_TO_POINT 8
#define NATRON_SERIALIZATION_PROJECT_GUI_SCRIPT_EDITOR 9
#define NATRON_SERIALIZATION_PROJECT_GUI_MERGE_BACKDROP 10
#define NATRON_SERIALIZATION_PROJECT_GUI_INTRODUCES_PYTHON_PANELS 11
#define NATRON_SERIALIZATION_PROJECT_GUI_INTRODUCES_PANEL_STATES 12
#define NATRON_SERIALIZATION_PROJECT_GUI_VERSION NATRON_SERIALIZATION_PROJECT_GUI_INTRODUCES_PANEL_STATES

#define NATRON_SERIALIZATION_PANE_INTRODUCES_CURRENT_TAB 2
#define NATRON_SERIALIZATION_PANE_INTRODUCES_SIZE 3
#define NATRON_SERIALIZATION_PANE_MAJOR_OVERHAUL 4
#define NATRON_SERIALIZATION_PANE_INTRODUCES_SCRIPT_NAME 5
#define NATRON_SERIALIZATION_PANE_VERSION NATRON_SERIALIZATION_PANE_INTRODUCES_SCRIPT_NAME

#define NATRON_SERIALIZATION_SPLITTER_VERSION 1
#define NATRON_SERIALIZATION_APPLICATION_WINDOW_VERSION 1

#define NATRON_SERIALIZATION_GUI_LAYOUT_MAJOR_OVERHAUL 2
#define NATRON_SERIALIZATION_GUI_LAYOUT_VERSION NATRON_SERIALIZATION_GUI_LAYOUT_MAJOR_OVERHAUL

// The name a project file gives the project settings panel wherever it names a settings panel.
#define NATRON_SERIALIZATION_PROJECT_SETTINGS_PANEL_NAME "Natron_Project_Settings_Panel"

NATRON_NAMESPACE_ENTER
namespace Serialization {

/**
 * @brief The node graph state of one node: its position, size, colours, whether its preview is
 * shown and whether it is selected, and, for a group, the same for the nodes it holds.
 * nodeName is the node's fully qualified script name.
 **/
struct NodeGuiSerialization {
    std::string nodeName;
    double positionX;
    double positionY;

    double width;
    double height;

    bool previewEnabled;
    float colorR;
    float colorG;
    float colorB;

    // Set when the file saved a colour.
    bool colorWasFound;

    bool selected;
    double overlayColorR;
    double overlayColorG;
    double overlayColorB;
    bool hasOverlayColor;

    bool hasUserColor;
    float userColorR;
    float userColorG;
    float userColorB;

    std::list<NodeGuiSerializationPtr> children;

    NodeGuiSerialization()
        : nodeName()
        , positionX(0.)
        , positionY(0.)
        , width(0.)
        , height(0.)
        , previewEnabled(false)
        , colorR(0.f)
        , colorG(0.f)
        , colorB(0.f)
        , colorWasFound(false)
        , selected(false)
        , overlayColorR(1.)
        , overlayColorG(1.)
        , overlayColorB(1.)
        , hasOverlayColor(false)
        , hasUserColor(false)
        , userColorR(0.f)
        , userColorG(0.f)
        , userColorB(0.f)
        , children()
    {
    }
};

/**
 * @brief Whether the node carries a user colour, and which one.
 **/
bool resolveNodeGuiUserColor(const NodeGuiSerialization& node,
                             float* r,
                             float* g,
                             float* b);

/**
 * @brief The state of one viewer: its viewport, region of interest, display settings, visible
 * toolbars, playback settings and active inputs.
 **/
struct ViewerDataSerialization {
    double zoomLeft;
    double zoomBottom;
    double zoomFactor;
    bool userRoIEnabled;

    // In canonical coordinates.
    RectDSerialization userRoI;

    bool isClippedToProject;
    bool isFullFrameProcessEnabled;
    bool autoContrastEnabled;
    double gain;
    double gamma;
    bool renderScaleActivated;
    unsigned int mipmapLevel;

    // Empty for the project's display defaults.
    std::string display;
    std::string view;

    // Empty for no look override.
    std::string look;

    std::string channels;
    std::string layerName;
    std::string alphaLayerName;
    bool zoomOrPanSinceLastFit;

    // The value of ViewerCompositingOperatorEnum.
    int wipeCompositingOperator;

    bool leftToolbarVisible;
    bool rightToolbarVisible;
    bool topToolbarVisible;
    bool playerVisible;
    bool infobarVisible;
    bool timelineVisible;
    bool checkerboardEnabled;
    bool isInputAPaused;
    bool isInputBPaused;
    double fps;
    bool fpsLocked;
    int leftBound;
    int rightBound;

    // The input index shown as A and as B, -1 for none.
    int activeInputA;
    int activeInputB;

    ViewerDataSerialization()
        : zoomLeft(0.)
        , zoomBottom(0.)
        , zoomFactor(1.)
        , userRoIEnabled(false)
        , userRoI()
        , isClippedToProject(true)
        , isFullFrameProcessEnabled(false)
        , autoContrastEnabled(false)
        , gain(1.)
        , gamma(1.)
        , renderScaleActivated(false)
        , mipmapLevel(0)
        , display()
        , view()
        , look()
        , channels()
        , layerName()
        , alphaLayerName()
        , zoomOrPanSinceLastFit(false)
        , wipeCompositingOperator(0)
        , leftToolbarVisible(true)
        , rightToolbarVisible(true)
        , topToolbarVisible(true)
        , playerVisible(true)
        , infobarVisible(true)
        , timelineVisible(true)
        , checkerboardEnabled(false)
        , isInputAPaused(false)
        , isInputBPaused(false)
        , fps(24.)
        , fpsLocked(true)
        , leftBound(1)
        , rightBound(1)
        , activeInputA(-1)
        , activeInputB(-1)
    {
    }
};

/**
 * @brief A Python panel the user opened: its label, the Python function that creates it, the
 * parameters it holds and the data its save function returned.
 **/
struct PythonPanelSerialization {
    std::string name;
    std::string pythonFunction;
    std::list<KnobSerialization> knobs;
    std::string userData;
};

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

/**
 * @brief The GUI section of a project file: the node graph state of the nodes, the layout, the
 * viewers, histograms, opened settings panels and Python panels, and the script editor input.
 **/
struct ProjectGuiSerialization {
    // The nodes at the top level of the project; a group saves the nodes it holds.
    std::list<NodeGuiSerialization> nodes;

    GuiLayoutSerialization layout;

    // By viewer node script name.
    std::map<std::string, ViewerDataSerialization> viewers;

    // The object names of the opened histograms.
    std::list<std::string> histograms;

    // The opened settings panels, top first, by node fully qualified script name or
    // NATRON_SERIALIZATION_PROJECT_SETTINGS_PANEL_NAME, and which of them are minimized or hide
    // their unmodified parameters.
    std::list<std::string> openedPanels;
    std::list<std::string> openedPanelsMinimized;
    std::list<std::string> openedPanelsHideUnmodified;

    std::string scriptEditorInput;
    std::list<PythonPanelSerialization> pythonPanels;

    ProjectGuiSerialization()
        : nodes()
        , layout()
        , viewers()
        , histograms()
        , openedPanels()
        , openedPanelsMinimized()
        , openedPanelsHideUnmodified()
        , scriptEditorInput()
        , pythonPanels()
    {
    }
};

} // namespace Serialization
NATRON_NAMESPACE_EXIT

#endif // Serialization_ProjectGuiSerialization_h
