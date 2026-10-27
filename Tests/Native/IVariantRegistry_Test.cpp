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

#include <set>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"

#include "Engine/Node.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Filter/ErodeDilate.h"
#include "Engine/Nodes/Filter/IBlur.h"
#include "Engine/Nodes/Filter/IErodeDilate.h"
#include "Engine/Nodes/IVariantRegistry.h"
#include "Engine/Nodes/Transform/ICornerPin.h"
#include "Engine/Nodes/Transform/ITransform.h"

NATRON_NAMESPACE_USING

namespace {
IVariantRegistry
testRegistry()
{
    return IVariantRegistry(std::vector<IVariant> {
        IVariant { "base.Blur", "test.IBlur", { { "size", "iSize" } } },
        IVariant { "base.Erode", "test.IErode", {} },
    });
}
} // namespace

TEST(IVariantRegistry, LooksUpFromTheBase)
{
    const IVariantRegistry registry = testRegistry();
    const IVariant* variant = registry.forBase("base.Blur");
    ASSERT_NE(nullptr, variant);
    EXPECT_EQ("test.IBlur", variant->iId);
    variant = registry.forBase("base.Erode");
    ASSERT_NE(nullptr, variant);
    EXPECT_EQ("test.IErode", variant->iId);
}

TEST(IVariantRegistry, LooksUpFromTheIVariant)
{
    const IVariantRegistry registry = testRegistry();
    const IVariant* variant = registry.forIVariant("test.IBlur");
    ASSERT_NE(nullptr, variant);
    EXPECT_EQ("base.Blur", variant->baseId);
}

TEST(IVariantRegistry, MissingEntriesReturnNull)
{
    const IVariantRegistry registry = testRegistry();
    EXPECT_EQ(nullptr, registry.forBase("test.IBlur")) << "an I-variant is not a base";
    EXPECT_EQ(nullptr, registry.forIVariant("base.Blur")) << "a base is not an I-variant";
    EXPECT_EQ(nullptr, registry.forBase("base.Unknown"));
    EXPECT_EQ(nullptr, registry.forIVariant("test.Unknown"));
    EXPECT_EQ(nullptr, IVariantRegistry(std::vector<IVariant>()).forBase("base.Blur"));
}

TEST(IVariantRegistry, KnobMapConvertsBothWaysAndDefaultsToTheName)
{
    const IVariantRegistry registry = testRegistry();
    const IVariant* variant = registry.forBase("base.Blur");
    ASSERT_NE(nullptr, variant);
    EXPECT_EQ("iSize", variant->iKnobFor("size"));
    EXPECT_EQ("size", variant->baseKnobFor("iSize"));
    EXPECT_EQ("channels", variant->iKnobFor("channels"));
    EXPECT_EQ("mapInvert", variant->baseKnobFor("mapInvert"));
}

TEST(IVariantRegistry, BuiltinIdsAreUnique)
{
    std::set<std::string> bases;
    std::set<std::string> iIds;
    for (const IVariant& variant : IVariantRegistry::builtin().variants()) {
        EXPECT_TRUE(bases.insert(variant.baseId).second) << variant.baseId << " has two I-variants";
        EXPECT_TRUE(iIds.insert(variant.iId).second) << variant.iId << " is the I-variant of two bases";
    }
    for (const std::string& iId : iIds) {
        EXPECT_EQ(0u, bases.count(iId)) << iId << " is both a base and an I-variant";
    }
}

TEST(IVariantRegistry, BuiltinPairsTheVariableSizeFilters)
{
    const std::pair<std::string, std::string> pairs[] = {
        { PLUGINID_NATRON_BLUR, PLUGINID_NATRON_IBLUR },
        { PLUGINID_NATRON_ERODE, PLUGINID_NATRON_IERODE },
        { PLUGINID_NATRON_DILATE, PLUGINID_NATRON_IDILATE },
        { PLUGINID_NATRON_TRANSFORM, PLUGINID_NATRON_ITRANSFORM },
        { PLUGINID_NATRON_CORNERPIN, PLUGINID_NATRON_ICORNERPIN },
    };
    const IVariantRegistry& registry = IVariantRegistry::builtin();

    for (const std::pair<std::string, std::string>& pair : pairs) {
        const IVariant* variant = registry.forBase(pair.first);
        ASSERT_NE(nullptr, variant) << pair.first << " has no I-variant";
        EXPECT_EQ(pair.second, variant->iId);
        EXPECT_EQ(variant, registry.forIVariant(pair.second));
    }
}

class IVariantRegistryNodeTest
    : public BaseTest {
};

// Every knob the registry pairs exists on its node, and each base node's size knob, where it has
// one, converts to a knob of its I-variant.
TEST_F(IVariantRegistryNodeTest, BuiltinKnobsExistOnBothNodes)
{
    for (const IVariant& variant : IVariantRegistry::builtin().variants()) {
        NodePtr base = createNode(QString::fromUtf8(variant.baseId.c_str()));
        NodePtr iNode = createNode(QString::fromUtf8(variant.iId.c_str()));
        ASSERT_NE(nullptr, base) << variant.baseId;
        ASSERT_NE(nullptr, iNode) << variant.iId;

        for (const IVariantKnobPair& pair : variant.knobMap) {
            EXPECT_NE(nullptr, base->getKnobByName(pair.baseKnob)) << variant.baseId << " has no knob " << pair.baseKnob;
            EXPECT_NE(nullptr, iNode->getKnobByName(pair.iKnob)) << variant.iId << " has no knob " << pair.iKnob;
        }
        // A variable warp has no size: its Map scales the whole transform.
        if (base->getKnobByName("size")) {
            EXPECT_NE(nullptr, iNode->getKnobByName(variant.iKnobFor("size"))) << variant.iId << " has no knob for the base's size";
        }
    }
}
