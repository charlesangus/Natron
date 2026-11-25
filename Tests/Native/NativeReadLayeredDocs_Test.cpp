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

#include <bitset>
#include <cmath>
#include <cstddef>
#include <list>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include <ofxImageEffect.h>

#include "BaseTest.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/LayerRegistry.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/IO/IoColorSpace.h"
#include "Engine/Nodes/IO/LayeredDocs/LayeredImageInput.h"
#include "Engine/Nodes/IO/NativeRead.h"
#include "Engine/Nodes/IO/OiioReadSupport.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
std::string
fixture(const char* name)
{
    return std::string(NATRON_TESTS_FIXTURES_DIR "/layered/") + name;
}

std::vector<std::string>
idsOf(const std::vector<std::vector<std::string>>& paths)
{
    return LayeredDocs::sanitisedLayerIds(paths);
}

void
expectValidFileLayer(const std::string& id)
{
    std::string error;

    EXPECT_TRUE(LayerRegistry::validate(ImageLayerDesc(id, std::vector<std::string>({ "R", "G", "B", "A" })), true, &error)) << id << ": " << error;
}

const ImageLayerDesc*
findProduced(const std::list<ImageLayerDesc>& layers,
             const std::string& id)
{
    for (std::list<ImageLayerDesc>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
        if (it->getLayerID() == id) {
            return &*it;
        }
    }

    return NULL;
}
} // namespace

TEST(LayeredDocsLayerIds, NamesAreJoinedAcrossGroupsAndSanitised)
{
    const std::vector<std::string> ids = idsOf({ { "Fur", "Base Layer" }, { "a.b-c" }, { "Outer", "", "Inner" }, { "__x__" } });

    EXPECT_EQ(std::vector<std::string>({ "Fur_Base_Layer", "a_b_c", "Outer_Inner", "x" }), ids);
    for (const std::string& id : ids) {
        expectValidFileLayer(id);
    }
}

TEST(LayeredDocsLayerIds, CollisionsTakeTheFirstFreeSuffix)
{
    EXPECT_EQ(std::vector<std::string>({ "X", "X_2", "X_3" }), idsOf({ { "X" }, { "X" }, { "X" } }));
    EXPECT_EQ(std::vector<std::string>({ "a_b", "a_b_2" }), idsOf({ { "a b" }, { "a.b" } }));
    EXPECT_EQ(std::vector<std::string>({ "X_2", "X", "X_3" }), idsOf({ { "X_2" }, { "X" }, { "X" } }));
}

TEST(LayeredDocsLayerIds, ReservedAndEmptyNamesGetASuffixOrAName)
{
    const std::vector<std::string> ids = idsOf({ { "Color" }, { "rgba" }, { "Alpha" }, { "depth" }, { "" }, { "  " }, { "\xc3\x9c" } });

    ASSERT_EQ(7u, ids.size());
    EXPECT_EQ("Color_2", ids[0]);
    EXPECT_EQ("rgba_2", ids[1]);
    EXPECT_EQ("Alpha_2", ids[2]);
    EXPECT_EQ("depth_2", ids[3]);
    EXPECT_EQ("layer", ids[4]);
    EXPECT_EQ("layer_2", ids[5]);
    EXPECT_EQ("layer_3", ids[6]);
    for (const std::string& id : ids) {
        expectValidFileLayer(id);
    }
}

class NativeReadLayeredDocsTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
        OiioReadSupport::clearHeaderCache();
        LayeredDocs::clearDocumentCache();
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    NodePtr createRead(const std::string& path)
    {
        CreateNodeArgs args(PLUGINID_NATRON_READ, getApp()->getProject());
        args.setProperty<int>(kCreateNodeArgsPropPluginVersion, PLUGIN_MAJOR_NATRON_READ, 0);
        args.setProperty<int>(kCreateNodeArgsPropPluginVersion, 0, 1);
        args.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, path);
        NodePtr node = getApp()->createNode(args);
        EXPECT_TRUE(bool(node));
        EXPECT_TRUE(node && dynamic_cast<NativeRead*>(node->getEffectInstance().get()));
        if (node) {
            // The pixels are compared with the file's, so the Read does no colour conversion.
            fileInWorkingSpace(node);
        }

        return node;
    }

    std::list<ImageLayerDesc> producedLayers(const NodePtr& node)
    {
        EffectInstance::ComponentsNeededMap comps;
        std::list<ImageLayerDesc> passThroughLayers;
        double passThroughTime = 0.;
        int passThroughView = 0;
        std::bitset<4> processChannels;
        EffectInstance::ProcessChannelsPerPlaneMap processChannelsPerPlane;
        int passThroughInputNb = -1;

        node->getEffectInstance()->getComponentsNeededAndProduced_public(node->getHashValue(), 1., ViewIdx(0), &comps, &passThroughLayers, &passThroughTime, &passThroughView, &processChannels, &processChannelsPerPlane, &passThroughInputNb);

        return comps[-1];
    }

    // Renders `layer` over `window` and checks that every pixel inside `covered` is `inside` and
    // every other one is zero.
    void expectRendered(const NodePtr& node,
                        const ImageLayerDesc& layer,
                        const RectI& window,
                        const RectI& covered,
                        const std::vector<float>& inside)
    {
        SCOPED_TRACE(layer.getLayerID());
        std::vector<RenderedPlane> planes;
        std::string error;
        ASSERT_TRUE(renderNodePlanesDirect(node, 1., ViewIdx(0), 0, window, std::list<ImageLayerDesc>(1, layer), &planes, &error)) << error;
        ASSERT_EQ(1u, planes.size());
        const std::size_t nComps = inside.size();
        ASSERT_EQ(nComps, planes[0].channels.size());
        ASSERT_EQ((std::size_t)window.width() * (std::size_t)window.height() * nComps, planes[0].pixels.size());

        std::size_t mismatches = 0;
        for (int y = window.y1; y < window.y2; ++y) {
            for (int x = window.x1; x < window.x2; ++x) {
                const bool in = x >= covered.x1 && x < covered.x2 && y >= covered.y1 && y < covered.y2;
                for (std::size_t c = 0; c < nComps; ++c) {
                    const float got = planes[0].pixels[((std::size_t)(y - window.y1) * (std::size_t)window.width() + (std::size_t)(x - window.x1)) * nComps + c];
                    const float want = in ? inside[c] : 0.f;
                    if (std::abs(got - want) > 1e-6f && ++mismatches <= 5) {
                        ADD_FAILURE() << "pixel " << x << "," << y << " channel " << c << ": got " << got << ", want " << want;
                    }
                }
            }
        }
        EXPECT_EQ(0u, mismatches);
    }
};

TEST_F(NativeReadLayeredDocsTest, TheReadListsTheSanitisedLayersAndRendersEach)
{
    NodePtr node = createRead(fixture("nested.ora"));
    ASSERT_TRUE(bool(node));

    const std::list<ImageLayerDesc> produced = producedLayers(node);
    const std::vector<std::string> rgba = { "R", "G", "B", "A" };
    struct Expected {
        const char* id;
        RectI covered;
        std::vector<float> value;
    };
    // Natron's rows run upwards from the bottom of the 8x6 canvas.
    const Expected expected[] = {
        { "Background", RectI(0, 0, 8, 6), { 64.f / 255.f, 64.f / 255.f, 64.f / 255.f, 1.f } },
        { "Color_2", RectI(6, 0, 8, 2), { 1.f, 1.f, 0.f, 1.f } },
        { "Group_A_Deep_Leaf", RectI(1, 4, 2, 5), { 0.f, 0.f, 1.f, 1.f } },
        { "Group_A_Inner", RectI(2, 3, 4, 5), { 0.f, 1.f, 0.f, 1.f } },
        { "Top_Layer", RectI(2, 2, 6, 5), { 1.f, 0.f, 0.f, 1.f } },
    };
    for (const Expected& e : expected) {
        const ImageLayerDesc* layer = findProduced(produced, e.id);
        ASSERT_TRUE(layer != NULL) << e.id;
        EXPECT_EQ(rgba, layer->getChannels()) << e.id;
        expectRendered(node, *layer, RectI(0, 0, 8, 6), e.covered, e.value);
    }

    expectRendered(node, ImageLayerDesc::getRGBAComponents(), RectI(0, 0, 8, 6), RectI(0, 0, 8, 6), { 10.f / 255.f, 20.f / 255.f, 30.f / 255.f, 1.f });
}

TEST_F(NativeReadLayeredDocsTest, AGrayCompositeFillsRgb)
{
    NodePtr node = createRead(fixture("gray16.ora"));
    ASSERT_TRUE(bool(node));

    std::vector<RenderedPlane> planes;
    std::string error;
    ASSERT_TRUE(renderNodePlanesDirect(node, 1., ViewIdx(0), 0, RectI(0, 0, 4, 2), std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()), &planes, &error)) << error;
    ASSERT_EQ(1u, planes.size());
    ASSERT_EQ(4u, planes[0].channels.size());
    ASSERT_EQ(4u * 2u * 4u, planes[0].pixels.size());
    const float value = (float)0x8000 / 65535.f;
    for (std::size_t p = 0; p < 4 * 2; ++p) {
        for (std::size_t c = 0; c < 3; ++c) {
            EXPECT_NEAR(value, planes[0].pixels[p * 4 + c], 1e-6f) << p << " channel " << c;
        }
    }

    const ImageLayerDesc* shade = findProduced(producedLayers(node), "Shade");
    ASSERT_TRUE(shade != NULL);
    const float shadeValue = (float)0x4000 / 65535.f;
    expectRendered(node, *shade, RectI(0, 0, 4, 2), RectI(0, 0, 4, 2), { shadeValue, shadeValue, shadeValue, 1.f });
}

