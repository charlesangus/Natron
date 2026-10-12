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

#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include "Serialization/BoostXml/ProjectGuiXml.h"
#include "Serialization/ProjectGuiSerialization.h"

NATRON_NAMESPACE_USING

namespace {

// The node GUI layout of the version before the user colour, which stored a
// single node colour and nothing to tell a user's pick from the category default.
struct LegacyNodeGuiSerialization {
    std::string nodeName = "Blur1";
    double posX = 10.;
    double posY = 20.;
    bool previewEnabled = false;
    float r = 0.8f;
    float g = 0.5f;
    float b = 0.3f;
    bool selected = false;
    double width = 80.;
    double height = 30.;
    bool hasOverlayColor = false;

    template <class Archive>
    void serialize(Archive& ar,
                   const unsigned int /*version*/)
    {
        ar& ::boost::serialization::make_nvp("Name", nodeName);
        ar& ::boost::serialization::make_nvp("X_position", posX);
        ar& ::boost::serialization::make_nvp("Y_position", posY);
        ar& ::boost::serialization::make_nvp("Preview_enabled", previewEnabled);
        ar& ::boost::serialization::make_nvp("r", r);
        ar& ::boost::serialization::make_nvp("g", g);
        ar& ::boost::serialization::make_nvp("b", b);
        ar& ::boost::serialization::make_nvp("Selected", selected);
        ar& ::boost::serialization::make_nvp("Width", width);
        ar& ::boost::serialization::make_nvp("Height", height);
        ar& ::boost::serialization::make_nvp("HasOverlayColor", hasOverlayColor);
        int nodesCount = 0;
        ar& ::boost::serialization::make_nvp("Children", nodesCount);
    }
};

Serialization::NodeGuiSerialization
roundTrip(const Serialization::NodeGuiSerialization& original)
{
    std::stringstream stream;
    {
        boost::archive::xml_oarchive oArchive(stream);
        oArchive << boost::serialization::make_nvp("NodeGui", original);
    }
    Serialization::NodeGuiSerialization restored;
    {
        boost::archive::xml_iarchive iArchive(stream);
        iArchive >> boost::serialization::make_nvp("NodeGui", restored);
    }

    return restored;
}

Serialization::NodeGuiSerialization
loadLegacy(const LegacyNodeGuiSerialization& legacy)
{
    std::stringstream stream;
    {
        boost::archive::xml_oarchive oArchive(stream);
        oArchive << boost::serialization::make_nvp("NodeGui", legacy);
    }
    Serialization::NodeGuiSerialization restored;
    {
        boost::archive::xml_iarchive iArchive(stream);
        iArchive >> boost::serialization::make_nvp("NodeGui", restored);
    }

    return restored;
}

} // namespace

BOOST_CLASS_VERSION(LegacyNodeGuiSerialization, NATRON_SERIALIZATION_NODE_GUI_INTRODUCES_CHILDREN)

TEST(NodeGuiUserColorSerialization, RoundTripsAUserColor)
{
    Serialization::NodeGuiSerialization original;
    original.hasUserColor = true;
    original.userColorR = 0.25f;
    original.userColorG = 0.5f;
    original.userColorB = 0.75f;

    Serialization::NodeGuiSerialization restored = roundTrip(original);

    const float category[3] = { 0.f, 0.f, 0.f };
    float r = -1.f, g = -1.f, b = -1.f;
    ASSERT_TRUE(Serialization::resolveNodeGuiUserColor(restored, category, NULL, &r, &g, &b));
    EXPECT_FLOAT_EQ(0.25f, r);
    EXPECT_FLOAT_EQ(0.5f, g);
    EXPECT_FLOAT_EQ(0.75f, b);
}

TEST(NodeGuiUserColorSerialization, ACurrentArchiveWithoutAUserColorNeverGuessesOne)
{
    Serialization::NodeGuiSerialization restored = roundTrip(Serialization::NodeGuiSerialization());

    const float category[3] = { 0.9f, 0.9f, 0.9f };
    float r, g, b;
    EXPECT_FALSE(Serialization::resolveNodeGuiUserColor(restored, category, NULL, &r, &g, &b));
}

TEST(NodeGuiUserColorSerialization, ALegacyColorFarFromTheCategoryColorSeedsAUserColor)
{
    const LegacyNodeGuiSerialization legacy;
    Serialization::NodeGuiSerialization restored = loadLegacy(legacy);

    ASSERT_TRUE(restored.colorWasFound);
    EXPECT_FLOAT_EQ(legacy.r, restored.colorR);
    EXPECT_FLOAT_EQ(legacy.g, restored.colorG);
    EXPECT_FLOAT_EQ(legacy.b, restored.colorB);

    const float category[3] = { 0.8f, 0.5f, 0.2f };
    const float legacyCategory[3] = { 0.1f, 0.1f, 0.1f };
    float r = -1.f, g = -1.f, b = -1.f;
    ASSERT_TRUE(Serialization::resolveNodeGuiUserColor(restored, category, legacyCategory, &r, &g, &b));
    EXPECT_FLOAT_EQ(legacy.r, r);
    EXPECT_FLOAT_EQ(legacy.g, g);
    EXPECT_FLOAT_EQ(legacy.b, b);
}

TEST(NodeGuiUserColorSerialization, ALegacyColorNearTheCategoryColorSeedsNoUserColor)
{
    const LegacyNodeGuiSerialization legacy;
    Serialization::NodeGuiSerialization restored = loadLegacy(legacy);

    const float category[3] = { 0.82f, 0.48f, 0.33f };
    float r, g, b;
    EXPECT_FALSE(Serialization::resolveNodeGuiUserColor(restored, category, NULL, &r, &g, &b));
}

// A pre-user-colour archive stored whatever the older grouping ladder resolved to, which can
// differ from the current category colour (e.g. a node that changed category), and that
// stored default must not be mistaken for a colour the user picked.
TEST(NodeGuiUserColorSerialization, ALegacyColorNearTheOldLadderColorSeedsNoUserColor)
{
    const LegacyNodeGuiSerialization legacy;
    Serialization::NodeGuiSerialization restored = loadLegacy(legacy);

    const float category[3] = { 0.1f, 0.1f, 0.4f };
    const float legacyCategory[3] = { 0.78f, 0.52f, 0.28f };
    float r, g, b;
    EXPECT_FALSE(Serialization::resolveNodeGuiUserColor(restored, category, legacyCategory, &r, &g, &b));
}

// Without a category colour there is no default to compare against, so nothing is guessed.
TEST(NodeGuiUserColorSerialization, ALegacyColorWithNoKnownCategoryColorSeedsNoUserColor)
{
    const LegacyNodeGuiSerialization legacy;
    Serialization::NodeGuiSerialization restored = loadLegacy(legacy);

    float r, g, b;
    EXPECT_FALSE(Serialization::resolveNodeGuiUserColor(restored, NULL, NULL, &r, &g, &b));
}
