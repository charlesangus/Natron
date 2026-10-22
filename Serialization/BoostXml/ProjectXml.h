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

#ifndef Serialization_BoostXml_ProjectXml_h
#define Serialization_BoostXml_ProjectXml_h

#include "Global/Macros.h"

#include <list>
#include <map>
#include <memory>
#include <string>

#include "Serialization/BasicSerialization.h"
#include "Serialization/BoostXml/BasicTypesXml.h"
#include "Serialization/BoostXml/KnobXml.h"
#include "Serialization/BoostXml/NodeXml.h"
#include "Serialization/KnobSerialization.h"
#include "Serialization/NodeSerialization.h"
#include "Serialization/ProjectSerialization.h"

#if !defined(Q_MOC_RUN) && !defined(SBK_RUN)
GCC_DIAG_UNUSED_LOCAL_TYPEDEFS_OFF
// clang-format off
GCC_DIAG_OFF(unused-parameter)
// /opt/local/include/boost/serialization/smart_cast.hpp:254:25: warning: unused parameter 'u' [-Wunused-parameter]
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

// Non-intrusive adapters reproducing the NVP sequence and class versions of the old intrusive
// ProjectSerialization and of its layer-registry entries. They write the current layout only.
namespace boost {
namespace serialization {

    template <class Archive>
    void
    save(Archive& ar,
         const NATRON_NAMESPACE::Serialization::LayerRegistryEntrySerialization& entry,
         const unsigned int /*version*/)
    {
        ar& ::boost::serialization::make_nvp("Desc", entry.layer);
        int origin = entry.origin;
        ar& ::boost::serialization::make_nvp("Origin", origin);
    }

    template <class Archive>
    void
    serialize(Archive& ar,
              NATRON_NAMESPACE::Serialization::LayerRegistryEntrySerialization& entry,
              const unsigned int version)
    {
        ::boost::serialization::split_free(ar, entry, version);
    }

    template <class Archive>
    void
    save(Archive& ar,
         const NATRON_NAMESPACE::Serialization::ProjectSerialization& project,
         const unsigned int /*version*/)
    {
        const NATRON_NAMESPACE::Serialization::ProjectSavedByVersionSerialization& savedBy = project.savedByVersion;

        ar& ::boost::serialization::make_nvp("VersionMajor", savedBy.versionMajor);
        ar& ::boost::serialization::make_nvp("VersionMinor", savedBy.versionMinor);
        ar& ::boost::serialization::make_nvp("VersionRev", savedBy.versionRevision);
        ar& ::boost::serialization::make_nvp("GitBranch", savedBy.gitBranch);
        ar& ::boost::serialization::make_nvp("GitCommit", savedBy.gitCommit);
        ar& ::boost::serialization::make_nvp("OS", savedBy.operatingSystem);
        ar& ::boost::serialization::make_nvp("Bits", savedBy.bits);

        ar& ::boost::serialization::make_nvp("NodesCollection", project.nodes);
        int knobCount = (int)project.projectKnobs.size();
        ar& ::boost::serialization::make_nvp("ProjectKnobsCount", knobCount);
        for (std::list<NATRON_NAMESPACE::Serialization::KnobSerialization>::const_iterator it = project.projectKnobs.begin(); it != project.projectKnobs.end(); ++it) {
            ar& ::boost::serialization::make_nvp("item", *it);
        }
        ar& ::boost::serialization::make_nvp("AdditionalFormats", project.additionalFormats);
        ar& ::boost::serialization::make_nvp("Layers", project.layers);
        ar& ::boost::serialization::make_nvp("Timeline_current_time", project.timelineCurrentTime);
        ar& ::boost::serialization::make_nvp("CreationDate", project.creationDate);
    }

    template <class Archive>
    void
    serialize(Archive& ar,
              NATRON_NAMESPACE::Serialization::ProjectSerialization& project,
              const unsigned int version)
    {
        ::boost::serialization::split_free(ar, project, version);
    }

} // namespace serialization
} // namespace boost

BOOST_CLASS_VERSION(NATRON_NAMESPACE::Serialization::ProjectSerialization, NATRON_SERIALIZATION_PROJECT_VERSION)

#endif // Serialization_BoostXml_ProjectXml_h
