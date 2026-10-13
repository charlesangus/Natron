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

#ifndef Serialization_BoostXml_ProjectGuiXml_h
#define Serialization_BoostXml_ProjectGuiXml_h

#include "Global/Macros.h"

#include <cassert>
#include <list>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "Serialization/BasicSerialization.h"
#include "Serialization/BoostXml/BasicTypesXml.h"
#include "Serialization/BoostXml/KnobXml.h"
#include "Serialization/KnobSerialization.h"
#include "Serialization/ProjectGuiSerialization.h"

#if !defined(Q_MOC_RUN) && !defined(SBK_RUN)
GCC_DIAG_UNUSED_LOCAL_TYPEDEFS_OFF
// clang-format off
GCC_DIAG_OFF(unused-parameter)
// /opt/local/include/boost/serialization/smart_cast.hpp:254:25: warning: unused parameter 'u' [-Wunused-parameter]
#include <boost/archive/xml_iarchive.hpp>
#include <boost/archive/xml_oarchive.hpp>
#include <boost/serialization/list.hpp>
#include <boost/serialization/map.hpp>
#include <boost/serialization/nvp.hpp>
#include <boost/serialization/split_free.hpp>
#include <boost/serialization/string.hpp>
#include <boost/serialization/utility.hpp>
#include <boost/serialization/version.hpp>
GCC_DIAG_UNUSED_LOCAL_TYPEDEFS_ON
GCC_DIAG_ON(unused-parameter)
// clang-format on
#endif

// Mirror kCurveEditorObjectName and kNodeGraphObjectName in Global/GlobalDefines.h: panes saved
// before NATRON_SERIALIZATION_PANE_INTRODUCES_SCRIPT_NAME name these tabs differently.
#define NATRON_SERIALIZATION_CURVE_EDITOR_TAB_NAME "curveEditor"
#define NATRON_SERIALIZATION_NODE_GRAPH_TAB_NAME "nodeGraph"

// Non-intrusive adapters reproducing the NVP sequence and class versions of the old intrusive
// NodeGuiSerialization, NodeBackdropSerialization, ViewerData, PythonPanelSerialization,
// PaneLayout, SplitterSerialization, ApplicationWindowSerialization, GuiLayoutSerialization and
// ProjectGuiSerialization, including every branch that reads files saved by older versions.
// Splitters and panes held directly by a window are archived through a pointer, which makes both
// types tracked: every splitter and pane carries an object_id wherever it is archived.
namespace boost {
namespace serialization {

    template <class Archive>
    void
    save(Archive& ar,
         const NATRON_NAMESPACE::Serialization::NodeGuiSerialization& node,
         const unsigned int /*version*/)
    {
        ar& ::boost::serialization::make_nvp("Name", node.nodeName);
        ar& ::boost::serialization::make_nvp("X_position", node.positionX);
        ar& ::boost::serialization::make_nvp("Y_position", node.positionY);
        ar& ::boost::serialization::make_nvp("Preview_enabled", node.previewEnabled);
        ar& ::boost::serialization::make_nvp("r", node.colorR);
        ar& ::boost::serialization::make_nvp("g", node.colorG);
        ar& ::boost::serialization::make_nvp("b", node.colorB);
        ar& ::boost::serialization::make_nvp("Selected", node.selected);
        ar& ::boost::serialization::make_nvp("Width", node.width);
        ar& ::boost::serialization::make_nvp("Height", node.height);
        ar& ::boost::serialization::make_nvp("HasOverlayColor", node.hasOverlayColor);

        if (node.hasOverlayColor) {
            ar& ::boost::serialization::make_nvp("oR", node.overlayColorR);
            ar& ::boost::serialization::make_nvp("oG", node.overlayColorG);
            ar& ::boost::serialization::make_nvp("oB", node.overlayColorB);
        }

        ar& ::boost::serialization::make_nvp("HasUserColor", node.hasUserColor);
        if (node.hasUserColor) {
            ar& ::boost::serialization::make_nvp("uR", node.userColorR);
            ar& ::boost::serialization::make_nvp("uG", node.userColorG);
            ar& ::boost::serialization::make_nvp("uB", node.userColorB);
        }

        int childCount = (int)node.children.size();
        ar& ::boost::serialization::make_nvp("Children", childCount);
        for (std::list<NATRON_NAMESPACE::Serialization::NodeGuiSerializationPtr>::const_iterator it = node.children.begin(); it != node.children.end(); ++it) {
            ar& ::boost::serialization::make_nvp("item", **it);
        }
    }

