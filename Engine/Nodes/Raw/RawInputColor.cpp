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
 * (commit 20891f6c6fa7), src/iop/colorin.c (commit_params(), reload_defaults(),
 * update_profile_list(), process() and process_cmatrix_proper()) and src/common/colorspaces.c, Copyright (C) 2009-2026 darktable
 * developers, distributed under the GNU GPL version 3 or later.
 */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "RawInputColor.h"

#include <algorithm>
#include <vector>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/Raw/RawColorMatrices.h"
#include "Engine/Nodes/Raw/RawGpuParams.h"
#include "Engine/Project.h"
#include "Engine/ProjectColorManagement.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {
struct ProfileOption {
    RawInputColorProfileEnum profile;
    const char* id;
    const char* label;
    const char* hint;
};

// The Input Profile choice in darktable's list order, its image profiles first. darktable's
// names, capitalised.
const ProfileOption kProfileOptions[] = {
    { eRawInputColorProfileEmbeddedMatrix, kRawInputColorProfileEmbeddedMatrix, "Embedded Matrix",
      "The matrix a DNG's colour tags give for D65." },
    { eRawInputColorProfileStandardMatrix, kRawInputColorProfileStandardMatrix, "Standard Color Matrix",
      "The camera's matrix the raw decoder gives for the file (raw/cam_to_xyz)." },
    { eRawInputColorProfileEnhancedMatrix, kRawInputColorProfileEnhancedMatrix, "Enhanced Color Matrix",
      "darktable's own matrix for the camera, profiled from a colour chart." },
    { eRawInputColorProfileVendorMatrix, kRawInputColorProfileVendorMatrix, "Vendor Color Matrix",
      "darktable's matrix for the camera matched to the vendor's own rendering." },
    { eRawInputColorProfileAlternateMatrix, kRawInputColorProfileAlternateMatrix, "Alternate Color Matrix",
      "darktable's alternate matrix for the camera." },
    { eRawInputColorProfileSrgb, kRawInputColorProfileSrgb, "sRGB",
      "sRGB primaries, D65 white and the sRGB tone curve." },
    { eRawInputColorProfileAdobeRgb, kRawInputColorProfileAdobeRgb, "Adobe RGB (compatible)",
      "Adobe RGB (1998) primaries, D65 white and a 2.2 (2 + 51/256) gamma." },
    { eRawInputColorProfileLinearRec709, kRawInputColorProfileLinearRec709, "Linear Rec709 RGB",
      "Treats camera RGB as linear Rec. 709." },
    { eRawInputColorProfileRec709, kRawInputColorProfileRec709, "Rec709 RGB",
      "Rec. 709 primaries and the Rec. 709 tone curve." },
    { eRawInputColorProfileLinearRec2020, kRawInputColorProfileLinearRec2020, "Linear Rec2020 RGB",
      "Linear Rec. 2020." },
    { eRawInputColorProfilePqRec2020, kRawInputColorProfilePqRec2020, "PQ Rec2020 RGB",
      "Rec. 2020 primaries and the PQ (SMPTE ST 2084) tone curve, 1 being 10000 nits." },
    { eRawInputColorProfileHlgRec2020, kRawInputColorProfileHlgRec2020, "HLG Rec2020 RGB",
      "Rec. 2020 primaries and the hybrid log-gamma tone curve." },
    { eRawInputColorProfilePqP3, kRawInputColorProfilePqP3, "PQ P3 RGB",
      "P3 primaries, D65 white and the PQ tone curve." },
    { eRawInputColorProfileHlgP3, kRawInputColorProfileHlgP3, "HLG P3 RGB",
      "P3 primaries, D65 white and the hybrid log-gamma tone curve." },
    { eRawInputColorProfileDisplayP3, kRawInputColorProfileDisplayP3, "Display P3 RGB",
      "P3 primaries, D65 white and the sRGB tone curve." },
    { eRawInputColorProfileLinearProPhoto, kRawInputColorProfileLinearProPhoto, "Linear ProPhoto RGB",
      "Linear ProPhoto RGB, D50 white." },
    { eRawInputColorProfileLinearXyz, kRawInputColorProfileLinearXyz, "Linear XYZ",
      "The input is CIE XYZ relative to D50." },
    { eRawInputColorProfileLab, kRawInputColorProfileLab, "Lab",
      "The input is CIE Lab relative to D50, L from 0 to 100. The late white balance correction and gamut clipping do not apply." },
    { eRawInputColorProfileLinearInfraredBgr, kRawInputColorProfileLinearInfraredBgr, "Linear Infrared BGR",
      "Linear Rec. 709 with red and blue swapped." },
    { eRawInputColorProfileBrg, kRawInputColorProfileBrg, "BRG (for testing)",
      "sRGB with its channels rotated, for testing." },
    { eRawInputColorProfileCustom, kRawInputColorProfileCustom, "Custom",
      "The camera RGB to XYZ (D65) matrix below, used as given." },
};

