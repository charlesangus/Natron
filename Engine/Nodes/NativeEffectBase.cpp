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

#include "NativeEffectBase.h"

#include "Engine/Node.h"

NATRON_NAMESPACE_ENTER

NativeEffectBase::NativeEffectBase(NodePtr node)
    : EffectInstance(node)
{
}

NativeEffectBase::~NativeEffectBase()
{
}

std::string
NativeEffectBase::getInputLabel(int inputNb) const
{
    const NativePluginDescription desc = getNativePluginDescription();

    if ((inputNb < 0) || ((std::size_t)inputNb >= desc.inputs.size())) {
        return EffectInstance::getInputLabel(inputNb);
    }

    return desc.inputs[inputNb].label;
}

bool
NativeEffectBase::isInputOptional(int inputNb) const
{
    const NativePluginDescription desc = getNativePluginDescription();

    if ((inputNb < 0) || ((std::size_t)inputNb >= desc.inputs.size())) {
        return false;
    }

    return desc.inputs[inputNb].optional;
}

DataKindEnum
NativeEffectBase::getInputDataKind(int inputNb) const
{
    const NativePluginDescription desc = getNativePluginDescription();

    if ((inputNb < 0) || ((std::size_t)inputNb >= desc.inputs.size())) {
        return eDataKindImage;
    }

    return desc.inputs[inputNb].kind;
}

DataKindEnum
NativeEffectBase::resolveOutputDataKind(bool* isAmbiguous) const
{
    NodePtr node = getNode();

    if (!node) {
        if (isAmbiguous) {
            *isAmbiguous = false;
        }

        return eDataKindPolymorphic;
    }

    return node->resolveStructuralOutputDataKind(isAmbiguous);
}

void
NativeEffectBase::addAcceptedComponents(int /*inputNb*/,
                                        std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBComponents());
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
    comps->push_back(ImagePlaneDesc::getAlphaComponents());
}

void
NativeEffectBase::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthByte);
    depths->push_back(eImageBitDepthShort);
    depths->push_back(eImageBitDepthFloat);
}

NATRON_NAMESPACE_EXIT
