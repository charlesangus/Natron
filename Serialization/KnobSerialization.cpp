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

#include "Serialization/KnobSerialization.h"

#include <locale>

NATRON_NAMESPACE_ENTER
namespace Serialization {

KnobTypeTraits
getKnobTypeTraits(const std::string& typeName)
{
    KnobTypeTraits traits;

    traits.isKnown = true;
    if (typeName == "Int") {
        traits.storageType = eKnobStorageTypeInt;
        traits.hasValue = true;
        traits.isInt = true;
    } else if (typeName == "Choice") {
        traits.storageType = eKnobStorageTypeInt;
        traits.hasValue = true;
        traits.isChoice = true;
    } else if (typeName == "Bool") {
        traits.storageType = eKnobStorageTypeBool;
        traits.hasValue = true;
        traits.isBool = true;
    } else if ((typeName == "Button") || (typeName == "Separator") || (typeName == "Group") || (typeName == "Page")) {
        traits.storageType = eKnobStorageTypeBool;
    } else if (typeName == "Double") {
        traits.storageType = eKnobStorageTypeDouble;
        traits.hasValue = true;
        traits.isDouble = true;
    } else if (typeName == "Color") {
        traits.storageType = eKnobStorageTypeDouble;
        traits.hasValue = true;
        traits.isColor = true;
    } else if (typeName == "Parametric") {
        traits.storageType = eKnobStorageTypeDouble;
        traits.isParametric = true;
    } else if (typeName == "String") {
        traits.storageType = eKnobStorageTypeString;
        traits.hasValue = true;
        traits.hasStringAnimation = true;
        traits.isString = true;
    } else if (typeName == "InputFile") {
        traits.storageType = eKnobStorageTypeString;
        traits.hasValue = true;
        traits.hasStringAnimation = true;
        traits.isInputFile = true;
    } else if (typeName == "OutputFile") {
        traits.storageType = eKnobStorageTypeString;
        traits.hasValue = true;
        traits.isOutputFile = true;
    } else if (typeName == "Path") {
        traits.storageType = eKnobStorageTypeString;
        traits.hasValue = true;
        traits.isPath = true;
    } else if (typeName == "LayerSelect") {
        traits.storageType = eKnobStorageTypeString;
        traits.hasValue = true;
        traits.isLayerSelect = true;
    } else if ((typeName == "Layers") || (typeName == "ChannelSet") || (typeName == "ChannelSelect") || (typeName == "ShuffleMap")) {
        traits.storageType = eKnobStorageTypeString;
        traits.hasValue = true;
    } else {
        traits.isKnown = false;
    }

    return traits;
}

std::string
scriptFriendlyKnobName(const std::string& name)
{
    if (name == "from") {
        return "pFrom";
    } else if (name == "lambda") {
        return "pLambda";
    }
    std::locale locale;
    std::string friendlyName;
    for (std::size_t i = 0; i < name.size(); ++i) {
        if (friendlyName.empty() && std::isdigit(name[i], locale)) {
            friendlyName.push_back('p');
            friendlyName.push_back(name[i]);
            continue;
        }
        if (std::isspace(name[i], locale)) {
            friendlyName.push_back('_');
        } else if ((name[i] == '_') || std::isalnum(name[i], locale)) {
            friendlyName.push_back(name[i]);
        }
    }

    return friendlyName;
}

} // namespace Serialization
NATRON_NAMESPACE_EXIT
