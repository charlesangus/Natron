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
 * 2009-2026 darktable developers, distributed under the GNU GPL version 3 or later: its matrix
 * input profiles, their fallbacks (commit_params()) and the file-derived default
 * (reload_defaults()). Its ICC path is not ported: the working space is the project's OpenColorIO
 * one.
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
#include <optional>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Raw/RawColorMath.h"
#include "Engine/Nodes/Raw/RawNodeSupport.h"

#define PLUGINID_NATRON_RAWINPUTCOLOR "fr.natron.RawInputColor"
#define PLUGIN_MAJOR_NATRON_RAWINPUTCOLOR 1

#define kRawInputColorParamProfileOverride "inputProfileOverride"
#define kRawInputColorParamProfile "inputProfile"
#define kRawInputColorParamRow0 "matrixRow0"
#define kRawInputColorParamRow1 "matrixRow1"
#define kRawInputColorParamRow2 "matrixRow2"

// The option ids of the Input Profile choice. A project saves the id, so options added to the
// list later do not move a saved choice.
#define kRawInputColorProfileEmbeddedMatrix "embeddedMatrix"
#define kRawInputColorProfileStandardMatrix "standardMatrix"
#define kRawInputColorProfileEnhancedMatrix "enhancedMatrix"
#define kRawInputColorProfileVendorMatrix "vendorMatrix"
#define kRawInputColorProfileAlternateMatrix "alternateMatrix"
#define kRawInputColorProfileLinearRec709 "linearRec709"
#define kRawInputColorProfileCustom "custom"

NATRON_NAMESPACE_ENTER

/**
 * @brief The input profiles, darktable's dt_colorspaces_color_profile_type_t ones this node
 * offers plus Natron's Custom. Not the choice's indices: rawInputColorProfileAt() maps those.
 **/
enum RawInputColorProfileEnum {
    eRawInputColorProfileEmbeddedMatrix = 0,
    eRawInputColorProfileStandardMatrix,
    eRawInputColorProfileEnhancedMatrix,
    eRawInputColorProfileVendorMatrix,
    eRawInputColorProfileAlternateMatrix,
    eRawInputColorProfileLinearRec709,
    eRawInputColorProfileCustom
};

/**
 * @brief The profile at index in the Input Profile choice, Linear Rec709 for an index out of range.
 **/
RawInputColorProfileEnum rawInputColorProfileAt(int index);

/**
 * @brief The index of profile in the Input Profile choice.
 **/
int rawInputColorProfileIndex(RawInputColorProfileEnum profile);

/**
 * @brief darktable's file-derived default (colorin reload_defaults()): the embedded matrix when
 * the source carries raw/dng_d65_matrix, the standard matrix when it carries raw/cam_to_xyz,
 * linear Rec709 otherwise.
 **/
RawInputColorProfileEnum rawInputColorDefaultProfile(const ImageMetadata& source);

/**
 * @brief The camera RGB to XYZ (D65) matrix of a matrix profile for source, nullopt when source
 * does not have it: no embedded or standard matrix, or no entry for its camera in darktable's
 * enhanced, vendor or alternate table. Custom has none here.
 **/
std::optional<RawColorMath::Mat3> rawInputColorProfileMatrix(RawInputColorProfileEnum profile,
                                                             const ImageMetadata& source);

/**
 * @brief The profile used for requested, after darktable's fallbacks (colorin commit_params()):
 * enhanced, vendor and alternate fall back to embedded, embedded to standard, standard to linear
 * Rec709. Sets camToXyz to its matrix. Custom is returned as is, camToXyz untouched.
 **/
RawInputColorProfileEnum rawInputColorResolveProfile(RawInputColorProfileEnum requested,
                                                     const ImageMetadata& source,
                                                     RawColorMath::Mat3* camToXyz);

inline constexpr InputSpec kRawInputColorInputs[] = {
    InputSpec { .label = "Source" },
};

/**
 * @brief Camera RGB to the project's working space, after RawDemosaic, through the input
 * profile's camera RGB to XYZ (D65) matrix. The file's matrices (raw/dng_d65_matrix and
 * raw/cam_to_xyz) become the RGB space whose primaries are their columns' chromaticities and
 * whose white is D65, as darktable builds its input profile from them; darktable's enhanced,
 * vendor and alternate tables give primaries and a white, adapted to D65 with Bradford. Unless
 * overridden the profile is the file-derived default. XYZ goes to the working space through the
 * inverse of the project's working-to-XYZ matrix, which the OpenColorIO config gives; a working
 * space with no matrix to XYZ (a log space) is treated as linear Rec. 709. When the source
 * carries raw/wb_late_correction, camera RGB is multiplied by it before the matrix.
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
    KnobBoolWPtr profileOverride;
    KnobChoiceWPtr profile;
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

    virtual void onInputChanged(int inputNo) OVERRIDE FINAL;

    virtual void onMetadataRefreshed(const NodeMetadata& metadata) OVERRIDE FINAL;

    virtual ImageMetadata deriveOutputMetadata(double time,
                                               ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    void refreshEnabled();

    // While the override is off the choice shows the source's default, which is what a render uses.
    void showSourceProfile();

    void refreshProfileWarning();

    RawNodeSupport::OwnedMessage _message;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Raw_RawInputColor_h
