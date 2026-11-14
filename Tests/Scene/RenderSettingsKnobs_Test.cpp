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

#include <cctype>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/vec2i.h>
#include <pxr/base/gf/vec4d.h>
#include <pxr/base/gf/vec4f.h>

#include "BaseTest.h"
#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/RenderSettingsKnobs.h"
#include "Engine/Nodes/Scene/RenderSettingsPrim.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDRenderSettings.h"
#include "Engine/Project.h"
#include "Engine/ScenePayload.h"
#include "Engine/ViewIdx.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_USING

namespace {
// Renderers no plug-in provides, whose settings the tests declare.
const char* const kRenderer = "NatronTestSettingsRenderer";
const char* const kOtherRenderer = "NatronTestOtherSettingsRenderer";

RenderSettingsKnobs::Descriptor
descriptor(const char* key,
           const VtValue& defaultValue)
{
    RenderSettingsKnobs::Descriptor d;

    d.key = key;
    d.name = std::string("The ") + key + " setting";
    d.defaultValue = defaultValue;

    return d;
}

VtFloatArray
floats(float a,
       float b)
{
    VtFloatArray v(2);
    v[0] = a;
    v[1] = b;

    return v;
}

VtIntArray
ints(int a,
     int b,
     int c)
{
    VtIntArray v(3);
    v[0] = a;
    v[1] = b;
    v[2] = c;

    return v;
}

VtDoubleArray
doubles(double a)
{
    VtDoubleArray v(1);
    v[0] = a;

    return v;
}

// One setting of every type with a knob, and one without, with its default and another value.
struct Case {
    const char* key;
    const char* scriptName;
    VtValue defaultValue;
    VtValue changed;
};

std::vector<Case>
cases()
{
    return {
        { "test:flag", "natronTestSettingsRendererFlag", VtValue(false), VtValue(true) },
        { "test:count", "natronTestSettingsRendererCount", VtValue(3), VtValue(-12) },
        { "test:unsigned_count", "natronTestSettingsRendererUnsignedCount", VtValue(4u), VtValue(9u) },
        { "test:gain", "natronTestSettingsRendererGain", VtValue(0.5f), VtValue(0.75f) },
        { "test:exposure", "natronTestSettingsRendererExposure", VtValue(1.25), VtValue(-2.5) },
        { "test:name", "natronTestSettingsRendererName", VtValue(std::string("abc")), VtValue(std::string("a b\tc")) },
        { "test:mode", "natronTestSettingsRendererMode", VtValue(TfToken("fast")), VtValue(TfToken("slow")) },
        { "test:size", "natronTestSettingsRendererSize", VtValue(GfVec2i(2, 3)), VtValue(GfVec2i(4, -5)) },
        { "test:offset", "natronTestSettingsRendererOffset", VtValue(GfVec2f(0.5f, 1.5f)), VtValue(GfVec2f(2.5f, -1.f)) },
        { "test:scale", "natronTestSettingsRendererScale", VtValue(GfVec2d(1., 2.)), VtValue(GfVec2d(3.25, 4.5)) },
        { "test:tint", "natronTestSettingsRendererTint", VtValue(GfVec3f(1.f, 0.5f, 0.25f)), VtValue(GfVec3f(0.f, 0.125f, 2.f)) },
        { "test:tint_d", "natronTestSettingsRendererTintD", VtValue(GfVec3d(1., 1., 1.)), VtValue(GfVec3d(0.5, 0.25, 0.125)) },
        { "test:background", "natronTestSettingsRendererBackground", VtValue(GfVec4f(0.f, 0.f, 0.f, 1.f)), VtValue(GfVec4f(0.25f, 0.5f, 0.75f, 0.5f)) },
        { "test:background_d", "natronTestSettingsRendererBackgroundD", VtValue(GfVec4d(0., 0., 0., 1.)), VtValue(GfVec4d(1., 0.5, 0.25, 0.)) },
        { "test:weights", "natronTestSettingsRendererWeights", VtValue(floats(0.25f, 0.5f)), VtValue(floats(1.f, -0.125f)) },
        { "test:distances", "natronTestSettingsRendererDistances", VtValue(doubles(10.)), VtValue(doubles(0.1)) },
        { "test:indices", "natronTestSettingsRendererIndices", VtValue(ints(1, 2, 3)), VtValue(ints(7, 8, 9)) },
    };
}

RenderSettingsKnobs::DescriptorList
everyType()
{
    RenderSettingsKnobs::DescriptorList list;

    for (const Case& c : cases()) {
        list.push_back(descriptor(c.key, c.defaultValue));
    }
    list.push_back(descriptor("test:asset", VtValue(SdfAssetPath("look.usda"))));

    return list;
}

// Sets the knob of rendererId's key to value through the knob's own type, as a user would.
void
setKnob(const NodePtr& node,
        const std::string& scriptName,
        const VtValue& value)
{
    KnobIPtr k = node->getKnobByName(scriptName);
    ASSERT_TRUE(bool(k)) << "no knob " << scriptName;
    if (value.IsHolding<bool>()) {
        KnobBool* b = dynamic_cast<KnobBool*>(k.get());
        ASSERT_TRUE(b != nullptr) << scriptName;
        b->setValue(value.UncheckedGet<bool>());
    } else if (value.IsHolding<int>() || value.IsHolding<unsigned int>()) {
        KnobInt* i = dynamic_cast<KnobInt*>(k.get());
        ASSERT_TRUE(i != nullptr) << scriptName;
        i->setValue(value.IsHolding<int>() ? value.UncheckedGet<int>() : (int)value.UncheckedGet<unsigned int>());
    } else if (value.IsHolding<float>() || value.IsHolding<double>()) {
        KnobDouble* d = dynamic_cast<KnobDouble*>(k.get());
        ASSERT_TRUE(d != nullptr) << scriptName;
        d->setValue(value.IsHolding<float>() ? (double)value.UncheckedGet<float>() : value.UncheckedGet<double>());
    } else if (value.IsHolding<GfVec2i>()) {
        KnobInt* i = dynamic_cast<KnobInt*>(k.get());
        ASSERT_TRUE(i != nullptr) << scriptName;
        ASSERT_EQ(2, i->getDimension());
        i->setValue(value.UncheckedGet<GfVec2i>()[0], ViewSpec::all(), 0);
        i->setValue(value.UncheckedGet<GfVec2i>()[1], ViewSpec::all(), 1);
    } else if (value.IsHolding<GfVec2f>() || value.IsHolding<GfVec2d>()) {
        KnobDouble* d = dynamic_cast<KnobDouble*>(k.get());
        ASSERT_TRUE(d != nullptr) << scriptName;
        ASSERT_EQ(2, d->getDimension());
        const GfVec2d v = value.IsHolding<GfVec2f>() ? GfVec2d(value.UncheckedGet<GfVec2f>()) : value.UncheckedGet<GfVec2d>();
        d->setValue(v[0], ViewSpec::all(), 0);
        d->setValue(v[1], ViewSpec::all(), 1);
    } else if (value.IsHolding<GfVec3f>() || value.IsHolding<GfVec3d>()) {
        KnobColor* c = dynamic_cast<KnobColor*>(k.get());
        ASSERT_TRUE(c != nullptr) << scriptName;
        ASSERT_EQ(3, c->getDimension());
        const GfVec3d v = value.IsHolding<GfVec3f>() ? GfVec3d(value.UncheckedGet<GfVec3f>()) : value.UncheckedGet<GfVec3d>();
        for (int dim = 0; dim < 3; ++dim) {
            c->setValue(v[dim], ViewSpec::all(), dim);
        }
    } else if (value.IsHolding<GfVec4f>() || value.IsHolding<GfVec4d>()) {
        KnobColor* c = dynamic_cast<KnobColor*>(k.get());
        ASSERT_TRUE(c != nullptr) << scriptName;
        ASSERT_EQ(4, c->getDimension());
        const GfVec4d v = value.IsHolding<GfVec4f>() ? GfVec4d(value.UncheckedGet<GfVec4f>()) : value.UncheckedGet<GfVec4d>();
        for (int dim = 0; dim < 4; ++dim) {
            c->setValue(v[dim], ViewSpec::all(), dim);
        }
    } else {
        KnobString* s = dynamic_cast<KnobString*>(k.get());
        ASSERT_TRUE(s != nullptr) << scriptName;
        s->setValue(RenderSettingsKnobs::valueText(value));
    }
}

RenderSettingsKnobs::CuratedSetting
curatedSetting(const char* key,
               const char* name,
               const char* group)
{
    RenderSettingsKnobs::CuratedSetting c;

    c.key = key;
    c.name = name;
    c.tooltip = std::string("The ") + name + " setting";
    c.defaultValue = VtValue(0);
    c.group = group;

    return c;
}

RenderSettingsKnobs::GroupSpec
groupSpec(const char* name,
          const char* parent,
          bool open)
{
    RenderSettingsKnobs::GroupSpec g;

    g.name = name;
    g.parent = parent;
    g.open = open;
    g.tooltip = std::string("The ") + name + " group";

    return g;
}

// Alpha holds Beta, Gamma is beside it and Unused has no setting.
RenderSettingsKnobs::Curated
groupedTable()
{
    RenderSettingsKnobs::Curated t;

    t.tooltip = "The loose settings";
    t.groups = { groupSpec("alpha", "", true), groupSpec("beta", "alpha", false), groupSpec("gamma", "", false), groupSpec("unused", "", true) };
    t.settings = { curatedSetting("test:a1", "a1", "alpha"),
                   curatedSetting("test:b1", "b1", "beta"),
                   curatedSetting("test:a2", "a2", "alpha"),
                   curatedSetting("test:g1", "g1", "gamma"),
                   curatedSetting("test:loose", "loose", "") };

    return t;
}

RenderSettingsKnobs::Curated
flatTable()
{
    RenderSettingsKnobs::Curated t = groupedTable();

    t.groups.clear();
    for (RenderSettingsKnobs::CuratedSetting& c : t.settings) {
        c.group.clear();
    }

    return t;
}

std::vector<std::string>
childNames(const KnobIPtr& parent)
{
    std::vector<std::string> names;
    std::vector<KnobIPtr> children;

    if (KnobPage* page = dynamic_cast<KnobPage*>(parent.get())) {
        children = page->getChildren();
    } else if (KnobGroup* group = dynamic_cast<KnobGroup*>(parent.get())) {
        children = group->getChildren();
    }
    for (const KnobIPtr& k : children) {
        names.push_back(k->getName());
    }

    return names;
}
} // anon namespace

