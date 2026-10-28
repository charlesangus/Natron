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

#include <cmath>
#include <cstdio>
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Scene/ProjectTexture.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/ViewIdx.h"
#include "RenderBothWays.h"

NATRON_NAMESPACE_USING

namespace {
const int kSize = 32;

// A camera at z = 10 looking down -z, seeing x and y in [-5, 5] at z = 0.
const char* const kCamera = R"(
def Camera "cam"
{
    float focalLength = 20
    float horizontalAperture = 20
    float verticalAperture = 20
    float2 clippingRange = (1, 1000)
    double3 xformOp:translate = (0, 0, 10)
    uniform token[] xformOpOrder = ["xformOp:translate"]
}
)";

// A 2 x 2 quad at z = 0 facing the camera (unless leftHanded), its st spanning [lo, hi] squared.
std::string
target(double lo = 0.,
       double hi = 1.,
       bool leftHanded = false)
{
    char st[160];

    std::snprintf(st, sizeof(st), "[(%g, %g), (%g, %g), (%g, %g), (%g, %g)]", lo, lo, hi, lo, hi, hi, lo, hi);

    return std::string("def Mesh \"Target\"\n{\n"
                       "    int[] faceVertexCounts = [4]\n"
                       "    int[] faceVertexIndices = [0, 1, 2, 3]\n"
                       "    point3f[] points = [(-1, -1, 0), (1, -1, 0), (1, 1, 0), (-1, 1, 0)]\n")
        + (leftHanded ? "    uniform token orientation = \"leftHanded\"\n" : "") + "    texCoord2f[] primvars:st = " + st + " (\n        interpolation = \"vertex\"\n    )\n}\n";
}

// A quad at z = 2 hiding the half x < 0 of the target from the camera.
const char* const kOccluder = R"(
def Mesh "Occluder"
{
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 1, 2, 3]
    point3f[] points = [(-2, -2, 2), (0, -2, 2), (0, 2, 2), (-2, 2, 2)]
}
)";

struct Texel {
    float r, g, b, a;
};

