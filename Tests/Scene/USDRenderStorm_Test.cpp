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

#include "Engine/Nodes/Scene/UsdIncludes.h"

#include "Global/Macros.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <list>
#include <set>
#include <string>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/Nodes/Scene/HydraRenderImpl.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDCamera.h"
#include "Engine/Nodes/Scene/USDCard.h"
#include "Engine/Nodes/Scene/USDEditTransform.h"
#include "Engine/Nodes/Scene/USDLight.h"
#include "Engine/Nodes/Scene/USDMaterial.h"
#include "Engine/Nodes/Scene/USDMerge.h"
#include "Engine/Nodes/Scene/USDPrune.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/Nodes/Scene/USDRender.h"
#include "Engine/Nodes/Scene/USDSphere.h"
#include "Engine/Nodes/Scene/USDXformPrim.h"
#include "Engine/ViewIdx.h"
#include "RenderBothWays.h"
#include "SceneTestEffect.h"
#include "Tests/Native/NativeGolden.h"
#include "ofxNatron.h"

NATRON_NAMESPACE_USING

namespace {
const char* const kFixture = NATRON_TESTS_FIXTURES_DIR "/scene-reference.usda";
const int kWidth = 200;
const int kHeight = 100;
const char* const kRenderCamera = "/World/Rig/RenderCam";

// Where the references live under the golden directories: <dir>/USDRender/<case>.f32.
const char* const kGoldenGroup = "USDRender";
const char* const kGoldenCase = "StormReference";
const char* const kGoldenCasePrimitives = "StormPrimitives";

/**
 * How far the colour may drift from the reference: GL drivers rasterise and shade with their own
 * rounding, and edges land on other pixels. A pixel differs when a channel is off by more than
 * kChannelTolerance; at most kDifferingFraction of them may, and the mean difference stays below
 * kMeanTolerance.
 **/
const float kChannelTolerance = 0.02f;
const double kDifferingFraction = 0.005;
const double kMeanTolerance = 0.002;

// The dev image's /usr/local glvnd lists only NVIDIA; the system directory holds Mesa. Set before
// the first EGL use, as the render thread's context is made once.
void
pointEGLAtMesa()
{
    if (!std::getenv("__EGL_VENDOR_LIBRARY_FILENAMES") && !std::getenv("__EGL_VENDOR_LIBRARY_DIRS") && (::access("/usr/share/glvnd/egl_vendor.d", F_OK) == 0) && (::access("/usr/local/share/glvnd/egl_vendor.d", F_OK) == 0)) {
        ::setenv("__EGL_VENDOR_LIBRARY_DIRS", "/usr/share/glvnd/egl_vendor.d", 1);
    }
}

// True when the test must stop: no 4.5 context could be made, which fails the test only under
// NATRON_TEST_REQUIRE_GL=1.
bool
skipWithoutGL()
{
    pointEGLAtMesa();
    std::string error;
    if (!HydraRender::contextDescription(&error).empty()) {
        return false;
    }
    const char* require = std::getenv("NATRON_TEST_REQUIRE_GL");
    if (require && (std::strcmp(require, "1") == 0)) {
        ADD_FAILURE() << error << " (NATRON_TEST_REQUIRE_GL=1)";

        return true;
    }
    std::cout << "[  SKIPPED ] USDRenderStorm: " << error << std::endl;

    return true;
}

std::uint32_t
bitsOf(float value)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));

    return bits;
}

// Where a prim landed: its pixel count, centroid (rows counted from the bottom), nearest and
// farthest depth, and mean colour.
struct Coverage {
    int count = 0;
    double x = 0.;
    double y = 0.;
    float minDepth = std::numeric_limits<float>::infinity();
    float maxDepth = -std::numeric_limits<float>::infinity();
    double rgb[3] = { 0., 0., 0. };
};

Coverage
coverageOf(const std::string& path,
           const std::vector<float>& rgba,
           const std::vector<float>& depth,
           const std::vector<float>& primId)
{
    Coverage c;
    const std::uint32_t id = bitsOf(HydraRender::hashToFloat(HydraRender::primPathHash(path)));

    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const std::size_t i = (std::size_t)y * (std::size_t)kWidth + (std::size_t)x;
            if (bitsOf(primId[i]) != id) {
                continue;
            }
            ++c.count;
            c.x += x;
            c.y += y;
            c.minDepth = std::min(c.minDepth, depth[i]);
            c.maxDepth = std::max(c.maxDepth, depth[i]);
            for (int k = 0; k < 3; ++k) {
                c.rgb[k] += rgba[i * 4 + k];
            }
        }
    }
    if (c.count) {
        c.x /= c.count;
        c.y /= c.count;
        for (int k = 0; k < 3; ++k) {
            c.rgb[k] /= c.count;
        }
    }

    return c;
}