class RenderSettingsKnobsTest
    : public BaseTest {
protected:
    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        SceneQueryPool::instance().releaseIdle();
        RenderSettingsKnobs::setDescriptorsForTests(kRenderer, RenderSettingsKnobs::DescriptorList());
        RenderSettingsKnobs::setDescriptorsForTests(kOtherRenderer, RenderSettingsKnobs::DescriptorList());
        RenderSettingsKnobs::setCuratedForTests(kRenderer, nullptr);
        BaseTest::TearDown();
    }

    NodePtr createSettings()
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDERSETTINGS));
        EXPECT_TRUE(bool(node)) << "USDRenderSettings is not registered";

        return node;
    }

    static const RenderSettingsKnobs* settingsOf(const NodePtr& node)
    {
        USDRenderSettings* settings = node ? dynamic_cast<USDRenderSettings*>(node->getEffectInstance().get()) : nullptr;

        return settings ? settings->renderSettings() : nullptr;
    }

    // The node's render settings prim, as the scene it outputs composes it.
    static UsdPrim authoredPrim(const NodePtr& node,
                                SceneQueryPool::Lease* lease)
    {
        ScenePayload payload;
        std::string error;
        EXPECT_EQ(EffectInstance::eRenderRoIRetCodeOk,
                  node->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0)), &payload, &error))
            << error;
        *lease = SceneQueryPool::instance().borrow(payload, SceneLoadSet::everything(), &error);
        UsdStageRefPtr stage = lease->getStage();
        EXPECT_TRUE(bool(stage)) << error;

        return stage ? stage->GetPrimAtPath(SdfPath(kUSDRenderSettingsDefaultPrimPath)) : UsdPrim();
    }

    bool saveResetLoad(const QTemporaryDir& tmp)
    {
        ProjectPtr project = getApp()->getProject();
        const QString dirPath = tmp.path() + QLatin1Char('/');
        const QString fileName = QString::fromUtf8("render-settings.ntpz");
        QString savedFilePath;

        if (!project->saveProject(dirPath, fileName, &savedFilePath) || !QFile::exists(savedFilePath)) {
            return false;
        }
        project->reset(false, true);

        return project->loadProject(dirPath, fileName);
    }
};

