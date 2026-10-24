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

/*
 * The camera matrix handling of darktable's input colour module, DARKTABLE_REF release-5.6.2
 * (commit 20891f6c6fa7), src/iop/colorin.c and src/common/colorspaces.c, Copyright (C)
 * 2009-2026 darktable developers, distributed under the GNU GPL version 3 or later. Its ICC
 * path is not ported: the working space is the project's OpenColorIO one.
 */

#ifndef Engine_Nodes_Raw_RawInputColor_h
#define Engine_Nodes_Raw_RawInputColor_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <array>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Raw/RawColorMath.h"
#include "Engine/Nodes/Raw/RawNodeSupport.h"

#define PLUGINID_NATRON_RAWINPUTCOLOR "fr.natron.RawInputColor"
#define PLUGIN_MAJOR_NATRON_RAWINPUTCOLOR 1

#define kRawInputColorParamMatrix "matrix"
#define kRawInputColorParamRow0 "matrixRow0"
#define kRawInputColorParamRow1 "matrixRow1"
#define kRawInputColorParamRow2 "matrixRow2"

NATRON_NAMESPACE_ENTER

enum RawInputColorMatrixEnum {
    eRawInputColorMatrixFile = 0,
    eRawInputColorMatrixStandard,
    eRawInputColorMatrixCustom
};

inline constexpr InputSpec kRawInputColorInputs[] = {
    InputSpec { .label = "Source" },
};

/**
 * @brief Camera RGB to the project's working space, after RawDemosaic. The camera matrix,
 * raw/cam_to_xyz from the source's metadata unless overridden, is turned into the RGB space whose
 * primaries are its columns' chromaticities and whose white is D65, as darktable builds its input
 * profile from it; XYZ goes to the working space through the inverse of the project's
 * working-to-XYZ matrix, which the OpenColorIO config gives. The Standard matrix, and the
 * fallback when the source has no camera matrix, is linear Rec. 709. A working space with no
 * matrix to XYZ (a log space) is treated as linear Rec. 709. When the source carries
 * raw/wb_late_correction, camera RGB is multiplied by it before the matrix.
 **/
struct RawInputColorOp {
    struct Params {
        std::array<float, 9> matrix;
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_RAWINPUTCOLOR,
        .label = "RawInputColor",
        .description = "Converts demosaiced camera RGB to the project's working space through the camera's colour matrix. "
                       "Goes after RawDemosaic.",
        .grouping = PLUGIN_GROUP_RAW,
        .majorVersion = PLUGIN_MAJOR_NATRON_RAWINPUTCOLOR,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/" PLUGINID_NATRON_RAWINPUTCOLOR ".png",
        .inputs = kRawInputColorInputs,
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .stages = { Stage::PassThroughUnprocessed },
        .layers = LayerPolicy::color(true, true, true, false),
        .op = OpKind::Point,
    };

    void createKnobs(KnobHolder* holder,
                     const KnobPagePtr& page);

    Params bake(const KnobReader& reader,
                const ImageMetadata& source) const;

    bool isIdentity(const Params& params) const;

    void row(const Params& params,
             const RowIO& io) const;

    GpuOpRef gpu(const Params& params) const;

    /**
     * @brief The XYZ (D65) to working space matrix of the project of holder, linear Rec. 709's
     * when it has none.
     **/
    static RawColorMath::Mat3 xyzToWorking(const KnobHolder* holder);

    KnobHolder* holder = nullptr;
    KnobChoiceWPtr matrixMode;
    KnobDoubleWPtr rows[3];
};

static_assert(PointOp<RawInputColorOp>);

class RawInputColor
    : public ImageNode<RawInputColorOp> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new RawInputColor(node);
    }

    explicit RawInputColor(NodePtr node);

    virtual ~RawInputColor();

private:
    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

    virtual void onKnobsLoaded() OVERRIDE FINAL;

    virtual ImageMetadata deriveOutputMetadata(double time,
                                               ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    void refreshEnabled();
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Raw_RawInputColor_h
