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

#include "Engine/Nodes/Scene/AovLayers.h"
#include "Engine/Nodes/Scene/CyclesAovs.h"
#include "Engine/Nodes/Scene/MoonRayAovs.h"

NATRON_NAMESPACE_USING

namespace {
std::vector<std::string>
channels(const char* a,
         const char* b = nullptr,
         const char* c = nullptr,
         const char* d = nullptr)
{
    std::vector<std::string> out;

    for (const char* name : { a, b, c, d }) {
        if (name) {
            out.push_back(name);
        }
    }

    return out;
}

ImageLayerDesc
layerOf(const char* aov,
        const char* dataType = "",
        const char* authored = "",
        int formatChannels = 0,
        std::string* warning = nullptr)
{
    return AovLayers::layer(kRenderDelegateCyclesId, aov, dataType, authored, formatChannels, warning);
}
} // anon namespace

TEST(AovLayersTest, NamesDropTheNamespace)
{
    const ImageLayerDesc diffuse = layerOf("cycles:diffuse_direct");
    EXPECT_EQ("diffuse_direct", diffuse.getLayerID());
    EXPECT_EQ(channels("R", "G", "B"), diffuse.getChannels());

    const ImageLayerDesc st = layerOf("primvars:st");
    EXPECT_EQ("st", st.getLayerID());
    EXPECT_EQ(channels("U", "V", "W"), st.getChannels());

    const ImageLayerDesc depth = layerOf("depth");
    EXPECT_EQ("depth", depth.getLayerID());
    EXPECT_EQ(channels("Z"), depth.getChannels());

    const ImageLayerDesc spec = layerOf("lpe:C<RS>[LO]", "", "specular");
    EXPECT_EQ("specular", spec.getLayerID());
    EXPECT_EQ(channels("R", "G", "B"), spec.getChannels());
}

TEST(AovLayersTest, AuthoredLayerOverrides)
{
    const ImageLayerDesc diffuse = layerOf("cycles:diffuse_direct", "", "diffuseDirect");
    EXPECT_EQ("diffuseDirect", diffuse.getLayerID());
    EXPECT_EQ(channels("R", "G", "B"), diffuse.getChannels());
}

TEST(AovLayersTest, DataTypeGivesTheChannels)
{
    EXPECT_EQ(channels("R", "G", "B", "A"), layerOf("cycles:emission", "color4f").getChannels());
    EXPECT_EQ(channels("U", "V"), layerOf("primvars:st", "texCoord2f").getChannels());
    EXPECT_EQ(channels("X", "Y", "Z"), layerOf("primvars:rest", "point3f").getChannels());
    EXPECT_EQ(channels("X", "Y", "Z"), layerOf("cycles:normal", "normal3f").getChannels());
    EXPECT_EQ(channels("Y"), layerOf("cycles:mist", "float").getChannels());
    EXPECT_EQ(channels("R", "G", "B"), layerOf("cycles:glossy_direct", "half3").getChannels());

    AovLayers::Layout layout;
    EXPECT_FALSE(AovLayers::layoutFromDataType("string", &layout));
    EXPECT_FALSE(AovLayers::layoutFromDataType("matrix4d", &layout));
}

// The 64 of the 64-bit integer types is their width: each is one channel.
TEST(AovLayersTest, SixtyFourBitIntegersAreScalars)
{
    for (const char* dataType : { "int64", "uint64" }) {
        AovLayers::Layout layout;
        EXPECT_TRUE(AovLayers::layoutFromDataType(dataType, &layout)) << dataType;
        EXPECT_EQ(AovLayers::eKindScalar, layout.kind) << dataType;
        EXPECT_EQ(1, layout.channels) << dataType;
    }
    EXPECT_EQ(channels("Y"), layerOf("primvars:objectIndex", "int64").getChannels());
    EXPECT_EQ(channels("Y"), layerOf("primvars:count", "uint64").getChannels());
}

TEST(AovLayersTest, FormatWinsOverDataTypeWithAWarning)
{
    std::string warning;
    const ImageLayerDesc rest = layerOf("primvars:rest", "point3f", "", 4, &warning);
    EXPECT_EQ(channels("X", "Y", "Z", "W"), rest.getChannels());
    EXPECT_NE(std::string::npos, warning.find("primvars:rest")) << warning;

    warning.clear();
    EXPECT_EQ(channels("R", "G", "B"), layerOf("cycles:diffuse_direct", "color3f", "", 3, &warning).getChannels());
    EXPECT_TRUE(warning.empty()) << warning;

    warning.clear();
    EXPECT_EQ(channels("R", "G", "B", "A"), layerOf("cycles:unknown", "", "", 4, &warning).getChannels());
    EXPECT_TRUE(warning.empty()) << warning;
}

TEST(AovLayersTest, ThreeChannelFormatKeepsTwoChannelUv)
{
    std::string warning;
    const ImageLayerDesc st = layerOf("primvars:st", "texCoord2f", "", 3, &warning);
    EXPECT_EQ(channels("U", "V"), st.getChannels());
    EXPECT_TRUE(warning.empty()) << warning;
}

TEST(AovLayersTest, ColonsInDerivedNamesBecomeUnderscores)
{
    EXPECT_EQ("lightgroup_key", layerOf("cycles:lightgroup:key").getLayerID());
    EXPECT_EQ("a_b_c", AovLayers::layerName("ns:a:b:c", ""));
    EXPECT_EQ("my:layer", AovLayers::layerName("cycles:lightgroup:key", "my:layer"));
}

TEST(AovLayersTest, ExistingLayerNamesAreUnchanged)
{
    EXPECT_EQ("depth", AovLayers::depthLayer().getLayerID());
    EXPECT_EQ(channels("Z"), AovLayers::depthLayer().getChannels());
    EXPECT_EQ("primId", AovLayers::primIdLayer().getLayerID());
    EXPECT_EQ(channels("id"), AovLayers::primIdLayer().getChannels());
    EXPECT_EQ("normal", AovLayers::normalLayer().getLayerID());
    EXPECT_EQ(channels("x", "y", "z"), AovLayers::normalLayer().getChannels());
    EXPECT_EQ(channels("Z"), AovLayers::layer(kMoonRayRendererId, MoonRayAovs::depthAov().GetString(), "", "", 0, nullptr).getChannels());
}

TEST(AovLayersTest, FormatChannelsAreRemembered)
{
    EXPECT_EQ(0, AovLayers::formatChannels("NatronTestAovRenderer", "primvars:st"));
    AovLayers::rememberFormatChannels("NatronTestAovRenderer", "primvars:st", 3);
    EXPECT_EQ(3, AovLayers::formatChannels("NatronTestAovRenderer", "primvars:st"));
    EXPECT_EQ(0, AovLayers::formatChannels(kRenderDelegateCyclesId, "primvars:st"));
}
