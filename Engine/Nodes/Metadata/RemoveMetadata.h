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

#ifndef Engine_Nodes_Metadata_RemoveMetadata_h
#define Engine_Nodes_Metadata_RemoveMetadata_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/NativeEffectBase.h"

#define PLUGINID_NATRON_REMOVEMETADATA "RemoveMetadata"

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kRemoveMetadataInputs[] = {
    InputSpec { .label = "Source", .optional = false },
};

// No key is protected: the ofx/ keys the engine writes go too when matched.
class RemoveMetadata
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new RemoveMetadata(node);
    }

    explicit RemoveMetadata(NodePtr node);

private:
    virtual LayerKnobSpec getLayerKnobSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return LayerKnobSpec();
    }

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_REMOVEMETADATA,
        .label = "RemoveMetadata",
        .description = "Removes the metadata keys matching the Keys list, or every key when Remove All "
                       "is on. Keys is a comma- or newline-separated list of globs, where * matches any "
                       "characters, including /, and ? matches one, for example exr/*, exif/Make. No key "
                       "is protected, so the engine's ofx/ keys go too if matched. The image itself is "
                       "passed through unchanged.",
        .grouping = PLUGIN_GROUP_METADATA,
        .majorVersion = 1,
        .minorVersion = 0,
        .inputs = kRemoveMetadataInputs,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    virtual Route route(double time,
                        ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

    virtual void onKnobsLoaded() OVERRIDE FINAL;

    virtual ImageMetadata deriveOutputMetadata(double time, ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    void refreshKeysVisibility();

    KnobBoolWPtr _removeAll;
    KnobStringWPtr _keys;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Metadata_RemoveMetadata_h