    template <class Archive>
    void
    load(Archive& ar,
         NATRON_NAMESPACE::Serialization::NodeGuiSerialization& node,
         const unsigned int version)
    {
        node = NATRON_NAMESPACE::Serialization::NodeGuiSerialization();
        node.version = version;

        ar& ::boost::serialization::make_nvp("Name", node.nodeName);
        ar& ::boost::serialization::make_nvp("X_position", node.positionX);
        ar& ::boost::serialization::make_nvp("Y_position", node.positionY);
        ar& ::boost::serialization::make_nvp("Preview_enabled", node.previewEnabled);

        if (version >= NATRON_SERIALIZATION_NODE_GUI_INTRODUCES_COLOR) {
            ar& ::boost::serialization::make_nvp("r", node.colorR);
            ar& ::boost::serialization::make_nvp("g", node.colorG);
            ar& ::boost::serialization::make_nvp("b", node.colorB);
            node.colorWasFound = true;
        }
        if (version >= NATRON_SERIALIZATION_NODE_GUI_INTRODUCES_SELECTED) {
            ar& ::boost::serialization::make_nvp("Selected", node.selected);
        }

        if (version >= NATRON_SERIALIZATION_NODE_GUI_MERGE_BACKDROP) {
            ar& ::boost::serialization::make_nvp("Width", node.width);
            ar& ::boost::serialization::make_nvp("Height", node.height);
        } else {
            // Engine's makeNameScriptFriendly() performs the same conversion as
            // scriptFriendlyKnobName(): these files refer to nodes by label.
            node.nodeName = NATRON_NAMESPACE::Serialization::scriptFriendlyKnobName(node.nodeName);
        }

        if (version >= NATRON_SERIALIZATION_NODE_GUI_INTRODUCES_OVERLAY_COLOR) {
            ar& ::boost::serialization::make_nvp("HasOverlayColor", node.hasOverlayColor);
            if (node.hasOverlayColor) {
                ar& ::boost::serialization::make_nvp("oR", node.overlayColorR);
                ar& ::boost::serialization::make_nvp("oG", node.overlayColorG);
                ar& ::boost::serialization::make_nvp("oB", node.overlayColorB);
            }
        } else {
            node.hasOverlayColor = false;
        }

        if (version >= NATRON_SERIALIZATION_NODE_GUI_INTRODUCES_USER_COLOR) {
            ar& ::boost::serialization::make_nvp("HasUserColor", node.hasUserColor);
            if (node.hasUserColor) {
                ar& ::boost::serialization::make_nvp("uR", node.userColorR);
                ar& ::boost::serialization::make_nvp("uG", node.userColorG);
                ar& ::boost::serialization::make_nvp("uB", node.userColorB);
            }
        }

        if (version >= NATRON_SERIALIZATION_NODE_GUI_INTRODUCES_CHILDREN) {
            int childCount = 0;
            ar& ::boost::serialization::make_nvp("Children", childCount);
            for (int i = 0; i < childCount; ++i) {
                NATRON_NAMESPACE::Serialization::NodeGuiSerializationPtr child = std::make_shared<NATRON_NAMESPACE::Serialization::NodeGuiSerialization>();
                ar& ::boost::serialization::make_nvp("item", *child);
                node.children.push_back(child);
            }
        }
    }

    template <class Archive>
    void
    serialize(Archive& ar,
              NATRON_NAMESPACE::Serialization::NodeGuiSerialization& node,
              const unsigned int version)
    {
        ::boost::serialization::split_free(ar, node, version);
    }

    template <class Archive>
    void
    save(Archive& ar,
         const NATRON_NAMESPACE::Serialization::NodeBackdropSerialization& backdrop,
         const unsigned int /*version*/)
    {
        ar& ::boost::serialization::make_nvp("X_position", backdrop.positionX);
        ar& ::boost::serialization::make_nvp("Y_position", backdrop.positionY);
        ar& ::boost::serialization::make_nvp("Width", backdrop.width);
        ar& ::boost::serialization::make_nvp("Height", backdrop.height);
        ar& ::boost::serialization::make_nvp("Name", backdrop.name);
        ar& ::boost::serialization::make_nvp("MasterName", backdrop.masterBackdropName);
        ar& ::boost::serialization::make_nvp("Label", backdrop.label);
        ar& ::boost::serialization::make_nvp("r", backdrop.colorR);
        ar& ::boost::serialization::make_nvp("g", backdrop.colorG);
        ar& ::boost::serialization::make_nvp("b", backdrop.colorB);
        ar& ::boost::serialization::make_nvp("Selected", backdrop.selected);
    }

    template <class Archive>
    void
    load(Archive& ar,
         NATRON_NAMESPACE::Serialization::NodeBackdropSerialization& backdrop,
         const unsigned int version)
    {
        backdrop = NATRON_NAMESPACE::Serialization::NodeBackdropSerialization();

        ar& ::boost::serialization::make_nvp("X_position", backdrop.positionX);
        ar& ::boost::serialization::make_nvp("Y_position", backdrop.positionY);
        ar& ::boost::serialization::make_nvp("Width", backdrop.width);
        ar& ::boost::serialization::make_nvp("Height", backdrop.height);
        ar& ::boost::serialization::make_nvp("Name", backdrop.name);
        ar& ::boost::serialization::make_nvp("MasterName", backdrop.masterBackdropName);
        ar& ::boost::serialization::make_nvp("Label", backdrop.label);
        ar& ::boost::serialization::make_nvp("r", backdrop.colorR);
        ar& ::boost::serialization::make_nvp("g", backdrop.colorG);
        ar& ::boost::serialization::make_nvp("b", backdrop.colorB);
        if (version >= NATRON_SERIALIZATION_NODE_BACKDROP_INTRODUCES_SELECTED) {
            ar& ::boost::serialization::make_nvp("Selected", backdrop.selected);
        }
    }

