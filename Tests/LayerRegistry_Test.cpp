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

#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/ImageLayerDesc.h"
#include "Engine/LayerRegistry.h"

NATRON_NAMESPACE_USING

TEST(LayerRegistry, BuiltinOrder)
{
    LayerRegistry registry;
    std::shared_ptr<const std::vector<LayerRegistryEntry>> snap = registry.snapshot();

    ASSERT_EQ(6u, snap->size());
    EXPECT_EQ(std::string(kNatronColorLayerID), (*snap)[0].desc.getLayerID());
    EXPECT_EQ(LayerRegistryEntry::eOriginBuiltin, (*snap)[0].origin);
    EXPECT_EQ(std::string(kNatronDisparityLeftLayerID), (*snap)[1].desc.getLayerID());
    EXPECT_EQ(std::string(kNatronDisparityRightLayerID), (*snap)[2].desc.getLayerID());
    EXPECT_EQ(std::string(kNatronBackwardMotionVectorsLayerID), (*snap)[3].desc.getLayerID());
    EXPECT_EQ(std::string(kNatronForwardMotionVectorsLayerID), (*snap)[4].desc.getLayerID());
    EXPECT_EQ("depth", (*snap)[5].desc.getLayerID());
    EXPECT_EQ(LayerRegistryEntry::eOriginUser, (*snap)[5].origin);
}

TEST(LayerRegistry, RgbaAliasReturnsColorAndRegistersNothing)
{
    LayerRegistry registry;
    std::string error;
    std::vector<std::string> ch(1, "R");
    ImageLayerDesc rgba("rgba", "rgba", "", ch);

    EXPECT_EQ(LayerRegistry::eAddResultUnchanged, registry.add(rgba, LayerRegistryEntry::eOriginUser, &error));

    ImageLayerDesc found;
    EXPECT_FALSE(registry.find("rgba", &found));
    EXPECT_TRUE(registry.find(kNatronColorLayerID, &found));
}

TEST(LayerRegistry, ReservedNamesRefused)
{
    LayerRegistry registry;
    std::string error;
    std::vector<std::string> ch(1, "A");

    ImageLayerDesc none("none", "none", "", ch);
    EXPECT_EQ(LayerRegistry::eAddResultRefused, registry.add(none, LayerRegistryEntry::eOriginUser, &error));

    ImageLayerDesc all("all", "all", "", ch);
    EXPECT_EQ(LayerRegistry::eAddResultRefused, registry.add(all, LayerRegistryEntry::eOriginUser, &error));

    ImageLayerDesc backward("Backward", "Backward", "", ch);
    EXPECT_EQ(LayerRegistry::eAddResultRefused, registry.add(backward, LayerRegistryEntry::eOriginUser, &error));
}

TEST(LayerRegistry, DottedIdRefusedUnlessFromFile)
{
    LayerRegistry registry;
    std::string error;
    std::vector<std::string> ch(1, "R");
    ImageLayerDesc dotted("light1.diffuse", "light1.diffuse", "", ch);

    EXPECT_EQ(LayerRegistry::eAddResultRefused, registry.add(dotted, LayerRegistryEntry::eOriginUser, &error));
    EXPECT_EQ(LayerRegistry::eAddResultAdded, registry.add(dotted, LayerRegistryEntry::eOriginFile, &error));
    EXPECT_TRUE(registry.contains("light1.diffuse"));
}

TEST(LayerRegistry, FiveChannelsRefused)
{
    LayerRegistry registry;
    std::string error;
    std::vector<std::string> ch;
    ch.push_back("R");
    ch.push_back("G");
    ch.push_back("B");
    ch.push_back("A");
    ch.push_back("Z");
    ImageLayerDesc five("five", "five", "", ch);

    EXPECT_EQ(LayerRegistry::eAddResultRefused, registry.add(five, LayerRegistryEntry::eOriginUser, &error));
    EXPECT_FALSE(error.empty());
}

TEST(LayerRegistry, DuplicateIdenticalIsUnchanged)
{
    LayerRegistry registry;
    std::string error;
    std::vector<std::string> ch;
    ch.push_back("R");
    ch.push_back("G");
    ch.push_back("B");
    ImageLayerDesc diffuse("diffuse", "diffuse", "", ch);

    ASSERT_EQ(LayerRegistry::eAddResultAdded, registry.add(diffuse, LayerRegistryEntry::eOriginFile, &error));
    EXPECT_EQ(LayerRegistry::eAddResultUnchanged, registry.add(diffuse, LayerRegistryEntry::eOriginFile, &error));
}