TEST_F(NativeReadLayeredDocsTest, LayerMetadataReachesANodeBehindADot)
{
    NodePtr read = createRead(fixture("nested.ora"));
    NodePtr dot = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
    NodePtr grade = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    ASSERT_TRUE(read && dot && grade);

    connectNodes(read, dot, 0, true);
    connectNodes(dot, grade, 0, true);

    NativeEffectBase* gradeEffect = dynamic_cast<NativeEffectBase*>(grade->getEffectInstance().get());
    ASSERT_TRUE(gradeEffect != NULL);
    const ImageMetadata metadata = gradeEffect->getOutputMetadata(1., ViewIdx(0));

    EXPECT_EQ(std::optional<std::string>("svg:multiply"), metadata.getString("layered/Top_Layer/blendMode"));
    EXPECT_EQ(std::optional<double>(0.5), metadata.getDouble("layered/Top_Layer/opacity"));
    EXPECT_EQ(std::optional<int>(1), metadata.getInt("layered/Top_Layer/visible"));
    EXPECT_EQ(std::optional<int>(0), metadata.getInt("layered/Top_Layer/clipping"));
    EXPECT_EQ(std::optional<std::string>("Group A/Deep"), metadata.getString("layered/Group_A_Deep_Leaf/group"));
    EXPECT_EQ(std::optional<std::string>("stored"), metadata.getString("layered/composite"));
}

TEST_F(NativeReadLayeredDocsTest, ALayerIsConvertedFromTheInputSpaceLikeTheComposite)
{
    const std::string input = "sRGB Encoded Rec.709 (sRGB)";
    NodePtr node = createRead(fixture("flatten.ora"));
    ASSERT_TRUE(bool(node));
    setFileColorspace(node, input);

    Project& project = *getApp()->getProject();
    OCIO_NAMESPACE::ConstCPUProcessorRcPtr toWorking;
    std::string error;
    ASSERT_TRUE(IoColorSpace::toWorkingProcessor(project, input, project.getWorkingColorSpace(), std::vector<std::pair<std::string, std::string>>(), &toWorking, &error)) << error;
    ASSERT_TRUE(bool(toWorking));
    const auto converted = [&](std::vector<float> pixel) {
        std::string conversionError;
        EXPECT_TRUE(IoColorSpace::convertRow(*toWorking, pixel.data(), 1, (int)pixel.size(), &conversionError)) << conversionError;

        return pixel;
    };

    const std::vector<float> blue = converted({ 0.f, 0.f, 1.f, 1.f });
    ASSERT_NE(0.f, blue[0]);
    const std::vector<float> halfRed = converted({ 128.f / 255.f, 0.f, 0.f, 128.f / 255.f });
    EXPECT_EQ(128.f / 255.f, halfRed[3]);

    const std::list<ImageLayerDesc> produced = producedLayers(node);
    const ImageLayerDesc* background = findProduced(produced, "Background");
    const ImageLayerDesc* hidden = findProduced(produced, "Hidden");
    const ImageLayerDesc* top = findProduced(produced, "Top");
    ASSERT_TRUE(background != NULL);
    ASSERT_TRUE(hidden != NULL);
    ASSERT_TRUE(top != NULL);
    expectRendered(node, *background, RectI(0, 0, 4, 4), RectI(0, 0, 4, 4), blue);
    expectRendered(node, *hidden, RectI(0, 0, 4, 4), RectI(0, 0, 4, 4), converted({ 1.f, 1.f, 1.f, 1.f }));
    expectRendered(node, *top, RectI(0, 0, 4, 4), RectI(1, 1, 3, 3), halfRed);

    // The flattened composite is the Background alone outside the Top layer's 2x2.
    std::vector<RenderedPlane> composite;
    ASSERT_TRUE(renderNodePlanesDirect(node, 1., ViewIdx(0), 0, RectI(0, 0, 4, 4), std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()), &composite, &error)) << error;
    ASSERT_EQ(1u, composite.size());
    ASSERT_EQ(4u * 4u * 4u, composite[0].pixels.size());
    const int corners[4][2] = { { 0, 0 }, { 3, 0 }, { 0, 3 }, { 3, 3 } };
    for (const auto& corner : corners) {
        const std::size_t pixel = (std::size_t)corner[1] * 4u + (std::size_t)corner[0];
        for (std::size_t c = 0; c < 4; ++c) {
            EXPECT_NEAR(blue[c], composite[0].pixels[pixel * 4u + c], 1e-6f) << corner[0] << "," << corner[1] << " channel " << c;
        }
    }
}
