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

#ifndef Serialization_BoostXml_TrackerXml_h
#define Serialization_BoostXml_TrackerXml_h

#include "Global/Macros.h"

#include <list>
#include <string>

#include "Serialization/BoostXml/KnobXml.h"
#include "Serialization/TrackerSerialization.h"

#if !defined(Q_MOC_RUN) && !defined(SBK_RUN)
GCC_DIAG_UNUSED_LOCAL_TYPEDEFS_OFF
// clang-format off
GCC_DIAG_OFF(unused-parameter)
// /opt/local/include/boost/serialization/smart_cast.hpp:254:25: warning: unused parameter 'u' [-Wunused-parameter]
#include <boost/archive/xml_oarchive.hpp>
#include <boost/serialization/nvp.hpp>
#include <boost/serialization/split_free.hpp>
#include <boost/serialization/string.hpp>
#include <boost/serialization/version.hpp>
GCC_DIAG_UNUSED_LOCAL_TYPEDEFS_ON
GCC_DIAG_ON(unused-parameter)
// clang-format on
#endif

#define NATRON_SERIALIZATION_TRACK_ADD_TRACKER_PM 2
#define NATRON_SERIALIZATION_TRACK_VERSION NATRON_SERIALIZATION_TRACK_ADD_TRACKER_PM

#define NATRON_SERIALIZATION_TRACKER_CONTEXT_VERSION 1

// Non-intrusive adapters reproducing the NVP sequence and class versions of the old intrusive
// TrackSerialization and TrackerContextSerialization. They write the current layout only.
namespace boost {
namespace serialization {

    template <class Archive>
    void
    save(Archive& ar,
         const NATRON_NAMESPACE::Serialization::TrackSerialization& track,
         const unsigned int /*version*/)
    {
        ar& ::boost::serialization::make_nvp("IsTrackerPM", track.isPatternMatching);
        ar& ::boost::serialization::make_nvp("Enabled", track.enabled);
        ar& ::boost::serialization::make_nvp("ScriptName", track.scriptName);
        ar& ::boost::serialization::make_nvp("Label", track.label);
        int knobCount = (int)track.knobs.size();
        ar& ::boost::serialization::make_nvp("NbItems", knobCount);
        for (std::list<NATRON_NAMESPACE::Serialization::KnobSerialization>::const_iterator it = track.knobs.begin(); it != track.knobs.end(); ++it) {
            ar& ::boost::serialization::make_nvp("Item", *it);
        }

        int userKeyCount = (int)track.userKeys.size();
        ar& ::boost::serialization::make_nvp("NbUserKeys", userKeyCount);
        for (std::list<int>::const_iterator it = track.userKeys.begin(); it != track.userKeys.end(); ++it) {
            ar& ::boost::serialization::make_nvp("UserKeys", *it);
        }
    }

    template <class Archive>
    void
    serialize(Archive& ar,
              NATRON_NAMESPACE::Serialization::TrackSerialization& track,
              const unsigned int version)
    {
        ::boost::serialization::split_free(ar, track, version);
    }

    template <class Archive>
    void
    save(Archive& ar,
         const NATRON_NAMESPACE::Serialization::TrackerContextSerialization& context,
         const unsigned int /*version*/)
    {
        int trackCount = (int)context.tracks.size();
        ar& ::boost::serialization::make_nvp("NbItems", trackCount);
        for (std::list<NATRON_NAMESPACE::Serialization::TrackSerialization>::const_iterator it = context.tracks.begin(); it != context.tracks.end(); ++it) {
            ar& ::boost::serialization::make_nvp("Item", *it);
        }
    }

    template <class Archive>
    void
    serialize(Archive& ar,
              NATRON_NAMESPACE::Serialization::TrackerContextSerialization& context,
              const unsigned int version)
    {
        ::boost::serialization::split_free(ar, context, version);
    }

} // namespace serialization
} // namespace boost

BOOST_CLASS_VERSION(NATRON_NAMESPACE::Serialization::TrackSerialization, NATRON_SERIALIZATION_TRACK_VERSION)
BOOST_CLASS_VERSION(NATRON_NAMESPACE::Serialization::TrackerContextSerialization, NATRON_SERIALIZATION_TRACKER_CONTEXT_VERSION)

#endif // Serialization_BoostXml_TrackerXml_h
