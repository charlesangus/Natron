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
#include <thread>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"
#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/Invert.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/Nodes/Scene/HydraRenderImpl.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDCamera.h"
#include "Engine/Nodes/Scene/USDCard.h"
#include "Engine/Nodes/Scene/USDEditTransform.h"
#include "Engine/Nodes/Scene/USDEmissiveMaterial.h"
#include "Engine/Nodes/Scene/USDLight.h"
#include "Engine/Nodes/Scene/USDMaterial.h"
#include "Engine/Nodes/Scene/USDMerge.h"
#include "Engine/Nodes/Scene/USDPrune.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/Nodes/Scene/USDRender.h"
#include "Engine/Nodes/Scene/USDSphere.h"
#include "Engine/Nodes/Scene/USDXformPrim.h"
#include "Engine/Project.h"
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

// The pixels of rgba more than half covered, alone in a scene on a transparent background. Unlike
// coverageOf it holds for a surface with an opacity, which Storm draws in its translucent pass,
// where no prim id is written.
Coverage
coverageByAlpha(const std::vector<float>& rgba)
{
    Coverage c;

    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const std::size_t i = (std::size_t)y * (std::size_t)kWidth + (std::size_t)x;
            if (rgba[i * 4 + 3] <= 0.5f) {
                continue;
            }
            ++c.count;
            c.x += x;
            c.y += y;
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
     * with a camera and a light. The unit card, scaled to 2 x 1, stands at (-1.5, 0, 0), the sphere of radius 1,
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

        KnobColor* diffuse = knob<KnobColor>(material, kUSDMaterialParamDiffuseColor);
        KnobChoice* renderCamera = knob<KnobChoice>(render, kUSDRenderParamCamera);
        KnobInt* size = knob<KnobInt>(render, kNatronParamFormatSize);
        KnobDouble* par = knob<KnobDouble>(render, kNatronParamFormatPar);
        if (!diffuse || !renderCamera || !size || !par) {
            return NodePtr();
        }
        diffuse->setValue(0.8, ViewSpec::all(), 0);
        diffuse->setValue(0.1, ViewSpec::all(), 1);
        diffuse->setValue(0.1, ViewSpec::all(), 2);

        bool ok = true;
        ok = ok && setString(card, kSceneParamPrimPath, "/Card");
        ok = ok && setTriple(card, kSceneParamScale, 2., 1., 1.);
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

    /**
     * A card at the origin facing the camera at (0, 0, 10), its material's Diffuse Color input
     * fed by a Constant, lit head on by a key light unless withLight is false, when the scene has
     * no light. Sets *constant to the Constant and returns the USDRender, kWidth x kHeight, null
     * after a gtest failure.
     **/
    // The constant feeds the diffuse input of a USDMaterial, or with emissive the one input of a
    // USDEmissiveMaterial.
    NodePtr createLiveTextureGraph(NodePtr* constant,
                                   bool withLight = true,
                                   bool emissive = false)
    {
        NodePtr source = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
        NodePtr material = createNode(QString::fromUtf8(emissive ? PLUGINID_NATRON_USDEMISSIVEMATERIAL : PLUGINID_NATRON_USDMATERIAL));
        NodePtr card = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCARD));
        NodePtr camera = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCAMERA));
        NodePtr light = withLight ? createNode(QString::fromUtf8(PLUGINID_NATRON_USDLIGHT)) : NodePtr();
        NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMERGE));
        NodePtr render = createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDER));
        if (!source || !material || !card || !camera || (withLight && !light) || !merge || !render) {
            ADD_FAILURE() << "a node is not registered";

            return NodePtr();
        }
        KnobChoice* renderCamera = knob<KnobChoice>(render, kUSDRenderParamCamera);
        KnobInt* size = knob<KnobInt>(render, kNatronParamFormatSize);
        KnobDouble* par = knob<KnobDouble>(render, kNatronParamFormatPar);
        if (!renderCamera || !size || !par) {
            return NodePtr();
        }
        bool ok = true;
        ok = ok && setString(material, kSceneParamPrimPath, "/Live");
        ok = ok && setString(card, kSceneParamPrimPath, "/Card");
        ok = ok && setString(camera, kSceneParamPrimPath, "/Cam");
        ok = ok && setTriple(camera, kSceneParamTranslate, 0., 0., 10.);
        ok = ok && (!light || setString(light, kSceneParamPrimPath, "/Key"));
        if (!ok) {
            return NodePtr();
        }
        connectNodes(source, material, emissive ? kUSDEmissiveMaterialInputEmissive : kUSDMaterialInputDiffuseColor, true);
        connectNodes(material, card, 0, true);
        connectNodes(card, merge, 0, true);
        connectNodes(camera, merge, 1, true);
        if (light) {
            connectNodes(light, merge, 2, true);
        }
        connectNodes(merge, render, 0, true);
        renderCamera->setActiveEntry(ChoiceOption("/Cam", "/Cam", ""));
        size->setValue(kWidth, ViewSpec::all(), 0);
        size->setValue(kHeight, ViewSpec::all(), 1);
        par->setValue(1.);
        *constant = source;

        return render;
    } // createLiveTextureGraph

    // The card's mean colour in render at frame, through the live texture graph. Rendered off the
    // main thread, where renders run outside the tests: LiveTextureStore::prepare does nothing on
    // the main thread, and the textures cannot be prepared beforehand because the render empties
    // the caches, the store included.
    static Coverage renderCard(const NodePtr& render,
                               double frame)
    {
        return coverageByAlpha(renderColour(render, frame));
    }

    // The colour plane of render at frame, rendered off the main thread as renderCard explains.
    static std::vector<float> renderColour(const NodePtr& render,
                                           double frame)
    {
        std::vector<RenderedPlane> planes;
        std::string error;
        bool rendered = false;

        std::thread worker([&]() {
            rendered = renderNodePlanesDirect(render, frame, ViewIdx(0), 0, RectI(0, 0, kWidth, kHeight), allPlanes(), &planes, &error);
        });
        worker.join();
        EXPECT_TRUE(rendered) << error;
        if (planes.size() != 3u) {
            ADD_FAILURE() << "frame " << frame << " rendered " << planes.size() << " planes";

            return std::vector<float>((std::size_t)kWidth * (std::size_t)kHeight * 4u, 0.f);
        }

        return planes[0].pixels;
    }

    // The mean colour and alpha of the 5 by 5 pixels at the centre of rgba.
    static void centreOf(const std::vector<float>& rgba,
                         double out[4])
    {
        for (int k = 0; k < 4; ++k) {
            out[k] = 0.;
        }
        for (int y = kHeight / 2 - 2; y <= kHeight / 2 + 2; ++y) {
            for (int x = kWidth / 2 - 2; x <= kWidth / 2 + 2; ++x) {
                const std::size_t i = (std::size_t)y * (std::size_t)kWidth + (std::size_t)x;
                for (int k = 0; k < 4; ++k) {
                    out[k] += rgba[i * 4 + k] / 25.;
                }
            }
        }
    }

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
    // vertex of the mesh. The sphere's visible cap ends at its silhouette, about 9.9 away, and
    // its pixel-edge depths can overshoot that slightly, so the far bound is the sphere's centre
    // plane plus a margin.
    const Coverage card = coverageOf("/Left/Card", rgba, depth, primId);
    const Coverage ball = coverageOf("/Right/Ball", rgba, depth, primId);
    ASSERT_GT(card.count, 500);
    ASSERT_GT(ball.count, 500);
    EXPECT_NEAR(10., card.minDepth, 0.01);
    EXPECT_NEAR(10., card.maxDepth, 0.01);
    EXPECT_NEAR(9., ball.minDepth, 0.05);
    EXPECT_LT(ball.maxDepth, 10.25);
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
    EXPECT_GT(ball.rgb[0], 0.1) << "ball colour " << ball.rgb[0] << ", " << ball.rgb[1] << ", " << ball.rgb[2];
    EXPECT_GT(ball.rgb[0], 3. * ball.rgb[1]);
    EXPECT_GT(ball.rgb[0], 3. * ball.rgb[2]);
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

