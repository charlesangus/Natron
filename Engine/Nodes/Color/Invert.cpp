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

#include <memory>
#include <string>

#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"

NATRON_NAMESPACE_ENTER

namespace {
class InvertKernel
    : public PixelKernel {
public:
    InvertKernel()
    {
    }

    virtual void processRow(const RowIO& io) const OVERRIDE FINAL
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
};

NativeImageTraits
invertTraits()
{
    NativeImageTraits traits;

    traits.hostUnPremult = true;

    return traits;
}
} // anonymous namespace

Invert::Invert(NodePtr node)
    : NativeImageEffect(node, invertTraits())
{
}

Invert::~Invert()
{
}

void
Invert::addAcceptedComponents(int inputNb,
                              std::list<ImageLayerDesc>* comps)
{
    NativeImageEffect::addAcceptedComponents(inputNb, comps);
    if (!isInputMask(inputNb)) {
        comps->push_back(ImageLayerDesc::getXYComponents());
    }
}

NativePluginDescription
Invert::getNativePluginDescription() const
{
    NativePluginDescription desc;

    desc.id = PLUGINID_NATRON_INVERT;
    desc.label = "Invert";
    desc.description = tr("Inverse the selected channels").toStdString();
    desc.grouping = PLUGIN_GROUP_COLOR;
    desc.majorVersion = PLUGIN_MAJOR_NATRON_INVERT;
    desc.minorVersion = 0;
    desc.inputs.push_back(NativeInputDescription("Source", false, eDataKindImage));
    desc.inputs.push_back(NativeInputDescription("Mask", true, eDataKindImage, true));
    desc.outputKind = eDataKindImage;

    return desc;
}

void
Invert::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    addMaskMixKnobs(page);
}

PixelKernelPtr
Invert::makeKernel(const KernelContext& /*context*/)
{
    return std::make_shared<InvertKernel>();
}

NATRON_NAMESPACE_EXIT
