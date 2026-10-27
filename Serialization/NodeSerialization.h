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

#ifndef Serialization_NodeSerialization_h
#define Serialization_NodeSerialization_h

#include "Global/Macros.h"

#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "Serialization/KnobSerialization.h"
#include "Serialization/RotoSerialization.h"
#include "Serialization/SerializationFwd.h"
#include "Serialization/TrackerSerialization.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

/**
 * @brief Plain-data equivalent of an image layer description: its ID, its label, the label of
 * its channel set and its channel names.
 **/
struct LayerSerialization {
    std::string layerID;
    std::string layerLabel;
    std::string channelsLabel;
    std::vector<std::string> channels;
};

/**
 * @brief Where a node sits in the node graph and how it is coloured. Each part is saved only
 * when its flag is set. color is the user colour, or a backdrop's body colour; it is never the
 * category colour.
 **/
struct NodeLayoutSerialization {
    bool hasPosition;
    double x;
    double y;

    bool hasSize;
    double width;
    double height;

    bool hasColor;
    float colorR;
    float colorG;
    float colorB;

    bool hasOverlayColor;
    float overlayR;
    float overlayG;
    float overlayB;

    NodeLayoutSerialization()
        : hasPosition(false)
        , x(0.)
        , y(0.)
        , hasSize(false)
        , width(0.)
        , height(0.)
        , hasColor(false)
        , colorR(0.f)
        , colorG(0.f)
        , colorB(0.f)
        , hasOverlayColor(false)
        , overlayR(0.f)
        , overlayG(0.f)
        , overlayB(0.f)
    {
    }
};

/**
 * @brief The saved state of one node: its plug-in, names and knobs, its input connections, its
 * roto and tracker data, its user knob layout and, for a group or a multi-instance node, the
 * nodes it holds. Node names are script names.
 **/
struct NodeSerialization {
    std::string label;
    std::string scriptName;
    std::string pluginID;
    int pluginMajorVersion;
    int pluginMinorVersion;

    // Empty unless the node is an unedited PyPlug instance, see Node::hasPyPlugBeenEdited().
    std::string pythonModule;
    unsigned int pythonModuleVersion;

    NodeLayoutSerialization layout;

    // The knobs that differ from their defaults, or carry an expression or a link.
    std::list<KnobSerialization> knobs;

    // Input label to the script name of the node connected there.
    std::map<std::string, std::string> inputs;

    // Still read and written so the file layout is stable, but ignored on load and saved as 0.
    std::uint64_t knobsAge;

    bool hasRotoContext;
    RotoContextSerialization rotoContext;
    bool hasTrackerContext;
    TrackerContextSerialization trackerContext;

    std::string multiInstanceParentName;
    std::list<GroupKnobSerialization> userPages;

    // The script names of every page of the node, in display order.
    std::list<std::string> pagesOrder;

    // The nodes of a group, or the instances of a multi-instance node.
    std::list<std::shared_ptr<NodeSerialization>> children;

    std::string cacheID;

    NodeSerialization()
        : label()
        , scriptName()
        , pluginID()
        , pluginMajorVersion(-1)
        , pluginMinorVersion(-1)
        , pythonModule()
        , pythonModuleVersion(0)
        , layout()
        , knobs()
        , inputs()
        , knobsAge(0)
        , hasRotoContext(false)
        , rotoContext()
        , hasTrackerContext(false)
        , trackerContext()
        , multiInstanceParentName()
        , userPages()
        , pagesOrder()
        , children()
        , cacheID()
    {
    }
};

/**
 * @brief The saved state of the top-level nodes of a project, in graph order.
 **/
struct NodeCollectionSerialization {
    std::list<NodeSerializationPtr> nodes;
};

} // namespace Serialization
NATRON_NAMESPACE_EXIT

#endif // Serialization_NodeSerialization_h