TEST_F(USDRenderStormTest, ALiveTextureColoursTheCardWithTheImageConnected)
{
    if (skipWithoutGL()) {
        return;
    }
    NodePtr constant;
    NodePtr render = createLiveTextureGraph(&constant);
    ASSERT_TRUE(render != nullptr);
    KnobColor* color = knob<KnobColor>(constant, kConstantParamColor);
    ASSERT_TRUE(color != nullptr);
    color->setValue(1., ViewSpec::all(), 0);
    color->setValue(0., ViewSpec::all(), 1);
    color->setValue(0., ViewSpec::all(), 2);
    color->setValue(1., ViewSpec::all(), 3);

    const Coverage card = renderCard(render, 1.);
    ASSERT_GT(card.count, 500);
    EXPECT_FALSE(render->hasPersistentMessage());
    // Red, not Storm's grey fallback.
    EXPECT_GT(card.rgb[0], 0.1) << "card colour " << card.rgb[0] << ", " << card.rgb[1] << ", " << card.rgb[2];
    EXPECT_GT(card.rgb[0], 5. * card.rgb[1]);
    EXPECT_GT(card.rgb[0], 5. * card.rgb[2]);
}

// With no light in the scene a lit card is black, unless Headlight, off by default, adds a light at
// the camera; turning it off again takes the light back.
TEST_F(USDRenderStormTest, TheHeadlightLightsAnUnlitSceneOnlyWhenOn)
{
    if (skipWithoutGL()) {
        return;
    }
    NodePtr constant;
    NodePtr render = createLiveTextureGraph(&constant, false);
    ASSERT_TRUE(render != nullptr);
    KnobColor* color = knob<KnobColor>(constant, kConstantParamColor);
    KnobBool* headlight = knob<KnobBool>(render, kUSDRenderParamHeadlight);
    ASSERT_TRUE(color != nullptr);
    ASSERT_TRUE(headlight != nullptr);
    EXPECT_FALSE(headlight->getValue()) << "Headlight is off by default";
    EXPECT_EQ(std::string("Headlight"), headlight->getLabel());
    color->setValue(1., ViewSpec::all(), 0);
    color->setValue(0., ViewSpec::all(), 1);
    color->setValue(0., ViewSpec::all(), 2);
    color->setValue(1., ViewSpec::all(), 3);

    const Coverage off = renderCard(render, 1.);
    ASSERT_GT(off.count, 500);
    EXPECT_LT(off.rgb[0], 0.02) << "card colour, headlight off " << off.rgb[0] << ", " << off.rgb[1] << ", " << off.rgb[2];
    EXPECT_LT(off.rgb[1], 0.02);
    EXPECT_LT(off.rgb[2], 0.02);

    headlight->setValue(true);
    const Coverage on = renderCard(render, 1.);
    ASSERT_GT(on.count, 500);
    EXPECT_GT(on.rgb[0], 0.1) << "card colour, headlight on " << on.rgb[0] << ", " << on.rgb[1] << ", " << on.rgb[2];
    EXPECT_GT(on.rgb[0], 5. * on.rgb[1]);
    EXPECT_GT(on.rgb[0], 5. * on.rgb[2]);

    headlight->setValue(false);
    const Coverage offAgain = renderCard(render, 1.);
    ASSERT_GT(offAgain.count, 500);
    EXPECT_LT(offAgain.rgb[0], 0.02) << "card colour, headlight off again " << offAgain.rgb[0];
}

