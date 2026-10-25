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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Bloom.h"

#include <algorithm>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Filter/BloomKernels.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {
const int kSlotSource = 0;
const int kSlotBloom = 1;

/// The blurs in pixels at the render scale.
BloomKernels::Settings
settingsFor(const BloomParams& params,
            const PassContext& context)
{
    const OfxPointD rs = context.scale.toOfxPointD();
    BloomKernels::Settings s;

    s.sizeX = rs.x * (params.sizeX / context.pixelAspectRatio);
    s.sizeY = rs.y * params.sizeY;
    s.ratio = params.ratio;
    s.count = params.count;
    s.filter = params.filter;

    return s;
}

/// The bloom of slot 0 into slot 1, with the nearest pixel repeated outside the image and no
/// spread of the region of definition.
PassList
bloomPasses(const BloomParams& params,
            const PassContext& context)
{
    const BloomKernels::PlanPtr plan = BloomKernels::makePlan(settingsFor(params, context), true);
    PassList list;

    list.boundary = PassExec::Boundary::Nearest;
    list.scratchSlots = 2;
    list.resultSlot = kSlotBloom;
    BloomKernels::appendPasses(plan, kSlotSource, kSlotBloom, 0, 0, &list.passes);

#ifdef NATRON_HAVE_GPU
    // The source filled over the CPU's buffer, then bloomed straight into the output.
    list.gpu = [plan](const PassGpuIO& io) -> StatusEnum {
        gpu::GpuBuffer* fill = nullptr;
        if (BloomKernels::recordFill(io, true, &fill) != eStatusOK) {
            return eStatusFailed;
        }

        return BloomKernels::recordGpu(*plan, io, fill, PassExec::toGpuBlurImage(io.filtered));
    };
#endif

    return list;
}
} // anonymous namespace

KnobChoicePtr
Bloom::createFilterKnob(KnobHolder* holder,
                        const KnobPagePtr& page,
                        const char* name)
{
    KnobChoicePtr filterKnob = AppManager::createKnob<KnobChoice>(holder, EffectInstance::tr("Filter"));
    filterKnob->setName(name);
    filterKnob->setHintToolTip(EffectInstance::tr("The blurs' filter, as Blur's. The default, Gaussian (FIR), blurs large sizes on a downsampled copy of the image, so its cost stays bounded; the others blur at full resolution."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kBlurParamFilterQuasiGaussian, EffectInstance::tr("Quasi-Gaussian").toStdString(), EffectInstance::tr("Quasi-Gaussian filter (0-order recursive Deriche filter, faster) - IIR (infinite support / impulsional response).").toStdString()));
        options.push_back(ChoiceOption(kBlurParamFilterGaussian, EffectInstance::tr("Gaussian").toStdString(), EffectInstance::tr("Gaussian filter (Van Vliet recursive Gaussian filter, more isotropic, slower) - IIR (infinite support / impulsional response).").toStdString()));
        options.push_back(ChoiceOption(kBlurParamFilterBox, EffectInstance::tr("Box").toStdString(), EffectInstance::tr("Box filter - FIR (finite support / impulsional response).").toStdString()));
        options.push_back(ChoiceOption(kBlurParamFilterTriangle, EffectInstance::tr("Triangle").toStdString(), EffectInstance::tr("Triangle/tent filter - FIR (finite support / impulsional response).").toStdString()));
        options.push_back(ChoiceOption(kBlurParamFilterQuadratic, EffectInstance::tr("Quadratic").toStdString(), EffectInstance::tr("Quadratic filter - FIR (finite support / impulsional response).").toStdString()));
        options.push_back(ChoiceOption(kBlurParamFilterFIRGaussian, EffectInstance::tr("Gaussian (FIR)").toStdString(), EffectInstance::tr("Gaussian filter cropped at 3 sigma and normalised - FIR (finite support / impulsional response). Sizes above 12 pixels are blurred on a downsampled pyramid.").toStdString()));
        filterKnob->populateChoices(options);
    }
    filterKnob->setDefaultValue((int)BlurKernels::eFilterFIRGaussian);
    page->addKnob(filterKnob);

    return filterKnob;
}