// Records the colour plane under NATRON_GOLDEN_RECORD_DIR, or compares it with the committed one
// within the tolerances above.
void
checkAgainstReference(const RenderedPlane& rgba,
                      const char* goldenCase)
{
    const ImageLayerDesc layer = ImageLayerDesc::getRGBAComponents();
    const std::string recordDir = goldenRecordDir();
    if (!recordDir.empty()) {
        ParityF32Image image;
        image.width = rgba.window.width();
        image.height = rgba.window.height();
        image.nComps = (int)rgba.channels.size();
        image.pixels = rgba.pixels;
        const std::string path = parityReferencePath(recordDir, kGoldenGroup, goldenCase, 0, layer);
        std::string error;
        EXPECT_TRUE(writeParityF32(path, image, &error)) << goldenCase << ": cannot record: " << error;
        std::cout << "[ golden ] recorded " << path << std::endl;

        return;
    }

    const std::string refDir = goldenReferenceDir();
    const std::string path = parityReferencePath(refDir, kGoldenGroup, goldenCase, 0, layer);
    ParityF32Image reference;
    std::string error;
    if (!readParityF32(path, &reference, &error)) {
        ADD_FAILURE() << goldenCase << ": no golden reference " << path << " (" << error << "). Record it from a build that renders the scene "
                      << "correctly by running this test with NATRON_GOLDEN_RECORD_DIR=" << refDir << " and commit the file.";

        return;
    }
    ASSERT_EQ(rgba.window.width(), reference.width) << path;
    ASSERT_EQ(rgba.window.height(), reference.height) << path;
    ASSERT_EQ((int)rgba.channels.size(), reference.nComps) << path;
    ASSERT_EQ(rgba.pixels.size(), reference.pixels.size()) << path;

    const int nComps = reference.nComps;
    const std::size_t pixels = reference.pixels.size() / (std::size_t)nComps;
    std::size_t differing = 0;
    double sum = 0.;
    float worst = 0.f;
    std::size_t worstPixel = 0;
    for (std::size_t p = 0; p < pixels; ++p) {
        bool differs = false;
        for (int k = 0; k < nComps; ++k) {
            const float a = rgba.pixels[p * nComps + k];
            const float b = reference.pixels[p * nComps + k];
            if (!std::isfinite(a) || !std::isfinite(b)) {
                differs = differs || (std::isfinite(a) != std::isfinite(b));
                continue;
            }
            const float d = std::fabs(a - b);
            sum += d;
            if (d > kChannelTolerance) {
                differs = true;
            }
            if (d > worst) {
                worst = d;
                worstPixel = p;
            }
        }
        if (differs) {
            ++differing;
        }
    }
    const double mean = sum / (double)reference.pixels.size();
    const std::string where = "worst difference " + std::to_string(worst) + " at (" + std::to_string(worstPixel % (std::size_t)reference.width) + ", "
        + std::to_string(worstPixel / (std::size_t)reference.width) + ")";
    EXPECT_LE((double)differing, kDifferingFraction * (double)pixels) << differing << " of " << pixels << " pixels differ from " << path << "; " << where;
    EXPECT_LE(mean, kMeanTolerance) << "mean difference from " << path << "; " << where;
} // checkAgainstReference