TEST_F(RenderSettingsKnobsTest, NamesAndLabels)
{
    EXPECT_EQ(std::string("cycles"), RenderSettingsKnobs::scriptPrefix("HdCyclesPlugin"));
    EXPECT_EQ(std::string("moonray"), RenderSettingsKnobs::scriptPrefix("HdMoonrayRendererPlugin"));
    EXPECT_EQ(std::string("storm"), RenderSettingsKnobs::scriptPrefix("HdStormRendererPlugin"));
    EXPECT_EQ(std::string("natronTestSettingsRenderer"), RenderSettingsKnobs::scriptPrefix(kRenderer));
    EXPECT_EQ(std::string("maxDiffuseBounce"), RenderSettingsKnobs::camelCaseFromKey("cycles:integrator:max_diffuse_bounce", false));
    EXPECT_EQ(std::string("cyclesIntegratorSeed"), RenderSettingsKnobs::camelCaseFromKey("cycles:integrator:seed", true));
    EXPECT_EQ(std::string("enableTinyPrimCulling"), RenderSettingsKnobs::camelCaseFromKey("enableTinyPrimCulling", false));
    EXPECT_EQ(std::string("Max Diffuse Bounce"), RenderSettingsKnobs::labelFromScriptName("cyclesMaxDiffuseBounce", "cycles"));
    EXPECT_EQ(std::string("AO Bounces"), RenderSettingsKnobs::labelFromScriptName("cyclesAoBounces", "cycles"));
    EXPECT_EQ(std::string("Enable OIDN"), RenderSettingsKnobs::labelFromScriptName("moonrayEnableOIDN", "moonray"));
    EXPECT_EQ(std::string("BSDF Samples"), RenderSettingsKnobs::labelFromScriptName("moonrayBsdfSamples", "moonray"));
    EXPECT_EQ(std::string("Cycles Settings"), RenderSettingsKnobs::labelFromScriptName("cyclesSettings", ""));
}