class ProjectTextureTest
    : public BaseTest {
protected:
    virtual void TearDown() OVERRIDE
    {
        SceneQueryPool::instance().releaseIdle();
        BaseTest::TearDown();
    }

    // USDRead of scene and a constant red into a ProjectTexture of the target at kSize squared,
    // without dilation.
    NodePtr createProjection(const std::string& scene)
    {
        if (!_dir.isValid()) {
            ADD_FAILURE() << "no temporary directory";

            return NodePtr();
        }
        const QString path = _dir.filePath(QString::fromUtf8("scene%1.usda").arg(++_files));
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            ADD_FAILURE() << "cannot write " << path.toStdString();

            return NodePtr();
        }
        file.write(("#usda 1.0\n" + std::string(kCamera) + scene).c_str());
        file.close();

        NodePtr reader = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));
        NodePtr constant = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
        NodePtr projection = createNode(QString::fromUtf8(PLUGINID_NATRON_PROJECTTEXTURE));
        if (!reader || !constant || !projection) {
            ADD_FAILURE() << "USDRead, Constant or ProjectTexture is not registered";

            return NodePtr();
        }
        KnobFile* fileKnob = dynamic_cast<KnobFile*>(reader->getKnobByName(kUSDReadParamFile).get());
        KnobColor* color = dynamic_cast<KnobColor*>(constant->getKnobByName(kConstantParamColor).get());
        KnobString* pattern = dynamic_cast<KnobString*>(projection->getKnobByName(kProjectTextureParamPrimPattern).get());
        KnobInt* resolution = dynamic_cast<KnobInt*>(projection->getKnobByName(kProjectTextureParamResolution).get());
        KnobInt* dilation = dynamic_cast<KnobInt*>(projection->getKnobByName(kProjectTextureParamEdgeDilation).get());
        if (!fileKnob || !color || !pattern || !resolution || !dilation) {
            ADD_FAILURE() << "missing knobs";

            return NodePtr();
        }
        fileKnob->setValue(path.toStdString());
        color->setValue(1., ViewSpec::all(), 0);
        color->setValue(0., ViewSpec::all(), 1);
        color->setValue(0., ViewSpec::all(), 2);
        color->setValue(1., ViewSpec::all(), 3);
        pattern->setValue(std::string("/Target"));
        resolution->setValue(kSize, ViewSpec::all(), 0);
        resolution->setValue(kSize, ViewSpec::all(), 1);
        dilation->setValue(0);
        connectNodes(reader, projection, kProjectTextureInputScene, true);
        connectNodes(constant, projection, kProjectTextureInputImage, true);

        return projection;
    }

    static bool render(const NodePtr& node,
                       std::vector<Texel>* texels,
                       std::string* error)
    {
        std::vector<RenderedPlane> planes;

        if (!renderNodePlanesDirect(node, 1., ViewIdx(0), 0, RectI(0, 0, kSize, kSize),
                                    std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()), &planes, error)) {
            return false;
        }
        if ((planes.size() != 1) || (planes[0].pixels.size() != (std::size_t)kSize * kSize * 4)) {
            *error = "unexpected planes";

            return false;
        }
        texels->resize((std::size_t)kSize * kSize);
        for (std::size_t i = 0; i < texels->size(); ++i) {
            const float* p = &planes[0].pixels[i * 4];
            (*texels)[i] = Texel { p[0], p[1], p[2], p[3] };
        }

        return true;
    }

    static void setBool(const NodePtr& node,
                        const char* name,
                        bool value)
    {
        KnobBool* knob = dynamic_cast<KnobBool*>(node->getKnobByName(name).get());
        ASSERT_TRUE(knob != NULL) << name;
        knob->setValue(value);
    }

    static std::string errorOf(const NodePtr& node)
    {
        QString message;
        int type = 0;

        node->getPersistentMessage(&message, &type, false);

        return message.toStdString();
    }

    QTemporaryDir _dir;
    int _files = 0;
};

bool
isRed(const Texel& t)
{
    return (std::abs(t.r - 1.f) < 1e-4f) && (std::abs(t.g) < 1e-4f) && (std::abs(t.b) < 1e-4f) && (std::abs(t.a - 1.f) < 1e-4f);
}

bool
isClear(const Texel& t)
{
    return (t.r == 0.f) && (t.g == 0.f) && (t.b == 0.f) && (t.a == 0.f);
}
} // anon namespace

TEST_F(ProjectTextureTest, AConstantFillsTheWholeLayoutOfACameraFacingQuad)
{
    NodePtr node = createProjection(target());
    ASSERT_TRUE(node != nullptr);
    std::vector<Texel> texels;
    std::string error;

    ASSERT_TRUE(render(node, &texels, &error)) << error << " " << errorOf(node);
    for (int i = 0; i < kSize * kSize; ++i) {
        ASSERT_TRUE(isRed(texels[i])) << "texel " << i % kSize << ", " << i / kSize << ": " << texels[i].r << " " << texels[i].a;
    }
}

TEST_F(ProjectTextureTest, AMeshInFrontHidesWhatItCoversOnlyWithOcclusion)
{
    NodePtr node = createProjection(target() + kOccluder);
    ASSERT_TRUE(node != nullptr);
    std::vector<Texel> texels;
    std::string error;

    // u runs with x on the target: the half u < 0.5 is behind the occluder.
    ASSERT_TRUE(render(node, &texels, &error)) << error << " " << errorOf(node);
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const double u = (x + 0.5) / kSize;
            const Texel& t = texels[(std::size_t)x + (std::size_t)y * kSize];
            if (u < 0.4) {
                EXPECT_TRUE(isClear(t)) << x << ", " << y << ": " << t.a;
            } else if (u > 0.6) {
                EXPECT_TRUE(isRed(t)) << x << ", " << y << ": " << t.a;
            }
        }
    }

    setBool(node, kProjectTextureParamOcclusion, false);
    ASSERT_TRUE(render(node, &texels, &error)) << error << " " << errorOf(node);
    for (int i = 0; i < kSize * kSize; ++i) {
        ASSERT_TRUE(isRed(texels[i])) << "texel " << i % kSize << ", " << i / kSize;
    }
}

