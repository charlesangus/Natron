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

#ifndef Serialization_ProjectSerialization_h
#define Serialization_ProjectSerialization_h

#include "Global/Macros.h"

#include <list>
#include <string>

#include "Serialization/BasicSerialization.h"
#include "Serialization/KnobSerialization.h"
#include "Serialization/NodeSerialization.h"
#include "Serialization/SerializationFwd.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

/**
 * @brief One project layer-registry entry: the layer and where it came from, as the value of
 * Engine's LayerRegistryEntry::OriginEnum. Built-in layers are never saved.
 **/
struct LayerRegistryEntrySerialization {
    LayerSerialization layer;
    int origin;

    LayerRegistryEntrySerialization()
        : layer()
        , origin(0)
    {
    }
};

/**
 * @brief The Natron build that saved a project.
 **/
struct ProjectSavedByVersionSerialization {
    int versionMajor;
    int versionMinor;
    int versionRevision;
    std::string gitBranch;
    std::string gitCommit;
    std::string operatingSystem;
    int bits;

    ProjectSavedByVersionSerialization()
        : versionMajor(-1)
        , versionMinor(-1)
        , versionRevision(-1)
        , gitBranch()
        , gitCommit()
        , operatingSystem()
        , bits(-1)
    {
    }
};

/**
 * @brief The saved state of a project: the build that saved it, its nodes, its knobs, the
 * formats and layers the user added, the timeline position and when it was created.
 **/
struct ProjectSerialization {
    ProjectSavedByVersionSerialization savedByVersion;

    NodeCollectionSerialization nodes;

    // The persistent project knobs that differ from their defaults.
    std::list<KnobSerialization> projectKnobs;

    std::list<FormatSerialization> additionalFormats;
    std::list<LayerRegistryEntrySerialization> layers;
    int timelineCurrentTime;

    // Milliseconds since the epoch.
    long long creationDate;

    ProjectSerialization()
        : savedByVersion()
        , nodes()
        , projectKnobs()
        , additionalFormats()
        , layers()
        , timelineCurrentTime(0)
        , creationDate(0)
    {
    }
};

} // namespace Serialization
NATRON_NAMESPACE_EXIT

#endif // Serialization_ProjectSerialization_h