struct SpaceOption {
    RawColorProfiles::Space space;
    const char* id;
    const char* label;
};

// darktable's work_pos profiles, in its order; its sRGB working profile is the "web-safe" one.
const SpaceOption kWorkingOptions[] = {
    { RawColorProfiles::Space::Srgb, kRawInputColorProfileSrgb, "sRGB (web-safe)" },
    { RawColorProfiles::Space::AdobeRgb, kRawInputColorProfileAdobeRgb, "Adobe RGB (compatible)" },
    { RawColorProfiles::Space::LinearRec709, kRawInputColorProfileLinearRec709, "Linear Rec709 RGB" },
    { RawColorProfiles::Space::Rec709, kRawInputColorProfileRec709, "Rec709 RGB" },
    { RawColorProfiles::Space::LinearRec2020, kRawInputColorProfileLinearRec2020, "Linear Rec2020 RGB" },
    { RawColorProfiles::Space::PqRec2020, kRawInputColorProfilePqRec2020, "PQ Rec2020 RGB" },
    { RawColorProfiles::Space::HlgRec2020, kRawInputColorProfileHlgRec2020, "HLG Rec2020 RGB" },
    { RawColorProfiles::Space::PqP3, kRawInputColorProfilePqP3, "PQ P3 RGB" },
    { RawColorProfiles::Space::HlgP3, kRawInputColorProfileHlgP3, "HLG P3 RGB" },
    { RawColorProfiles::Space::DisplayP3, kRawInputColorProfileDisplayP3, "Display P3 RGB" },
    { RawColorProfiles::Space::LinearProPhoto, kRawInputColorProfileLinearProPhoto, "Linear ProPhoto RGB" },
};

// colorin's dt_iop_color_normalize_t after off. Only the matrices count: the clip is in linear
// RGB, so sRGB and linear Rec709 clip alike.
const SpaceOption kGamutOptions[] = {
    { RawColorProfiles::Space::Srgb, kRawInputColorProfileSrgb, "sRGB" },
    { RawColorProfiles::Space::AdobeRgb, kRawInputColorProfileAdobeRgb, "Adobe RGB (compatible)" },
    { RawColorProfiles::Space::LinearRec709, kRawInputColorProfileLinearRec709, "Linear Rec709 RGB" },
    { RawColorProfiles::Space::LinearRec2020, kRawInputColorProfileLinearRec2020, "Linear Rec2020 RGB" },
};

template <std::size_t N>
std::optional<RawColorProfiles::Space>
spaceAt(const SpaceOption (&options)[N],
        int index)
{
    // Index 0 is the choice's first option, the project's working space or off.
    if ((index < 1) || (index > (int)N)) {
        return std::nullopt;
    }

    return options[index - 1].space;
}

template <std::size_t N>
int
spaceIndex(const SpaceOption (&options)[N],
           const std::optional<RawColorProfiles::Space>& space)
{
    if (space) {
        for (int i = 0; i < (int)N; ++i) {
            if (options[i].space == *space) {
                return i + 1;
            }
        }
    }

    return 0;
}

template <std::size_t N>
const char*
spaceLabel(const SpaceOption (&options)[N],
           RawColorProfiles::Space space)
{
    return options[spaceIndex(options, space) - 1].label;
}

std::array<float, 9>
toFloats(const RawColorMath::Mat3& m)
{
    std::array<float, 9> out;
    for (int i = 0; i < 9; ++i) {
        out[i] = (float)m[i];
    }

    return out;
}

