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
#include <stdexcept>
#include <string>
#include <vector>

#include "Serialization/KnobSerialization.h"
#include "Serialization/NodeSerialization.h"
#include "Serialization/BoostXml/KnobXml.h"
#include "Serialization/BoostXml/RotoXml.h"
#include "Serialization/BoostXml/TrackerXml.h"

#if !defined(Q_MOC_RUN) && !defined(SBK_RUN)
GCC_DIAG_UNUSED_LOCAL_TYPEDEFS_OFF
// clang-format off
GCC_DIAG_OFF(unused-parameter)
GCC_DIAG_OFF(sign-compare)
// /opt/local/include/boost/serialization/smart_cast.hpp:254:25: warning: unused parameter 'u' [-Wunused-parameter]
#include <boost/archive/xml_iarchive.hpp>
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

// Mirrors PLUGINID_NATRON_GROUP in Engine/EffectInstance.h: files older than
// NATRON_SERIALIZATION_NODE_SERIALIZE_PYTHON_MODULE_ALWAYS only save the Python module of groups.
#define NATRON_SERIALIZATION_GROUP_PLUGIN_ID (NATRON_ORGANIZATION_DOMAIN_TOPLEVEL "." NATRON_ORGANIZATION_DOMAIN_SUB ".built-in.Group")