    template <class Archive>
    void
    serialize(Archive& ar,
              NATRON_NAMESPACE::Serialization::NodeBackdropSerialization& backdrop,
              const unsigned int version)
    {
        ::boost::serialization::split_free(ar, backdrop, version);
    }

    template <class Archive>
    void
    save(Archive& ar,
         const NATRON_NAMESPACE::Serialization::ViewerDataSerialization& viewer,
         const unsigned int /*version*/)
    {
        ar& ::boost::serialization::make_nvp("zoomLeft", viewer.zoomLeft);
        ar& ::boost::serialization::make_nvp("zoomBottom", viewer.zoomBottom);
        ar& ::boost::serialization::make_nvp("zoomFactor", viewer.zoomFactor);
        ar& ::boost::serialization::make_nvp("UserRoIEnabled", viewer.userRoIEnabled);
        ar& ::boost::serialization::make_nvp("UserRoI", viewer.userRoI);
        ar& ::boost::serialization::make_nvp("ClippedToProject", viewer.isClippedToProject);
        ar& ::boost::serialization::make_nvp("AutoContrast", viewer.autoContrastEnabled);
        ar& ::boost::serialization::make_nvp("Gain", viewer.gain);
        // The gamma is saved under the name "Gain" as well.
        ar& ::boost::serialization::make_nvp("Gain", viewer.gamma);
        ar& ::boost::serialization::make_nvp("Display", viewer.display);
        ar& ::boost::serialization::make_nvp("View", viewer.view);
        ar& ::boost::serialization::make_nvp("Look", viewer.look);
        ar& ::boost::serialization::make_nvp("Layer", viewer.layerName);
        ar& ::boost::serialization::make_nvp("AlphaLayer", viewer.alphaLayerName);
        ar& ::boost::serialization::make_nvp("Channels", viewer.channels);
        ar& ::boost::serialization::make_nvp("RenderScaleActivated", viewer.renderScaleActivated);
        ar& ::boost::serialization::make_nvp("MipMapLevel", viewer.mipmapLevel);
        ar& ::boost::serialization::make_nvp("ZoomOrPanSinceFit", viewer.zoomOrPanSinceLastFit);
        ar& ::boost::serialization::make_nvp("CompositingOP", viewer.wipeCompositingOperator);
        ar& ::boost::serialization::make_nvp("LeftBound", viewer.leftBound);
        ar& ::boost::serialization::make_nvp("RightBound", viewer.rightBound);
        ar& ::boost::serialization::make_nvp("LeftToolbarVisible", viewer.leftToolbarVisible);
        ar& ::boost::serialization::make_nvp("RightToolbarVisible", viewer.rightToolbarVisible);
        ar& ::boost::serialization::make_nvp("TopToolbarVisible", viewer.topToolbarVisible);
        ar& ::boost::serialization::make_nvp("PlayerVisible", viewer.playerVisible);
        ar& ::boost::serialization::make_nvp("TimelineVisible", viewer.timelineVisible);
        ar& ::boost::serialization::make_nvp("InfobarVisible", viewer.infobarVisible);
        ar& ::boost::serialization::make_nvp("isInputAPaused", viewer.isInputAPaused);
        ar& ::boost::serialization::make_nvp("isInputBPaused", viewer.isInputBPaused);
        ar& ::boost::serialization::make_nvp("CheckerboardEnabled", viewer.checkerboardEnabled);
        ar& ::boost::serialization::make_nvp("Fps", viewer.fps);
        ar& ::boost::serialization::make_nvp("FpsLocked", viewer.fpsLocked);
        ar& ::boost::serialization::make_nvp("aInput", viewer.activeInputA);
        ar& ::boost::serialization::make_nvp("bInput", viewer.activeInputB);
        ar& ::boost::serialization::make_nvp("fullFrame", viewer.isFullFrameProcessEnabled);
    }

