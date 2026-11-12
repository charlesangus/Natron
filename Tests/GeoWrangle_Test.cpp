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

#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "CacheMemoryPressureGuard.h"
#include "Native/NativePluginIdentity.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/GeoDetail.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Geometry/GeoPrimitive.h"
#include "Engine/Nodes/Geometry/GeoWrangle.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

std::vector<float>
positions(const GeoDetail& detail)
{
    const GeoAttribute& p = detail.position();
    std::vector<float> out(p.size() * 3);
    p.copyDense<float>(0, p.size(), std::span<float>(out));

    return out;
}

std::vector<float>
primitiveFloats(const GeoDetail& detail,
                const char* name)
{
    const GeoAttribute* a = detail.findAttribute(eGeoAttribClassPrimitive, name);
    if (!a) {
        return {};
    }
    std::vector<float> out(a->size());
    a->copyDense<float>(0, a->size(), std::span<float>(out));

    return out;
}

} // namespace

class GeoWrangleTest
    : public BaseTest {
protected:
    DisableUnreachableRAMPurging _noPurging;

    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        _cube = createNode(QString::fromUtf8(PLUGINID_NATRON_GEOPRIMITIVE));
        ASSERT_TRUE(_cube != NULL);
        KnobChoicePtr type = std::dynamic_pointer_cast<KnobChoice>(_cube->getKnobByName(kGeoPrimitiveParamType));
        ASSERT_TRUE(type != NULL);
        type->setValue((int)eGeoShapeCube);
        _nodes.push_back(_cube);
    }

    virtual void TearDown() OVERRIDE
    {
        for (auto it = _nodes.rbegin(); it != _nodes.rend(); ++it) {
            appPTR->removeAllCacheEntriesForHolder(it->get(), true);
            (*it)->destroyNode(false, false);
        }
        _nodes.clear();
        _cube.reset();
        BaseTest::TearDown();
    }

    NodePtr wrangle(const NodePtr& input,
                    const std::string& snippet,
                    int runOver = 0,
                    const std::string& group = std::string())
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_GEOWRANGLE));
        EXPECT_TRUE(node != NULL);
        if (!node) {
            return node;
        }
        _nodes.push_back(node);
        if (input) {
            connectNodes(input, node, 0, true);
        }
        setString(node, kGeoWrangleParamSnippet, snippet);
        setString(node, kGeoWrangleParamGroup, group);
        KnobChoicePtr runOverKnob = std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(kGeoWrangleParamRunOver));
        EXPECT_TRUE(runOverKnob != NULL);
        if (runOverKnob) {
            runOverKnob->setValue(runOver);
        }

        return node;
    }

    static void setString(const NodePtr& node,
                          const char* name,
                          const std::string& value)
    {
        KnobStringPtr knob = std::dynamic_pointer_cast<KnobString>(node->getKnobByName(name));
        ASSERT_TRUE(knob != NULL) << name;
        knob->setValue(value);
    }

    EffectInstance::RenderRoIRetCode pull(const NodePtr& node,
                                          double time,
                                          GeoDetailConstPtr* detail,
                                          std::string* error)
    {
        AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(true, 0);
        ParallelRenderArgsSetter frameRenderArgs(time,
                                                 ViewIdx(0),
                                                 true /*isRenderUserInteraction*/,
                                                 false /*isSequential*/,
                                                 abortInfo,
                                                 node,
                                                 0 /*textureIndex*/,
                                                 getApp()->getTimeLine().get(),
                                                 NodePtr(),
                                                 false /*isAnalysis*/,
                                                 false /*draftMode*/,
                                                 RenderStatsPtr());

        return node->getEffectInstance()->evaluateGeometry(EffectInstance::EvaluateGeometryArgs(time, ViewIdx(0)), detail, error);
    }

    NodePtr _cube;
    std::vector<NodePtr> _nodes;
};

TEST_F(GeoWrangleTest, Identity)
{
    expectNativeIdentity(PLUGINID_NATRON_GEOWRANGLE, 1, "GeoWrangle");
}

TEST_F(GeoWrangleTest, TheHelpPageCarriesTheBundledReference)
{
    NodePtr node = wrangle(_cube, "");
    ASSERT_TRUE(node != NULL);

    const QString page = node->makeDocumentation(false);
    EXPECT_TRUE(page.contains(QString::fromUtf8("## The language"))) << page.toStdString();
    EXPECT_TRUE(page.contains(QString::fromUtf8("valuenoise(p)")));
    EXPECT_FALSE(page.contains(QString::fromUtf8("Tolerance:")));
}

TEST_F(GeoWrangleTest, DoublingPZDoublesZ)
{
    NodePtr node = wrangle(_cube, "@P.z *= 2;");
    ASSERT_TRUE(node != NULL);

    GeoDetailConstPtr source;
    GeoDetailConstPtr result;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, pull(_cube, 1., &source, &error)) << error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, pull(node, 1., &result, &error)) << error;
    ASSERT_TRUE(source != NULL);
    ASSERT_TRUE(result != NULL);

    const std::vector<float> before = positions(*source);
    const std::vector<float> after = positions(*result);
    ASSERT_EQ(before.size(), after.size());
    ASSERT_FALSE(before.empty());
    bool sawNonZeroZ = false;
    for (std::size_t i = 0; i < before.size(); i += 3) {
        EXPECT_FLOAT_EQ(before[i], after[i]) << "point " << i / 3;
        EXPECT_FLOAT_EQ(before[i + 1], after[i + 1]) << "point " << i / 3;
        EXPECT_FLOAT_EQ(2.f * before[i + 2], after[i + 2]) << "point " << i / 3;
        sawNonZeroZ = sawNonZeroZ || before[i + 2] != 0.f;
    }
    EXPECT_TRUE(sawNonZeroZ);
    EXPECT_EQ(source->primitiveCount(), result->primitiveCount());
    EXPECT_FALSE(node->hasPersistentMessage());
}

