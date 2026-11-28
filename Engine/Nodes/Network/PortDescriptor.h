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

#ifndef Engine_Nodes_Network_PortDescriptor_h
#define Engine_Nodes_Network_PortDescriptor_h

#include "Global/Macros.h"

#include <map>
#include <string>

NATRON_NAMESPACE_ENTER

/**
 * @brief The core class of a port's type. Each target maps the type names of its renderer or
 * language onto these in one table (registerPortTypeTable()), so code that only needs to know
 * what a port carries never reads a target's own names.
 **/
enum class PortTypeClass {
    Float,
    Int,
    Vector3,
    Normal,
    Point,
    Color,
    String,
    Closure,
    Displacement,
    Texture,
    Channel,
    Custom,
};

const char* portTypeClassName(PortTypeClass typeClass);

/**
 * @brief Writes the class named name ("float", "vector3", ...) to *typeClass. False, leaving it
 * untouched, for an unknown name.
 **/
bool portTypeClassFromName(const std::string& name,
                           PortTypeClass* typeClass);

/**
 * @brief Makes table the type table of target, replacing any it had. Main thread, before the
 * first lookup for target.
 **/
void registerPortTypeTable(const std::string& target,
                           const std::map<std::string, PortTypeClass>& table);

/**
 * @brief The class target's table gives type, Custom when target has no table or the table does
 * not list type.
 **/
PortTypeClass portTypeClassFor(const std::string& target,
                               const std::string& type);

/**
 * @brief One input port of a network node: an attribute that can be connected. The node's
 * inputs are its shown ports, in order.
 **/
struct PortDescriptor {
    // The attribute's script name, which is also the input's label and the name of its knob, if
    // it has one.
    std::string attribute;
    // The target's own name for the port's type.
    std::string type;
    PortTypeClass typeClass = PortTypeClass::Custom;
    // The label of the page the attribute's knob, or the port alone, belongs on.
    std::string page;
    bool shown = false;
    bool array = false;
    // The value can differ per point, sample or shading point, rather than once per evaluation.
    bool varying = false;

    bool operator==(const PortDescriptor& other) const
    {
        return (attribute == other.attribute) && (type == other.type) && (typeClass == other.typeClass) && (page == other.page) && (shown == other.shown) && (array == other.array) && (varying == other.varying);
    }
};

/**
 * @brief One named output of a network node. An input reads its upstream node's first output
 * unless the downstream node names another for it.
 **/
struct OutputPortDescriptor {
    std::string name;
    std::string type;
    PortTypeClass typeClass = PortTypeClass::Custom;
    bool array = false;

    bool operator==(const OutputPortDescriptor& other) const
    {
        return (name == other.name) && (type == other.type) && (typeClass == other.typeClass) && (array == other.array);
    }
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Network_PortDescriptor_h
