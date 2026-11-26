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

#include "Global/Macros.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Nodes/Network/PortDescriptor.h"
#include "Engine/Nodes/Network/PortTypes.h"
#include "Engine/Nodes/Scene/Shaders/ShaderManifest.h"

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

OutputPortDescriptor
outputOf(const std::string& type,
         PortTypeClass typeClass)
{
    OutputPortDescriptor output;
    output.name = "out";
    output.type = type;
    output.typeClass = typeClass;

    return output;
}

PortDescriptor
portOf(const std::string& type,
       PortTypeClass typeClass)
{
    PortDescriptor port;
    port.attribute = "in";
    port.type = type;
    port.typeClass = typeClass;

    return port;
}

const std::vector<std::string> kCycles = { kShaderTargetCycles };
const std::vector<std::string> kMoonRay = { kShaderTargetMoonRay };

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

TEST(PortTypes, floatFeedsColorOnCycles)
{
    registerShaderPortTypeTables();
    std::string reason;
    EXPECT_TRUE(canConnectPorts(kCycles, outputOf("float", PortTypeClass::Float), kCycles, portOf("color", PortTypeClass::Color), &reason));
    EXPECT_TRUE(reason.empty());
}

TEST(PortTypes, closureCannotFeedFloat)
{
    registerShaderPortTypeTables();
    std::string reason;
    EXPECT_FALSE(canConnectPorts(kCycles, outputOf("closure", PortTypeClass::Closure), kCycles, portOf("float", PortTypeClass::Float), &reason));
    EXPECT_EQ(std::string("closure cannot feed float"), reason);
}

TEST(PortTypes, nodesWithoutASharedTargetNeverConnect)
{
    registerShaderPortTypeTables();
    std::string reason;
    EXPECT_FALSE(canConnectPorts(kCycles, outputOf("float", PortTypeClass::Float), kMoonRay, portOf("float", PortTypeClass::Float), &reason));
    EXPECT_EQ(std::string("no shared target"), reason);
}

TEST(PortTypes, unregisteredTargetConnectsOnlyItsOwnClass)
{
    const std::vector<std::string> bare = { "unregistered" };
    EXPECT_TRUE(canConnectPorts(bare, outputOf("color", PortTypeClass::Color), bare, portOf("color", PortTypeClass::Color)));
    EXPECT_FALSE(canConnectPorts(bare, outputOf("float", PortTypeClass::Float), bare, portOf("color", PortTypeClass::Color)));
    EXPECT_FALSE(canConnectPorts(bare, outputOf("typeA", PortTypeClass::Custom), bare, portOf("typeB", PortTypeClass::Custom)));
}

NATRON_NAMESPACE_EXIT
