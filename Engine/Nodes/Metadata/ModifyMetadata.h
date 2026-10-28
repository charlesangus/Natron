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

#ifndef Engine_Nodes_Metadata_ModifyMetadata_h
#define Engine_Nodes_Metadata_ModifyMetadata_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <array>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/NativeEffectBase.h"

#define PLUGINID_NATRON_MODIFYMETADATA "ModifyMetadata"

#define kModifyMetadataSlotCount 8

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kModifyMetadataInputs[] = {
    InputSpec { .label = "Source", .optional = false },
};

// The slots are ordinary knobs rather than table rows so that values animate and take expressions.
class ModifyMetadata
    : public NativeEffectBase {
public:
    enum OperationEnum {
        eOperationNone = 0,
        eOperationSet,
        eOperationRemove,
        eOperationRename
    };

    enum ValueTypeEnum {
        eValueTypeInt = 0,
        eValueTypeDouble,
        eValueTypeString
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new ModifyMetadata(node);
    }

    explicit ModifyMetadata(NodePtr node);

private:
    struct Slot {
        KnobChoiceWPtr operation;
        KnobStringWPtr key;
        KnobStringWPtr renameTo;
        KnobChoiceWPtr type;
        KnobIntWPtr intValue;
        KnobDoubleWPtr doubleValue;
        KnobStringWPtr stringValue;
    };

    virtual LayerKnobSpec getLayerKnobSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return LayerKnobSpec();
    }

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_MODIFYMETADATA,
        .label = "ModifyMetadata",
        .description = "Sets, removes or renames keys of the image's metadata. Each of the eight "
                       "operation slots has an Operation (None, Set, Remove or Rename), a Key with its "
                       "prefix, for example exr/owner, and, for Set, a Type (Integer, Float or String) "
                       "and a Value, which can be animated or driven by an expression. Rename moves the "
                       "value of Key to New Key, replacing any value already there. Slots are applied in "
                       "order, so a later slot sees the result of an earlier one. The image itself is "
                       "passed through unchanged.",
        .grouping = PLUGIN_GROUP_METADATA,
        .majorVersion = 1,
        .minorVersion = 0,
        .inputs = kModifyMetadataInputs,
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

    void refreshSlotVisibility(const Slot& slot);

    std::array<Slot, kModifyMetadataSlotCount> _slots;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Metadata_ModifyMetadata_h