// With no light and no headlight, the emissive material's card shows its input as it is.
TEST_F(USDRenderStormTest, TheEmissiveMaterialShowsItsImageUnchangedWithNoLights)
{
    if (skipWithoutGL()) {
        return;
    }
    NodePtr constant;
    NodePtr render = createLiveTextureGraph(&constant, false, true);
    ASSERT_TRUE(render != nullptr);
    KnobColor* color = knob<KnobColor>(constant, kConstantParamColor);
    KnobBool* headlight = knob<KnobBool>(render, kUSDRenderParamHeadlight);
    ASSERT_TRUE(color != nullptr);
    ASSERT_TRUE(headlight != nullptr);
    ASSERT_FALSE(headlight->getValue());
    const double source[3] = { 0.6, 0.3, 0.1 };
    for (int c = 0; c < 3; ++c) {
        color->setValue(source[c], ViewSpec::all(), c);
    }
    color->setValue(1., ViewSpec::all(), 3);

    const Coverage card = renderCard(render, 1.);
    ASSERT_GT(card.count, 500);
    EXPECT_FALSE(render->hasPersistentMessage());
    for (int c = 0; c < 3; ++c) {
        EXPECT_NEAR(source[c], card.rgb[c], 0.03) << "channel " << c << ", card colour " << card.rgb[0] << ", " << card.rgb[1] << ", " << card.rgb[2];
    }
}

