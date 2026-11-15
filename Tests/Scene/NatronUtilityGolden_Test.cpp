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

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <pxr/base/tf/token.h>
#include <pxr/imaging/hio/image.h>

#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>

#include "BaseTest.h"
#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/HydraRenderImpl.h"
#include "Engine/Nodes/Scene/RenderDelegates.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/Nodes/Scene/USDRender.h"
#include "Engine/Nodes/Scene/USDRenderSettings.h"
#include "Engine/Nodes/Scene/Utility/UtilityRenderDelegate.h"
#include "Engine/Nodes/Scene/Utility/UtilitySubdiv.h"
#include "RenderSettingsTestUtils.h"
#include "Tests/Native/NativeGolden.h"
#include "ofxNatron.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/AppManager.h"
#include "Engine/Gpu/GpuRuntime.h"
#include "Engine/GpuContext.h"
#include "Engine/Nodes/Scene/Utility/WavefrontGpu.h"
#include "Gpu/GpuTestDevice.h"
#include "Gpu/GpuTestSelector.h"
#endif

NATRON_NAMESPACE_USING

namespace {
const char* const kFixture = NATRON_TESTS_FIXTURES_DIR "/scene-reference.usda";
const char* const kGroup = "NatronUtility";

// The default clipping range of the reference scene's camera, which sets no clippingRange.
const double kNear = 1.;
const double kFar = 1e6;

// The stored depth is a distance along the view axis, recovered from a 32-bit window depth. The
// ray tracer's approximate reciprocals differ by runner CPU, which moves that window depth by about
// one of its ulps, many Z ulps near the far end of the range. So depth alone is accepted within
// that many window ulps at the stored Z; colour and primId stay bit for bit.
bool
sameDepth(float reference,
          float rendered)
{
    if (!std::isfinite(reference) || !std::isfinite(rendered)) {
        return (reference == rendered) || (std::isnan(reference) && std::isnan(rendered));
    }
    const double z = reference;
    const double window = kFar * (1. - kNear / z) / (kFar - kNear);
    if (window <= 0.) {
        return reference == rendered;
    }
    const double windowUlp = std::ldexp(1., std::ilogb(window) - 23);
    const double zPerWindow = z * z * (kFar - kNear) / (kFar * kNear);
    const double tolerance = 1.5 * windowUlp * zPerWindow;

    return std::fabs(z - (double)rendered) <= tolerance;
}

const int kTextureSize = 8;

// A checker whose two colours shade the other way along each axis, so that any shift, flip or
// stretch of the texture coordinates changes the picture.
bool
writeCheckerTexture(const std::string& path)
{
    std::vector<unsigned char> bytes;

    for (int y = 0; y < kTextureSize; ++y) {
        for (int x = 0; x < kTextureSize; ++x) {
            const bool light = ((x + y) % 2) == 0;

            bytes.push_back(light ? 230 : 30);
            bytes.push_back((unsigned char)(light ? 60 + 20 * x : 50));
            bytes.push_back((unsigned char)(light ? 40 : 70 + 20 * y));
            bytes.push_back(255);
        }
    }

    PXR_NS::HioImage::StorageSpec spec;

    spec.width = kTextureSize;
    spec.height = kTextureSize;
    spec.depth = 1;
    spec.format = PXR_NS::HioFormatUNorm8Vec4;
    spec.flipped = false;
    spec.data = bytes.data();

    const PXR_NS::HioImageSharedPtr image = PXR_NS::HioImage::OpenForWriting(path);

    return image && image->Write(spec);
}

// A cube cage turned to show three faces, each face carrying the whole texture through one indexed
// face-varying st, so every cube edge is a UV seam. Refined, it is a rounded box.
bool
writeTexturedCubeScene(const std::string& scenePath,
                       const std::string& texturePath)
{
    const std::string usda = std::string("#usda 1.0\n"
                                         "(\n"
                                         "    defaultPrim = \"World\"\n"
                                         "    upAxis = \"Y\"\n"
                                         ")\n"
                                         "def Xform \"World\" {\n"
                                         "    def Camera \"Cam\" {\n"
                                         "        float focalLength = 35\n"
                                         "        double3 xformOp:translate = (0, 0, 8)\n"
                                         "        uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
                                         "    }\n"
                                         "    def Material \"Tex\" {\n"
                                         "        token outputs:surface.connect = </World/Tex/Surface.outputs:surface>\n"
                                         "        def Shader \"Surface\" {\n"
                                         "            uniform token info:id = \"UsdPreviewSurface\"\n"
                                         "            color3f inputs:diffuseColor.connect = </World/Tex/Image.outputs:rgb>\n"
                                         "            token outputs:surface\n"
                                         "        }\n"
                                         "        def Shader \"Image\" {\n"
                                         "            uniform token info:id = \"UsdUVTexture\"\n"
                                         "            asset inputs:file = @")
        + texturePath + "@\n"
                        "            token inputs:sourceColorSpace = \"raw\"\n"
                        "            token inputs:wrapS = \"clamp\"\n"
                        "            token inputs:wrapT = \"clamp\"\n"
                        "            float2 inputs:st.connect = </World/Tex/St.outputs:result>\n"
                        "            float3 outputs:rgb\n"
                        "        }\n"
                        "        def Shader \"St\" {\n"
                        "            uniform token info:id = \"UsdPrimvarReader_float2\"\n"
                        "            string inputs:varname = \"st\"\n"
                        "            float2 outputs:result\n"
                        "        }\n"
                        "    }\n"
                        "    def Mesh \"Box\" (prepend apiSchemas = [\"MaterialBindingAPI\"]) {\n"
                        "        int[] faceVertexCounts = [4, 4, 4, 4, 4, 4]\n"
                        "        int[] faceVertexIndices = [0, 1, 3, 2, 2, 3, 5, 4, 4, 5, 7, 6, 6, 7, 1, 0, 1, 7, 5, 3, 6, 0, 2, 4]\n"
                        "        point3f[] points = [(-1.2, -1.2, 1.2), (1.2, -1.2, 1.2), (-1.2, 1.2, 1.2), (1.2, 1.2, 1.2), "
                        "(-1.2, 1.2, -1.2), (1.2, 1.2, -1.2), (-1.2, -1.2, -1.2), (1.2, -1.2, -1.2)]\n"
                        "        texCoord2f[] primvars:st = [(0, 0), (1, 0), (1, 1), (0, 1)] (interpolation = \"faceVarying\")\n"
                        "        int[] primvars:st:indices = [0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3]\n"
                        "        rel material:binding = </World/Tex>\n"
                        "        float3 xformOp:rotateXYZ = (30, 40, 0)\n"
                        "        uniform token[] xformOpOrder = [\"xformOp:rotateXYZ\"]\n"
                        "    }\n"
                        "}\n";
    QFile file(QString::fromStdString(scenePath));

    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }

    return file.write(usda.data(), (qint64)usda.size()) == (qint64)usda.size();
}

#ifdef NATRON_HAVE_GPU
// Puts a GPU context of the test's own in the application's place while it lives, able to pick a
// CPU-type device.
class OwnGpuContext {
public:
    OwnGpuContext()
    {
        GpuContextOptions options;
        std::shared_ptr<std::string> logged = std::make_shared<std::string>();

        options.mode = GpuResolvedMode::Force;
        options.device = gputest::cpuFallbackSelector();
        options.infoLog = [logged](const std::string& line) {
            *logged = line;
        };

        std::unique_ptr<GpuContext> own(new GpuContext(options));

        if (!own->waitForInit(std::chrono::seconds(60)) || !own->isAvailable()) {
            unavailable = logged->empty() ? std::string("the GPU context did not come up") : *logged;

            return;
        }
        _saved = appPTR->swapGpuContextForTests(std::move(own));
        _swapped = true;
    }

    ~OwnGpuContext()
    {
        HydraRender::releaseAll();
        appPTR->clearAllCaches();
        if (_swapped) {
            appPTR->swapGpuContextForTests(std::move(_saved));
        }
    }

    gpu::GpuRuntime* runtime() const
    {
        return _swapped ? appPTR->getGpuContext()->runtime() : nullptr;
    }

    std::string unavailable;

private:
    std::unique_ptr<GpuContext> _saved;
    bool _swapped = false;
};
#endif // NATRON_HAVE_GPU