TEST(LayerRegistry, FileUnionGrowsChannels)
{
    LayerRegistry registry;
    std::string error;
    std::vector<std::string> rgb;
    rgb.push_back("R");
    rgb.push_back("G");
    rgb.push_back("B");
    ImageLayerDesc diffuseRgb("diffuse", "diffuse", "", rgb);
    ASSERT_EQ(LayerRegistry::eAddResultAdded, registry.add(diffuseRgb, LayerRegistryEntry::eOriginFile, &error));

    std::vector<std::string> rgba;
    rgba.push_back("R");
    rgba.push_back("G");
    rgba.push_back("B");
    rgba.push_back("A");
    ImageLayerDesc diffuseRgba("diffuse", "diffuse", "", rgba);
    EXPECT_EQ(LayerRegistry::eAddResultGrown, registry.add(diffuseRgba, LayerRegistryEntry::eOriginFile, &error));

    ImageLayerDesc found;
    ASSERT_TRUE(registry.find("diffuse", &found));
    ASSERT_EQ(4, found.getNumComponents());
    EXPECT_EQ("R", found.getChannels()[0]);
    EXPECT_EQ("G", found.getChannels()[1]);
    EXPECT_EQ("B", found.getChannels()[2]);
    EXPECT_EQ("A", found.getChannels()[3]);
}

TEST(LayerRegistry, UserConflictRefused)
{
    LayerRegistry registry;
    std::string error;
    std::vector<std::string> rgb;
    rgb.push_back("R");
    rgb.push_back("G");
    rgb.push_back("B");
    ImageLayerDesc diffuseRgb("diffuse", "diffuse", "", rgb);
    ASSERT_EQ(LayerRegistry::eAddResultAdded, registry.add(diffuseRgb, LayerRegistryEntry::eOriginUser, &error));

    std::vector<std::string> rgba;
    rgba.push_back("R");
    rgba.push_back("G");
    rgba.push_back("B");
    rgba.push_back("A");
    ImageLayerDesc diffuseRgba("diffuse", "diffuse", "", rgba);
    EXPECT_EQ(LayerRegistry::eAddResultRefused, registry.add(diffuseRgba, LayerRegistryEntry::eOriginUser, &error));
    EXPECT_FALSE(error.empty());
}

TEST(LayerRegistry, RemoveBuiltinRefused)
{
    LayerRegistry registry;
    std::string error;

    EXPECT_FALSE(registry.remove(kNatronColorLayerID, &error));
    EXPECT_FALSE(error.empty());
}

TEST(LayerRegistry, RemoveDepthSucceeds)
{
    LayerRegistry registry;
    std::string error;

    EXPECT_TRUE(registry.remove("depth", &error));
    EXPECT_FALSE(registry.contains("depth"));
}

TEST(LayerRegistry, GroupChannelNames)
{
    std::vector<std::string> flat;
    flat.push_back("R");
    flat.push_back("G");
    flat.push_back("B");
    flat.push_back("A");
    flat.push_back("Z");
    flat.push_back("diffuse.R");
    flat.push_back("diffuse.G");

    std::vector<ImageLayerDesc> layers;
    LayerRegistry::groupChannelNames(flat, &layers);

    ASSERT_EQ(3u, layers.size());

    EXPECT_EQ(std::string(kNatronColorLayerID), layers[0].getLayerID());
    ASSERT_EQ(4, layers[0].getNumComponents());
    EXPECT_EQ("R", layers[0].getChannels()[0]);
    EXPECT_EQ("G", layers[0].getChannels()[1]);
    EXPECT_EQ("B", layers[0].getChannels()[2]);
    EXPECT_EQ("A", layers[0].getChannels()[3]);

    EXPECT_EQ("depth", layers[1].getLayerID());
    ASSERT_EQ(1, layers[1].getNumComponents());
    EXPECT_EQ("Z", layers[1].getChannels()[0]);

    EXPECT_EQ("diffuse", layers[2].getLayerID());
    ASSERT_EQ(2, layers[2].getNumComponents());
    EXPECT_EQ("R", layers[2].getChannels()[0]);
    EXPECT_EQ("G", layers[2].getChannels()[1]);
}

TEST(LayerRegistry, SnapshotIsStableAcrossAdd)
{
    LayerRegistry registry;
    std::shared_ptr<const std::vector<LayerRegistryEntry>> before = registry.snapshot();
    std::size_t sizeBefore = before->size();

    std::string error;
    std::vector<std::string> ch(1, "R");
    ImageLayerDesc newLayer("extra", "extra", "", ch);
    registry.add(newLayer, LayerRegistryEntry::eOriginUser, &error);

    EXPECT_EQ(sizeBefore, before->size());

    std::shared_ptr<const std::vector<LayerRegistryEntry>> after = registry.snapshot();
    EXPECT_EQ(sizeBefore + 1, after->size());
}