// A card showing a Constant of half-transparent red, premultiplied as Natron's images are, in front
// of an opaque blue card: the red composites over the blue, so half of each shows through.
TEST_F(USDRenderStormTest, AHalfTransparentLiveTextureShowsTheBackgroundThroughIt)
{
    if (skipWithoutGL()) {
        return;
    }
    NodePtr constant;
    NodePtr render = createLiveTextureGraph(&constant, false, true);
    ASSERT_TRUE(render != nullptr);
    NodePtr merge = render->getInput(0);
    ASSERT_TRUE(merge != nullptr);
    NodePtr backMaterial = createNode(QString::fromUtf8(PLUGINID_NATRON_USDEMISSIVEMATERIAL));
    NodePtr backCard = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCARD));
    ASSERT_TRUE(backMaterial && backCard);
    ASSERT_TRUE(setString(backMaterial, kSceneParamPrimPath, "/Blue"));
    ASSERT_TRUE(setString(backCard, kSceneParamPrimPath, "/Back"));
    ASSERT_TRUE(setTriple(backCard, kSceneParamTranslate, 0., 0., -1.));
    KnobColor* blue = knob<KnobColor>(backMaterial, kUSDEmissiveMaterialParamEmissiveColor);
    KnobColor* color = knob<KnobColor>(constant, kConstantParamColor);
    ASSERT_TRUE(blue && color);
    blue->setValue(0., ViewSpec::all(), 0);
    blue->setValue(0., ViewSpec::all(), 1);
    blue->setValue(1., ViewSpec::all(), 2);
    connectNodes(backMaterial, backCard, 0, true);
    connectNodes(backCard, merge, 2, true);
    color->setValue(0.5, ViewSpec::all(), 0);
    color->setValue(0., ViewSpec::all(), 1);
    color->setValue(0., ViewSpec::all(), 2);
    color->setValue(0.5, ViewSpec::all(), 3);

    double centre[4];
    centreOf(renderColour(render, 1.), centre);
    EXPECT_FALSE(render->hasPersistentMessage());
    const double expected[4] = { 0.5, 0., 0.5, 1. };
    for (int k = 0; k < 4; ++k) {
        EXPECT_NEAR(expected[k], centre[k], 0.04) << "channel " << k << ", centre " << centre[0] << ", " << centre[1] << ", " << centre[2] << ", " << centre[3];
    }
}