class USDRenderStormTest
    : public BaseTest {
protected:
    virtual void TearDown() OVERRIDE
    {
        SceneQueryPool::instance().releaseIdle();
        BaseTest::TearDown();
    }

    template <typename KNOB>
    static KNOB* knob(const NodePtr& node,
                      const char* name)
    {
        KNOB* k = dynamic_cast<KNOB*>(node->getKnobByName(name).get());
        EXPECT_TRUE(k != nullptr) << node->getPluginID() << " has no knob " << name;

        return k;
    }

    static bool setString(const NodePtr& node,
                          const char* name,
                          const char* value)
    {
        KnobString* k = knob<KnobString>(node, name);
        if (k) {
            k->setValue(std::string(value));
        }

        return k != nullptr;
    }

    static bool setTriple(const NodePtr& node,
                          const char* name,
                          double x,
                          double y,
                          double z)
    {
        KnobDouble* k = knob<KnobDouble>(node, name);
        if (k) {
            k->setValue(x, ViewSpec::all(), 0);
            k->setValue(y, ViewSpec::all(), 1);
            k->setValue(z, ViewSpec::all(), 2);
        }

        return k != nullptr;
    }

    /**
     * The reference scene, laid out by the create and edit nodes over the fixture's box and ball:
     * the ball lifted by 1, the box given a green material by a geometry stand-in defining the same
     * cube at its path and carrying the material, the fixture's camera pruned, and a rig
     * holding the camera rendered through, two units further back than the pruned one, and a key
     * light. Returns the USDRender, kWidth x kHeight, null after a gtest failure.
     **/
    NodePtr createReferenceGraph()
    {
        NodePtr reader = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));
        NodePtr transform = createNode(QString::fromUtf8(PLUGINID_NATRON_USDEDITTRANSFORM));
        NodePtr material = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMATERIAL));
        NodePtr box = createNode(QString::fromUtf8(kTestPluginIDSceneGeometry));
        NodePtr rig = createNode(QString::fromUtf8(PLUGINID_NATRON_USDXFORMPRIM));
        NodePtr camera = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCAMERA));
        NodePtr light = createNode(QString::fromUtf8(PLUGINID_NATRON_USDLIGHT));
        NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMERGE));
        NodePtr prune = createNode(QString::fromUtf8(PLUGINID_NATRON_USDPRUNE));
        NodePtr render = createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDER));
        if (!reader || !transform || !material || !box || !rig || !camera || !light || !merge || !prune || !render) {
            ADD_FAILURE() << "a 3D node is not registered";

            return NodePtr();
        }

        KnobFile* file = knob<KnobFile>(reader, kUSDReadParamFile);
        KnobColor* diffuse = knob<KnobColor>(material, kUSDMaterialParamDiffuseColor);
        KnobChoice* renderCamera = knob<KnobChoice>(render, kUSDRenderParamCamera);
        KnobInt* size = knob<KnobInt>(render, kNatronParamFormatSize);
        KnobDouble* par = knob<KnobDouble>(render, kNatronParamFormatPar);
        if (!file || !diffuse || !renderCamera || !size || !par) {
            return NodePtr();
        }
        file->setValue(std::string(kFixture));

        bool ok = true;
        ok = ok && setString(transform, kUSDEditTransformParamPrimPattern, "/World/Ball");
        ok = ok && setTriple(transform, kSceneParamTranslate, 0., 1., 0.);

        ok = ok && setString(material, kSceneParamPrimPath, "/Green");
        ok = ok && setString(box, kSceneParamPrimPath, "/World/Box");
        diffuse->setValue(0.1, ViewSpec::all(), 0);
        diffuse->setValue(0.8, ViewSpec::all(), 1);
        diffuse->setValue(0.2, ViewSpec::all(), 2);

        ok = ok && setString(rig, kSceneParamPrimPath, "/World/Rig");
        ok = ok && setTriple(rig, kSceneParamTranslate, 1., 1., 0.);
        ok = ok && setString(camera, kSceneParamPrimPath, "/RenderCam");
        ok = ok && setTriple(camera, kSceneParamTranslate, 0., 0., 12.);
        ok = ok && setString(light, kSceneParamPrimPath, "/Key");

        ok = ok && setString(prune, kUSDPruneParamPrimPattern, "/World/Cam");
        if (!ok) {
            return NodePtr();
        }

        connectNodes(reader, transform, 0, true);
        connectNodes(material, box, 0, true);
        connectNodes(camera, rig, 0, true);
        connectNodes(light, rig, 1, true);
        connectNodes(box, merge, 0, true);
        connectNodes(transform, merge, 1, true);
        connectNodes(rig, merge, 2, true);
        connectNodes(merge, prune, 0, true);
        connectNodes(prune, render, 0, true);

        renderCamera->setActiveEntry(ChoiceOption(kRenderCamera, kRenderCamera, ""));
        size->setValue(kWidth, ViewSpec::all(), 0);
        size->setValue(kHeight, ViewSpec::all(), 1);
        par->setValue(1.);

        return render;
    } // createReferenceGraph

    /**
     * Primitives as a user lays them out: a card and a sphere, each placed by a USDXformPrim, merged
     * with a camera and a light. The 2 x 1 card stands at (-1.5, 0, 0), the sphere of radius 1,
     * bound to a red material through its Material input, at (1.5, 0, 0), both facing the camera
     * at (0, 0, 10) looking down -Z; the distant light shines down -Z. Returns the USDRender,
     * kWidth x kHeight, null after a gtest failure.
     **/
    NodePtr createPrimitivesGraph()
    {
        NodePtr card = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCARD));
        NodePtr left = createNode(QString::fromUtf8(PLUGINID_NATRON_USDXFORMPRIM));
        NodePtr material = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMATERIAL));
        NodePtr sphere = createNode(QString::fromUtf8(PLUGINID_NATRON_USDSPHERE));
        NodePtr right = createNode(QString::fromUtf8(PLUGINID_NATRON_USDXFORMPRIM));
        NodePtr camera = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCAMERA));
        NodePtr light = createNode(QString::fromUtf8(PLUGINID_NATRON_USDLIGHT));
        NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMERGE));
        NodePtr render = createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDER));
        if (!card || !left || !material || !sphere || !right || !camera || !light || !merge || !render) {
            ADD_FAILURE() << "a 3D node is not registered";

            return NodePtr();
        }

        KnobDouble* aspect = knob<KnobDouble>(card, kUSDCardParamAspect);
        KnobColor* diffuse = knob<KnobColor>(material, kUSDMaterialParamDiffuseColor);
        KnobChoice* renderCamera = knob<KnobChoice>(render, kUSDRenderParamCamera);
        KnobInt* size = knob<KnobInt>(render, kNatronParamFormatSize);
        KnobDouble* par = knob<KnobDouble>(render, kNatronParamFormatPar);
        if (!aspect || !diffuse || !renderCamera || !size || !par) {
            return NodePtr();
        }
        aspect->setValue(2.);
        diffuse->setValue(0.8, ViewSpec::all(), 0);
        diffuse->setValue(0.1, ViewSpec::all(), 1);
        diffuse->setValue(0.1, ViewSpec::all(), 2);

        bool ok = true;
        ok = ok && setString(card, kSceneParamPrimPath, "/Card");
        ok = ok && setString(left, kSceneParamPrimPath, "/Left");
        ok = ok && setTriple(left, kSceneParamTranslate, -1.5, 0., 0.);
        ok = ok && setString(material, kSceneParamPrimPath, "/Red");
        ok = ok && setString(sphere, kSceneParamPrimPath, "/Ball");
        ok = ok && setString(right, kSceneParamPrimPath, "/Right");
        ok = ok && setTriple(right, kSceneParamTranslate, 1.5, 0., 0.);
        ok = ok && setString(camera, kSceneParamPrimPath, "/Cam");
        ok = ok && setTriple(camera, kSceneParamTranslate, 0., 0., 10.);
        ok = ok && setString(light, kSceneParamPrimPath, "/Key");
        if (!ok) {
            return NodePtr();
        }

        connectNodes(card, left, 0, true);
        connectNodes(material, sphere, 0, true);
        connectNodes(sphere, right, 0, true);
        connectNodes(left, merge, 0, true);
        connectNodes(right, merge, 1, true);
        connectNodes(camera, merge, 2, true);
        connectNodes(light, merge, 3, true);
        connectNodes(merge, render, 0, true);

        renderCamera->setActiveEntry(ChoiceOption("/Cam", "/Cam", ""));
        size->setValue(kWidth, ViewSpec::all(), 0);
        size->setValue(kHeight, ViewSpec::all(), 1);
        par->setValue(1.);

        return render;
    } // createPrimitivesGraph

    static std::list<ImageLayerDesc> allPlanes()
    {
        std::list<ImageLayerDesc> layers;
        layers.push_back(ImageLayerDesc::getRGBAComponents());
        layers.push_back(ImageLayerDesc("depth", "depth", "", std::vector<std::string>(1, "Z")));
        layers.push_back(ImageLayerDesc("primId", "primId", "", std::vector<std::string>(1, "id")));

        return layers;
    }
};
} // anonymous namespace