// Every supported type gets a knob of its kind holding the declared default, labelled from its
// script name; a type with no knob is left out; at the defaults nothing is sent.
TEST_F(RenderSettingsKnobsTest, BuildsAKnobForEverySupportedType)
{
    RenderSettingsKnobs::setDescriptorsForTests(kRenderer, everyType());
    NodePtr node = createSettings();
    ASSERT_TRUE(bool(node));
    const RenderSettingsKnobs* settings = settingsOf(node);
    ASSERT_TRUE(settings != nullptr);

    for (const Case& c : cases()) {
        KnobIPtr k = settings->knob(kRenderer, c.key);
        ASSERT_TRUE(bool(k)) << c.key;
        EXPECT_EQ(std::string(c.scriptName), k->getName());
        EXPECT_EQ(k.get(), node->getKnobByName(c.scriptName).get());
        EXPECT_EQ(RenderSettingsKnobs::labelFromScriptName(c.scriptName, "natronTestSettingsRenderer"), k->getLabel());
        // Every renderer's knobs are shown, on its own page.
        EXPECT_FALSE(k->getIsSecret()) << c.key;
        KnobIPtr group = k->getParentKnob();
        ASSERT_TRUE(bool(group)) << c.key;
        EXPECT_EQ(settings->page(kRenderer), group->getParentKnob()) << c.key;
    }
    KnobPagePtr page = settings->page(kRenderer);
    ASSERT_TRUE(bool(page));
    EXPECT_EQ(std::string("natronTestSettingsRenderer"), page->getName());
    EXPECT_EQ(page.get(), node->getKnobByName("natronTestSettingsRenderer").get());
    EXPECT_FALSE(page->getIsSecret());
    EXPECT_TRUE(dynamic_cast<KnobBool*>(settings->knob(kRenderer, "test:flag").get()) != nullptr);
    EXPECT_TRUE(dynamic_cast<KnobInt*>(settings->knob(kRenderer, "test:count").get()) != nullptr);
    EXPECT_TRUE(dynamic_cast<KnobDouble*>(settings->knob(kRenderer, "test:gain").get()) != nullptr);
    EXPECT_TRUE(dynamic_cast<KnobString*>(settings->knob(kRenderer, "test:mode").get()) != nullptr);
    EXPECT_TRUE(dynamic_cast<KnobColor*>(settings->knob(kRenderer, "test:tint").get()) != nullptr);
    EXPECT_TRUE(dynamic_cast<KnobColor*>(settings->knob(kRenderer, "test:background").get()) != nullptr);
    EXPECT_TRUE(dynamic_cast<KnobString*>(settings->knob(kRenderer, "test:weights").get()) != nullptr);
    EXPECT_FALSE(bool(settings->knob(kRenderer, "test:asset")));
    EXPECT_TRUE(bool(node->getKnobByName("natronTestSettingsRendererOtherSettings")));
    EXPECT_TRUE(settings->values(kRenderer).empty());

    const std::vector<std::pair<std::string, std::string>> audit = settings->audit(kRenderer);
    ASSERT_EQ(everyType().size(), audit.size());
    for (const std::pair<std::string, std::string>& reach : audit) {
        EXPECT_EQ(reach.first == "test:asset", reach.second.empty()) << reach.first << " -> " << reach.second;
    }
    const std::string dump = RenderSettingsKnobs::dump();
    EXPECT_NE(std::string::npos, dump.find(std::string(kRenderer) + "\ttest:gain\tfloat\t0.5\t")) << dump;
    EXPECT_NE(std::string::npos, dump.find(std::string(kRenderer) + "\ttest:asset\tSdfAssetPath\t(no knob for this type)")) << dump;
}

// Changed knobs are sent as the type each delegate declared, change the node's hash, and come
// back from a save and load.
TEST_F(RenderSettingsKnobsTest, ValuesRoundTripThroughSaveAndLoad)
{
    RenderSettingsKnobs::setDescriptorsForTests(kRenderer, everyType());
    RenderSettingsKnobs::DescriptorList other;
    other.push_back(descriptor("test:gain", VtValue(0.5f)));
    RenderSettingsKnobs::setDescriptorsForTests(kOtherRenderer, other);
    NodePtr node = createSettings();
    ASSERT_TRUE(bool(node));
    const std::string name = node->getScriptName();
    U64 hash = node->getHashValue();
    for (const Case& c : cases()) {
        setKnob(node, c.scriptName, c.changed);
        const U64 changed = node->getHashValue();
        EXPECT_NE(hash, changed) << c.key;
        hash = changed;
    }
    setKnob(node, "natronTestOtherSettingsRendererGain", VtValue(0.25f));
    const RenderSettingsKnobs* settings = settingsOf(node);
    ASSERT_TRUE(settings != nullptr);
    RenderSettingsKnobs::Values sent = settings->values(kRenderer);
    ASSERT_EQ(cases().size(), sent.size());
    for (const Case& c : cases()) {
        EXPECT_EQ(c.changed, sent[c.key]) << c.key << ": " << RenderSettingsKnobs::valueText(sent[c.key]);
    }

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(saveResetLoad(tmp));
    NodePtr loaded = getApp()->getProject()->getNodeByName(name);
    ASSERT_TRUE(bool(loaded));
    const RenderSettingsKnobs* loadedSettings = settingsOf(loaded);
    ASSERT_TRUE(loadedSettings != nullptr);
    sent = loadedSettings->values(kRenderer);
    ASSERT_EQ(cases().size(), sent.size());
    for (const Case& c : cases()) {
        EXPECT_EQ(c.changed, sent[c.key]) << c.key << ": " << RenderSettingsKnobs::valueText(sent[c.key]);
    }
    sent = loadedSettings->values(kOtherRenderer);
    ASSERT_EQ(1u, sent.size());
    EXPECT_EQ(VtValue(0.25f), sent["test:gain"]);
}

