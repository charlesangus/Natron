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

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "CacheMemoryPressureGuard.h"

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

int
userKnobCount(const NodePtr& node)
{
    int n = 0;
    for (const KnobIPtr& knob : node->getEffectInstance()->getKnobs_mt_safe()) {
        if (knob->isUserKnob() && !dynamic_cast<KnobPage*>(knob.get())) {
            ++n;
        }
    }

    return n;
}

} // namespace

class GeoWrangleParamsTest
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

        _wrangle = createNode(QString::fromUtf8(PLUGINID_NATRON_GEOWRANGLE));
        ASSERT_TRUE(_wrangle != NULL);
        connectNodes(_cube, _wrangle, 0, true);
    }

    virtual void TearDown() OVERRIDE
    {
        NodePtr* nodes[] = { &_wrangle, &_cube };
        for (NodePtr* node : nodes) {
            if (*node) {
                appPTR->removeAllCacheEntriesForHolder(node->get(), true);
                (*node)->destroyNode(false, false);
                node->reset();
            }
        }
        BaseTest::TearDown();
    }

    void setSnippet(const std::string& snippet)
    {
        KnobStringPtr knob = std::dynamic_pointer_cast<KnobString>(_wrangle->getKnobByName(kGeoWrangleParamSnippet));
        ASSERT_TRUE(knob != NULL);
        knob->setValue(snippet);
    }

    void pressCreateParameters()
    {
        KnobButtonPtr button = std::dynamic_pointer_cast<KnobButton>(_wrangle->getKnobByName(kGeoWrangleParamCreateParameters));
        ASSERT_TRUE(button != NULL);
        EXPECT_TRUE(button->trigger());
    }

    EffectInstance::RenderRoIRetCode pull(const NodePtr& node,
                                          double time,
                                          GeoDetailConstPtr* detail,
                                          std::string* error,
                                          bool byPassCache = false)
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

        return node->getEffectInstance()->evaluateGeometry(EffectInstance::EvaluateGeometryArgs(time, ViewIdx(0), byPassCache), detail, error);
    }

    NodePtr _cube;
    NodePtr _wrangle;
};

TEST_F(GeoWrangleParamsTest, TheSnippetsParameterCallsAreFoundOnceEach)
{
    const std::vector<GeoWrangleParameterCall> calls = geoWrangleParameterCalls(
        "// chf(\"commented\")\n"
        "float a = chf(\"amp\") + ch(\"amp\");\n"
        "int n = chi( \"count\" );\n"
        "vector o = chv(\"offset\");\n"
        "string s = chs(\"label\");\n"
        "float r = chramp(\"falloff\", @P.y);\n"
        "string t = \"chf(\\\"quoted\\\")\";\n");
    ASSERT_EQ(5u, calls.size());
    EXPECT_EQ("amp", calls[0].name);
    EXPECT_EQ("chf", calls[0].function);
    EXPECT_EQ("count", calls[1].name);
    EXPECT_EQ("chi", calls[1].function);
    EXPECT_EQ("offset", calls[2].name);
    EXPECT_EQ("label", calls[3].name);
    EXPECT_EQ("falloff", calls[4].name);
    EXPECT_EQ("chramp", calls[4].function);
}

TEST_F(GeoWrangleParamsTest, CreateParametersAddsAFloatOnceOnly)
{
    setSnippet("@P.y += chf(\"amp\");");
    const int before = userKnobCount(_wrangle);

    pressCreateParameters();
    KnobDoublePtr amp = std::dynamic_pointer_cast<KnobDouble>(_wrangle->getKnobByName("amp"));
    ASSERT_TRUE(amp != NULL);
    EXPECT_TRUE(amp->isUserKnob());
    EXPECT_EQ(1, amp->getDimension());
    EXPECT_EQ("Amp", amp->getLabel());
    EXPECT_EQ(before + 1, userKnobCount(_wrangle));

    pressCreateParameters();
    EXPECT_EQ(before + 1, userKnobCount(_wrangle));
    EXPECT_EQ(amp, std::dynamic_pointer_cast<KnobDouble>(_wrangle->getKnobByName("amp")));

    GeoWrangle* effect = dynamic_cast<GeoWrangle*>(_wrangle->getEffectInstance().get());
    ASSERT_TRUE(effect != NULL);
    EXPECT_TRUE(effect->createParameters().empty());
}