// Non-intrusive adapters reproducing the NVP sequence and class versions of the old intrusive
// ImageLayerDesc, NodeSerialization and NodeCollectionSerialization, including every branch
// that reads nodes saved by older versions.
namespace boost {
namespace serialization {

template<class Archive>
void
save(Archive & ar,
     const NATRON_NAMESPACE::Serialization::LayerSerialization & layer,
     const unsigned int /*version*/)
{
    ar & ::boost::serialization::make_nvp("LayerID", layer.layerID);
    ar & ::boost::serialization::make_nvp("LayerLabel", layer.layerLabel);
    ar & ::boost::serialization::make_nvp("ChannelsLabel", layer.channelsLabel);
    ar & ::boost::serialization::make_nvp("Channels", layer.channels);
}

template<class Archive>
void
load(Archive & ar,
     NATRON_NAMESPACE::Serialization::LayerSerialization & layer,
     const unsigned int /*version*/)
{
    layer = NATRON_NAMESPACE::Serialization::LayerSerialization();

    ar & ::boost::serialization::make_nvp("LayerID", layer.layerID);
    ar & ::boost::serialization::make_nvp("LayerLabel", layer.layerLabel);
    ar & ::boost::serialization::make_nvp("ChannelsLabel", layer.channelsLabel);
    ar & ::boost::serialization::make_nvp("Channels", layer.channels);
}

template<class Archive>
void
serialize(Archive & ar,
          NATRON_NAMESPACE::Serialization::LayerSerialization & layer,
          const unsigned int version)
{
    ::boost::serialization::split_free(ar, layer, version);
}

template<class Archive>
void
save(Archive & ar,
     const NATRON_NAMESPACE::Serialization::NodeSerialization & node,
     const unsigned int /*version*/)
{
    ar & ::boost::serialization::make_nvp("Plugin_label", node.label);
    ar & ::boost::serialization::make_nvp("Plugin_script_name", node.scriptName);
    ar & ::boost::serialization::make_nvp("Plugin_id", node.pluginID);
    ar & ::boost::serialization::make_nvp("PythonModule", node.pythonModule);
    ar & ::boost::serialization::make_nvp("PythonModuleVersion", node.pythonModuleVersion);
    ar & ::boost::serialization::make_nvp("Plugin_major_version", node.pluginMajorVersion);
    ar & ::boost::serialization::make_nvp("Plugin_minor_version", node.pluginMinorVersion);
    int knobCount = (int)node.knobs.size();
    ar & ::boost::serialization::make_nvp("KnobsCount", knobCount);
    for (std::list<NATRON_NAMESPACE::Serialization::KnobSerialization>::const_iterator it = node.knobs.begin(); it != node.knobs.end(); ++it) {
        ar & ::boost::serialization::make_nvp("item", *it);
    }
    ar & ::boost::serialization::make_nvp("Inputs_map", node.inputs);
    ar & ::boost::serialization::make_nvp("KnobsAge", node.knobsAge);
    ar & ::boost::serialization::make_nvp("MasterNode", node.masterNodeName);
    ar & ::boost::serialization::make_nvp("HasRotoContext", node.hasRotoContext);
    if (node.hasRotoContext) {
        ar & ::boost::serialization::make_nvp("RotoContext", node.rotoContext);
    }

    ar & ::boost::serialization::make_nvp("HasTrackerContext", node.hasTrackerContext);
    if (node.hasTrackerContext) {
        ar & ::boost::serialization::make_nvp("TrackerContext", node.trackerContext);
    }

    ar & ::boost::serialization::make_nvp("MultiInstanceParent", node.multiInstanceParentName);
    int userPageCount = (int)node.userPages.size();
    ar & ::boost::serialization::make_nvp("UserPagesCount", userPageCount);
    for (std::list<NATRON_NAMESPACE::Serialization::GroupKnobSerialization>::const_iterator it = node.userPages.begin(); it != node.userPages.end(); ++it) {
        ar & ::boost::serialization::make_nvp("item", *it);
    }

    int pageCount = (int)node.pagesOrder.size();
    ar & ::boost::serialization::make_nvp("PagesCount", pageCount);
    for (std::list<std::string>::const_iterator it = node.pagesOrder.begin(); it != node.pagesOrder.end(); ++it) {
        ar & ::boost::serialization::make_nvp("name", *it);
    }

    // Children are archived by value, never through their shared_ptr, so the format carries no
    // object tracking for them.
    int childCount = (int)node.children.size();
    ar & ::boost::serialization::make_nvp("Children", childCount);
    for (std::list<NATRON_NAMESPACE::Serialization::NodeSerializationPtr>::const_iterator it = node.children.begin(); it != node.children.end(); ++it) {
        ar & ::boost::serialization::make_nvp("item", **it);
    }

    ar & ::boost::serialization::make_nvp("CacheID", node.cacheID);
}

template<class Archive>
void
load(Archive & ar,
     NATRON_NAMESPACE::Serialization::NodeSerialization & node,
     const unsigned int version)
{
    if (version > NATRON_SERIALIZATION_NODE_VERSION) {
        throw std::invalid_argument("The project you're trying to load contains data produced by a more recent "
                                    "version of Natron, which makes it unreadable");
    }
    node = NATRON_NAMESPACE::Serialization::NodeSerialization();
    node.version = version;

    // Engine's makeNameScriptFriendly() performs the same conversion as scriptFriendlyKnobName():
    // files older than script names refer to nodes by label.
    const bool namesAreLabels = version < NATRON_SERIALIZATION_NODE_INTRODUCES_SCRIPT_NAME;

    ar & ::boost::serialization::make_nvp("Plugin_label", node.label);
    if (!namesAreLabels) {
        ar & ::boost::serialization::make_nvp("Plugin_script_name", node.scriptName);
    } else {
        node.scriptName = NATRON_NAMESPACE::Serialization::scriptFriendlyKnobName(node.label);
    }
    ar & ::boost::serialization::make_nvp("Plugin_id", node.pluginID);

    if (version >= NATRON_SERIALIZATION_NODE_INTRODUCES_PYTHON_MODULE) {
        if ( (version >= NATRON_SERIALIZATION_NODE_SERIALIZE_PYTHON_MODULE_ALWAYS) || (node.pluginID == NATRON_SERIALIZATION_GROUP_PLUGIN_ID) ) {
            ar & ::boost::serialization::make_nvp("PythonModule", node.pythonModule);
            if (version >= NATRON_SERIALIZATION_NODE_INTRODUCES_PYTHON_MODULE_VERSION) {
                ar & ::boost::serialization::make_nvp("PythonModuleVersion", node.pythonModuleVersion);
            } else {
                node.pythonModuleVersion = 1;
            }
        }
    }

    ar & ::boost::serialization::make_nvp("Plugin_major_version", node.pluginMajorVersion);
    ar & ::boost::serialization::make_nvp("Plugin_minor_version", node.pluginMinorVersion);
    int knobCount = 0;
    ar & ::boost::serialization::make_nvp("KnobsCount", knobCount);
    for (int i = 0; i < knobCount; ++i) {
        node.knobs.push_back( NATRON_NAMESPACE::Serialization::KnobSerialization() );
        ar & ::boost::serialization::make_nvp("item", node.knobs.back());
    }

    if (version < NATRON_SERIALIZATION_NODE_CHANGE_INPUTS_SERIALIZATION) {
        ar & ::boost::serialization::make_nvp("Inputs_map", node.oldInputs);
        if (namesAreLabels) {
            for (std::size_t i = 0; i < node.oldInputs.size(); ++i) {
                node.oldInputs[i] = NATRON_NAMESPACE::Serialization::scriptFriendlyKnobName(node.oldInputs[i]);
            }
        }
    } else {
        ar & ::boost::serialization::make_nvp("Inputs_map", node.inputs);
    }

    ar & ::boost::serialization::make_nvp("KnobsAge", node.knobsAge);
    ar & ::boost::serialization::make_nvp("MasterNode", node.masterNodeName);
    if (namesAreLabels) {
        node.masterNodeName = NATRON_NAMESPACE::Serialization::scriptFriendlyKnobName(node.masterNodeName);
    }

    if (version >= NATRON_SERIALIZATION_NODE_INTRODUCES_ROTO) {
        ar & ::boost::serialization::make_nvp("HasRotoContext", node.hasRotoContext);
        if (node.hasRotoContext) {
            ar & ::boost::serialization::make_nvp("RotoContext", node.rotoContext);
        }

        if (version >= NATRON_SERIALIZATION_NODE_INTRODUCES_TRACKER_CONTEXT) {
            ar & ::boost::serialization::make_nvp("HasTrackerContext", node.hasTrackerContext);
            if (node.hasTrackerContext) {
                ar & ::boost::serialization::make_nvp("TrackerContext", node.trackerContext);
            }
        }
    }
    if (version >= NATRON_SERIALIZATION_NODE_INTRODUCES_MULTI_INSTANCE) {
        ar & ::boost::serialization::make_nvp("MultiInstanceParent", node.multiInstanceParentName);
        if (namesAreLabels) {
            node.multiInstanceParentName = NATRON_NAMESPACE::Serialization::scriptFriendlyKnobName(node.multiInstanceParentName);
        }
    }

    if (version >= NATRON_SERIALIZATION_NODE_INTRODUCES_USER_KNOBS) {
        int userPageCount = 0;
        ar & ::boost::serialization::make_nvp("UserPagesCount", userPageCount);
        for (int i = 0; i < userPageCount; ++i) {
            node.userPages.push_back( NATRON_NAMESPACE::Serialization::GroupKnobSerialization() );
            ar & ::boost::serialization::make_nvp("item", node.userPages.back());
        }
        if (version >= NATRON_SERIALIZATION_NODE_SERIALIZE_PAGE_INDEX) {
            int pageCount = 0;
            ar & ::boost::serialization::make_nvp("PagesCount", pageCount);
            for (int i = 0; i < pageCount; ++i) {
                std::string pageName;
                ar & ::boost::serialization::make_nvp("name", pageName);
                node.pagesOrder.push_back(pageName);
            }
        }
    }

    if (version >= NATRON_SERIALIZATION_NODE_INTRODUCES_GROUPS) {
        int childCount = 0;
        ar & ::boost::serialization::make_nvp("Children", childCount);
        for (int i = 0; i < childCount; ++i) {
            NATRON_NAMESPACE::Serialization::NodeSerializationPtr child = std::make_shared<NATRON_NAMESPACE::Serialization::NodeSerialization>();
            ar & ::boost::serialization::make_nvp("item", *child);
            node.children.push_back(child);
        }
    }
    if ( (version >= NATRON_SERIALIZATION_NODE_INTRODUCES_USER_COMPONENTS) && (version < NATRON_SERIALIZATION_NODE_REMOVES_USER_COMPONENTS) ) {
        std::list<NATRON_NAMESPACE::Serialization::LayerSerialization> deprecatedUserComponents;
        ar & ::boost::serialization::make_nvp("UserComponents", deprecatedUserComponents);
    }
    if (version >= NATRON_SERIALIZATION_NODE_INTRODUCES_CACHE_ID) {
        ar & ::boost::serialization::make_nvp("CacheID", node.cacheID);
    }
}

template<class Archive>
void
serialize(Archive & ar,
          NATRON_NAMESPACE::Serialization::NodeSerialization & node,
          const unsigned int version)
{
    ::boost::serialization::split_free(ar, node, version);
}

template<class Archive>
void
save(Archive & ar,
     const NATRON_NAMESPACE::Serialization::NodeCollectionSerialization & collection,
     const unsigned int /*version*/)
{
    int nodeCount = (int)collection.nodes.size();
    ar & ::boost::serialization::make_nvp("NodesCount", nodeCount);
    for (std::list<NATRON_NAMESPACE::Serialization::NodeSerializationPtr>::const_iterator it = collection.nodes.begin(); it != collection.nodes.end(); ++it) {
        ar & ::boost::serialization::make_nvp("item", **it);
    }
}

template<class Archive>
void
load(Archive & ar,
     NATRON_NAMESPACE::Serialization::NodeCollectionSerialization & collection,
     const unsigned int /*version*/)
{
    collection = NATRON_NAMESPACE::Serialization::NodeCollectionSerialization();

    int nodeCount = 0;
    ar & ::boost::serialization::make_nvp("NodesCount", nodeCount);
    for (int i = 0; i < nodeCount; ++i) {
        NATRON_NAMESPACE::Serialization::NodeSerializationPtr node = std::make_shared<NATRON_NAMESPACE::Serialization::NodeSerialization>();
        ar & ::boost::serialization::make_nvp("item", *node);
        collection.nodes.push_back(node);
    }
}

template<class Archive>
void
serialize(Archive & ar,
          NATRON_NAMESPACE::Serialization::NodeCollectionSerialization & collection,
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