// The changed settings of every renderer are authored on the prim, each named as
// RenderSettingsPrim::attributeName() names it, and read back as the values sent; the defaults are not.
TEST_F(RenderSettingsKnobsTest, ValuesAreAuthoredInEachRenderersNamespace)
{
    RenderSettingsKnobs::setDescriptorsForTests(kRenderer, everyType());
    RenderSettingsKnobs::DescriptorList other;
    other.push_back(descriptor("test:gain", VtValue(0.5f)));
    other.push_back(descriptor("test:count", VtValue(3)));
    RenderSettingsKnobs::setDescriptorsForTests(kOtherRenderer, other);
    NodePtr node = createSettings();
    ASSERT_TRUE(bool(node));
    const RenderSettingsKnobs* settings = settingsOf(node);
    ASSERT_TRUE(settings != nullptr);
    {
        SceneQueryPool::Lease lease;
        const UsdPrim prim = authoredPrim(node, &lease);
        ASSERT_TRUE(bool(prim));
        EXPECT_TRUE(RenderSettingsPrim::settings(prim, kRenderer, UsdTimeCode::Default()).empty());
        EXPECT_TRUE(RenderSettingsPrim::settings(prim, kOtherRenderer, UsdTimeCode::Default()).empty());
    }
    for (const Case& c : cases()) {
        setKnob(node, c.scriptName, c.changed);
    }
    setKnob(node, "natronTestOtherSettingsRendererGain", VtValue(0.25f));

    SceneQueryPool::Lease lease;
    const UsdPrim prim = authoredPrim(node, &lease);
    ASSERT_TRUE(bool(prim));
    for (const Case& c : cases()) {
        const std::string name = std::string("natronTestSettingsRenderer:") + c.key;
        EXPECT_EQ(name, RenderSettingsPrim::attributeName(kRenderer, c.key));
        const UsdAttribute attribute = prim.GetAttribute(TfToken(name));
        ASSERT_TRUE(bool(attribute)) << name;
        VtValue authored;
        EXPECT_TRUE(attribute.Get(&authored)) << name;
        EXPECT_EQ(c.changed, authored) << name;
    }
    EXPECT_EQ(settings->values(kRenderer), RenderSettingsPrim::settings(prim, kRenderer, UsdTimeCode::Default()));
    const RenderSettingsPrim::Values otherValues = RenderSettingsPrim::settings(prim, kOtherRenderer, UsdTimeCode::Default());
    EXPECT_EQ(settings->values(kOtherRenderer), otherValues);
    ASSERT_EQ(1u, otherValues.size());
    EXPECT_EQ(VtValue(0.25f), otherValues.at("test:gain"));
    EXPECT_FALSE(prim.HasAttribute(TfToken("natronTestOtherSettingsRenderer:test:count")));
}

TEST_F(RenderSettingsKnobsTest, GroupTablesAreChecked)
{
    std::string error;

    EXPECT_TRUE(RenderSettingsKnobs::validateGroups(groupedTable(), &error)) << error;

    RenderSettingsKnobs::Curated deep = groupedTable();
    deep.groups.push_back(groupSpec("inner", "beta", true));
    EXPECT_FALSE(RenderSettingsKnobs::validateGroups(deep, &error));

    RenderSettingsKnobs::Curated orphan = groupedTable();
    orphan.groups.push_back(groupSpec("lost", "missing", true));
    EXPECT_FALSE(RenderSettingsKnobs::validateGroups(orphan, &error));

    RenderSettingsKnobs::Curated duplicate = groupedTable();
    duplicate.groups.push_back(groupSpec("alpha", "", true));
    EXPECT_FALSE(RenderSettingsKnobs::validateGroups(duplicate, &error));

    // A group named like a setting would push that setting onto the fallback script name.
    RenderSettingsKnobs::Curated clash = groupedTable();
    clash.groups.push_back(groupSpec("loose", "", true));
    EXPECT_FALSE(RenderSettingsKnobs::validateGroups(clash, &error));

    RenderSettingsKnobs::Curated reserved = groupedTable();
    reserved.groups.push_back(groupSpec("otherSettings", "", true));
    EXPECT_FALSE(RenderSettingsKnobs::validateGroups(reserved, &error));

    RenderSettingsKnobs::Curated unknown = groupedTable();
    unknown.settings.push_back(curatedSetting("test:x", "x", "nowhere"));
    EXPECT_FALSE(RenderSettingsKnobs::validateGroups(unknown, &error));

    EXPECT_EQ(std::string("GI Bounces"), RenderSettingsKnobs::labelFromScriptName("cyclesGiBounces", "cycles"));
}

