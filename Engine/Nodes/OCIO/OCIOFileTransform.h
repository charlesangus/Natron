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

#ifndef Engine_Nodes_OCIO_OCIOFileTransform_h
#define Engine_Nodes_OCIO_OCIOFileTransform_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/OCIO/OcioFileKnobs.h"
#include "Engine/Nodes/OCIO/OcioNodeBase.h"

#define PLUGINID_NATRON_OCIOFILETRANSFORM "OCIOFileTransform"
#define PLUGIN_MAJOR_NATRON_OCIOFILETRANSFORM 1

#define kOCIOFileTransformParamDirection "direction"
#define kOCIOFileTransformParamInterpolation "interpolation"

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kOCIOFileTransformInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

/**
 * @brief OCIOFileTransform: applies a transform OpenColorIO reads from a file (a LUT, a matrix,
 * a CDL, a CLF...) to RGB, forward or inverse, with the Context variables. No file is an
 * identity; a file that cannot be read is a persistent error, and the input passes through.
 **/
struct OCIOFileTransformOp
    : public OcioOp<OCIOFileTransformOp> {
    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_OCIOFILETRANSFORM,
        .label = "OCIOFileTransform",
        .description = "Apply a transform read from a file by OpenColorIO, such as a LUT, a matrix or a colour "
                       "correction, to the RGB channels. Alpha is left as it is.",
        .grouping = PLUGIN_GROUP_COLOR "/OCIO",
        .majorVersion = PLUGIN_MAJOR_NATRON_OCIOFILETRANSFORM,
        .minorVersion = 0,
        .inputs = kOCIOFileTransformInputs,
    };

    void createKnobs(KnobHolder* holder,
                     const KnobPagePtr& page);

    OCIO_NAMESPACE::ConstTransformRcPtr transform(const KnobReader& reader) const;

    OcioFileKnobs files;
    KnobChoiceWPtr direction;
    KnobChoiceWPtr interpolation;
};

static_assert(PointOp<OCIOFileTransformOp>);

class OCIOFileTransform
    : public OcioNode<OCIOFileTransformOp> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new OCIOFileTransform(node);
    }

    explicit OCIOFileTransform(NodePtr node);

    virtual ~OCIOFileTransform();

private:
    virtual void onKnobsLoaded() OVERRIDE FINAL;

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL WARN_UNUSED_RETURN;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_OCIO_OCIOFileTransform_h