    template <class Archive>
    void
    load(Archive& ar,
         NATRON_NAMESPACE::Serialization::ViewerDataSerialization& viewer,
         const unsigned int version)
    {
        viewer = NATRON_NAMESPACE::Serialization::ViewerDataSerialization();
        viewer.version = version;

        ar& ::boost::serialization::make_nvp("zoomLeft", viewer.zoomLeft);
        ar& ::boost::serialization::make_nvp("zoomBottom", viewer.zoomBottom);
        ar& ::boost::serialization::make_nvp("zoomFactor", viewer.zoomFactor);
        if (version < NATRON_SERIALIZATION_VIEWER_DATA_REMOVES_ASPECT_RATIO) {
            double zoomPixelAspectRatio = 1.;
            ar& ::boost::serialization::make_nvp("zoomPAR", zoomPixelAspectRatio);
        }
        ar& ::boost::serialization::make_nvp("UserRoIEnabled", viewer.userRoIEnabled);
        ar& ::boost::serialization::make_nvp("UserRoI", viewer.userRoI);
        ar& ::boost::serialization::make_nvp("ClippedToProject", viewer.isClippedToProject);
        ar& ::boost::serialization::make_nvp("AutoContrast", viewer.autoContrastEnabled);
        ar& ::boost::serialization::make_nvp("Gain", viewer.gain);
        if (version >= NATRON_SERIALIZATION_VIEWER_DATA_INTRODUCES_GAMMA) {
            ar& ::boost::serialization::make_nvp("Gain", viewer.gamma);
        } else {
            viewer.gamma = 1.;
        }
        if (version >= NATRON_SERIALIZATION_VIEWER_DATA_INTRODUCES_OCIO_DISPLAY) {
            ar& ::boost::serialization::make_nvp("Display", viewer.display);
            ar& ::boost::serialization::make_nvp("View", viewer.view);
            ar& ::boost::serialization::make_nvp("Look", viewer.look);
        } else {
            // The old viewer colourspace has no meaning under OCIO; the project's defaults apply.
            std::string colorSpace;
            ar& ::boost::serialization::make_nvp("ColorSpace", colorSpace);
            viewer.display.clear();
            viewer.view.clear();
            viewer.look.clear();
        }
        if (version >= NATRON_SERIALIZATION_VIEWER_DATA_INTRODUCES_LAYER) {
            ar& ::boost::serialization::make_nvp("Layer", viewer.layerName);
            ar& ::boost::serialization::make_nvp("AlphaLayer", viewer.alphaLayerName);
        }
        ar& ::boost::serialization::make_nvp("Channels", viewer.channels);
        ar& ::boost::serialization::make_nvp("RenderScaleActivated", viewer.renderScaleActivated);
        ar& ::boost::serialization::make_nvp("MipMapLevel", viewer.mipmapLevel);

        if (version >= NATRON_SERIALIZATION_VIEWER_DATA_INTRODUCES_WIPE_COMPOSITING) {
            ar& ::boost::serialization::make_nvp("ZoomOrPanSinceFit", viewer.zoomOrPanSinceLastFit);
            ar& ::boost::serialization::make_nvp("CompositingOP", viewer.wipeCompositingOperator);
        } else {
            viewer.zoomOrPanSinceLastFit = false;
            viewer.wipeCompositingOperator = 0;
        }
        if ((version >= NATRON_SERIALIZATION_VIEWER_DATA_INTRODUCES_FRAME_RANGE) && (version < NATRON_SERIALIZATION_VIEWER_DATA_REMOVES_FRAME_RANGE_LOCK)) {
            bool frameRangeLocked = false;
            ar& ::boost::serialization::make_nvp("FrameRangeLocked", frameRangeLocked);
        }
        if (version >= NATRON_SERIALIZATION_VIEWER_DATA_REMOVES_FRAME_RANGE_LOCK) {
            ar& ::boost::serialization::make_nvp("LeftBound", viewer.leftBound);
            ar& ::boost::serialization::make_nvp("RightBound", viewer.rightBound);
        } else {
            viewer.leftBound = viewer.rightBound = 1;
        }

        if (version >= NATRON_SERIALIZATION_VIEWER_DATA_INTRODUCES_TOOLBARS_VISIBLITY) {
            ar& ::boost::serialization::make_nvp("LeftToolbarVisible", viewer.leftToolbarVisible);
            ar& ::boost::serialization::make_nvp("RightToolbarVisible", viewer.rightToolbarVisible);
            ar& ::boost::serialization::make_nvp("TopToolbarVisible", viewer.topToolbarVisible);
            ar& ::boost::serialization::make_nvp("PlayerVisible", viewer.playerVisible);
            ar& ::boost::serialization::make_nvp("TimelineVisible", viewer.timelineVisible);
            ar& ::boost::serialization::make_nvp("InfobarVisible", viewer.infobarVisible);
        } else {
            viewer.leftToolbarVisible = true;
            viewer.rightToolbarVisible = true;
            viewer.topToolbarVisible = true;
            viewer.playerVisible = true;
            viewer.timelineVisible = true;
            viewer.infobarVisible = true;
        }

        if (version >= NATRON_SERIALIZATION_VIEWER_DATA_INTRODUCES_PAUSE_VIEWER) {
            ar& ::boost::serialization::make_nvp("isInputAPaused", viewer.isInputAPaused);
            ar& ::boost::serialization::make_nvp("isInputBPaused", viewer.isInputBPaused);
        } else {
            viewer.isInputAPaused = viewer.isInputBPaused = false;
        }

        if (version >= NATRON_SERIALIZATION_VIEWER_DATA_INTRODUCES_CHECKERBOARD) {
            ar& ::boost::serialization::make_nvp("CheckerboardEnabled", viewer.checkerboardEnabled);
        } else {
            viewer.checkerboardEnabled = false;
        }

        if (version >= NATRON_SERIALIZATION_VIEWER_DATA_INTRODUCES_FPS) {
            ar& ::boost::serialization::make_nvp("Fps", viewer.fps);
        } else {
            viewer.fps = 24.;
        }

        if (version >= NATRON_SERIALIZATION_VIEWER_DATA_INTRODUCES_FPS_LOCK) {
            ar& ::boost::serialization::make_nvp("FpsLocked", viewer.fpsLocked);
        } else {
            viewer.fpsLocked = true;
        }

        if (version >= NATRON_SERIALIZATION_VIEWER_DATA_INTRODUCES_ACTIVE_INPUTS) {
            ar& ::boost::serialization::make_nvp("aInput", viewer.activeInputA);
            ar& ::boost::serialization::make_nvp("bInput", viewer.activeInputB);
        } else {
            viewer.activeInputA = -1;
            viewer.activeInputB = -1;
        }

        if (version >= NATRON_SERIALIZATION_VIEWER_DATA_INTRODUCES_FULL_FRAME_PROC) {
            ar& ::boost::serialization::make_nvp("fullFrame", viewer.isFullFrameProcessEnabled);
        } else {
            viewer.isFullFrameProcessEnabled = false;
        }
    }

