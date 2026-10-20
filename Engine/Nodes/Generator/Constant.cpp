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

#include "Constant.h"

#include <cfloat>

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Image/PixelKernel.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

void
ConstantOp::createKnobs(KnobHolder* holder,
                        const KnobPagePtr& page)
{
    const int dims = opaque ? 3 : 4;
    KnobColorPtr knob = AppManager::createKnob<KnobColor>(holder, EffectInstance::tr("Color").toStdString(), dims);

    knob->setName(kConstantParamColor);
    knob->setHintToolTip(EffectInstance::tr("Color to fill the image with.").toStdString());
    for (int i = 0; i < dims; ++i) {
        knob->setDefaultValue(0., i);
        knob->setMinimum(-DBL_MAX, i);
        knob->setMaximum(DBL_MAX, i);
        knob->setDisplayMinimum(0., i);
        knob->setDisplayMaximum(1., i);
    }
    page->addKnob(knob);
    color = knob;
}

ConstantOp::Params
ConstantOp::bake(const KnobReader& reader) const
{
    Params params;
    const int dims = opaque ? 3 : 4;

    params.rgba[0] = params.rgba[1] = params.rgba[2] = 0.f;
    params.rgba[3] = opaque ? 1.f : 0.f;
    for (int i = 0; i < dims; ++i) {
        params.rgba[i] = (float)reader.valueOr(color, 0., i);
    }

    return params;
}

bool
ConstantOp::isIdentity(const Params& /*params*/) const
{
    return false;
}

void
ConstantOp::row(const Params& params,
                const RowIO& io) const
{
    const GeneratorRowChannels channels(io);
    float* dst = io.dst;

    for (int x = 0; x < io.width; ++x, dst += io.nComps) {
        channels.write(params.rgba, dst);
    }
}

GpuOpRef
ConstantOp::gpu(const Params& params) const
{
#ifdef NATRON_HAVE_GPU
    return GpuOpRef::compiled((int)gpu::GpuKernelId::constant, params.rgba);
#else
    Q_UNUSED(params);

    return GpuOpRef::none();
#endif
}

NATRON_NAMESPACE_EXIT