// A card showing red over the left half of its image, its edge softened, in front of a white card: the
// red composites over the white, so across the edge every pixel lies between red and white. Black
// filtered in from the transparent texels would show as a band darker than both.
TEST_F(USDRenderStormTest, ASoftLiveTextureEdgeHasNoDarkFringe)
{
    if (skipWithoutGL()) {
        return;
    }
    NodePtr constant;
    NodePtr render = createLiveTextureGraph(&constant, false, true);
    ASSERT_TRUE(render != nullptr);
    NodePtr merge = render->getInput(0);
    NodePtr material = render->getInput(0)->getInput(0)->getInput(0);
    ASSERT_TRUE(merge && material);
    ASSERT_EQ(std::string(PLUGINID_NATRON_USDEMISSIVEMATERIAL), material->getPluginID());
    NodePtr blur = createNode(QString::fromUtf8(PLUGINID_NATRON_BLUR), PLUGIN_MAJOR_NATRON_BLUR);
    NodePtr backMaterial = createNode(QString::fromUtf8(PLUGINID_NATRON_USDEMISSIVEMATERIAL));
    NodePtr backCard = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCARD));
    ASSERT_TRUE(blur && backMaterial && backCard);
    ASSERT_TRUE(setString(backMaterial, kSceneParamPrimPath, "/White"));
    ASSERT_TRUE(setString(backCard, kSceneParamPrimPath, "/Back"));
    ASSERT_TRUE(setTriple(backCard, kSceneParamTranslate, 0., 0., -1.));
    ASSERT_TRUE(setTriple(backCard, kSceneParamScale, 2., 2., 1.));
    KnobColor* white = knob<KnobColor>(backMaterial, kUSDEmissiveMaterialParamEmissiveColor);
    KnobColor* color = knob<KnobColor>(constant, kConstantParamColor);
    KnobChoice* extent = knob<KnobChoice>(constant, kNativeGeneratorParamExtent);
    KnobDouble* bottomLeft = knob<KnobDouble>(constant, kNativeGeneratorParamBottomLeft);
    KnobDouble* size = knob<KnobDouble>(constant, kNativeGeneratorParamSize);
    KnobDouble* blurSize = knob<KnobDouble>(blur, kBlurParamSize);
    ASSERT_TRUE(white && color && extent && bottomLeft && size && blurSize);
    for (int k = 0; k < 3; ++k) {
        white->setValue(1., ViewSpec::all(), k);
    }
    connectNodes(backMaterial, backCard, 0, true);
    connectNodes(backCard, merge, 2, true);

    Format format;
    getApp()->getProject()->getProjectDefaultFormat(&format);
    color->setValue(1., ViewSpec::all(), 0);
    color->setValue(0., ViewSpec::all(), 1);
    color->setValue(0., ViewSpec::all(), 2);
    color->setValue(1., ViewSpec::all(), 3);
    extent->setValueFromID(kNativeGeneratorExtentSize, 0);
    bottomLeft->setValue(format.x1 - format.width(), ViewSpec::all(), 0);
    bottomLeft->setValue(format.y1 - format.height(), ViewSpec::all(), 1);
    size->setValue(format.width() + format.width() / 2, ViewSpec::all(), 0);
    size->setValue(3 * format.height(), ViewSpec::all(), 1);
    // An edge a few texels wide: one wide enough to fade to almost nothing before its alpha reaches
    // 0 would hide the black, which Storm's mipmaps blend in a texel or so from where the alpha is 0.
    blurSize->setValue(3., ViewSpec::all(), 0);
    blurSize->setValue(3., ViewSpec::all(), 1);
    disconnectNodes(constant, material, true);
    connectNodes(constant, blur, 0, true);
    connectNodes(blur, material, kUSDEmissiveMaterialInputEmissive, true);

    const std::vector<float> rgba = renderColour(render, 1.);
    EXPECT_FALSE(render->hasPersistentMessage());
    bool sawRed = false;
    bool sawWhite = false;
    for (int y = kHeight / 2 - 2; y <= kHeight / 2 + 2; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const std::size_t i = (std::size_t)y * (std::size_t)kWidth + (std::size_t)x;
            const float* p = &rgba[i * 4];
            if (p[3] < 0.99f) {
                continue;
            }
            sawRed = sawRed || (p[1] < 0.05f);
            sawWhite = sawWhite || (p[1] > 0.95f);
            EXPECT_GT(p[0], 0.96f) << "pixel " << x << ", " << y << ": " << p[0] << ", " << p[1] << ", " << p[2];
            EXPECT_GT(p[1], -0.04f) << "pixel " << x << ", " << y;
            EXPECT_LT(p[1], 1.04f) << "pixel " << x << ", " << y;
            EXPECT_NEAR(p[1], p[2], 0.04) << "pixel " << x << ", " << y << ": " << p[0] << ", " << p[1] << ", " << p[2];
        }
    }
    EXPECT_TRUE(sawRed) << "the row crosses the red";
    EXPECT_TRUE(sawWhite) << "the row crosses the white";
}