// Groups are made in the order settings first need them, nest under their parent, start in
// their declared state, are absent when empty, and every setting keeps its natural script name.
TEST_F(RenderSettingsKnobsTest, CuratedGroupsNestInTableOrder)
{
    const std::string prefix = "natronTestSettingsRenderer";
    const RenderSettingsKnobs::Curated table = groupedTable();
    RenderSettingsKnobs::setCuratedForTests(kRenderer, &table);
    RenderSettingsKnobs::setDescriptorsForTests(kRenderer, RenderSettingsKnobs::DescriptorList(1, descriptor("test:other", VtValue(1))));
    NodePtr node = createSettings();
    ASSERT_TRUE(bool(node));
    const RenderSettingsKnobs* settings = settingsOf(node);
    ASSERT_TRUE(settings != nullptr);
    KnobPagePtr page = settings->page(kRenderer);
    ASSERT_TRUE(bool(page));

    EXPECT_EQ((std::vector<std::string> { prefix + "Alpha", prefix + "Gamma", prefix + "Settings", prefix + "OtherSettings" }), childNames(page));
    KnobIPtr alpha = node->getKnobByName(prefix + "Alpha");
    KnobIPtr beta = node->getKnobByName(prefix + "Beta");
    KnobIPtr gamma = node->getKnobByName(prefix + "Gamma");
    ASSERT_TRUE(bool(alpha) && bool(beta) && bool(gamma));
    EXPECT_FALSE(bool(node->getKnobByName(prefix + "Unused")));
    EXPECT_EQ((std::vector<std::string> { prefix + "A1", prefix + "Beta", prefix + "A2" }), childNames(alpha));
    EXPECT_EQ((std::vector<std::string> { prefix + "B1" }), childNames(beta));
    EXPECT_EQ((std::vector<std::string> { prefix + "G1" }), childNames(gamma));
    EXPECT_EQ((std::vector<std::string> { prefix + "Loose" }), childNames(node->getKnobByName(prefix + "Settings")));
    EXPECT_EQ(alpha, beta->getParentKnob());
    EXPECT_EQ(page, alpha->getParentKnob());
    EXPECT_EQ(std::string("Alpha"), alpha->getLabel());
    EXPECT_EQ(std::string("Beta"), beta->getLabel());

    KnobGroup* alphaGroup = dynamic_cast<KnobGroup*>(alpha.get());
    KnobGroup* betaGroup = dynamic_cast<KnobGroup*>(beta.get());
    KnobGroup* gammaGroup = dynamic_cast<KnobGroup*>(gamma.get());
    ASSERT_TRUE(alphaGroup && betaGroup && gammaGroup);
    EXPECT_TRUE(alphaGroup->getValue());
    EXPECT_FALSE(betaGroup->getValue());
    EXPECT_FALSE(gammaGroup->getValue());
    EXPECT_EQ(std::string("The beta group"), beta->getHintToolTip());

    for (const RenderSettingsKnobs::CuratedSetting& c : table.settings) {
        KnobIPtr k = settings->knob(kRenderer, c.key);
        ASSERT_TRUE(bool(k)) << c.key;
        EXPECT_EQ(prefix + std::string(1, (char)std::toupper((unsigned char)c.name[0])) + c.name.substr(1), k->getName());
    }
}

namespace {
RenderSettingsKnobs::Curated
switchTable()
{
    RenderSettingsKnobs::Curated t;
    RenderSettingsKnobs::CuratedSetting sw = curatedSetting("test:sw", "sw", "");
    sw.defaultValue = VtValue(true);
    RenderSettingsKnobs::CuratedSetting count = curatedSetting("test:count", "count", "");
    RenderSettingsKnobs::EnabledBy bySwitch;
    bySwitch.key = "test:sw";
    RenderSettingsKnobs::EnabledBy bySwitchOff = bySwitch;
    bySwitchOff.inverted = true;
    RenderSettingsKnobs::EnabledBy byCount;
    byCount.key = "test:count";
    RenderSettingsKnobs::CuratedSetting dependent = curatedSetting("test:dependent", "dependent", "");
    dependent.enabledBy = { bySwitch };
    RenderSettingsKnobs::CuratedSetting inverse = curatedSetting("test:inverse", "inverse", "");
    inverse.enabledBy = { bySwitchOff };
    RenderSettingsKnobs::CuratedSetting both = curatedSetting("test:both", "both", "");
    both.enabledBy = { bySwitch, byCount };
    RenderSettingsKnobs::CuratedSetting locked = curatedSetting("test:locked", "locked", "");
    locked.enabledBy = { bySwitch };
    locked.disabledReason = "Never available.";
    t.settings = { sw, count, dependent, inverse, both, locked };

    return t;
}
} // namespace