TEST_F(USDRenderStormTest, TheReferenceSceneMatchesItsImageDepthAndPrimIds)
{
    if (skipWithoutGL()) {
        return;
    }
    NodePtr node = createReferenceGraph();
    ASSERT_TRUE(node != nullptr);
    std::vector<RenderedPlane> planes;
    std::string error;
    ASSERT_TRUE(renderNodePlanesDirect(node, 1., ViewIdx(0), 0, RectI(0, 0, kWidth, kHeight), allPlanes(), &planes, &error)) << error;
    EXPECT_FALSE(node->hasPersistentMessage());
    ASSERT_EQ(3u, planes.size());
    const std::size_t pixels = (std::size_t)kWidth * kHeight;
    ASSERT_EQ(pixels * 4, planes[0].pixels.size());
    ASSERT_EQ(pixels, planes[1].pixels.size());
    ASSERT_EQ(pixels, planes[2].pixels.size());
    const std::vector<float>& rgba = planes[0].pixels;
    const std::vector<float>& depth = planes[1].pixels;
    const std::vector<float>& primId = planes[2].pixels;

    checkAgainstReference(planes[0], kGoldenCase);

    // The camera is at (1, 1, 12) looking down -Z: the box's front face, at z = 1, is 11 away, and
    // the lifted ball's nearest point, at (3, 1, 0.5), 11.5.
    const Coverage box = coverageOf("/World/Box", rgba, depth, primId);
    const Coverage ball = coverageOf("/World/Ball", rgba, depth, primId);
    ASSERT_GT(box.count, 100);
    ASSERT_GT(ball.count, 20);
    EXPECT_NEAR(11., box.minDepth, 0.01);
    // Storm draws the sphere as a mesh inside it, so its nearest point may lie a little further.
    EXPECT_NEAR(11.5, ball.minDepth, 0.05);
    EXPECT_LT(ball.maxDepth, 12.);
    // The box sits left of and below the camera, the ball right of it at its height.
    EXPECT_LT(box.x, kWidth / 2.);
    EXPECT_LT(box.y, kHeight / 2.);
    EXPECT_GT(ball.x, kWidth / 2.);
    EXPECT_NEAR(kHeight / 2., ball.y, 3.);
    // The material made the box green; the ball keeps its blue display colour. The key light,
    // normalized and of intensity 1, faces the box's front face head on and lights the ball's
    // camera side, so both are well lit and not merely tinted.
    EXPECT_GT(box.rgb[1], 0.2) << "box colour " << box.rgb[0] << ", " << box.rgb[1] << ", " << box.rgb[2];
    EXPECT_GT(box.rgb[1], 2. * box.rgb[0]);
    EXPECT_GT(box.rgb[1], 2. * box.rgb[2]);
    EXPECT_GT(ball.rgb[2], 0.1) << "ball colour " << ball.rgb[0] << ", " << ball.rgb[1] << ", " << ball.rgb[2];
    EXPECT_GT(ball.rgb[2], 2. * ball.rgb[0]);
    EXPECT_GT(ball.rgb[2], ball.rgb[1]);

    // Where nothing was hit: no colour, depth +inf, primId 0. Elsewhere depth is finite and positive.
    const std::size_t corner = (std::size_t)(kHeight - 1) * (std::size_t)kWidth;
    EXPECT_EQ(0u, bitsOf(primId[corner]));
    EXPECT_TRUE(std::isinf(depth[corner]) && (depth[corner] > 0.f)) << depth[corner];
    EXPECT_EQ(0.f, rgba[corner * 4 + 3]);
    std::set<std::uint32_t> ids;
    int badDepths = 0;
    for (std::size_t i = 0; i < pixels; ++i) {
        const std::uint32_t id = bitsOf(primId[i]);
        if (id == 0) {
            badDepths += std::isinf(depth[i]) ? 0 : 1;
            continue;
        }
        ids.insert(id);
        badDepths += (std::isfinite(depth[i]) && (depth[i] > 0.f)) ? 0 : 1;
    }
    EXPECT_EQ(0, badDepths);
    // Only the box and the ball are drawn; the pruned camera, the rig and the light are not.
    EXPECT_EQ(2u, ids.size());

    // The frame metadata names every id the plane holds.
    NativeEffectBase* effect = dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());
    ASSERT_TRUE(effect);
    const ImageMetadata metadata = effect->getOutputMetadata(1., ViewIdx(0));
    const std::uint32_t boxId = bitsOf(HydraRender::hashToFloat(HydraRender::primPathHash("/World/Box")));
    const std::uint32_t ballId = bitsOf(HydraRender::hashToFloat(HydraRender::primPathHash("/World/Ball")));
    for (std::uint32_t id : ids) {
        char key[32];
        std::snprintf(key, sizeof(key), kUSDRenderMetadataPrimIdPrefix "%08x", id);
        const std::optional<std::string> path = metadata.getString(key);
        ASSERT_TRUE(path.has_value()) << key;
        EXPECT_TRUE((id == boxId) ? (*path == "/World/Box") : (id == ballId) ? (*path == "/World/Ball")
                                                                             : false)
            << key << " names " << *path;
    }
} // TEST_F