TEST_F(GeoWrangleParamsTest, CreateParametersPicksTheKnobTypeFromTheFunction)
{
    setSnippet("i@n = chi(\"count\");\n"
               "@P += chv(\"offsetAmount\");\n"
               "s@tag = chs(\"label\");\n"
               "@P.y *= chramp(\"falloff\", @P.y);\n");
    pressCreateParameters();

    KnobIntPtr count = std::dynamic_pointer_cast<KnobInt>(_wrangle->getKnobByName("count"));
    KnobDoublePtr offset = std::dynamic_pointer_cast<KnobDouble>(_wrangle->getKnobByName("offsetAmount"));
    KnobStringPtr label = std::dynamic_pointer_cast<KnobString>(_wrangle->getKnobByName("label"));
    KnobParametricPtr falloff = std::dynamic_pointer_cast<KnobParametric>(_wrangle->getKnobByName("falloff"));
    ASSERT_TRUE(count != NULL);
    ASSERT_TRUE(offset != NULL);
    ASSERT_TRUE(label != NULL);
    ASSERT_TRUE(falloff != NULL);
    EXPECT_EQ(3, offset->getDimension());
    EXPECT_EQ("Offset Amount", offset->getLabel());

    count->setValue(4);
    offset->setValue(1., ViewSpec::all(), 0);
    label->setValue("tagged");

    GeoDetailConstPtr source;
    GeoDetailConstPtr result;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, pull(_cube, 1., &source, &error)) << error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, pull(_wrangle, 1., &result, &error)) << error;
    ASSERT_TRUE(result != NULL);

    const GeoAttribute* n = result->findAttribute(eGeoAttribClassPoint, "n");
    ASSERT_TRUE(n != NULL);
    std::vector<std::int32_t> ns(n->size());
    n->copyDense<std::int32_t>(0, n->size(), std::span<std::int32_t>(ns));
    ASSERT_FALSE(ns.empty());
    EXPECT_EQ(4, ns[0]);

    const std::vector<float> before = positions(*source);
    const std::vector<float> after = positions(*result);
    ASSERT_EQ(before.size(), after.size());
    for (std::size_t i = 0; i < before.size(); i += 3) {
        EXPECT_FLOAT_EQ(before[i] + 1.f, after[i]) << "point " << i / 3;
        EXPECT_FLOAT_EQ(before[i + 2], after[i + 2]) << "point " << i / 3;
    }
}

TEST_F(GeoWrangleParamsTest, AnAnimatedParameterIsFollowedAcrossFrames)
{
    setSnippet("@P.y += chf(\"amp\");");
    pressCreateParameters();
    KnobDoublePtr amp = std::dynamic_pointer_cast<KnobDouble>(_wrangle->getKnobByName("amp"));
    ASSERT_TRUE(amp != NULL);
    amp->setValueAtTime(1, 0.5, ViewSpec::all(), 0);
    amp->setValueAtTime(10, 5., ViewSpec::all(), 0);

    GeoDetailConstPtr source;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, pull(_cube, 1., &source, &error)) << error;
    const std::vector<float> before = positions(*source);

    const double frames[] = { 1., 10., 1. };
    for (double frame : frames) {
        GeoDetailConstPtr result;
        ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, pull(_wrangle, frame, &result, &error)) << error;
        ASSERT_TRUE(result != NULL);
        const float expected = float(amp->getValueAtTime(frame, 0));
        const std::vector<float> after = positions(*result);
        ASSERT_EQ(before.size(), after.size());
        for (std::size_t i = 0; i < before.size(); i += 3) {
            EXPECT_FLOAT_EQ(before[i + 1] + expected, after[i + 1]) << "frame " << frame << " point " << i / 3;
        }
    }
    EXPECT_FLOAT_EQ(0.5f, float(amp->getValueAtTime(1., 0)));
    EXPECT_FLOAT_EQ(5.f, float(amp->getValueAtTime(10., 0)));
}

