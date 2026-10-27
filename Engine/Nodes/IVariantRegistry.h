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

#ifndef Engine_Nodes_IVariantRegistry_h
#define Engine_Nodes_IVariantRegistry_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>
#include <vector>

NATRON_NAMESPACE_ENTER

/**
 * @brief A base knob and the I-variant knob it converts to.
 **/
struct IVariantKnobPair {
    std::string baseKnob;
    std::string iKnob;
};

/**
 * @brief A base node and its I-variant: the node that takes a Map input scaling its effect per
 * pixel. knobMap lists only the knobs whose names differ; every other knob converts to the knob
 * of the same name, when the other node has one.
 **/
struct IVariant {
    std::string baseId;
    std::string iId;
    std::vector<IVariantKnobPair> knobMap;

    std::string iKnobFor(const std::string& baseKnob) const WARN_UNUSED_RETURN;

    std::string baseKnobFor(const std::string& iKnob) const WARN_UNUSED_RETURN;
};

/**
 * @brief The base node to I-variant pairs. The built-in registry is filled from the native
 * registration fragments: beside its NATRON_NATIVE_NODE line, an I-node's fragment holds
 *
 *     NATRON_I_VARIANT(BASE_ID, I_ID, { "baseKnob", "iKnob" }, ...)
 *
 * naming the base plug-in ID, its own ID and any knob pairs. The IDs may be the PLUGINID_ macros
 * of any native node header.
 **/
class IVariantRegistry {
public:
    explicit IVariantRegistry(std::vector<IVariant> variants);

    static const IVariantRegistry& builtin() WARN_UNUSED_RETURN;

    /**
     * @brief The I-variant of the base node baseId, or null when it has none.
     **/
    const IVariant* forBase(const std::string& baseId) const WARN_UNUSED_RETURN;

    /**
     * @brief The entry whose I-variant is iId, or null when iId is no I-variant.
     **/
    const IVariant* forIVariant(const std::string& iId) const WARN_UNUSED_RETURN;

    const std::vector<IVariant>& variants() const
    {
        return _variants;
    }

private:
    std::vector<IVariant> _variants;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_IVariantRegistry_h
