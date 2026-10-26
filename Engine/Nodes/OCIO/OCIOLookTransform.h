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

#ifndef Engine_Nodes_OCIO_OCIOLookTransform_h
#define Engine_Nodes_OCIO_OCIOLookTransform_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/OCIO/OcioNodeBase.h"

#define PLUGINID_NATRON_OCIOLOOKTRANSFORM "OCIOLookTransform"
#define PLUGIN_MAJOR_NATRON_OCIOLOOKTRANSFORM 1

#define kOCIOLookTransformParamInputSpace "in_colorspace"
#define kOCIOLookTransformParamOutputSpace "out_colorspace"
#define kOCIOLookTransformParamLook "look"
#define kOCIOLookTransformParamAppend "append"
#define kOCIOLookTransformParamSingleLook "single_look"
#define kOCIOLookTransformParamLookCombination "look_combination"
#define kOCIOLookTransformParamDirection "direction"

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kOCIOLookTransformInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

/**
 * @brief OCIOLookTransform: applies one look of the project's config, or a combination of them,
 * on the way from an input to an output colourspace, empty ones being the working space. The
 * inverse undoes the looks from the output back to the input, as OCIOLookTransform did.
 **/
struct OCIOLookTransformOp
    : public OcioOp<OCIOLookTransformOp> {
    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_OCIOLOOKTRANSFORM,
        .label = "OCIOLookTransform",
        .description = "Apply OpenColorIO looks of the project's config to the RGB channels, converting from the input "
                       "to the output colourspace. Alpha is left as it is.",
        .grouping = PLUGIN_GROUP_COLOR "/OCIO",
        .majorVersion = PLUGIN_MAJOR_NATRON_OCIOLOOKTRANSFORM,
        .minorVersion = 0,
        .inputs = kOCIOLookTransformInputs,
    };

    void createKnobs(KnobHolder* holder,
                     const KnobPagePtr& page);

    OCIO_NAMESPACE::ConstTransformRcPtr transform(const KnobReader& reader) const;

    /**
     * @brief Fills the look menu from the project's config.
     **/
    void refreshLookMenu();

    /**
     * @brief Adds the look the menu shows to the end of the combination.
     **/
    void appendLook();

    /**
     * @brief Enables the combination while single_look is off.
     **/
    void refreshSingleLook();

    KnobChoiceWPtr look;
    KnobButtonWPtr append;
    KnobBoolWPtr singleLook;
    KnobStringWPtr combination;
    KnobChoiceWPtr direction;
};

static_assert(PointOp<OCIOLookTransformOp>);

class OCIOLookTransform
    : public OcioNode<OCIOLookTransformOp> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new OCIOLookTransform(node);
    }

    explicit OCIOLookTransform(NodePtr node);

    virtual ~OCIOLookTransform();

private:
    virtual void onKnobsLoaded() OVERRIDE FINAL;

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL WARN_UNUSED_RETURN;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_OCIO_OCIOLookTransform_h
