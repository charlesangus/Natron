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

#define PLUGINID_NATRON_MODIFYMETADATA "fr.natron.ModifyMetadata"

#define kModifyMetadataSlotCount 8

NATRON_NAMESPACE_ENTER

/**
 * @brief Edits the per-frame metadata of its input: each of a fixed number of slots, applied in
 * order, sets, removes or renames one key. The image passes through untouched, as an
 * isIdentity() answer. The slots are knobs rather than table rows so that values animate and
 * take expressions.
 **/
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

    virtual NativePluginDescription getNativePluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

    virtual void onKnobsLoaded() OVERRIDE FINAL;

    virtual bool isIdentity(double time,
                            const RenderScale& scale,
                            const RectI& roi,
                            ViewIdx view,
                            double* inputTime,
                            ViewIdx* inputView,
                            int* inputNb) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual ImageMetadata deriveOutputMetadata(double time, ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    void refreshSlotVisibility(const Slot& slot);

    std::array<Slot, kModifyMetadataSlotCount> _slots;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Metadata_ModifyMetadata_h