void
applyMatrix(const float* m,
            const float in[3],
            float out[3])
{
    out[0] = m[0] * in[0] + m[1] * in[1] + m[2] * in[2];
    out[1] = m[3] * in[0] + m[4] * in[1] + m[5] * in[2];
    out[2] = m[6] * in[0] + m[7] * in[1] + m[8] * in[2];
}

const char* const kProfileUnavailable = "input profile: %1 not available for this camera. falling back to %2.";

const ProfileOption&
optionOf(RawInputColorProfileEnum profile)
{
    for (const ProfileOption& option : kProfileOptions) {
        if (option.profile == profile) {
            return option;
        }
    }

    return kProfileOptions[rawInputColorProfileIndex(eRawInputColorProfileLinearRec709)];
}

std::optional<RawColorMath::Mat3>
tableMatrix(RawColorMatrices::Table table,
            const ImageMetadata& source)
{
    const std::string makermodel = RawColorMatrices::makerModel(RawMetadata::getMake(source).value_or(std::string()), RawMetadata::getModel(source).value_or(std::string()));

    if (const RawColorMatrices::Entry* entry = RawColorMatrices::find(table, makermodel)) {
        return RawColorMatrices::cameraToXyz(*entry);
    }

    return std::nullopt;
}

// darktable's dt_colorspaces_create_xyzimatrix_profile(): the XYZ to camera matrix inverted, then
// white-normalised at D65.
std::optional<RawColorMath::Mat3>
xyzToCamProfile(const std::optional<std::array<double, 9>>& xyzToCam)
{
    if (!xyzToCam) {
        return std::nullopt;
    }
    if (const std::optional<RawColorMath::Mat3> camToXyz = RawColorMath::invert(*xyzToCam)) {
        return RawColorMath::whiteNormalisedCameraToXyz(*camToXyz);
    }

    return std::nullopt;
}
} // namespace

std::optional<RawColorProfiles::Space>
rawInputColorProfileSpace(RawInputColorProfileEnum profile)
{
    using RawColorProfiles::Space;
    switch (profile) {
    case eRawInputColorProfileSrgb:
        return Space::Srgb;
    case eRawInputColorProfileAdobeRgb:
        return Space::AdobeRgb;
    case eRawInputColorProfileLinearRec709:
        return Space::LinearRec709;
    case eRawInputColorProfileRec709:
        return Space::Rec709;
    case eRawInputColorProfileLinearRec2020:
        return Space::LinearRec2020;
    case eRawInputColorProfilePqRec2020:
        return Space::PqRec2020;
    case eRawInputColorProfileHlgRec2020:
        return Space::HlgRec2020;
    case eRawInputColorProfilePqP3:
        return Space::PqP3;
    case eRawInputColorProfileHlgP3:
        return Space::HlgP3;
    case eRawInputColorProfileDisplayP3:
        return Space::DisplayP3;
    case eRawInputColorProfileLinearProPhoto:
        return Space::LinearProPhoto;
    case eRawInputColorProfileLinearXyz:
        return Space::LinearXyz;
    case eRawInputColorProfileLab:
        return Space::Lab;
    case eRawInputColorProfileLinearInfraredBgr:
        return Space::LinearInfraredBgr;
    case eRawInputColorProfileBrg:
        return Space::Brg;
    case eRawInputColorProfileEmbeddedMatrix:
    case eRawInputColorProfileStandardMatrix:
    case eRawInputColorProfileEnhancedMatrix:
    case eRawInputColorProfileVendorMatrix:
    case eRawInputColorProfileAlternateMatrix:
    case eRawInputColorProfileCustom:
        break;
    }

    return std::nullopt;
}

std::optional<RawColorProfiles::Space>
rawInputColorWorkingProfileAt(int index)
{
    return spaceAt(kWorkingOptions, index);
}

int
rawInputColorWorkingProfileIndex(const std::optional<RawColorProfiles::Space>& space)
{
    return spaceIndex(kWorkingOptions, space);
}

std::optional<RawColorProfiles::Space>
rawInputColorGamutAt(int index)
{
    return spaceAt(kGamutOptions, index);
}

int
rawInputColorGamutIndex(const std::optional<RawColorProfiles::Space>& space)
{
    return spaceIndex(kGamutOptions, space);
}

RawInputColorProfileEnum
rawInputColorProfileAt(int index)
{
    if ((index < 0) || (index >= (int)(sizeof(kProfileOptions) / sizeof(kProfileOptions[0])))) {
        return eRawInputColorProfileLinearRec709;
    }

    return kProfileOptions[index].profile;
}