// The node feeding a live texture, disabled, passes its input through, as it does in the 2D graph:
// an Invert of red shows cyan, and red once the Invert is disabled.
TEST_F(USDRenderStormTest, ALiveTextureFollowsADisabledNodeToItsInput)
{
    if (skipWithoutGL()) {
        return;
    }
    NodePtr constant;
    NodePtr render = createLiveTextureGraph(&constant, false, true);
    ASSERT_TRUE(render != nullptr);
    NodePtr material = render->getInput(0)->getInput(0)->getInput(0);
    ASSERT_TRUE(material != nullptr);
    ASSERT_EQ(std::string(PLUGINID_NATRON_USDEMISSIVEMATERIAL), material->getPluginID());
    NodePtr invert = createNode(QString::fromUtf8(PLUGINID_NATRON_INVERT));
    ASSERT_TRUE(invert != nullptr);
    KnobChannelSetPtr channels = std::dynamic_pointer_cast<KnobChannelSet>(invert->getKnobByName(kNodeParamChannelSet));
    ASSERT_TRUE(channels != nullptr);
    std::vector<std::string> rgb;
    rgb.push_back("R");
    rgb.push_back("G");
    rgb.push_back("B");
    channels->setChannels(0, rgb);
    disconnectNodes(constant, material, true);
    connectNodes(constant, invert, 0, true);
    connectNodes(invert, material, kUSDEmissiveMaterialInputEmissive, true);
    KnobColor* color = knob<KnobColor>(constant, kConstantParamColor);
    ASSERT_TRUE(color != nullptr);
    color->setValue(1., ViewSpec::all(), 0);
    color->setValue(0., ViewSpec::all(), 1);
    color->setValue(0., ViewSpec::all(), 2);
    color->setValue(1., ViewSpec::all(), 3);

    // The centre, not the card's mean: the mean takes in the antialiased edges, which pull a full
    // channel about 3% below 1.
    double inverted[4];
    centreOf(renderColour(render, 1.), inverted);
    EXPECT_NEAR(0., inverted[0], 0.03) << "inverted " << inverted[0] << ", " << inverted[1] << ", " << inverted[2];
    EXPECT_NEAR(1., inverted[1], 0.03);
    EXPECT_NEAR(1., inverted[2], 0.03);

    invert->setNodeDisabled(true);
    double passed[4];
    centreOf(renderColour(render, 1.), passed);
    EXPECT_FALSE(render->hasPersistentMessage());
    EXPECT_NEAR(1., passed[0], 0.03) << "disabled " << passed[0] << ", " << passed[1] << ", " << passed[2];
    EXPECT_NEAR(0., passed[1], 0.03);
    EXPECT_NEAR(0., passed[2], 0.03);
}

// The plan's animated Ramp is an animated Constant here: the Ramp is an OpenFX plug-in the test
// binary may not load, and an animated colour exercises the same per-frame URIs.
TEST_F(USDRenderStormTest, AnAnimatedLiveTextureFollowsTheFrame)
{
    if (skipWithoutGL()) {
        return;
    }
    NodePtr constant;
    NodePtr render = createLiveTextureGraph(&constant);
    ASSERT_TRUE(render != nullptr);
    KnobColor* color = knob<KnobColor>(constant, kConstantParamColor);
    ASSERT_TRUE(color != nullptr);
    color->setValueAtTime(1., 1., ViewSpec::all(), 0);
    color->setValueAtTime(10., 0., ViewSpec::all(), 0);
    color->setValueAtTime(1., 0., ViewSpec::all(), 1);
    color->setValueAtTime(10., 1., ViewSpec::all(), 1);
    color->setValue(0., ViewSpec::all(), 2);
    color->setValue(1., ViewSpec::all(), 3);

    const Coverage first = renderCard(render, 1.);
    const Coverage tenth = renderCard(render, 10.);
    ASSERT_GT(first.count, 500);
    ASSERT_GT(tenth.count, 500);
    EXPECT_GT(first.rgb[0], 5. * first.rgb[1]) << "frame 1 " << first.rgb[0] << ", " << first.rgb[1] << ", " << first.rgb[2];
    EXPECT_GT(tenth.rgb[1], 5. * tenth.rgb[0]) << "frame 10 " << tenth.rgb[0] << ", " << tenth.rgb[1] << ", " << tenth.rgb[2];
}

