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

#ifndef Engine_Nodes_OCIO_OCIOColorSpace_h
#define Engine_Nodes_OCIO_OCIOColorSpace_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/OCIO/OcioNodeBase.h"

#define PLUGINID_NATRON_OCIOCOLORSPACE "OCIOColorSpace"
#define PLUGIN_MAJOR_NATRON_OCIOCOLORSPACE 1

#define kOCIOColorSpaceParamInputSpace "in_colorspace"
#define kOCIOColorSpaceParamOutputSpace "out_colorspace"
#define kOCIOColorSpaceParamPreset "preset"

// The preset menu's option ids.
#define kOCIOColorSpacePresetCustom "custom"
#define kOCIOColorSpacePresetLogToLin "log2lin"
#define kOCIOColorSpacePresetLinToLog "lin2log"

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kOCIOColorSpaceInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

/**
 * @brief OCIOColorSpace: converts RGB from the input to the output colourspace of the project's
 * config, with the Context variables. An empty space is the working space. The preset menu sets
 * both spaces to the compositing_log and scene_linear roles, in either direction, which is what
 * OCIOLogConvert did; changing a space by hand afterwards returns it to Custom.
 **/
struct OCIOColorSpaceOp
    : public OcioOp<OCIOColorSpaceOp> {
    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_OCIOCOLORSPACE,
        .label = "OCIOColorSpace",
        .description = "Convert the RGB channels from one OpenColorIO colourspace to another, using the project's "
                       "OpenColorIO config. The Log to Lin and Lin to Log presets convert between the "
                       "compositing_log and scene_linear roles. Alpha is left as it is.",
        .grouping = PLUGIN_GROUP_COLOR "/OCIO",
        .majorVersion = PLUGIN_MAJOR_NATRON_OCIOCOLORSPACE,
        .minorVersion = 0,
        .inputs = kOCIOColorSpaceInputs,
    };

    void createKnobs(KnobHolder* holder,
                     const KnobPagePtr& page);

    OCIO_NAMESPACE::ConstTransformRcPtr transform(const KnobReader& reader) const;

    /**
     * @brief Sets both spaces from the preset menu's current option; Custom leaves them.
     **/
    void applyPreset();

    /**
     * @brief Returns the preset menu to Custom when the spaces no longer are its preset's.
     **/
    void spacesChosen();

    KnobChoiceWPtr preset;
    KnobStringWPtr inputSpace;
    KnobStringWPtr outputSpace;
    // Set while applyPreset() writes the spaces, which must not turn the preset back to Custom.
    bool applyingPreset = false;
};

static_assert(PointOp<OCIOColorSpaceOp>);

class OCIOColorSpace
    : public OcioNode<OCIOColorSpaceOp> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new OCIOColorSpace(node);
    }

    explicit OCIOColorSpace(NodePtr node);

    virtual ~OCIOColorSpace();

private:
    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL WARN_UNUSED_RETURN;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_OCIO_OCIOColorSpace_h