class NatronUtilityGoldenTest
    : public BaseTest {
protected:
    // The goldens hold one ray through each pixel's centre, and are the cage, which a level above 0
    // would refine.
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        HydraRender::setRendererSettings(kRenderDelegateUtilityId, { { kUtilitySettingPixelSamples, PXR_NS::VtValue(1) },
                                                                     { kUtilitySettingSubdivLevel, PXR_NS::VtValue(0) } });
    }

    virtual void TearDown() OVERRIDE
    {
        HydraRender::setRendererSettings(kRenderDelegateUtilityId, std::map<std::string, PXR_NS::VtValue>());
        HydraRender::releaseAll();
        SceneQueryPool::instance().releaseIdle();
        BaseTest::TearDown();
    }

    // On device, one ray through each pixel's centre, meshes refined to level.
    static void setUtilitySettings(const char* device,
                                   int level)
    {
        HydraRender::setRendererSettings(kRenderDelegateUtilityId, { { kUtilitySettingPixelSamples, PXR_NS::VtValue(1) }, { kUtilitySettingSubdivLevel, PXR_NS::VtValue(level) }, { kUtilitySettingDevice, PXR_NS::VtValue(PXR_NS::TfToken(device)) } });
        HydraRender::releaseAll();
    }

    // The textured cube, refined to level two, on device.
    void checkTexturedSubdivision(const char* device,
                                  const std::string& caseName)
    {
        QTemporaryDir dir;

        ASSERT_TRUE(dir.isValid());
        const std::string texture = dir.filePath(QString::fromUtf8("checker.png")).toStdString();
        const std::string scene = dir.filePath(QString::fromUtf8("box.usda")).toStdString();

        ASSERT_TRUE(writeCheckerTexture(texture));
        ASSERT_TRUE(writeTexturedCubeScene(scene, texture));
        setUtilitySettings(device, 2);

        NodePtr render = createRender(scene);

        ASSERT_TRUE(render != nullptr);
        GoldenPlaneAcceptance acceptance;

        acceptance["depth"] = sameDepth;
        EXPECT_TRUE(checkGolden(render, kGroup, caseName, 0, 1., acceptance));
        HydraRender::releaseAll();
    }

    // USDRead of scene, the reference scene by default, through a USDRenderSettings framing its
    // camera at the golden window's size, into a Natron Utility USDRender.
    NodePtr createRender(const std::string& scene = kFixture)
    {
        NodePtr reader = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));
        NodePtr settings = createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDERSETTINGS));
        NodePtr render = createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDER));

        if (!reader || !settings || !render) {
            ADD_FAILURE() << "USDRead, USDRenderSettings or USDRender is not registered";

            return NodePtr();
        }
        KnobFile* file = dynamic_cast<KnobFile*>(reader->getKnobByName(kUSDReadParamFile).get());
        KnobChoice* renderer = dynamic_cast<KnobChoice*>(render->getKnobByName(kUSDRenderParamRenderer).get());
        KnobBool* headlight = dynamic_cast<KnobBool*>(render->getKnobByName(kUSDRenderParamHeadlight).get());

        if (!file || !renderer || !headlight) {
            ADD_FAILURE() << "missing knobs";

            return NodePtr();
        }
        file->setValue(scene);
        // The scenes have no lights, so only the headlight gives the colour goldens lit shading.
        headlight->setValue(true);
        if (!RenderSettingsTestUtils::configure(settings, "/World/Cam", kGoldenWidth, kGoldenHeight)) {
            return NodePtr();
        }

        const std::vector<ChoiceOption> entries = renderer->getEntries_mt_safe();
        bool selected = false;

        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].id == kRenderDelegateUtilityId) {
                renderer->setValue((int)i);
                selected = true;
            }
        }
        if (!selected) {
            ADD_FAILURE() << "USDRender does not list Natron Utility";

            return NodePtr();
        }
        connectNodes(reader, settings, 0, true);
        connectNodes(settings, render, 0, true);

        return render;
    }
};
} // anon namespace

TEST_F(NatronUtilityGoldenTest, ReferenceScene)
{
    NodePtr render = createRender();

    ASSERT_TRUE(render != nullptr);
    GoldenPlaneAcceptance acceptance;

    acceptance["depth"] = sameDepth;
    EXPECT_TRUE(checkGolden(render, kGroup, "reference_f1", 0, 1., acceptance));
    EXPECT_TRUE(checkGolden(render, kGroup, "reference_f24", 0, 24., acceptance));
}

TEST_F(NatronUtilityGoldenTest, TexturedSubdivisionCpu)
{
    checkTexturedSubdivision(kUtilityDeviceCpu, "subdiv_textured_cpu");
}

#ifdef NATRON_HAVE_GPU
TEST_F(NatronUtilityGoldenTest, TexturedSubdivisionGpu)
{
    OwnGpuContext gpu;

    GPUTEST_DEVICE_OR_SKIP(gpu.unavailable.empty(), gpu.unavailable);

    const std::string missing = WavefrontGpu::missingSupport(*gpu.runtime());

    if (!missing.empty()) {
        GPUTEST_SKIP() << "Natron Utility cannot render on this GPU: " << missing;
    }
    checkTexturedSubdivision(kUtilityDeviceGpu, "subdiv_textured_gpu");
}
#endif // NATRON_HAVE_GPU