// A quad whose u runs from -0.5 to 1, bound to an emissive material showing a Constant that covers
// half a format past the format's left edge and the format's left half. Coordinates 0 to 1 span the
// format, so the overscan shows at negative u, the left third of the quad: the image covers its
// left two thirds, and the right third, the format's right half, is black.
TEST_F(USDRenderStormTest, OverscanShowsWhereTextureCoordinatesLeaveZeroToOne)
{
    if (skipWithoutGL()) {
        return;
    }
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString quadPath = tmp.path() + QString::fromUtf8("/quad.usda");
    {
        QFile file(quadPath);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("#usda 1.0\n"
                   "(\n    upAxis = \"Y\"\n    metersPerUnit = 1\n)\n"
                   "def Mesh \"Quad\" (\n    prepend apiSchemas = [\"MaterialBindingAPI\"]\n)\n{\n"
                   "    int[] faceVertexCounts = [4]\n"
                   "    int[] faceVertexIndices = [0, 1, 2, 3]\n"
                   "    point3f[] points = [(-1.5, -0.5, 0), (1.5, -0.5, 0), (1.5, 0.5, 0), (-1.5, 0.5, 0)]\n"
                   "    texCoord2f[] primvars:st = [(-0.5, 0), (1, 0), (1, 1), (-0.5, 1)] (\n        interpolation = \"vertex\"\n    )\n"
                   "    uniform token subdivisionScheme = \"none\"\n"
                   "    rel material:binding = </Live>\n"
                   "}\n");
    }

    Format format;
    getApp()->getProject()->getProjectDefaultFormat(&format);
    ASSERT_GT(format.width(), 8);
    NodePtr constant = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
    NodePtr material = createNode(QString::fromUtf8(PLUGINID_NATRON_USDEMISSIVEMATERIAL));
    NodePtr reader = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));
    NodePtr camera = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCAMERA));
    NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMERGE));
    NodePtr render = createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDER));
    ASSERT_TRUE(constant && material && reader && camera && merge && render);
    KnobColor* color = knob<KnobColor>(constant, kConstantParamColor);
    KnobChoice* extent = knob<KnobChoice>(constant, kNativeGeneratorParamExtent);
    KnobDouble* bottomLeft = knob<KnobDouble>(constant, kNativeGeneratorParamBottomLeft);
    KnobDouble* size = knob<KnobDouble>(constant, kNativeGeneratorParamSize);
    KnobFile* file = knob<KnobFile>(reader, kUSDReadParamFile);
    KnobChoice* renderCamera = knob<KnobChoice>(render, kUSDRenderParamCamera);
    KnobInt* renderSize = knob<KnobInt>(render, kNatronParamFormatSize);
    KnobDouble* par = knob<KnobDouble>(render, kNatronParamFormatPar);
    ASSERT_TRUE(color && extent && bottomLeft && size && file && renderCamera && renderSize && par);
    color->setValue(1., ViewSpec::all(), 0);
    color->setValue(0., ViewSpec::all(), 1);
    color->setValue(0., ViewSpec::all(), 2);
    color->setValue(1., ViewSpec::all(), 3);
    extent->setValueFromID(kNativeGeneratorExtentSize, 0);
    const int half = format.width() / 2;
    bottomLeft->setValue(format.x1 - half, ViewSpec::all(), 0);
    bottomLeft->setValue(format.y1, ViewSpec::all(), 1);
    size->setValue(2 * half, ViewSpec::all(), 0);
    size->setValue(format.height(), ViewSpec::all(), 1);
    file->setValue(quadPath.toStdString());
    ASSERT_TRUE(setString(material, kSceneParamPrimPath, "/Live"));
    KnobBool* useAlpha = knob<KnobBool>(material, kUSDEmissiveMaterialParamUseAlpha);
    ASSERT_TRUE(useAlpha != nullptr);
    // The quad is found by its prim id, which Storm does not write for a surface with an opacity.
    useAlpha->setValue(false);
    ASSERT_TRUE(setString(camera, kSceneParamPrimPath, "/Cam"));
    ASSERT_TRUE(setTriple(camera, kSceneParamTranslate, 0., 0., 10.));
    connectNodes(constant, material, kUSDEmissiveMaterialInputEmissive, true);
    connectNodes(reader, merge, 0, true);
    connectNodes(material, merge, 1, true);
    connectNodes(camera, merge, 2, true);
    connectNodes(merge, render, 0, true);
    renderCamera->setActiveEntry(ChoiceOption("/Cam", "/Cam", ""));
    renderSize->setValue(kWidth, ViewSpec::all(), 0);
    renderSize->setValue(kHeight, ViewSpec::all(), 1);
    par->setValue(1.);

    std::vector<RenderedPlane> planes;
    std::string error;
    bool rendered = false;
    std::thread worker([&]() {
        rendered = renderNodePlanesDirect(render, 1., ViewIdx(0), 0, RectI(0, 0, kWidth, kHeight), allPlanes(), &planes, &error);
    });
    worker.join();
    ASSERT_TRUE(rendered) << error;
    ASSERT_EQ(3u, planes.size());
    const std::vector<float>& rgba = planes[0].pixels;
    const std::vector<float>& primId = planes[2].pixels;
    const std::uint32_t quadId = bitsOf(HydraRender::hashToFloat(HydraRender::primPathHash("/Quad")));
    int left = kWidth;
    int right = -1;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            if (bitsOf(primId[(std::size_t)y * (std::size_t)kWidth + (std::size_t)x]) == quadId) {
                left = std::min(left, x);
                right = std::max(right, x);
            }
        }
    }
    ASSERT_GT(right - left, 30) << "the quad is drawn";

    // Mean red and green over the quad's pixels whose horizontal position across it is in [from, to].
    const auto meanOver = [&](double from,
                              double to,
                              double rg[2]) {
        int count = 0;
        rg[0] = rg[1] = 0.;
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                const std::size_t i = (std::size_t)y * (std::size_t)kWidth + (std::size_t)x;
                const double across = (x - left + 0.5) / (double)(right - left + 1);
                if ((bitsOf(primId[i]) != quadId) || (across < from) || (across > to)) {
                    continue;
                }
                rg[0] += rgba[i * 4 + 0];
                rg[1] += rgba[i * 4 + 1];
                ++count;
            }
        }
        if (count) {
            rg[0] /= count;
            rg[1] /= count;
        }

        return count;
    };
    double overscan[2];
    double formatLeft[2];
    double formatRight[2];
    ASSERT_GT(meanOver(0.05, 0.28, overscan), 50);
    ASSERT_GT(meanOver(0.39, 0.61, formatLeft), 50);
    ASSERT_GT(meanOver(0.72, 0.95, formatRight), 50);
    EXPECT_GT(overscan[0], 0.9) << "u < 0 reads the overscan, red " << overscan[0];
    EXPECT_LT(overscan[1], 0.05);
    EXPECT_GT(formatLeft[0], 0.9) << "0 < u < 0.5 is the format's left half, red " << formatLeft[0];
    EXPECT_LT(formatRight[0], 0.05) << "0.5 < u < 1 is the format's right half the image does not cover, red " << formatRight[0];
}
