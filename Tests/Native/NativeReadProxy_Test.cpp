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

#include <cstddef>
#include <cstdio>
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <OpenImageIO/imageio.h>

#include <QString>
#include <QTemporaryDir>

#include <ofxImageEffect.h>

#include "BaseTest.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/IO/NativeRead.h"
#include "Engine/Nodes/IO/OiioReadSupport.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
const int kFullSize = 16;
const int kProxySize = 8;
const float kNotRendered = -1.f;

// Frame 3 is absent. Every value is exact in float, so it can be compared with EXPECT_EQ.
const int kFrames[] = { 1, 2, 4 };
const float kFullValues[] = { 0.125f, 0.25f, 0.5f };
const float kProxyValues[] = { 0.625f, 0.75f, 0.875f };

bool
writeConstantImage(const std::string& path,
                   int size,
                   float value)
{
    OIIO::ImageSpec spec(size, size, 4, OIIO::TypeDesc::FLOAT);
    std::vector<float> pixels((std::size_t)size * size * 4, value);
    OIIO::ImageOutput::unique_ptr out = OIIO::ImageOutput::create(path);

    return out && out->open(path, spec) && out->write_image(OIIO::TypeDesc::FLOAT, pixels.data()) && out->close();
}

std::string
framePath(const QTemporaryDir& dir,
          const char* stem,
          int frame)
{
    char number[16];

    snprintf(number, sizeof(number), "%04d", frame);

    return (dir.path() + QString::fromUtf8("/%1.%2.exr").arg(QString::fromUtf8(stem), QString::fromUtf8(number))).toStdString();
}

std::string
patternPath(const QTemporaryDir& dir,
            const char* stem)
{
    return (dir.path() + QString::fromUtf8("/%1.####.exr").arg(QString::fromUtf8(stem))).toStdString();
}

std::string
stillPath(const QTemporaryDir& dir,
          const char* name)
{
    return (dir.path() + QString::fromUtf8("/%1").arg(QString::fromUtf8(name))).toStdString();
}
} // namespace

class NativeReadProxyTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
        OiioReadSupport::clearHeaderCache();
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    NodePtr createRead()
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_READ), PLUGIN_MAJOR_NATRON_READ);
        EXPECT_TRUE(bool(node));
        EXPECT_TRUE(node && dynamic_cast<NativeRead*>(node->getEffectInstance().get()));

        return node;
    }

    void setFile(const NodePtr& node,
                 const char* knobName,
                 const std::string& path)
    {
        KnobFile* file = dynamic_cast<KnobFile*>(node->getKnobByName(knobName).get());
        ASSERT_TRUE(file != NULL) << knobName;
        file->setValue(path);
        node->getEffectInstance()->refreshMetadata_public(false);
    }

    NodePtr createStillRead(const QTemporaryDir& dir)
    {
        const std::string full = stillPath(dir, "full.exr");
        const std::string proxy = stillPath(dir, "proxy.exr");
        EXPECT_TRUE(writeConstantImage(full, kFullSize, 0.25f));
        EXPECT_TRUE(writeConstantImage(proxy, kProxySize, 0.75f));

        NodePtr node = createRead();
        setFile(node, kOfxImageEffectFileParamName, full);
        setFile(node, kOfxImageEffectProxyParamName, proxy);

        return node;
    }

    // The value of the first pixel of the frame rendered at `time` and `level`, or kNotRendered on
    // failure. Every pixel of the plane is checked to be that value.
    float render(const NodePtr& node,
                 double time,
                 unsigned int level)
    {
        const int size = kFullSize >> level;
        std::list<ImageLayerDesc> layers(1, ImageLayerDesc::getRGBAComponents());
        std::vector<RenderedPlane> planes;
        std::string error;

        if (!renderNodePlanesDirect(node, time, ViewIdx(0), level, RectI(0, 0, size, size), layers, &planes, &error) || planes.size() != 1 || planes[0].pixels.empty()) {
            return kNotRendered;
        }
        for (std::size_t i = 0; i < planes[0].pixels.size(); ++i) {
            EXPECT_EQ(planes[0].pixels[0], planes[0].pixels[i]) << i;
        }

        return planes[0].pixels[0];
    }

    RectD rodAt(const NodePtr& node,
                double time,
                unsigned int level)
    {
        RectD rod;

        NativeRead* read = dynamic_cast<NativeRead*>(node->getEffectInstance().get());
        EXPECT_TRUE(read != NULL);
        if (read) {
            EXPECT_EQ(eStatusOK, read->getRegionOfDefinition(0, time, RenderScale::fromMipmapLevel(level), ViewIdx(0), &rod));
        }

        return rod;
    }

    double doubleValue(const NodePtr& node,
                       const char* name,
                       int dimension)
    {
        KnobDouble* k = dynamic_cast<KnobDouble*>(node->getKnobByName(name).get());
        EXPECT_TRUE(k != NULL) << name;

        return k ? k->getValue(dimension) : -1.;
    }
};