TEST_F(GeoWrangleParamsTest, ARenamedParameterIsACompileErrorNamingIt)
{
    setSnippet("@P.y += chf(\"amp\");");
    pressCreateParameters();
    KnobDoublePtr amp = std::dynamic_pointer_cast<KnobDouble>(_wrangle->getKnobByName("amp"));
    ASSERT_TRUE(amp != NULL);

    GeoDetailConstPtr result;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, pull(_wrangle, 1., &result, &error, true)) << error;

    amp->setName("gain");
    result.reset();
    EXPECT_EQ(EffectInstance::eRenderRoIRetCodeFailed, pull(_wrangle, 1., &result, &error, true));
    EXPECT_NE(std::string::npos, error.find("'amp'")) << error;
    EXPECT_NE(std::string::npos, error.find("line 1, column 13")) << error;
    EXPECT_TRUE(_wrangle->hasPersistentMessage());
}

TEST_F(GeoWrangleParamsTest, AParameterOfTheWrongKindIsACompileError)
{
    setSnippet("s@tag = chs(\"amp\");\n@P.y += chf(\"amp\");");
    pressCreateParameters();
    KnobStringPtr amp = std::dynamic_pointer_cast<KnobString>(_wrangle->getKnobByName("amp"));
    ASSERT_TRUE(amp != NULL);

    setSnippet("@P.y += chf(\"amp\");");
    GeoDetailConstPtr result;
    std::string error;
    EXPECT_EQ(EffectInstance::eRenderRoIRetCodeFailed, pull(_wrangle, 1., &result, &error, true));
    EXPECT_NE(std::string::npos, error.find("'amp'")) << error;
}

TEST_F(GeoWrangleParamsTest, ANameThatIsNotAParameterNameIsNeverCreated)
{
    setSnippet("@P.y += chf(\"my amp\");");
    const int before = userKnobCount(_wrangle);

    GeoWrangle* effect = dynamic_cast<GeoWrangle*>(_wrangle->getEffectInstance().get());
    ASSERT_TRUE(effect != NULL);
    EXPECT_TRUE(effect->createParameters().empty());
    pressCreateParameters();
    pressCreateParameters();
    EXPECT_EQ(before, userKnobCount(_wrangle));

    GeoDetailConstPtr result;
    std::string error;
    EXPECT_EQ(EffectInstance::eRenderRoIRetCodeFailed, pull(_wrangle, 1., &result, &error, true));
    EXPECT_NE(std::string::npos, error.find("'my amp' cannot name a parameter")) << error;
    EXPECT_NE(std::string::npos, error.find("line 1, column 13")) << error;
}

TEST_F(GeoWrangleParamsTest, ANameABuiltInControlHoldsIsNeverCreated)
{
    setSnippet("@P.y += chf(\"snippet\");");
    const int before = userKnobCount(_wrangle);

    pressCreateParameters();
    pressCreateParameters();
    EXPECT_EQ(before, userKnobCount(_wrangle));
    KnobIPtr snippet = _wrangle->getKnobByName(kGeoWrangleParamSnippet);
    ASSERT_TRUE(snippet != NULL);
    EXPECT_FALSE(snippet->isUserKnob());

    GeoDetailConstPtr result;
    std::string error;
    EXPECT_EQ(EffectInstance::eRenderRoIRetCodeFailed, pull(_wrangle, 1., &result, &error, true));
    EXPECT_NE(std::string::npos, error.find("built-in control")) << error;
    EXPECT_NE(std::string::npos, error.find("'snippet'")) << error;
}
