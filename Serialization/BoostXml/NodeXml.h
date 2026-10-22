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

#ifndef Serialization_BoostXml_NodeXml_h
#define Serialization_BoostXml_NodeXml_h

#include "Global/Macros.h"

#include <list>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "Serialization/BoostXml/KnobXml.h"
#include "Serialization/BoostXml/RotoXml.h"
#include "Serialization/BoostXml/TrackerXml.h"
#include "Serialization/KnobSerialization.h"
#include "Serialization/NodeSerialization.h"

#if !defined(Q_MOC_RUN) && !defined(SBK_RUN)
GCC_DIAG_UNUSED_LOCAL_TYPEDEFS_OFF
// clang-format off
GCC_DIAG_OFF(unused-parameter)
GCC_DIAG_OFF(sign-compare)
// /opt/local/include/boost/serialization/smart_cast.hpp:254:25: warning: unused parameter 'u' [-Wunused-parameter]
#include <boost/archive/xml_oarchive.hpp>
#include <boost/serialization/list.hpp>
#include <boost/serialization/map.hpp>
#include <boost/serialization/nvp.hpp>
#include <boost/serialization/split_free.hpp>
#include <boost/serialization/string.hpp>
#include <boost/serialization/utility.hpp>
#include <boost/serialization/vector.hpp>
#include <boost/serialization/version.hpp>
GCC_DIAG_UNUSED_LOCAL_TYPEDEFS_ON
GCC_DIAG_ON(unused-parameter)
GCC_DIAG_ON(sign-compare)
// clang-format on
#endif

// Mirrors IMAGELAYERDESC_SERIALIZATION_VERSION in Engine/ImageLayerDesc.h.
#define NATRON_SERIALIZATION_LAYER_VERSION 3

// Non-intrusive adapters reproducing the NVP sequence and class versions of the old intrusive
// ImageLayerDesc, NodeSerialization and NodeCollectionSerialization. They write the current
// layout only.
namespace boost {
namespace serialization {

    template <class Archive>
    void
    save(Archive& ar,
         const NATRON_NAMESPACE::Serialization::LayerSerialization& layer,
         const unsigned int /*version*/)
    {
        ar& ::boost::serialization::make_nvp("LayerID", layer.layerID);
        ar& ::boost::serialization::make_nvp("LayerLabel", layer.layerLabel);
        ar& ::boost::serialization::make_nvp("ChannelsLabel", layer.channelsLabel);
        ar& ::boost::serialization::make_nvp("Channels", layer.channels);
    }

    template <class Archive>
    void
    serialize(Archive& ar,
              NATRON_NAMESPACE::Serialization::LayerSerialization& layer,
              const unsigned int version)
    {
        ::boost::serialization::split_free(ar, layer, version);
    }

    template <class Archive>
    void
    save(Archive& ar,
         const NATRON_NAMESPACE::Serialization::NodeSerialization& node,
         const unsigned int /*version*/)
    {
        ar& ::boost::serialization::make_nvp("Plugin_label", node.label);
        ar& ::boost::serialization::make_nvp("Plugin_script_name", node.scriptName);
        ar& ::boost::serialization::make_nvp("Plugin_id", node.pluginID);
        ar& ::boost::serialization::make_nvp("PythonModule", node.pythonModule);
        ar& ::boost::serialization::make_nvp("PythonModuleVersion", node.pythonModuleVersion);
        ar& ::boost::serialization::make_nvp("Plugin_major_version", node.pluginMajorVersion);
        ar& ::boost::serialization::make_nvp("Plugin_minor_version", node.pluginMinorVersion);
        int knobCount = (int)node.knobs.size();
        ar& ::boost::serialization::make_nvp("KnobsCount", knobCount);
        for (std::list<NATRON_NAMESPACE::Serialization::KnobSerialization>::const_iterator it = node.knobs.begin(); it != node.knobs.end(); ++it) {
            ar& ::boost::serialization::make_nvp("item", *it);
        }
        ar& ::boost::serialization::make_nvp("Inputs_map", node.inputs);
        ar& ::boost::serialization::make_nvp("KnobsAge", node.knobsAge);
        ar& ::boost::serialization::make_nvp("MasterNode", node.masterNodeName);
        ar& ::boost::serialization::make_nvp("HasRotoContext", node.hasRotoContext);
        if (node.hasRotoContext) {
            ar& ::boost::serialization::make_nvp("RotoContext", node.rotoContext);
        }

        ar& ::boost::serialization::make_nvp("HasTrackerContext", node.hasTrackerContext);
        if (node.hasTrackerContext) {
            ar& ::boost::serialization::make_nvp("TrackerContext", node.trackerContext);
        }

        ar& ::boost::serialization::make_nvp("MultiInstanceParent", node.multiInstanceParentName);
        int userPageCount = (int)node.userPages.size();
        ar& ::boost::serialization::make_nvp("UserPagesCount", userPageCount);
        for (std::list<NATRON_NAMESPACE::Serialization::GroupKnobSerialization>::const_iterator it = node.userPages.begin(); it != node.userPages.end(); ++it) {
            ar& ::boost::serialization::make_nvp("item", *it);
        }

        int pageCount = (int)node.pagesOrder.size();
        ar& ::boost::serialization::make_nvp("PagesCount", pageCount);
        for (std::list<std::string>::const_iterator it = node.pagesOrder.begin(); it != node.pagesOrder.end(); ++it) {
            ar& ::boost::serialization::make_nvp("name", *it);
        }

        // Children are archived by value, never through their shared_ptr, so the format carries no
        // object tracking for them.
        int childCount = (int)node.children.size();
        ar& ::boost::serialization::make_nvp("Children", childCount);
        for (std::list<NATRON_NAMESPACE::Serialization::NodeSerializationPtr>::const_iterator it = node.children.begin(); it != node.children.end(); ++it) {
            ar& ::boost::serialization::make_nvp("item", **it);
        }

        ar& ::boost::serialization::make_nvp("CacheID", node.cacheID);
    }

    template <class Archive>
    void
    serialize(Archive& ar,
              NATRON_NAMESPACE::Serialization::NodeSerialization& node,
              const unsigned int version)
    {
        ::boost::serialization::split_free(ar, node, version);
    }

    template <class Archive>
    void
    save(Archive& ar,
         const NATRON_NAMESPACE::Serialization::NodeCollectionSerialization& collection,
         const unsigned int /*version*/)
    {
        int nodeCount = (int)collection.nodes.size();
        ar& ::boost::serialization::make_nvp("NodesCount", nodeCount);
        for (std::list<NATRON_NAMESPACE::Serialization::NodeSerializationPtr>::const_iterator it = collection.nodes.begin(); it != collection.nodes.end(); ++it) {
            ar& ::boost::serialization::make_nvp("item", **it);
        }
    }

    template <class Archive>
    void
    serialize(Archive& ar,
              NATRON_NAMESPACE::Serialization::NodeCollectionSerialization& collection,
              const unsigned int version)
    {
        ::boost::serialization::split_free(ar, collection, version);
    }

} // namespace serialization
} // namespace boost

BOOST_CLASS_VERSION(NATRON_NAMESPACE::Serialization::LayerSerialization, NATRON_SERIALIZATION_LAYER_VERSION)
BOOST_CLASS_VERSION(NATRON_NAMESPACE::Serialization::NodeSerialization, NATRON_SERIALIZATION_NODE_VERSION)
BOOST_CLASS_VERSION(NATRON_NAMESPACE::Serialization::NodeCollectionSerialization, NATRON_SERIALIZATION_NODE_COLLECTION_VERSION)

#endif // Serialization_BoostXml_NodeXml_h
