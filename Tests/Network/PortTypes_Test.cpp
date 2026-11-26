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

#include <gtest/gtest.h>

#include "Global/Macros.h"

#include "Engine/Nodes/Network/PortDescriptor.h"
#include "Engine/Nodes/Network/PortTypes.h"

NATRON_NAMESPACE_ENTER

namespace {

const PortTypeClass kAllClasses[] = {
    PortTypeClass::Float,
    PortTypeClass::Int,
    PortTypeClass::Vector3,
    PortTypeClass::Normal,
    PortTypeClass::Point,
    PortTypeClass::Color,
    PortTypeClass::String,
    PortTypeClass::Closure,
    PortTypeClass::Displacement,
    PortTypeClass::Texture,
    PortTypeClass::Channel,
    PortTypeClass::Custom,
};

} // namespace

TEST(PortTypes, everyClassHasAColour)
{
    for (PortTypeClass typeClass : kAllClasses) {
        const PortLook look = portLookFor(typeClass, "any", false, false);
        EXPECT_EQ(look.fill, portTypeColor(typeClass)) << portTypeClassName(typeClass);
    }
    // Every non-custom class is distinct from white, except where the table groups classes.
    for (PortTypeClass typeClass : kAllClasses) {
        if (typeClass == PortTypeClass::Custom) {
            continue;
        }
        EXPECT_FALSE(portTypeColor(typeClass) == portTypeColor(PortTypeClass::Custom)) << portTypeClassName(typeClass);
    }
    EXPECT_TRUE(portTypeColor(PortTypeClass::Normal) == portTypeColor(PortTypeClass::Point));
}

TEST(PortTypes, shapeFollowsArrayAndVarying)
{
    EXPECT_EQ(PortShape::Circle, portShapeFor(false, false));
    EXPECT_EQ(PortShape::Pill, portShapeFor(true, false));
    EXPECT_EQ(PortShape::Diamond, portShapeFor(false, true));
    EXPECT_EQ(PortShape::Diamond, portShapeFor(true, true));
}

TEST(PortTypes, customTypesAreWhiteWithDistinctRings)
{
    const PortLook a = portLookFor(PortTypeClass::Custom, "typeA", false, false);
    const PortLook b = portLookFor(PortTypeClass::Custom, "typeB", false, false);
    EXPECT_TRUE(a.hasRing);
    EXPECT_TRUE(b.hasRing);
    EXPECT_TRUE(a.fill == PortColor());
    EXPECT_FALSE(a.ring == b.ring);
    EXPECT_TRUE(a.ring == portLookFor(PortTypeClass::Custom, "typeA", true, false).ring);

    const PortLook plain = portLookFor(PortTypeClass::Color, "color", false, false);
    EXPECT_FALSE(plain.hasRing);
}

TEST(PortTypes, descriptorOverloadUsesItsFlags)
{
    PortDescriptor port;
    port.type = "float";
    port.typeClass = PortTypeClass::Float;
    port.varying = true;
    const PortLook look = portLookFor(port);
    EXPECT_EQ(PortShape::Diamond, look.shape);
    EXPECT_TRUE(look.fill == portTypeColor(PortTypeClass::Float));
}

NATRON_NAMESPACE_EXIT