TEST_F(USDRenderStormTest, PrimitivesRenderWhereTheirTransformsPlaceThem)
{
    if (skipWithoutGL()) {
        return;
    }
    NodePtr node = createPrimitivesGraph();
    ASSERT_TRUE(node != nullptr);
    std::vector<RenderedPlane> planes;
    std::string error;
    ASSERT_TRUE(renderNodePlanesDirect(node, 1., ViewIdx(0), 0, RectI(0, 0, kWidth, kHeight), allPlanes(), &planes, &error)) << error;
    EXPECT_FALSE(node->hasPersistentMessage());
    ASSERT_EQ(3u, planes.size());
    const std::size_t pixels = (std::size_t)kWidth * kHeight;
    ASSERT_EQ(pixels * 4, planes[0].pixels.size());
    ASSERT_EQ(pixels, planes[1].pixels.size());
    ASSERT_EQ(pixels, planes[2].pixels.size());
    const std::vector<float>& rgba = planes[0].pixels;
    const std::vector<float>& depth = planes[1].pixels;
    const std::vector<float>& primId = planes[2].pixels;

    checkAgainstReference(planes[0], kGoldenCasePrimitives);

    // The camera is 10 from the card's plane and 9 from the sphere's nearest point, which is a
    // vertex of the mesh.
    const Coverage card = coverageOf("/Left/Card", rgba, depth, primId);
    const Coverage ball = coverageOf("/Right/Ball", rgba, depth, primId);
    ASSERT_GT(card.count, 500);
    ASSERT_GT(ball.count, 500);
    EXPECT_NEAR(10., card.minDepth, 0.01);
    EXPECT_NEAR(10., card.maxDepth, 0.01);
    EXPECT_NEAR(9., ball.minDepth, 0.05);
    EXPECT_LT(ball.maxDepth, 10.);
    // Mirror images about the centre: the card left, the ball right, both at the camera's height.
    EXPECT_LT(card.x, kWidth / 2.);
    EXPECT_GT(ball.x, kWidth / 2.);
    EXPECT_NEAR(kWidth / 2., (card.x + ball.x) / 2., 3.);
    EXPECT_NEAR(kHeight / 2., card.y, 3.);
    EXPECT_NEAR(kHeight / 2., ball.y, 3.);
    // The card is twice as wide as it is high.
    int cardLeft = kWidth, cardRight = -1, cardBottom = kHeight, cardTop = -1;
    const std::uint32_t cardId = bitsOf(HydraRender::hashToFloat(HydraRender::primPathHash("/Left/Card")));
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            if (bitsOf(primId[(std::size_t)y * (std::size_t)kWidth + (std::size_t)x]) == cardId) {
                cardLeft = std::min(cardLeft, x);
                cardRight = std::max(cardRight, x);
                cardBottom = std::min(cardBottom, y);
                cardTop = std::max(cardTop, y);
            }
        }
    }
    ASSERT_GT(cardTop, cardBottom);
    EXPECT_NEAR(2., (double)(cardRight - cardLeft + 1) / (double)(cardTop - cardBottom + 1), 0.15);
    // The light faces both head on. The bound material makes the ball red; the card keeps the
    // neutral fallback colour.
    EXPECT_GT(ball.rgb[0], 0.2) << "ball colour " << ball.rgb[0] << ", " << ball.rgb[1] << ", " << ball.rgb[2];
    EXPECT_GT(ball.rgb[0], 2. * ball.rgb[1]);
    EXPECT_GT(ball.rgb[0], 2. * ball.rgb[2]);
    EXPECT_GT(card.rgb[0], 0.1) << "card colour " << card.rgb[0] << ", " << card.rgb[1] << ", " << card.rgb[2];
    EXPECT_NEAR(card.rgb[0], card.rgb[1], 0.05);
    EXPECT_NEAR(card.rgb[0], card.rgb[2], 0.05);

    // Only the two meshes are drawn; the material, camera and light are not.
    std::set<std::uint32_t> ids;
    for (std::size_t i = 0; i < pixels; ++i) {
        const std::uint32_t id = bitsOf(primId[i]);
        if (id != 0) {
            ids.insert(id);
        }
    }
    EXPECT_EQ(2u, ids.size());
} // TEST_F
