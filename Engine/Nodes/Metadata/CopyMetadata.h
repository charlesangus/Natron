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

#ifndef Engine_Nodes_Metadata_CopyMetadata_h
#define Engine_Nodes_Metadata_CopyMetadata_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/NativeEffectBase.h"

#define PLUGINID_NATRON_COPYMETADATA "fr.natron.CopyMetadata"

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kCopyMetadataInputs[] = {
    InputSpec { .label = "A", .optional = false },
    InputSpec { .label = "B", .optional = true },
};

class CopyMetadata
    : public NativeEffectBase {
public:
    enum ModeEnum {
        eModeReplace = 0,
        eModeMergeBWins,
        eModeMergeAWins
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new CopyMetadata(node);
    }

    explicit CopyMetadata(NodePtr node);

private:
    virtual LayerKnobSpec getLayerKnobSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return LayerKnobSpec();
    }

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_COPYMETADATA,
        .label = "CopyMetadata",
        .description = "Copies the metadata of B onto the metadata of A, restricted to the keys matching "
                       "the Keys filter, a comma- or newline-separated list of globs where * matches any "
                       "characters and ? one (every key if it is empty). The image of A is passed through "
                       "unchanged, and so is its metadata while B is not connected.\n\n"
                       "Replace: the keys of A matching the filter are dropped and replaced by those "
                       "of B matching the filter; the keys of A that do not match survive, so a key "
                       "of A matching the filter that B lacks is removed.\n"
                       "Merge, B wins: the keys of B matching the filter are added to A's, B's value "
                       "kept on a clash.\n"
                       "Merge, A wins: likewise, but A's value is kept on a clash.",
        .grouping = PLUGIN_GROUP_METADATA,
        .majorVersion = 1,
        .minorVersion = 0,
        .inputs = kCopyMetadataInputs,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    virtual Route route(double time,
                        ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual ImageMetadata deriveOutputMetadata(double time, ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    KnobChoiceWPtr _mode;
    KnobStringWPtr _keys;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Metadata_CopyMetadata_h