    template <class Archive>
    void
    serialize(Archive& ar,
              NATRON_NAMESPACE::Serialization::ViewerDataSerialization& viewer,
              const unsigned int version)
    {
        ::boost::serialization::split_free(ar, viewer, version);
    }

    template <class Archive>
    void
    save(Archive& ar,
         const NATRON_NAMESPACE::Serialization::PythonPanelSerialization& panel,
         const unsigned int /*version*/)
    {
        ar& ::boost::serialization::make_nvp("Label", panel.name);
        ar& ::boost::serialization::make_nvp("PythonFunction", panel.pythonFunction);
        int knobCount = (int)panel.knobs.size();
        ar& ::boost::serialization::make_nvp("NumParams", knobCount);
        for (std::list<NATRON_NAMESPACE::Serialization::KnobSerialization>::const_iterator it = panel.knobs.begin(); it != panel.knobs.end(); ++it) {
            ar& ::boost::serialization::make_nvp("item", *it);
        }
        ar& ::boost::serialization::make_nvp("UserData", panel.userData);
    }

    template <class Archive>
    void
    load(Archive& ar,
         NATRON_NAMESPACE::Serialization::PythonPanelSerialization& panel,
         const unsigned int /*version*/)
    {
        panel = NATRON_NAMESPACE::Serialization::PythonPanelSerialization();

        ar& ::boost::serialization::make_nvp("Label", panel.name);
        ar& ::boost::serialization::make_nvp("PythonFunction", panel.pythonFunction);
        int knobCount = 0;
        ar& ::boost::serialization::make_nvp("NumParams", knobCount);
        for (int i = 0; i < knobCount; ++i) {
            panel.knobs.push_back(NATRON_NAMESPACE::Serialization::KnobSerialization());
            ar& ::boost::serialization::make_nvp("item", panel.knobs.back());
        }
        ar& ::boost::serialization::make_nvp("UserData", panel.userData);
    }

    template <class Archive>
    void
    serialize(Archive& ar,
              NATRON_NAMESPACE::Serialization::PythonPanelSerialization& panel,
              const unsigned int version)
    {
        ::boost::serialization::split_free(ar, panel, version);
    }

    template <class Archive>
    void
    save(Archive& ar,
         const NATRON_NAMESPACE::Serialization::PaneLayoutSerialization& pane,
         const unsigned int /*version*/)
    {
        ar& ::boost::serialization::make_nvp("Tabs", pane.tabs);
        ar& ::boost::serialization::make_nvp("Index", pane.currentIndex);
        ar& ::boost::serialization::make_nvp("Name", pane.name);
        ar& ::boost::serialization::make_nvp("IsAnchor", pane.isAnchor);
    }

    template <class Archive>
    void
    load(Archive& ar,
         NATRON_NAMESPACE::Serialization::PaneLayoutSerialization& pane,
         const unsigned int version)
    {
        pane = NATRON_NAMESPACE::Serialization::PaneLayoutSerialization();

        if (version < NATRON_SERIALIZATION_PANE_MAJOR_OVERHAUL) {
            // Only the tabs and the current tab of these panes are kept; how they were split and
            // where they floated is dropped.
            bool floating = false;
            std::list<bool> splits;
            std::string parentName;
            std::list<std::string> splitsNames;
            ar& ::boost::serialization::make_nvp("Floating", floating);
            ar& ::boost::serialization::make_nvp("Splits", splits);
            ar& ::boost::serialization::make_nvp("ParentName", parentName);
            ar& ::boost::serialization::make_nvp("SplitsNames", splitsNames);
            ar& ::boost::serialization::make_nvp("Tabs", pane.tabs);
            if (version >= NATRON_SERIALIZATION_PANE_INTRODUCES_CURRENT_TAB) {
                ar& ::boost::serialization::make_nvp("Index", pane.currentIndex);
            }
            if (version >= NATRON_SERIALIZATION_PANE_INTRODUCES_SIZE) {
                if (floating) {
                    int positionX = 0;
                    int positionY = 0;
                    int width = 0;
                    int height = 0;
                    ar& ::boost::serialization::make_nvp("x", positionX);
                    ar& ::boost::serialization::make_nvp("y", positionY);
                    ar& ::boost::serialization::make_nvp("w", width);
                    ar& ::boost::serialization::make_nvp("h", height);
                }
            }
        } else {
            ar& ::boost::serialization::make_nvp("Tabs", pane.tabs);
            ar& ::boost::serialization::make_nvp("Index", pane.currentIndex);
            ar& ::boost::serialization::make_nvp("Name", pane.name);
            ar& ::boost::serialization::make_nvp("IsAnchor", pane.isAnchor);
        }
        if (version < NATRON_SERIALIZATION_PANE_INTRODUCES_SCRIPT_NAME) {
            for (std::list<std::string>::iterator it = pane.tabs.begin(); it != pane.tabs.end(); ++it) {
                if (*it == "CurveEditor") {
                    *it = NATRON_SERIALIZATION_CURVE_EDITOR_TAB_NAME;
                } else if (*it == "NodeGraph") {
                    *it = NATRON_SERIALIZATION_NODE_GRAPH_TAB_NAME;
                } else if (*it == "Properties") {
                    *it = "properties";
                }
            }
        }
    }