int
rawInputColorProfileIndex(RawInputColorProfileEnum profile)
{
    for (int i = 0; i < (int)(sizeof(kProfileOptions) / sizeof(kProfileOptions[0])); ++i) {
        if (kProfileOptions[i].profile == profile) {
            return i;
        }
    }

    return 0;
}

RawInputColorProfileEnum
rawInputColorDefaultProfile(const ImageMetadata& source)
{
    // reload_defaults() from the embedded matrix on: the embedded ICC, 4Bayer, monochrome and LDR
    // branches before it have no source here. An image with neither matrix is darktable's HDR
    // case.
    if (RawMetadata::getDngD65Matrix(source)) {
        return eRawInputColorProfileEmbeddedMatrix;
    }
    if (RawMetadata::getCamToXyz(source)) {
        return eRawInputColorProfileStandardMatrix;
    }

    return eRawInputColorProfileLinearRec709;
}

std::optional<RawColorMath::Mat3>
rawInputColorProfileMatrix(RawInputColorProfileEnum profile,
                           const ImageMetadata& source)
{
    switch (profile) {
    case eRawInputColorProfileEmbeddedMatrix:
        return xyzToCamProfile(RawMetadata::getDngD65Matrix(source));
    case eRawInputColorProfileStandardMatrix:
        // raw/cam_to_xyz is already the inverse of darktable's adobe_XYZ_to_CAM.
        if (const std::optional<std::array<double, 9>> camToXyz = RawMetadata::getCamToXyz(source)) {
            return RawColorMath::whiteNormalisedCameraToXyz(*camToXyz);
        }

        return std::nullopt;
    case eRawInputColorProfileEnhancedMatrix:
        return tableMatrix(RawColorMatrices::Table::Enhanced, source);
    case eRawInputColorProfileVendorMatrix:
        return tableMatrix(RawColorMatrices::Table::Vendor, source);
    case eRawInputColorProfileAlternateMatrix:
        return tableMatrix(RawColorMatrices::Table::Alternate, source);
    case eRawInputColorProfileCustom:
        break;
    default:
        if (const std::optional<RawColorProfiles::Space> space = rawInputColorProfileSpace(profile)) {
            return RawColorProfiles::toXyz(*space);
        }
        break;
    }

    return std::nullopt;
}

RawInputColorProfileEnum
rawInputColorResolveProfile(RawInputColorProfileEnum requested,
                            const ImageMetadata& source,
                            RawColorMath::Mat3* camToXyz)
{
    if (requested == eRawInputColorProfileCustom) {
        return requested;
    }
    RawInputColorProfileEnum type = requested;
    // commit_params(): a missing table matrix tries the embedded ICC profile, which this node
    // has not, then the embedded matrix, the standard matrix and linear Rec709.
    for (;;) {
        if (const std::optional<RawColorMath::Mat3> matrix = rawInputColorProfileMatrix(type, source)) {
            *camToXyz = *matrix;

            return type;
        }
        switch (type) {
        case eRawInputColorProfileEnhancedMatrix:
        case eRawInputColorProfileVendorMatrix:
        case eRawInputColorProfileAlternateMatrix:
            type = eRawInputColorProfileEmbeddedMatrix;
            break;
        case eRawInputColorProfileEmbeddedMatrix:
            type = eRawInputColorProfileStandardMatrix;
            break;
        default:
            *camToXyz = RawColorMath::srgbToXyz();

            return eRawInputColorProfileLinearRec709;
        }
    }
}

