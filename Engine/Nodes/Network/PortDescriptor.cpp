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

#include "PortDescriptor.h"

#include <mutex>

NATRON_NAMESPACE_ENTER

NATRON_NAMESPACE_ANONYMOUS_ENTER

struct PortTypeClassName {
    PortTypeClass typeClass;
    const char* name;
};

const PortTypeClassName kPortTypeClassNames[] = {
    { PortTypeClass::Float, "float" },
    { PortTypeClass::Int, "int" },
    { PortTypeClass::Vector3, "vector3" },
    { PortTypeClass::Normal, "normal" },
    { PortTypeClass::Point, "point" },
    { PortTypeClass::Color, "color" },
    { PortTypeClass::String, "string" },
    { PortTypeClass::Closure, "closure" },
    { PortTypeClass::Displacement, "displacement" },
    { PortTypeClass::Texture, "texture" },
    { PortTypeClass::Channel, "channel" },
    { PortTypeClass::Custom, "custom" },
};

std::mutex&
portTypeTablesMutex()
{
    static std::mutex mutex;

    return mutex;
}

std::map<std::string, std::map<std::string, PortTypeClass>>&
portTypeTables()
{
    static std::map<std::string, std::map<std::string, PortTypeClass>> tables;

    return tables;
}

NATRON_NAMESPACE_ANONYMOUS_EXIT

const char*
portTypeClassName(PortTypeClass typeClass)
{
    for (const PortTypeClassName& entry : kPortTypeClassNames) {
        if (entry.typeClass == typeClass) {
            return entry.name;
        }
    }

    return "custom";
}

bool
portTypeClassFromName(const std::string& name,
                      PortTypeClass* typeClass)
{
    for (const PortTypeClassName& entry : kPortTypeClassNames) {
        if (name == entry.name) {
            *typeClass = entry.typeClass;

            return true;
        }
    }

    return false;
}

void
registerPortTypeTable(const std::string& target,
                      const std::map<std::string, PortTypeClass>& table)
{
    std::lock_guard<std::mutex> lock(portTypeTablesMutex());

    portTypeTables()[target] = table;
}

PortTypeClass
portTypeClassFor(const std::string& target,
                 const std::string& type)
{
    std::lock_guard<std::mutex> lock(portTypeTablesMutex());
    const std::map<std::string, std::map<std::string, PortTypeClass>>& tables = portTypeTables();
    const auto table = tables.find(target);

    if (table == tables.end()) {
        return PortTypeClass::Custom;
    }
    const auto it = table->second.find(type);

    return (it == table->second.end()) ? PortTypeClass::Custom : it->second;
}

NATRON_NAMESPACE_EXIT