    template <class Archive>
    void
    serialize(Archive& ar,
              NATRON_NAMESPACE::Serialization::PaneLayoutSerialization& pane,
              const unsigned int version)
    {
        ::boost::serialization::split_free(ar, pane, version);
    }

    template <class Archive>
    void
    save(Archive& ar,
         const NATRON_NAMESPACE::Serialization::SplitterSerialization& splitter,
         const unsigned int /*version*/)
    {
        ar& ::boost::serialization::make_nvp("Sizes", splitter.sizes);
        ar& ::boost::serialization::make_nvp("Orientation", splitter.orientation);

        assert(splitter.children.size() == 2);
        for (std::vector<NATRON_NAMESPACE::Serialization::SplitterChildSerialization>::const_iterator it = splitter.children.begin(); it != splitter.children.end(); ++it) {
            bool isChildSplitter = (bool)it->splitter;
            ar& ::boost::serialization::make_nvp("ChildIsSplitter", isChildSplitter);
            if (isChildSplitter) {
                ar& ::boost::serialization::make_nvp("Child", *it->splitter);
            } else {
                if (!it->pane) {
                    throw std::invalid_argument("A splitter side holds neither a splitter nor a pane");
                }
                ar& ::boost::serialization::make_nvp("Child", *it->pane);
            }
        }
    }

    template <class Archive>
    void
    load(Archive& ar,
         NATRON_NAMESPACE::Serialization::SplitterSerialization& splitter,
         const unsigned int /*version*/)
    {
        splitter = NATRON_NAMESPACE::Serialization::SplitterSerialization();

        ar& ::boost::serialization::make_nvp("Sizes", splitter.sizes);
        ar& ::boost::serialization::make_nvp("Orientation", splitter.orientation);

        for (int i = 0; i < 2; ++i) {
            NATRON_NAMESPACE::Serialization::SplitterChildSerialization child;
            bool isChildSplitter = false;
            ar& ::boost::serialization::make_nvp("ChildIsSplitter", isChildSplitter);
            if (isChildSplitter) {
                child.splitter = std::make_shared<NATRON_NAMESPACE::Serialization::SplitterSerialization>();
                ar& ::boost::serialization::make_nvp("Child", *child.splitter);
            } else {
                child.pane = std::make_shared<NATRON_NAMESPACE::Serialization::PaneLayoutSerialization>();
                ar& ::boost::serialization::make_nvp("Child", *child.pane);
            }
            splitter.children.push_back(child);
        }
    }

    template <class Archive>
    void
    serialize(Archive& ar,
              NATRON_NAMESPACE::Serialization::SplitterSerialization& splitter,
              const unsigned int version)
    {
        ::boost::serialization::split_free(ar, splitter, version);
    }

    template <class Archive>
    void
    save(Archive& ar,
         const NATRON_NAMESPACE::Serialization::ApplicationWindowSerialization& window,
         const unsigned int /*version*/)
    {
        int childType;

        if (window.childSplitter) {
            childType = 0;
        } else if (window.childPane) {
            childType = 1;
        } else {
            childType = 2;
        }
        ar& ::boost::serialization::make_nvp("ChildType", childType);
        if (childType == 0) {
            NATRON_NAMESPACE::Serialization::SplitterSerialization* childSplitter = window.childSplitter.get();
            ar& ::boost::serialization::make_nvp("Child", childSplitter);
        } else if (childType == 1) {
            NATRON_NAMESPACE::Serialization::PaneLayoutSerialization* childPane = window.childPane.get();
            ar& ::boost::serialization::make_nvp("Child", childPane);
        } else {
            ar& ::boost::serialization::make_nvp("Child", window.childDockablePanel);
        }
        ar& ::boost::serialization::make_nvp("MainWindow", window.isMainWindow);
        ar& ::boost::serialization::make_nvp("x", window.positionX);
        ar& ::boost::serialization::make_nvp("y", window.positionY);
        ar& ::boost::serialization::make_nvp("w", window.width);
        ar& ::boost::serialization::make_nvp("h", window.height);
    }

