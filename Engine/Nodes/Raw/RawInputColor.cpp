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
 * 2009-2026 darktable developers, distributed under the GNU GPL version 3 or later.
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
#include "Engine/Nodes/Raw/RawGpuParams.h"
#include "Engine/Project.h"
#include "Engine/ProjectColorManagement.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

void
RawInputColorOp::createKnobs(KnobHolder* knobHolder,
                             const KnobPagePtr& page)
{
    holder = knobHolder;

    KnobChoicePtr mode = AppManager::createKnob<KnobChoice>(knobHolder, EffectInstance::tr("Camera Matrix").toStdString());
    mode->setName(kRawInputColorParamMatrix);
    mode->setHintToolTip(EffectInstance::tr("The camera RGB to XYZ (D65) matrix the conversion goes through.").toStdString());
    std::vector<ChoiceOption> modes;
    modes.push_back(ChoiceOption("file", EffectInstance::tr("From File").toStdString(), EffectInstance::tr("The camera's matrix, from the raw file; the standard matrix when the file has none.").toStdString()));
    modes.push_back(ChoiceOption("standard", EffectInstance::tr("Standard (Linear Rec. 709)").toStdString(), EffectInstance::tr("Treats camera RGB as linear Rec. 709.").toStdString()));
    modes.push_back(ChoiceOption("custom", EffectInstance::tr("Custom").toStdString(), EffectInstance::tr("The camera RGB to XYZ (D65) matrix below, used as given.").toStdString()));
    mode->populateChoices(modes);
    mode->setDefaultValue((int)eRawInputColorMatrixFile);
    mode->setAnimationEnabled(false);
    page->addKnob(mode);
    matrixMode = mode;

    const RawColorMath::Mat3 rec709 = RawColorMath::srgbToXyz();
    const char* names[3] = { kRawInputColorParamRow0, kRawInputColorParamRow1, kRawInputColorParamRow2 };
    const QString labels[3] = { EffectInstance::tr("X"), EffectInstance::tr("Y"), EffectInstance::tr("Z") };
    for (int r = 0; r < 3; ++r) {
        KnobDoublePtr row = AppManager::createKnob<KnobDouble>(knobHolder, labels[r].toStdString(), 3);
        row->setName(names[r]);
        row->setHintToolTip(EffectInstance::tr("A row of the custom camera RGB to XYZ (D65) matrix: the weights of camera red, green and blue in this XYZ component.").toStdString());
        for (int c = 0; c < 3; ++c) {
            row->setDefaultValue(rec709[r * 3 + c], c);
            row->setDisplayMinimum(-2., c);
            row->setDisplayMaximum(2., c);
        }
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
    const int mode = reader.value(matrixMode);

    if (mode == eRawInputColorMatrixCustom) {
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                camToXyz[r * 3 + c] = reader.value(rows[r], c);
            }
        }
    } else if (mode == eRawInputColorMatrixFile) {
        if (const std::optional<std::array<double, 9>> fromFile = RawMetadata::getCamToXyz(source)) {
            camToXyz = RawColorMath::whiteNormalisedCameraToXyz(*fromFile).value_or(camToXyz);
        }
    }
    const RawColorMath::Mat3 total = RawColorMath::multiply(xyzToWorking(holder), camToXyz);
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

void
RawInputColor::refreshEnabled()
{
    const KnobChoicePtr mode = op().matrixMode.lock();

    if (!mode) {
        return;
    }
    const bool custom = mode->getValue() == (int)eRawInputColorMatrixCustom;
    for (const KnobDoubleWPtr& weak : op().rows) {
        if (KnobDoublePtr row = weak.lock()) {
            row->setAllDimensionsEnabled(custom);
        }
    }
}

void
RawInputColor::onKnobsLoaded()
{
    refreshEnabled();
}

bool
RawInputColor::knobChanged(KnobI* k,
                           ValueChangedReasonEnum /*reason*/,
                           ViewSpec /*view*/,
                           double /*time*/,
                           bool /*originatedFromMainThread*/)
{
    const KnobChoicePtr mode = op().matrixMode.lock();

    if (!mode || (k != mode.get())) {
        return false;
    }
    refreshEnabled();

    return true;
}

NATRON_NAMESPACE_EXIT
