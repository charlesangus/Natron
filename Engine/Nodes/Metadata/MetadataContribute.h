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

#ifndef Engine_Nodes_Metadata_MetadataContribute_h
#define Engine_Nodes_Metadata_MetadataContribute_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/NativeEffectBase.h"

#define PLUGINID_NATRON_METADATACONTRIBUTE "MetadataContribute"
#define PLUGIN_MAJOR_NATRON_METADATACONTRIBUTE 1
#define kMetadataContributeKeyPrefix "MetadataContribute."

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kMetadataContributeInputs[] = {
    InputSpec { .label = "Source", .optional = false },
};

class MetadataContribute
    : public NativeEffectBase {
public:
    enum ModeEnum {
        eModeInheritAll = 0,
        eModeDropOneKey,
        eModeInheritNothing
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new MetadataContribute(node);
    }

    explicit MetadataContribute(NodePtr node);

private:
    virtual LayerKnobSpec getLayerKnobSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return LayerKnobSpec();
    }

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_METADATACONTRIBUTE,
        .label = "MetadataContribute",
        .description = "Adds a fixed set of demonstration keys to the metadata of Source, one of each "
                       "type: MetadataContribute.note (the Note text), .revision (1), "
                       ".quality (0.75), .tags (\"reviewed, approved\"), .renderRegion (0, 0, 1280, 720) "
                       "and .weights (1, 0.5, 0.25), and sets natron/framerate to 30. Mode picks what it "
                       "keeps of the metadata of Source: all of it, all but the Drop Key key, or nothing. "
                       "The image is passed through unchanged.",
        .grouping = PLUGIN_GROUP_METADATA,
        .majorVersion = PLUGIN_MAJOR_NATRON_METADATACONTRIBUTE,
        .minorVersion = 0,
        .inputs = kMetadataContributeInputs,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    virtual Route route(double time,
                        ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual ImageMetadata deriveOutputMetadata(double time, ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    KnobStringWPtr _note;
    KnobChoiceWPtr _mode;
    KnobStringWPtr _dropKey;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Metadata_MetadataContribute_h