    template <class Archive>
    void
    load(Archive& ar,
         NATRON_NAMESPACE::Serialization::ApplicationWindowSerialization& window,
         const unsigned int /*version*/)
    {
        window = NATRON_NAMESPACE::Serialization::ApplicationWindowSerialization();

        int childType = -1;
        ar& ::boost::serialization::make_nvp("ChildType", childType);
        if (childType == 0) {
            NATRON_NAMESPACE::Serialization::SplitterSerialization* childSplitter = nullptr;
            ar& ::boost::serialization::make_nvp("Child", childSplitter);
            window.childSplitter.reset(childSplitter);
        } else if (childType == 1) {
            NATRON_NAMESPACE::Serialization::PaneLayoutSerialization* childPane = nullptr;
            ar& ::boost::serialization::make_nvp("Child", childPane);
            window.childPane.reset(childPane);
        } else if (childType == 2) {
            ar& ::boost::serialization::make_nvp("Child", window.childDockablePanel);
        }
        ar& ::boost::serialization::make_nvp("MainWindow", window.isMainWindow);
        ar& ::boost::serialization::make_nvp("x", window.positionX);
        ar& ::boost::serialization::make_nvp("y", window.positionY);
        ar& ::boost::serialization::make_nvp("w", window.width);
        ar& ::boost::serialization::make_nvp("h", window.height);
    }

    template <class Archive>
    void
    serialize(Archive& ar,
              NATRON_NAMESPACE::Serialization::ApplicationWindowSerialization& window,
              const unsigned int version)
    {
        ::boost::serialization::split_free(ar, window, version);
    }

    template <class Archive>
    void
    save(Archive& ar,
         const NATRON_NAMESPACE::Serialization::GuiLayoutSerialization& layout,
         const unsigned int /*version*/)
    {
        int windowCount = (int)layout.windows.size();
        ar& ::boost::serialization::make_nvp("NbWindows", windowCount);
        for (std::list<NATRON_NAMESPACE::Serialization::ApplicationWindowSerialization>::const_iterator it = layout.windows.begin(); it != layout.windows.end(); ++it) {
            ar& ::boost::serialization::make_nvp("Window", *it);
        }
    }

    template <class Archive>
    void
    load(Archive& ar,
         NATRON_NAMESPACE::Serialization::GuiLayoutSerialization& layout,
         const unsigned int version)
    {
        layout = NATRON_NAMESPACE::Serialization::GuiLayoutSerialization();

        if (version < NATRON_SERIALIZATION_GUI_LAYOUT_MAJOR_OVERHAUL) {
            ar& ::boost::serialization::make_nvp("Gui_Layout", layout.legacyPanes);
            ar& ::boost::serialization::make_nvp("Splitters_states", layout.legacySplitterStates);
        } else {
            int windowCount = 0;
            ar& ::boost::serialization::make_nvp("NbWindows", windowCount);
            for (int i = 0; i < windowCount; ++i) {
                layout.windows.push_back(NATRON_NAMESPACE::Serialization::ApplicationWindowSerialization());
                ar& ::boost::serialization::make_nvp("Window", layout.windows.back());
            }
        }
    }

    template <class Archive>
    void
    serialize(Archive& ar,
              NATRON_NAMESPACE::Serialization::GuiLayoutSerialization& layout,
              const unsigned int version)
    {
        ::boost::serialization::split_free(ar, layout, version);
    }

    template <class Archive>
    void
    save(Archive& ar,
         const NATRON_NAMESPACE::Serialization::ProjectGuiSerialization& projectGui,
         const unsigned int /*version*/)
    {
        ar& ::boost::serialization::make_nvp("NodesGui", projectGui.nodes);
        ar& ::boost::serialization::make_nvp("Gui_Layout", projectGui.layout);
        ar& ::boost::serialization::make_nvp("ViewersData", projectGui.viewers);
        ar& ::boost::serialization::make_nvp("Histograms", projectGui.histograms);
        ar& ::boost::serialization::make_nvp("OpenedPanels", projectGui.openedPanels);
        ar& ::boost::serialization::make_nvp("ScriptEditorInput", projectGui.scriptEditorInput);
        int pythonPanelCount = (int)projectGui.pythonPanels.size();
        ar& ::boost::serialization::make_nvp("NumPyPanels", pythonPanelCount);
        for (std::list<NATRON_NAMESPACE::Serialization::PythonPanelSerialization>::const_iterator it = projectGui.pythonPanels.begin(); it != projectGui.pythonPanels.end(); ++it) {
            ar& ::boost::serialization::make_nvp("item", *it);
        }
        ar& ::boost::serialization::make_nvp("OpenedPanelsMinimized", projectGui.openedPanelsMinimized);
        ar& ::boost::serialization::make_nvp("OpenedPanelsHideUnmodified", projectGui.openedPanelsHideUnmodified);
    }

