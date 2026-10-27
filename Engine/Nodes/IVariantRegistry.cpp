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

#include "IVariantRegistry.h"

#include <utility>

// The fragments may name their IDs through any native node's PLUGINID_ macros.
#define NATRON_NATIVE_REGISTRY_HEADERS
#include "NativeRegistryAll.inc"
#undef NATRON_NATIVE_REGISTRY_HEADERS

NATRON_NAMESPACE_ENTER

std::string
IVariant::iKnobFor(const std::string& baseKnob) const
{
    for (const IVariantKnobPair& pair : knobMap) {
        if (pair.baseKnob == baseKnob) {
            return pair.iKnob;
        }
    }

    return baseKnob;
}

std::string
IVariant::baseKnobFor(const std::string& iKnob) const
{
    for (const IVariantKnobPair& pair : knobMap) {
        if (pair.iKnob == iKnob) {
            return pair.baseKnob;
        }
    }

    return iKnob;
}

IVariantRegistry::IVariantRegistry(std::vector<IVariant> variants)
    : _variants(std::move(variants))
{
}

const IVariantRegistry&
IVariantRegistry::builtin()
{
    static const IVariantRegistry registry(std::vector<IVariant> {
#define NATRON_I_VARIANT(BASE_ID, I_ID, ...) IVariant { BASE_ID, I_ID, { __VA_ARGS__ } },
#include "NativeRegistryAll.inc"
#undef NATRON_I_VARIANT
    });

    return registry;
}

const IVariant*
IVariantRegistry::forBase(const std::string& baseId) const
{
    for (const IVariant& variant : _variants) {
        if (variant.baseId == baseId) {
            return &variant;
        }
    }

    return nullptr;
}

const IVariant*
IVariantRegistry::forIVariant(const std::string& iId) const
{
    for (const IVariant& variant : _variants) {
        if (variant.iId == iId) {
            return &variant;
        }
    }

    return nullptr;
}

NATRON_NAMESPACE_EXIT
