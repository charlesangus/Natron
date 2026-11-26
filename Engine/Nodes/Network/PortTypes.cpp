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

#include "PortTypes.h"

#include <cmath>
#include <cstdint>

NATRON_NAMESPACE_ENTER

PortColor
portTypeColor(PortTypeClass typeClass)
{
    switch (typeClass) {
    case PortTypeClass::Float:
        return { 200, 200, 200 };
    case PortTypeClass::Int:
        return { 160, 220, 60 };
    case PortTypeClass::Vector3:
        return { 40, 60, 160 };
    case PortTypeClass::Normal:
    case PortTypeClass::Point:
        return { 110, 90, 220 };
    case PortTypeClass::Color:
        return { 240, 210, 40 };
    case PortTypeClass::String:
        return { 130, 190, 240 };
    case PortTypeClass::Closure:
        return { 40, 220, 90 };
    case PortTypeClass::Displacement:
        return { 30, 170, 160 };
    case PortTypeClass::Texture:
        return { 250, 180, 120 };
    case PortTypeClass::Channel:
        return { 220, 50, 200 };
    case PortTypeClass::Custom:
        break;
    }
    return { 255, 255, 255 };
}

PortShape
portShapeFor(bool array,
             bool varying)
{
    if (varying) {
        return PortShape::Diamond;
    }
    return array ? PortShape::Pill : PortShape::Circle;
}

PortColor
customPortRingColor(const std::string& typeName)
{
    // FNV-1a, so the hue does not depend on the standard library's std::hash.
    std::uint32_t hash = 2166136261u;
    for (unsigned char c : typeName) {
        hash ^= c;
        hash *= 16777619u;
    }
    const double hue = (hash % 360u) / 60.0;
    const double saturation = 0.75;
    const double value = 0.95;
    const int sector = static_cast<int>(hue);
    const double f = hue - sector;
    const double p = value * (1. - saturation);
    const double q = value * (1. - saturation * f);
    const double t = value * (1. - saturation * (1. - f));
    double r = value, g = t, b = p;
    switch (sector) {
    case 1:
        r = q;
        g = value;
        b = p;
        break;
    case 2:
        r = p;
        g = value;
        b = t;
        break;
    case 3:
        r = p;
        g = q;
        b = value;
        break;
    case 4:
        r = t;
        g = p;
        b = value;
        break;
    case 5:
        r = value;
        g = p;
        b = q;
        break;
    default:
        break;
    }
    PortColor color;
    color.r = static_cast<unsigned char>(std::lround(r * 255.));
    color.g = static_cast<unsigned char>(std::lround(g * 255.));
    color.b = static_cast<unsigned char>(std::lround(b * 255.));
    return color;
}

PortLook
portLookFor(PortTypeClass typeClass,
            const std::string& type,
            bool array,
            bool varying)
{
    PortLook look;
    look.fill = portTypeColor(typeClass);
    look.shape = portShapeFor(array, varying);
    if (typeClass == PortTypeClass::Custom) {
        look.hasRing = true;
        look.ring = customPortRingColor(type);
    }
    return look;
}

PortLook
portLookFor(const PortDescriptor& port)
{
    return portLookFor(port.typeClass, port.type, port.array, port.varying);
}

NATRON_NAMESPACE_EXIT