void
RawInputColorOp::createKnobs(KnobHolder* knobHolder,
                             const KnobPagePtr& page)
{
    holder = knobHolder;

    KnobBoolPtr overrideKnob = AppManager::createKnob<KnobBool>(knobHolder, EffectInstance::tr("Override Input Profile").toStdString());
    overrideKnob->setName(kRawInputColorParamProfileOverride);
    overrideKnob->setHintToolTip(EffectInstance::tr("Use the input profile below instead of the one the raw file calls for: the embedded matrix for a DNG that has one, the standard color matrix for another raw file, linear Rec709 for an image with no camera matrix.").toStdString());
    overrideKnob->setDefaultValue(false);
    overrideKnob->setAnimationEnabled(false);
    page->addKnob(overrideKnob);
    profileOverride = overrideKnob;

    KnobChoicePtr choice = AppManager::createKnob<KnobChoice>(knobHolder, EffectInstance::tr("Input Profile").toStdString());
    choice->setName(kRawInputColorParamProfile);
    choice->setHintToolTip(EffectInstance::tr("The profile giving the camera RGB to XYZ (D65) matrix. A matrix the camera has not falls back as darktable's does: a darktable table matrix to the embedded matrix, that to the standard color matrix, that to linear Rec709.").toStdString());
    std::vector<ChoiceOption> options;
    for (const ProfileOption& option : kProfileOptions) {
        options.push_back(ChoiceOption(option.id, EffectInstance::tr(option.label).toStdString(), EffectInstance::tr(option.hint).toStdString()));
    }
    choice->populateChoices(options);
    choice->setDefaultValue(rawInputColorProfileIndex(eRawInputColorProfileStandardMatrix));
    choice->setAnimationEnabled(false);
    // Off until the override is on; RawInputColor::refreshEnabled() keeps it so after a load.
    choice->setAllDimensionsEnabled(false);
    page->addKnob(choice);
    profile = choice;

    const RawColorMath::Mat3 rec709 = RawColorMath::srgbToXyz();
    const char* names[3] = { kRawInputColorParamRow0, kRawInputColorParamRow1, kRawInputColorParamRow2 };
    const QString labels[3] = { EffectInstance::tr("X"), EffectInstance::tr("Y"), EffectInstance::tr("Z") };
    const char* columns[3] = { "r", "g", "b" };
    for (int r = 0; r < 3; ++r) {
        KnobDoublePtr row = AppManager::createKnob<KnobDouble>(knobHolder, labels[r].toStdString(), 3);
        row->setName(names[r]);
        row->setHintToolTip(EffectInstance::tr("A row of the custom camera RGB to XYZ (D65) matrix: the weights of camera red, green and blue in this XYZ component.").toStdString());
        for (int c = 0; c < 3; ++c) {
            // The columns weigh camera red, green and blue; the default x, y, z would read as XYZ.
            row->setDimensionName(c, columns[c]);
            row->setDefaultValue(rec709[r * 3 + c], c);
            row->setDisplayMinimum(-2., c);
            row->setDisplayMaximum(2., c);
        }
        row->setAllDimensionsEnabled(false);
        page->addKnob(row);
        rows[r] = row;
    }

    KnobChoicePtr working = AppManager::createKnob<KnobChoice>(knobHolder, EffectInstance::tr("Working Profile").toStdString());
    working->setName(kRawInputColorParamWorkingProfile);
    working->setHintToolTip(EffectInstance::tr("The space of the output. Project Working Space is the project's OpenColorIO working space, which the nodes after this one expect; another profile writes the output in that profile, its tone curve included, as darktable's working profile does.").toStdString());
    std::vector<ChoiceOption> workingOptions;
    workingOptions.push_back(ChoiceOption(kRawInputColorWorkingProject, EffectInstance::tr("Project Working Space").toStdString(), EffectInstance::tr("The project's working space.").toStdString()));
    for (const SpaceOption& option : kWorkingOptions) {
        workingOptions.push_back(ChoiceOption(option.id, EffectInstance::tr(option.label).toStdString(), std::string()));
    }
    working->populateChoices(workingOptions);
    working->setDefaultValue(0);
    working->setAnimationEnabled(false);
    page->addKnob(working);
    workingProfile = working;

    KnobChoicePtr gamut = AppManager::createKnob<KnobChoice>(knobHolder, EffectInstance::tr("Gamut Clipping").toStdString());
    gamut->setName(kRawInputColorParamGamutClipping);
    gamut->setHintToolTip(EffectInstance::tr("Confine the colours to the gamut of an RGB colour space: after the input profile the values are clamped to [0, 1] in that space's linear RGB, highlights above its white included. Not applied to a Lab input.").toStdString());
    std::vector<ChoiceOption> gamutOptions;
    gamutOptions.push_back(ChoiceOption(kRawInputColorGamutOff, EffectInstance::tr("Off").toStdString(), std::string()));
    for (const SpaceOption& option : kGamutOptions) {
        gamutOptions.push_back(ChoiceOption(option.id, EffectInstance::tr(option.label).toStdString(), std::string()));
    }
    gamut->populateChoices(gamutOptions);
    gamut->setDefaultValue(0);
    gamut->setAnimationEnabled(false);
    page->addKnob(gamut);
    gamutClipping = gamut;
}