TEST_F(ProjectTextureTest, FacesTurnedAwayFromTheCameraStayClear)
{
    NodePtr node = createProjection(target(0., 1., true));
    ASSERT_TRUE(node != nullptr);
    std::vector<Texel> texels;
    std::string error;

    ASSERT_TRUE(render(node, &texels, &error)) << error << " " << errorOf(node);
    for (int i = 0; i < kSize * kSize; ++i) {
        ASSERT_TRUE(isClear(texels[i])) << "texel " << i % kSize << ", " << i / kSize;
    }
}

TEST_F(ProjectTextureTest, DilationGrowsTheLayoutByItsWidthInTexels)
{
    // The layout covers texel columns and rows 8 to 23.
    NodePtr node = createProjection(target(0.25, 0.75));
    ASSERT_TRUE(node != nullptr);
    KnobInt* dilation = dynamic_cast<KnobInt*>(node->getKnobByName(kProjectTextureParamEdgeDilation).get());
    ASSERT_TRUE(dilation != NULL);
    std::vector<Texel> texels;
    std::string error;

    ASSERT_TRUE(render(node, &texels, &error)) << error << " " << errorOf(node);
    const std::size_t row = (std::size_t)(kSize / 2) * kSize;
    EXPECT_TRUE(isClear(texels[row + 7]));
    EXPECT_TRUE(isRed(texels[row + 8]));
    EXPECT_TRUE(isRed(texels[row + 23]));
    EXPECT_TRUE(isClear(texels[row + 24]));

    dilation->setValue(2);
    ASSERT_TRUE(render(node, &texels, &error)) << error << " " << errorOf(node);
    EXPECT_TRUE(isClear(texels[row + 5]));
    EXPECT_TRUE(isRed(texels[row + 6]));
    EXPECT_TRUE(isRed(texels[row + 25]));
    EXPECT_TRUE(isClear(texels[row + 26]));
    EXPECT_TRUE(isRed(texels[6 + 6 * kSize]));
    EXPECT_TRUE(isClear(texels[5 + 5 * kSize]));
}

TEST_F(ProjectTextureTest, AMissingPrimvarIsANodeErrorNamingTheMesh)
{
    NodePtr node = createProjection(target());
    ASSERT_TRUE(node != nullptr);
    KnobString* primvar = dynamic_cast<KnobString*>(node->getKnobByName(kProjectTextureParamUVPrimvar).get());
    ASSERT_TRUE(primvar != NULL);
    primvar->setValue(std::string("uv"));
    std::vector<Texel> texels;
    std::string error;

    EXPECT_FALSE(render(node, &texels, &error));
    ASSERT_TRUE(node->hasPersistentMessage());
    EXPECT_NE(std::string::npos, errorOf(node).find("/Target")) << errorOf(node);
}

TEST_F(ProjectTextureTest, APatternWithoutAMeshIsANodeError)
{
    NodePtr node = createProjection(target());
    ASSERT_TRUE(node != nullptr);
    KnobString* pattern = dynamic_cast<KnobString*>(node->getKnobByName(kProjectTextureParamPrimPattern).get());
    ASSERT_TRUE(pattern != NULL);
    pattern->setValue(std::string("/cam"));
    std::vector<Texel> texels;
    std::string error;

    EXPECT_FALSE(render(node, &texels, &error));
    ASSERT_TRUE(node->hasPersistentMessage());
    EXPECT_NE(std::string::npos, errorOf(node).find("/cam")) << errorOf(node);
}
