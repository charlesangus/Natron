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

#ifndef Engine_Nodes_Network_PortTypes_h
#define Engine_Nodes_Network_PortTypes_h

#include "Global/Macros.h"

#include <string>

#include "Engine/Nodes/Network/PortDescriptor.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief An 8-bit sRGB colour. Plain data so Engine stays free of Gui types.
 **/
struct PortColor {
    unsigned char r = 255;
    unsigned char g = 255;
    unsigned char b = 255;

    bool operator==(const PortColor& other) const
    {
        return (r == other.r) && (g == other.g) && (b == other.b);
    }
};

enum class PortShape {
    Circle,
    Pill,
    Diamond,
};

/**
 * @brief How a port is drawn: the fill, the ring around it when it has one (custom types), and
 * the shape.
 **/
struct PortLook {
    PortColor fill;
    bool hasRing = false;
    PortColor ring;
    PortShape shape = PortShape::Circle;
};

/**
 * @brief The display colour of typeClass. Custom is white.
 **/
PortColor portTypeColor(PortTypeClass typeClass);

/**
 * @brief Array gives a pill, varying a diamond, otherwise a circle. An array that is also varying
 * is a diamond.
 **/
PortShape portShapeFor(bool array,
                       bool varying);

/**
 * @brief The ring colour of a custom type: a hue taken from a stable hash of typeName, so the same
 * name gets the same ring in every session.
 **/
PortColor customPortRingColor(const std::string& typeName);

/**
 * @brief The look of a port of the given class. type is the target's own type name, used for the
 * ring colour when typeClass is Custom.
 **/
PortLook portLookFor(PortTypeClass typeClass,
                     const std::string& type,
                     bool array,
                     bool varying);

PortLook portLookFor(const PortDescriptor& port);

/**
 * @brief Makes color the stripe colour drawn for target on the nodes and builders that list it,
 * replacing any it had.
 **/
void registerNetworkTargetColor(const std::string& target,
                                const PortColor& color);

/**
 * @brief Writes target's stripe colour to *color. False, leaving it untouched, when target has
 * none registered.
 **/
bool networkTargetColor(const std::string& target,
                        PortColor* color);

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Network_PortTypes_h