TEST_F(RenderSettingsKnobsTest, SwitchesGreyTheirDependents)
{
    const std::string prefix = "natronTestSettingsRenderer";
    const RenderSettingsKnobs::Curated table = switchTable();
    RenderSettingsKnobs::setCuratedForTests(kRenderer, &table);
    RenderSettingsKnobs::setDescriptorsForTests(kRenderer, RenderSettingsKnobs::DescriptorList(1, descriptor("test:other", VtValue(1))));
    NodePtr node = createSettings();
    ASSERT_TRUE(bool(node));
    const RenderSettingsKnobs* settings = settingsOf(node);
    ASSERT_TRUE(settings != nullptr);
    const auto enabled = [&](const char* name) {
        KnobIPtr k = node->getKnobByName(prefix + name);
        EXPECT_TRUE(bool(k)) << name;

        return k && k->isEnabled(0);
    };

    EXPECT_TRUE(enabled("Dependent"));
    EXPECT_FALSE(enabled("Inverse"));
    EXPECT_FALSE(enabled("Both"));
    EXPECT_FALSE(enabled("Locked"));

    setKnob(node, prefix + "Count", VtValue(2));
    EXPECT_TRUE(enabled("Both"));
    const U64 hash = node->getHashValue();
    const RenderSettingsKnobs::Values values = settings->values(kRenderer);

    setKnob(node, prefix + "Sw", VtValue(false));
    EXPECT_FALSE(enabled("Dependent"));
    EXPECT_TRUE(enabled("Inverse"));
    EXPECT_FALSE(enabled("Both"));
    EXPECT_FALSE(enabled("Locked"));
    setKnob(node, prefix + "Sw", VtValue(true));
    EXPECT_TRUE(enabled("Dependent"));
    EXPECT_TRUE(enabled("Both"));
    EXPECT_FALSE(enabled("Locked"));
    EXPECT_EQ(hash, node->getHashValue());
    EXPECT_EQ(values, settings->values(kRenderer));

    setKnob(node, prefix + "Sw", VtValue(false));
    const std::string name = node->getScriptName();
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(saveResetLoad(tmp));
    NodePtr loaded = getApp()->getProject()->getNodeByName(name);
    ASSERT_TRUE(bool(loaded));
    node = loaded;
    EXPECT_FALSE(enabled("Dependent"));
    EXPECT_TRUE(enabled("Inverse"));
    EXPECT_FALSE(enabled("Locked"));
}

// A table whose groups are invalid is ignored: every setting lands in the curated group.
TEST_F(RenderSettingsKnobsTest, InvalidGroupTableFallsBackToTheCuratedGroup)
{
    const std::string prefix = "natronTestSettingsRenderer";
    RenderSettingsKnobs::Curated table = groupedTable();
    table.groups.push_back(groupSpec("inner", "beta", true));
    RenderSettingsKnobs::setCuratedForTests(kRenderer, &table);
    RenderSettingsKnobs::setDescriptorsForTests(kRenderer, RenderSettingsKnobs::DescriptorList(1, descriptor("test:other", VtValue(1))));
    NodePtr node = createSettings();
    ASSERT_TRUE(bool(node));
    const RenderSettingsKnobs* settings = settingsOf(node);
    ASSERT_TRUE(settings != nullptr);

    EXPECT_FALSE(bool(node->getKnobByName(prefix + "Alpha")));
    EXPECT_EQ(table.settings.size(), childNames(node->getKnobByName(prefix + "Settings")).size());
}

// Where knobs sit changes neither the namespace digests nor values(), and opening or closing a
// group changes neither.
TEST_F(RenderSettingsKnobsTest, GroupsDoNotMoveTheDigestOrTheValues)
{
    const std::string prefix = "natronTestSettingsRenderer";
    std::map<std::string, U64> digests[2];
    RenderSettingsKnobs::Values values[2];

    for (int layout = 0; layout < 2; ++layout) {
        const RenderSettingsKnobs::Curated table = layout ? flatTable() : groupedTable();
        RenderSettingsKnobs::setCuratedForTests(kRenderer, &table);
        RenderSettingsKnobs::setDescriptorsForTests(kRenderer, RenderSettingsKnobs::DescriptorList(1, descriptor("test:other", VtValue(1))));
        NodePtr node = createSettings();
        ASSERT_TRUE(bool(node));
        setKnob(node, prefix + "A1", VtValue(5));
        setKnob(node, prefix + "B1", VtValue(6));
        setKnob(node, prefix + "Loose", VtValue(7));
        digests[layout] = node->getKnobsNamespaceDigests();
        values[layout] = settingsOf(node)->values(kRenderer);
        if (layout == 0) {
            KnobGroup* alpha = dynamic_cast<KnobGroup*>(node->getKnobByName(prefix + "Alpha").get());
            ASSERT_TRUE(alpha != nullptr);
            alpha->setValue(!alpha->getValue());
            EXPECT_EQ(digests[0], node->getKnobsNamespaceDigests());
            EXPECT_EQ(values[0], settingsOf(node)->values(kRenderer));
        }
        getApp()->getProject()->reset(false, true);
    }
    EXPECT_FALSE(digests[0].empty());
    EXPECT_EQ(digests[0], digests[1]);
    EXPECT_EQ(3u, values[0].size());
    EXPECT_EQ(values[0], values[1]);
}

