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
 * (commit 20891f6c6fa7), src/iop/colorin.c (commit_params(), reload_defaults() and
 * update_profile_list()) and src/common/colorspaces.c, Copyright (C) 2009-2026 darktable
 * developers, distributed under the GNU GPL version 3 or later.
 */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "RawInputColor.h"

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
    { eRawInputColorProfileLinearRec709, kRawInputColorProfileLinearRec709, "Linear Rec709 RGB",
      "Treats camera RGB as linear Rec. 709." },
    { eRawInputColorProfileCustom, kRawInputColorProfileCustom, "Custom",
      "The camera RGB to XYZ (D65) matrix below, used as given." },
};

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
    case eRawInputColorProfileLinearRec709:
        return RawColorMath::srgbToXyz();
    case eRawInputColorProfileCustom:
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

    if (rawInputColorResolveProfile(requested, source, &camToXyz) == eRawInputColorProfileCustom) {
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                camToXyz[r * 3 + c] = reader.value(rows[r], c);
            }
        }
    }
    RawColorMath::Mat3 total = RawColorMath::multiply(xyzToWorking(holder), camToXyz);
    // darktable's process_cmatrix() multiplies camera RGB by D65coeffs / as_shot before the
    // matrix when RawTemperature asked for the late correction; scaling the matrix's columns
    // does the same. darktable skips it for a Lab input profile, which this node has not.
    if (const std::optional<std::array<double, 3>> correction = RawMetadata::getWbLateCorrection(source)) {
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                total[r * 3 + c] *= (*correction)[c];
            }
        }
    }
    Params params;
    for (int i = 0; i < 9; ++i) {
        params.matrix[i] = (float)total[i];
    }

    return params;
}

bool
RawInputColorOp::isIdentity(const Params& params) const
{
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

    for (int x = 0; x < io.width; ++x, src += io.nComps, dst += io.nComps) {
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

    if (!correction || !d65) {
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

void
RawInputColor::refreshProfileWarning()
{
    const KnobBoolPtr overrideKnob = op().profileOverride.lock();
    const KnobChoicePtr choice = op().profile.lock();

    if (!overrideKnob || !choice || !getInput(getSourceInput())) {
        _message.withdraw(getNode());

        return;
    }
    const ImageMetadata source = getSourceMetadata(getCurrentTime(), ViewIdx(0));
    const RawInputColorProfileEnum requested = overrideKnob->getValue() ? rawInputColorProfileAt(choice->getValue()) : rawInputColorDefaultProfile(source);
    RawColorMath::Mat3 unused;
    const RawInputColorProfileEnum used = rawInputColorResolveProfile(requested, source, &unused);
    if (used != requested) {
        const QString text = tr(kProfileUnavailable).arg(tr(optionOf(requested).label), tr(optionOf(used).label));
        _message.post(getNode(), eMessageTypeWarning, text.toStdString());
    } else {
        _message.withdraw(getNode());
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
RawInputColor::onMetadataRefreshed(const NodeMetadata& metadata)
{
    ImageNode<RawInputColorOp>::onMetadataRefreshed(metadata);
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

    if (overrideKnob && (k == overrideKnob.get())) {
        if (!overrideKnob->getValue()) {
            showSourceProfile();
        }
    } else if (!choice || (k != choice.get())) {
        return false;
    }
    refreshEnabled();
    refreshProfileWarning();

    return true;
}

NATRON_NAMESPACE_EXIT