TEST_F(GeoWrangleTest, AnEmptySnippetPassesTheInputThrough)
{
    NodePtr node = wrangle(_cube, "  \n");
    ASSERT_TRUE(node != NULL);

    GeoDetailConstPtr source;
    GeoDetailConstPtr result;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, pull(_cube, 1., &source, &error)) << error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, pull(node, 1., &result, &error)) << error;
    ASSERT_TRUE(result != NULL);
    EXPECT_EQ(positions(*source), positions(*result));
}

TEST_F(GeoWrangleTest, WithNoInputTheSnippetRunsOverEmptyGeometry)
{
    NodePtr node = wrangle(NodePtr(), "int p = addpoint(0, {1, 2, 3});", 3);
    ASSERT_TRUE(node != NULL);

    GeoDetailConstPtr result;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, pull(node, 1., &result, &error)) << error;
    ASSERT_TRUE(result != NULL);
    ASSERT_EQ(1, result->pointCount());
    EXPECT_EQ((std::vector<float> { 1.f, 2.f, 3.f }), positions(*result));
}

TEST_F(GeoWrangleTest, APrimitiveRunWithAGroupTouchesOnlyThatGroup)
{
    NodePtr grouping = wrangle(_cube, "int r = setprimgroup(0, \"picked\", @primnum, @primnum == 1 || @primnum == 4);", 1);
    ASSERT_TRUE(grouping != NULL);
    NodePtr marking = wrangle(grouping, "f@mark = 1; @Cd = {1, 0, 0};", 1, "picked");
    ASSERT_TRUE(marking != NULL);

    GeoDetailConstPtr result;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, pull(marking, 1., &result, &error)) << error;
    ASSERT_TRUE(result != NULL);
    ASSERT_GT(result->primitiveCount(), 4);

    const std::vector<float> mark = primitiveFloats(*result, "mark");
    ASSERT_EQ(std::size_t(result->primitiveCount()), mark.size());
    for (std::size_t i = 0; i < mark.size(); ++i) {
        EXPECT_FLOAT_EQ((i == 1 || i == 4) ? 1.f : 0.f, mark[i]) << "primitive " << i;
    }
}

TEST_F(GeoWrangleTest, AMissingGroupIsANodeError)
{
    NodePtr node = wrangle(_cube, "f@mark = 1;", 1, "nowhere");
    ASSERT_TRUE(node != NULL);

    GeoDetailConstPtr result;
    std::string error;
    EXPECT_EQ(EffectInstance::eRenderRoIRetCodeFailed, pull(node, 1., &result, &error));
    EXPECT_NE(std::string::npos, error.find("nowhere")) << error;
    EXPECT_TRUE(node->hasPersistentMessage());
}

TEST_F(GeoWrangleTest, ACompileErrorReportsLineAndColumnOnTheNode)
{
    NodePtr node = wrangle(_cube, "@P.z *= 2;\n@P.y = missing;");
    ASSERT_TRUE(node != NULL);

    GeoDetailConstPtr result;
    std::string error;
    EXPECT_EQ(EffectInstance::eRenderRoIRetCodeFailed, pull(node, 1., &result, &error));
    EXPECT_TRUE(result == NULL);
    EXPECT_NE(std::string::npos, error.find("line 2, column 8")) << error;
    EXPECT_NE(std::string::npos, error.find("missing")) << error;

    ASSERT_TRUE(node->hasPersistentMessage());
    QString message;
    int type = 0;
    node->getPersistentMessage(&message, &type, false);
    EXPECT_EQ(int(eMessageTypeError), type);
    EXPECT_NE(-1, message.indexOf(QString::fromUtf8("line 2, column 8"))) << message.toStdString();

    // The render thread survives the error: fixing the snippet renders and clears the message.
    setString(node, kGeoWrangleParamSnippet, "@P.z *= 2;\n@P.y = 0;");
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, pull(node, 1., &result, &error)) << error;
    ASSERT_TRUE(result != NULL);
    EXPECT_FALSE(node->hasPersistentMessage());
}

TEST_F(GeoWrangleTest, TheSecondInputIsReadable)
{
    NodePtr moved = wrangle(_cube, "@P += {10, 0, 0};");
    ASSERT_TRUE(moved != NULL);
    NodePtr node = wrangle(_cube, "@P = @opinput1_P;");
    ASSERT_TRUE(node != NULL);
    connectNodes(moved, node, 1, true);

    GeoDetailConstPtr expected;
    GeoDetailConstPtr result;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, pull(moved, 1., &expected, &error)) << error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, pull(node, 1., &result, &error)) << error;
    ASSERT_TRUE(result != NULL);
    EXPECT_EQ(positions(*expected), positions(*result));
}