// Each rule matches on the token prefixes of the key's last segment, the earlier rule wins when
// several match, and a key no rule matches is left for Other Settings.
TEST_F(RenderSettingsKnobsTest, KeysAreClassifiedByTheirTokens)
{
    const std::vector<std::pair<std::string, std::string>> cases = {
        { "denoiseEnabled", "denoise" },
        { "arras:oidn_mode", "denoise" },
        { "guidingEnabled", "pathGuiding" },
        { "volumeRaymarchingStepSize", "volumes" },
        { "raymarchSteps", "volumes" },
        { "maxBounces", "bounces" },
        { "max_depth", "bounces" },
        { "sampleClamp", "clamping" },
        { "clampValue", "clamping" },
        { "maxLights", "lights" },
        { "shadowBias", "lights" },
        { "pixelSamples", "sampling" },
        { "seed", "sampling" },
        { "adaptiveThreshold", "sampling" },
        { "noiseFloor", "sampling" },
        { "pixelFilterWidth", "filtering" },
        { "shutterOpen", "motion" },
        { "motionBlur", "motion" },
        { "threadCount", "performance" },
        { "enableTinyPrimCulling", "performance" },
        { "volumeMaxTextureMemoryPerField", "volumes" },
        { "executionMode", "performance" },
        { "bucketSize", "performance" },
        { "logLevel", "logging" },
        { "debugOutput", "logging" },
        { "verbose", "logging" },
        { "denoiseLightSamples", "denoise" },
        { "guidingBounceSamples", "pathGuiding" },
        { "lightVolumeSamples", "volumes" },
        { "bounceClamp", "bounces" },
        { "clampLight", "clamping" },
        { "lightSampleCount", "lights" },
        { "sampleFilter", "sampling" },
        { "filterBlur", "filtering" },
        { "blurCache", "motion" },
        { "AOBounces", "bounces" },
        { "gain", "" },
        { "test:offset", "" },
        { "tint", "" },
    };

    for (const std::pair<std::string, std::string>& c : cases) {
        EXPECT_EQ(c.second, RenderSettingsKnobs::classifyKey(c.first)) << c.first;
    }
}

// A renderer without a table gets only the groups its keys fill, in a fixed order with Other
// Settings last and closed; all open up to 20 knobs, only the first beyond that.
TEST_F(RenderSettingsKnobsTest, GeneratedSettingsAreGrouped)
{
    const std::string prefix = "natronTestSettingsRenderer";
    for (int many = 0; many < 2; ++many) {
        RenderSettingsKnobs::DescriptorList list;
        list.push_back(descriptor("test:logLevel", VtValue(1)));
        list.push_back(descriptor("test:gain", VtValue(0.5f)));
        list.push_back(descriptor("test:pixelSamples", VtValue(8)));
        list.push_back(descriptor("test:maxLights", VtValue(4)));
        for (int i = 0; many && i < 20; ++i) {
            list.push_back(descriptor(("test:other" + std::to_string(i)).c_str(), VtValue(i)));
        }
        RenderSettingsKnobs::setDescriptorsForTests(kRenderer, list);
        NodePtr node = createSettings();
        ASSERT_TRUE(bool(node));
        const RenderSettingsKnobs* settings = settingsOf(node);
        ASSERT_TRUE(settings != nullptr);
        KnobPagePtr page = settings->page(kRenderer);
        ASSERT_TRUE(bool(page));

        EXPECT_EQ((std::vector<std::string> { prefix + "Sampling", prefix + "Lights", prefix + "Logging", prefix + "OtherSettings" }), childNames(page));
        EXPECT_EQ(prefix + "Sampling", settings->knob(kRenderer, "test:pixelSamples")->getParentKnob()->getName());
        EXPECT_EQ(prefix + "Lights", settings->knob(kRenderer, "test:maxLights")->getParentKnob()->getName());
        EXPECT_EQ(prefix + "Logging", settings->knob(kRenderer, "test:logLevel")->getParentKnob()->getName());
        EXPECT_EQ(prefix + "OtherSettings", settings->knob(kRenderer, "test:gain")->getParentKnob()->getName());
        EXPECT_EQ(prefix + "PixelSamples", settings->knob(kRenderer, "test:pixelSamples")->getName());
        const char* const groups[] = { "Sampling", "Lights", "Logging", "OtherSettings" };
        const bool open[2][4] = { { true, true, true, false }, { true, false, false, false } };
        for (int g = 0; g < 4; ++g) {
            KnobGroup* group = dynamic_cast<KnobGroup*>(node->getKnobByName(prefix + groups[g]).get());
            ASSERT_TRUE(group != nullptr) << groups[g];
            EXPECT_EQ(open[many][g], group->getValue()) << groups[g] << (many ? " with many" : " with few");
        }
        for (const std::pair<std::string, std::string>& reach : settings->audit(kRenderer)) {
            EXPECT_FALSE(reach.second.empty()) << reach.first;
        }
        getApp()->getProject()->reset(false, true);
    }
}

// Prints every installed delegate's declared settings: the list the curated groups are built from.
TEST_F(RenderSettingsKnobsTest, DumpEveryDelegatesDescriptors)
{
    const std::string dump = RenderSettingsKnobs::dump();

    std::printf("renderer\tkey\ttype\tdefault\tname\n%s", dump.c_str());
    EXPECT_FALSE(dump.empty());
}