RawColorMath::Mat3
RawInputColorOp::xyzToWorking(const KnobHolder* knobHolder)
{
    const RawColorMath::Mat3 fallback = *RawColorMath::invert(RawColorMath::srgbToXyz());
    AppInstancePtr app = knobHolder ? knobHolder->getApp() : AppInstancePtr();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();
    ProjectColorManagementPtr cm = project ? project->getColorManagement() : ProjectColorManagementPtr();
    double workingToXyz[9];

    if (!cm || !cm->getWorkingToXYZ(workingToXyz)) {
        return fallback;
    }
    RawColorMath::Mat3 m;
    for (int i = 0; i < 9; ++i) {
        m[i] = workingToXyz[i];
    }

    return RawColorMath::invert(m).value_or(fallback);
}

RawInputColorOp::Params
RawInputColorOp::bake(const KnobReader& reader,
                      const ImageMetadata& source) const
{
    RawColorMath::Mat3 camToXyz = RawColorMath::srgbToXyz();
    const RawInputColorProfileEnum requested = reader.value(profileOverride) ? rawInputColorProfileAt(reader.value(profile)) : rawInputColorDefaultProfile(source);
    const RawInputColorProfileEnum used = rawInputColorResolveProfile(requested, source, &camToXyz);

    if (used == eRawInputColorProfileCustom) {
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                camToXyz[r * 3 + c] = reader.value(rows[r], c);
            }
        }
    }
    const std::optional<RawColorProfiles::Space> inputSpace = rawInputColorProfileSpace(used);
    const std::optional<RawColorProfiles::Space> working = rawInputColorWorkingProfileAt(reader.value(workingProfile));
    // colorin is disabled for a Lab input (commit_params()), so nothing is clipped; the pipe
    // still takes Lab to the working profile.
    const bool lab = (used == eRawInputColorProfileLab);
    const std::optional<RawColorProfiles::Space> gamut = lab ? std::nullopt : rawInputColorGamutAt(reader.value(gamutClipping));
    Params params;

    params.lab = lab;
    params.clip = bool(gamut);
    params.inputTrc = inputSpace ? RawColorProfiles::trcOf(*inputSpace) : RawColorProfiles::Trc::Linear;
    params.outputTrc = working ? RawColorProfiles::trcOf(*working) : RawColorProfiles::Trc::Linear;
    params.correction = { 1.f, 1.f, 1.f };

    const RawColorMath::Mat3 xyzToOut = working ? RawColorProfiles::fromXyz(*working) : xyzToWorking(holder);
    RawColorMath::Mat3 first = RawColorMath::multiply(gamut ? RawColorProfiles::fromXyz(*gamut) : xyzToOut, camToXyz);
    // process() multiplies camera RGB by D65coeffs / as_shot before the tone curves and the
    // matrix when RawTemperature asked for the late correction, except for a Lab input. With no
    // tone curve, scaling the matrix's columns does the same.
    if (const std::optional<std::array<double, 3>> correction = RawMetadata::getWbLateCorrection(source); correction && !lab) {
        if (params.inputTrc == RawColorProfiles::Trc::Linear) {
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c) {
                    first[r * 3 + c] *= (*correction)[c];
                }
            }
        } else {
            for (int c = 0; c < 3; ++c) {
                params.correction[c] = (float)(*correction)[c];
            }
        }
    }
    params.matrix = toFloats(first);
    params.gamutToWorking = gamut ? toFloats(RawColorMath::multiply(xyzToOut, RawColorProfiles::toXyz(*gamut))) : toFloats(RawColorMath::Mat3 { 1., 0., 0., 0., 1., 0., 0., 0., 1. });
    params.cpuOnly = lab || params.clip || working || (params.inputTrc != RawColorProfiles::Trc::Linear);

    return params;
}