TEST_F(NativeReadProxyTest, TheProxyScaleIsComputedFromTheTwoFiles)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    NodePtr node = createStillRead(dir);

    for (int d = 0; d < 2; ++d) {
        EXPECT_EQ(0.5, doubleValue(node, "originalProxyScale", d));
        EXPECT_EQ(0.5, doubleValue(node, "proxyThreshold", d));
    }
}

TEST_F(NativeReadProxyTest, MipmapOneComesFromTheProxyAndMipmapZeroFromTheFullFile)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    NodePtr node = createStillRead(dir);

    EXPECT_EQ(0.25f, render(node, 1., 0));
    EXPECT_EQ(0.75f, render(node, 1., 1));
}

TEST_F(NativeReadProxyTest, TheProxyFurtherDownscalesBelowItsOwnScale)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    NodePtr node = createStillRead(dir);

    EXPECT_EQ(0.75f, render(node, 1., 2));
}

TEST_F(NativeReadProxyTest, TheRegionOfDefinitionIsTheFullResolutionOneAtEveryLevel)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    NodePtr node = createStillRead(dir);
    const RectD expected = RectI(0, 0, kFullSize, kFullSize).toCanonical_noClipping(0, 1.);

    for (unsigned int level = 0; level < 3; ++level) {
        const RectD rod = rodAt(node, 1., level);
        EXPECT_EQ(expected.x1, rod.x1) << level;
        EXPECT_EQ(expected.y1, rod.y1) << level;
        EXPECT_EQ(expected.x2, rod.x2) << level;
        EXPECT_EQ(expected.y2, rod.y2) << level;
    }
    NativeRead* read = dynamic_cast<NativeRead*>(node->getEffectInstance().get());
    ASSERT_TRUE(read != NULL);
    EXPECT_TRUE(RectI(0, 0, kFullSize, kFullSize) == read->getOutputFormat());
}

TEST_F(NativeReadProxyTest, WithoutAProxyMipmapOneDownscalesTheFullFile)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    NodePtr node = createStillRead(dir);

    setFile(node, kOfxImageEffectProxyParamName, std::string());
    EXPECT_EQ(0.25f, render(node, 1., 1));
}

TEST_F(NativeReadProxyTest, ACustomThresholdBelowTheProxyScaleKeepsTheFullFileAtMipmapOne)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    NodePtr node = createStillRead(dir);

    KnobBool* custom = dynamic_cast<KnobBool*>(node->getKnobByName("customProxyScale").get());
    KnobDouble* threshold = dynamic_cast<KnobDouble*>(node->getKnobByName("proxyThreshold").get());
    ASSERT_TRUE(custom != NULL);
    ASSERT_TRUE(threshold != NULL);
    custom->setValue(true);
    threshold->setValue(0.25, ViewSpec::all(), 0);
    threshold->setValue(0.25, ViewSpec::all(), 1);

    EXPECT_EQ(0.25f, render(node, 1., 1));
    EXPECT_EQ(0.75f, render(node, 1., 2));
    EXPECT_EQ(0.5, doubleValue(node, "originalProxyScale", 0));
}

TEST_F(NativeReadProxyTest, TheProxySequenceFollowsTheSameTimeRules)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(writeConstantImage(framePath(dir, "full", kFrames[i]), kFullSize, kFullValues[i]));
        ASSERT_TRUE(writeConstantImage(framePath(dir, "proxy", kFrames[i]), kProxySize, kProxyValues[i]));
    }
    NodePtr node = createRead();
    setFile(node, kOfxImageEffectFileParamName, patternPath(dir, "full"));
    setFile(node, kOfxImageEffectProxyParamName, patternPath(dir, "proxy"));

    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(kFullValues[i], render(node, kFrames[i], 0)) << kFrames[i];
        EXPECT_EQ(kProxyValues[i], render(node, kFrames[i], 1)) << kFrames[i];
    }

    KnobChoice* onMissing = dynamic_cast<KnobChoice*>(node->getKnobByName("onMissingFrame").get());
    ASSERT_TRUE(onMissing != NULL);
    onMissing->setValue(0);
    EXPECT_EQ(kFullValues[1], render(node, 3., 0));
    EXPECT_EQ(kProxyValues[1], render(node, 3., 1));
}
