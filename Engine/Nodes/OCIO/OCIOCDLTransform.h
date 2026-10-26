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

#ifndef Engine_Nodes_OCIO_OCIOCDLTransform_h
#define Engine_Nodes_OCIO_OCIOCDLTransform_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/OCIO/OcioFileKnobs.h"
#include "Engine/Nodes/OCIO/OcioNodeBase.h"

#define PLUGINID_NATRON_OCIOCDLTRANSFORM "OCIOCDLTransform"
#define PLUGIN_MAJOR_NATRON_OCIOCDLTRANSFORM 1

#define kOCIOCDLTransformParamSlope "slope"
#define kOCIOCDLTransformParamOffset "offset"
#define kOCIOCDLTransformParamPower "power"
#define kOCIOCDLTransformParamSaturation "saturation"
#define kOCIOCDLTransformParamDirection "direction"
#define kOCIOCDLTransformParamReadFromFile "read_from_file"
#define kOCIOCDLTransformParamExport "export_cc"

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kOCIOCDLTransformInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

/**
 * @brief OCIOCDLTransform: applies an ASC CDL to RGB, forward or inverse: the slope, offset, power
 * and saturation knobs, or, with read_from_file, the CDL of a .cc, .ccc or .cdl file, whose values
 * the knobs then show. The default values are an identity. export_cc writes the CDL as a .cc.
 **/
struct OCIOCDLTransformOp
    : public OcioOp<OCIOCDLTransformOp> {
    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_OCIOCDLTRANSFORM,
        .label = "OCIOCDLTransform",
        .description = "Apply an ASC CDL (slope, offset, power and saturation) with OpenColorIO to the RGB channels, "
                       "from its knobs or from a .cc, .ccc or .cdl file. Alpha is left as it is.",
        .grouping = PLUGIN_GROUP_COLOR "/OCIO",
        .majorVersion = PLUGIN_MAJOR_NATRON_OCIOCDLTRANSFORM,
        .minorVersion = 0,
        .inputs = kOCIOCDLTransformInputs,
    };

    void createKnobs(KnobHolder* holder,
                     const KnobPagePtr& page);

    OCIO_NAMESPACE::ConstTransformRcPtr transform(const KnobReader& reader) const;

    /**
     * @brief The CDL of the knobs at \p time, without its direction.
     **/
    OCIO_NAMESPACE::CDLTransformRcPtr knobCDL(double time) const;

    /**
     * @brief Shows the file's values on the knobs, which it disables, while read_from_file is on;
     * enables them otherwise. A file that cannot be read leaves the values.
     **/
    void refreshFromFile();

    /**
     * @brief Writes the CDL of the knobs at \p time to \p path as a .cc file. Returns false and
     * fills \p error on failure.
     **/
    bool exportCC(const std::string& path,
                  double time,
                  std::string* error) const;

    OcioFileKnobs files;
    KnobColorWPtr slope;
    KnobColorWPtr offset;
    KnobColorWPtr power;
    KnobDoubleWPtr saturation;
    KnobChoiceWPtr direction;
    KnobBoolWPtr readFromFile;
    KnobOutputFileWPtr exportFile;
};

static_assert(PointOp<OCIOCDLTransformOp>);

class OCIOCDLTransform
    : public OcioNode<OCIOCDLTransformOp> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new OCIOCDLTransform(node);
    }

    explicit OCIOCDLTransform(NodePtr node);

    virtual ~OCIOCDLTransform();

private:
    virtual void onKnobsLoaded() OVERRIDE FINAL;

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL WARN_UNUSED_RETURN;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_OCIO_OCIOCDLTransform_h