void
BloomOp::createKnobs(KnobHolder* holder,
                     const KnobPagePtr& page)
{
    KnobDoublePtr sizeKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Size"), 2);
    sizeKnob->setName(kBloomParamSize);
    sizeKnob->setHintToolTip(EffectInstance::tr("Size (diameter) of the first, smallest blur, in pixel units (>=0). The standard deviation of the corresponding Gaussian is size/2.4. 0 leaves the image unchanged."));
    sizeKnob->setSpatial(true);
    sizeKnob->setCanAutoFoldDimensions(true);
    for (int d = 0; d < 2; ++d) {
        sizeKnob->setMinimum(0., d);
        sizeKnob->setMaximum(1000., d);
        sizeKnob->setDisplayMinimum(0., d);
        sizeKnob->setDisplayMaximum(100., d);
        sizeKnob->setIncrement(0.1, d);
        sizeKnob->setDecimals(1, d);
        sizeKnob->setDefaultValue(0., d);
    }
    page->addKnob(sizeKnob);
    size = sizeKnob;

    KnobDoublePtr ratioKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Ratio"));
    ratioKnob->setName(kBloomParamRatio);
    ratioKnob->setHintToolTip(EffectInstance::tr("How many times larger each blur is than the previous one. At 1 or less, there is a single blur."));
    ratioKnob->setMinimum(1.);
    ratioKnob->setDisplayMinimum(1.);
    ratioKnob->setDisplayMaximum(4.);
    ratioKnob->setDefaultValue(2.);
    page->addKnob(ratioKnob);
    ratio = ratioKnob;

    KnobIntPtr countKnob = AppManager::createKnob<KnobInt>(holder, EffectInstance::tr("Count"));
    countKnob->setName(kBloomParamCount);
    countKnob->setHintToolTip(EffectInstance::tr("The number of blurs averaged."));
    countKnob->setMinimum(1);
    countKnob->setDisplayMinimum(1);
    countKnob->setDisplayMaximum(10);
    countKnob->setDefaultValue(5);
    page->addKnob(countKnob);
    count = countKnob;

    filter = Bloom::createFilterKnob(holder, page, kBloomParamFilter);
} // BloomOp::createKnobs

BloomOp::Params
BloomOp::bake(const KnobReader& reader) const
{
    Params params;

    params.sizeX = reader.value(size, 0);
    params.sizeY = reader.value(size, 1);
    params.ratio = reader.value(ratio);
    params.count = reader.value(count);
    const int f = reader.value(filter);
    if ((f >= (int)BlurKernels::eFilterQuasiGaussian) && (f <= (int)BlurKernels::eFilterFIRGaussian)) {
        params.filter = (BlurKernels::Filter)f;
    }

    return params;
}

bool
BloomOp::isIdentity(const Params& params,
                    const PassContext& context) const
{
    return BloomKernels::isIdentity(settingsFor(params, context));
}

PassList
BloomOp::passes(const Params& params,
                const PassContext& context) const
{
    return bloomPasses(params, context);
}

Bloom::Bloom(NodePtr node)
    : ImageNode<BloomOp>(node)
{
}

Bloom::~Bloom()
{
}

void
Bloom::addAcceptedComponents(int inputNb,
                             std::list<ImageLayerDesc>* comps)
{
    ImageNode<BloomOp>::addAcceptedComponents(inputNb, comps);
    if (!isInputMask(inputNb)) {
        comps->push_back(ImageLayerDesc::getXYComponents());
    }
}

RectI
Bloom::getSourceRoI(const RectI& rect,
                    const RenderScale& scale,
                    const BloomParams& params)
{
    PassContext context;

    context.scale = scale;

    return passListSourceRoI(rect, bloomPasses(params, context));
}

NATRON_NAMESPACE_EXIT