    template <class Archive>
    void
    load(Archive& ar,
         NATRON_NAMESPACE::Serialization::ProjectGuiSerialization& projectGui,
         const unsigned int version)
    {
        projectGui = NATRON_NAMESPACE::Serialization::ProjectGuiSerialization();
        projectGui.version = version;

        ar& ::boost::serialization::make_nvp("NodesGui", projectGui.nodes);
        if (version < NATRON_SERIALIZATION_PROJECT_GUI_EXTERNALISE_GUI_LAYOUT) {
            ar& ::boost::serialization::make_nvp("Gui_Layout", projectGui.layout.legacyPanes);
            ar& ::boost::serialization::make_nvp("Splitters_states", projectGui.layout.legacySplitterStates);
            if (version < NATRON_SERIALIZATION_PROJECT_GUI_CHANGES_SPLITTERS) {
                projectGui.layout.legacySplitterStates.clear();
            }
        } else {
            ar& ::boost::serialization::make_nvp("Gui_Layout", projectGui.layout);
        }

        ar& ::boost::serialization::make_nvp("ViewersData", projectGui.viewers);
        if (version < NATRON_SERIALIZATION_PROJECT_GUI_REMOVES_ALL_NODE_PREVIEW_TOGGLED) {
            bool previewsTurnedOffGlobally = false;
            ar& ::boost::serialization::make_nvp("PreviewsTurnedOffGlobaly", previewsTurnedOffGlobally);
        }
        ar& ::boost::serialization::make_nvp("Histograms", projectGui.histograms);
        if ((version >= NATRON_SERIALIZATION_PROJECT_GUI_INTRODUCES_BACKDROPS) && (version < NATRON_SERIALIZATION_PROJECT_GUI_MERGE_BACKDROP)) {
            ar& ::boost::serialization::make_nvp("Backdrops", projectGui.legacyBackdrops);
        }
        if (version >= NATRON_SERIALIZATION_PROJECT_GUI_INTRODUCES_PANELS) {
            ar& ::boost::serialization::make_nvp("OpenedPanels", projectGui.openedPanels);
        }
        if (version >= NATRON_SERIALIZATION_PROJECT_GUI_SCRIPT_EDITOR) {
            ar& ::boost::serialization::make_nvp("ScriptEditorInput", projectGui.scriptEditorInput);
        }

        if (version >= NATRON_SERIALIZATION_PROJECT_GUI_INTRODUCES_PYTHON_PANELS) {
            int pythonPanelCount = 0;
            ar& ::boost::serialization::make_nvp("NumPyPanels", pythonPanelCount);
            for (int i = 0; i < pythonPanelCount; ++i) {
                projectGui.pythonPanels.push_back(NATRON_NAMESPACE::Serialization::PythonPanelSerialization());
                ar& ::boost::serialization::make_nvp("item", projectGui.pythonPanels.back());
            }
        }

        if (version >= NATRON_SERIALIZATION_PROJECT_GUI_INTRODUCES_PANEL_STATES) {
            ar& ::boost::serialization::make_nvp("OpenedPanelsMinimized", projectGui.openedPanelsMinimized);
            ar& ::boost::serialization::make_nvp("OpenedPanelsHideUnmodified", projectGui.openedPanelsHideUnmodified);
        }
    }

    template <class Archive>
    void
    serialize(Archive& ar,
              NATRON_NAMESPACE::Serialization::ProjectGuiSerialization& projectGui,
              const unsigned int version)
    {
        ::boost::serialization::split_free(ar, projectGui, version);
    }

} // namespace serialization
} // namespace boost

BOOST_CLASS_VERSION(NATRON_NAMESPACE::Serialization::NodeGuiSerialization, NATRON_SERIALIZATION_NODE_GUI_VERSION)
BOOST_CLASS_VERSION(NATRON_NAMESPACE::Serialization::NodeBackdropSerialization, NATRON_SERIALIZATION_NODE_BACKDROP_VERSION)
BOOST_CLASS_VERSION(NATRON_NAMESPACE::Serialization::ViewerDataSerialization, NATRON_SERIALIZATION_VIEWER_DATA_VERSION)
BOOST_CLASS_VERSION(NATRON_NAMESPACE::Serialization::PythonPanelSerialization, NATRON_SERIALIZATION_PYTHON_PANEL_VERSION)
BOOST_CLASS_VERSION(NATRON_NAMESPACE::Serialization::PaneLayoutSerialization, NATRON_SERIALIZATION_PANE_VERSION)
BOOST_CLASS_VERSION(NATRON_NAMESPACE::Serialization::SplitterSerialization, NATRON_SERIALIZATION_SPLITTER_VERSION)
BOOST_CLASS_VERSION(NATRON_NAMESPACE::Serialization::ApplicationWindowSerialization, NATRON_SERIALIZATION_APPLICATION_WINDOW_VERSION)
BOOST_CLASS_VERSION(NATRON_NAMESPACE::Serialization::GuiLayoutSerialization, NATRON_SERIALIZATION_GUI_LAYOUT_VERSION)
BOOST_CLASS_VERSION(NATRON_NAMESPACE::Serialization::ProjectGuiSerialization, NATRON_SERIALIZATION_PROJECT_GUI_VERSION)

#endif // Serialization_BoostXml_ProjectGuiXml_h