bool
RawInputColorOp::isIdentity(const Params& params) const
{
    if (params.lab || params.clip || (params.inputTrc != RawColorProfiles::Trc::Linear) || (params.outputTrc != RawColorProfiles::Trc::Linear)) {
        return false;
    }
    for (int i = 0; i < 9; ++i) {
        if (params.matrix[i] != (((i % 4) == 0) ? 1.f : 0.f)) {
            return false;
        }
    }

    return true;
}

void
RawInputColorOp::row(const Params& params,
                     const RowIO& io) const
{
    const float* src = io.src[0];
    float* dst = io.dst;
    const float* m = params.matrix.data();
    const bool matrixOnly = !params.lab && !params.clip && (params.inputTrc == RawColorProfiles::Trc::Linear) && (params.outputTrc == RawColorProfiles::Trc::Linear);

    for (int x = 0; x < io.width; ++x, src += io.nComps, dst += io.nComps) {
        if ((io.nComps >= 3) && !matrixOnly) {
            float in[3];
            if (params.lab) {
                const std::array<float, 3> xyz = RawColorProfiles::labToXyzD50(src[0], src[1], src[2]);
                in[0] = xyz[0];
                in[1] = xyz[1];
                in[2] = xyz[2];
            } else {
                for (int c = 0; c < 3; ++c) {
                    in[c] = RawColorProfiles::decode(params.inputTrc, src[c] * params.correction[c]);
                }
            }
            float out[3];
            applyMatrix(m, in, out);
            if (params.clip) {
                // process_cmatrix_proper()'s dt_vector_clip() in the gamut, then on to the output.
                for (int c = 0; c < 3; ++c) {
                    in[c] = std::min(std::max(out[c], 0.f), 1.f);
                }
                applyMatrix(params.gamutToWorking.data(), in, out);
            }
            for (int c = 0; c < 3; ++c) {
                dst[c] = RawColorProfiles::encode(params.outputTrc, out[c]);
            }
            if (io.nComps > 3) {
                dst[3] = src[3];
            }
            continue;
        }
        if (io.nComps < 3) {
            for (int c = 0; c < io.nComps; ++c) {
                dst[c] = src[c];
            }
            continue;
        }
        const float r = src[0];
        const float g = src[1];
        const float b = src[2];
        dst[0] = m[0] * r + m[1] * g + m[2] * b;
        dst[1] = m[3] * r + m[4] * g + m[5] * b;
        dst[2] = m[6] * r + m[7] * g + m[8] * b;
        if (io.nComps > 3) {
            dst[3] = src[3];
        }
    }
}

GpuOpRef
RawInputColorOp::gpu(const Params& params) const
{
#ifdef NATRON_HAVE_GPU
    if (params.cpuOnly) {
        return GpuOpRef::none();
    }
    RawInputColorGpuParams p = {};
    for (int i = 0; i < 9; ++i) {
        p.matrix[i] = params.matrix[i];
    }

    return GpuOpRef::compiled((int)gpu::GpuKernelId::rawinputcolor_wrapped, p);
#else
    Q_UNUSED(params);

    return GpuOpRef::none();
#endif
}

RawInputColor::RawInputColor(NodePtr node)
    : ImageNode<RawInputColorOp>(node)
{
}

RawInputColor::~RawInputColor()
{
}

ImageMetadata
RawInputColor::deriveOutputMetadata(double time,
                                    ViewIdx view)
{
    ImageMetadata metadata = ImageNode<RawInputColorOp>::deriveOutputMetadata(time, view);
    const std::optional<std::array<double, 3>> correction = RawMetadata::getWbLateCorrection(metadata);
    const std::optional<std::array<double, 3>> d65 = RawMetadata::getWbD65Coeffs(metadata);

    if (!correction || !d65 || (usedProfile(metadata, nullptr) == eRawInputColorProfileLab)) {
        return metadata;
    }
    // As darktable's colorin process(): the data now carries the D65 multipliers, and its
    // maximum moves with the correction.
    std::array<double, 3> maximum = RawMetadata::getProcessedMaximum(metadata).value_or(std::array<double, 3> { 1., 1., 1. });
    for (int c = 0; c < 3; ++c) {
        maximum[c] *= (*correction)[c];
    }
    RawMetadata::setWbApplied(&metadata, *d65);
    RawMetadata::setProcessedMaximum(&metadata, maximum);

    return metadata;
}

