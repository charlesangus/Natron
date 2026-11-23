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

#include "KnobLayerCustomData.h"

NATRON_NAMESPACE_ENTER

const std::string KnobLayerCustomData::_typeNameStr("LayerCustomData");

const std::string&
KnobLayerCustomData::typeNameStatic()
{
    return _typeNameStr;
}

KnobLayerCustomData::KnobLayerCustomData(KnobHolder* holder,
                                         const std::string& description,
                                         int dimension,
                                         bool declaredByPlugin)
    : KnobTable(holder, description, dimension, declaredByPlugin)
{
}

KnobLayerCustomData::~KnobLayerCustomData()
{
}

std::string
KnobLayerCustomData::getColumnLabel(int col) const
{
    switch (col) {
    case 0:
        return tr("Name").toStdString();
    case 1:
        return tr("Type").toStdString();
    case 2:
        return tr("Value").toStdString();
    default:
        return std::string();
    }
}

std::string
KnobLayerCustomData::getColumnTag(int col) const
{
    switch (col) {
    case 0:
        return "Name";
    case 1:
        return "Type";
    case 2:
        return "Value";
    default:
        return std::string();
    }
}

const std::vector<std::string>&
KnobLayerCustomData::typeNames()
{
    static const std::vector<std::string> names = {
        "String", "String Array", "Token", "Token Array", "Float", "Integer", "Boolean"
    };

    return names;
}

NATRON_NAMESPACE_EXIT
