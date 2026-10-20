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

#include "Invert.h"

#include "Engine/ImageLayerDesc.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

InvertOp::Params
InvertOp::bake(const KnobReader& /*reader*/) const
{
    return Params();
}

bool
InvertOp::isIdentity(const Params& /*params*/) const
{
    return false;
}

void
InvertOp::row(const Params& /*params*/,
              const RowIO& io) const
{
    const int nComps = io.nComps;
    int indices[4] = { -1, -1, -1, -1 };
    int nProcessed = 0;

    for (int c = 0; (c < nComps) && (c < 4); ++c) {
        if (io.channels[pixelKernelChannelBit(nComps, c)]) {
            indices[nProcessed++] = c;
        }
    }
    const float* src = io.src[0];
    float* dst = io.dst;
    for (int x = 0; x < io.width; ++x, src += nComps, dst += nComps) {
        for (int i = 0; i < nProcessed; ++i) {
            dst[indices[i]] = 1.f - src[indices[i]];
        }
    }
}

GpuOpRef
InvertOp::gpu(const Params& /*params*/) const
{
#ifdef NATRON_HAVE_GPU
    GpuOpRef ref;

    ref.kind = GpuOpRef::Kind::Kernel;
    ref.kernel.kernel = (int)gpu::GpuKernelId::invert;

    return ref;
#else
    return GpuOpRef::none();
#endif
}

Invert::Invert(NodePtr node)
    : ImageNode<InvertOp>(node)
{
}

Invert::~Invert()
{
}

void
Invert::addAcceptedComponents(int inputNb,
                              std::list<ImageLayerDesc>* comps)
{
    StagedImageEffect::addAcceptedComponents(inputNb, comps);
    if (!isInputMask(inputNb)) {
        comps->push_back(ImageLayerDesc::getXYComponents());
    }
}

NATRON_NAMESPACE_EXIT