void
RawInputColor::refreshEnabled()
{
    const KnobBoolPtr overrideKnob = op().profileOverride.lock();
    const KnobChoicePtr choice = op().profile.lock();

    if (!overrideKnob || !choice) {
        return;
    }
    const bool overridden = overrideKnob->getValue();
    choice->setAllDimensionsEnabled(overridden);
    const bool custom = overridden && (rawInputColorProfileAt(choice->getValue()) == eRawInputColorProfileCustom);
    for (const KnobDoubleWPtr& weak : op().rows) {
        if (KnobDoublePtr row = weak.lock()) {
            row->setAllDimensionsEnabled(custom);
        }
    }
}

void
RawInputColor::showSourceProfile()
{
    const KnobBoolPtr overrideKnob = op().profileOverride.lock();
    const KnobChoicePtr choice = op().profile.lock();

    if (!overrideKnob || !choice || overrideKnob->getValue() || !getInput(getSourceInput())) {
        return;
    }
    const int index = rawInputColorProfileIndex(rawInputColorDefaultProfile(getSourceMetadata(getCurrentTime(), ViewIdx(0))));
    if (choice->getValue() != index) {
        choice->setValue(index);
    }
}

RawInputColorProfileEnum
RawInputColor::usedProfile(const ImageMetadata& source,
                           RawInputColorProfileEnum* requested) const
{
    const KnobBoolPtr overrideKnob = op().profileOverride.lock();
    const KnobChoicePtr choice = op().profile.lock();
    const RawInputColorProfileEnum wanted = (overrideKnob && choice && overrideKnob->getValue()) ? rawInputColorProfileAt(choice->getValue()) : rawInputColorDefaultProfile(source);
    RawColorMath::Mat3 unused;

    if (requested) {
        *requested = wanted;
    }

    return rawInputColorResolveProfile(wanted, source, &unused);
}

void
RawInputColor::refreshProfileWarning()
{
    const KnobChoicePtr working = op().workingProfile.lock();
    const std::optional<RawColorProfiles::Space> workingSpace = working ? rawInputColorWorkingProfileAt(working->getValue()) : std::nullopt;
    QString warning;

    if (getInput(getSourceInput())) {
        RawInputColorProfileEnum requested;
        const RawInputColorProfileEnum used = usedProfile(getSourceMetadata(getCurrentTime(), ViewIdx(0)), &requested);
        if (used != requested) {
            warning = tr(kProfileUnavailable).arg(tr(optionOf(requested).label), tr(optionOf(used).label));
        }
    }
    QString text = warning;
    if (workingSpace) {
        const QString note = tr("output: %1, not the project's working space.").arg(tr(spaceLabel(kWorkingOptions, *workingSpace)));
        text = text.isEmpty() ? note : (text + QString::fromUtf8("\n") + note);
    }
    if (text.isEmpty()) {
        _message.withdraw(getNode());
    } else {
        _message.post(getNode(), warning.isEmpty() ? eMessageTypeInfo : eMessageTypeWarning, text.toStdString());
    }
}

void
RawInputColor::onKnobsLoaded()
{
    showSourceProfile();
    refreshEnabled();
    refreshProfileWarning();
}

void
RawInputColor::onInputChanged(int /*inputNo*/)
{
    showSourceProfile();
    refreshProfileWarning();
}

void
RawInputColor::onCurrentFrameMetadataChanged(const NodeMetadata& metadata)
{
    ImageNode<RawInputColorOp>::onCurrentFrameMetadataChanged(metadata);
    showSourceProfile();
    refreshProfileWarning();
}

bool
RawInputColor::knobChanged(KnobI* k,
                           ValueChangedReasonEnum /*reason*/,
                           ViewSpec /*view*/,
                           double /*time*/,
                           bool /*originatedFromMainThread*/)
{
    const KnobBoolPtr overrideKnob = op().profileOverride.lock();
    const KnobChoicePtr choice = op().profile.lock();
    const KnobChoicePtr working = op().workingProfile.lock();

    if (overrideKnob && (k == overrideKnob.get())) {
        if (!overrideKnob->getValue()) {
            showSourceProfile();
        }
    } else if (working && (k == working.get())) {
        refreshProfileWarning();

        return true;
    } else if (!choice || (k != choice.get())) {
        return false;
    }
    refreshEnabled();
    refreshProfileWarning();

    return true;
}

NATRON_NAMESPACE_EXIT
