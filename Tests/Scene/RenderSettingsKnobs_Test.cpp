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

#include <cstdio>
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
#include "Engine/Nodes/Scene/USDRender.h"
#include "Engine/Project.h"

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
} // anon namespace

class RenderSettingsKnobsTest
    : public BaseTest {
protected:
    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        RenderSettingsKnobs::setDescriptorsForTests(kRenderer, RenderSettingsKnobs::DescriptorList());
        RenderSettingsKnobs::setDescriptorsForTests(kOtherRenderer, RenderSettingsKnobs::DescriptorList());
        BaseTest::TearDown();
    }

    NodePtr createRender()
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDER));
        EXPECT_TRUE(bool(node)) << "USDRender is not registered";

        return node;
    }

    static const RenderSettingsKnobs* settingsOf(const NodePtr& node)
    {
        USDRender* render = node ? dynamic_cast<USDRender*>(node->getEffectInstance().get()) : nullptr;

        return render ? render->renderSettings() : nullptr;
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
    NodePtr node = createRender();
    ASSERT_TRUE(bool(node));
    const RenderSettingsKnobs* settings = settingsOf(node);
    ASSERT_TRUE(settings != nullptr);

    for (const Case& c : cases()) {
        KnobIPtr k = settings->knob(kRenderer, c.key);
        ASSERT_TRUE(bool(k)) << c.key;
        EXPECT_EQ(std::string(c.scriptName), k->getName());
        EXPECT_EQ(k.get(), node->getKnobByName(c.scriptName).get());
        EXPECT_EQ(RenderSettingsKnobs::labelFromScriptName(c.scriptName, "natronTestSettingsRenderer"), k->getLabel());
        // The renderer is not the chosen one, so its knobs are hidden.
        EXPECT_TRUE(k->getIsSecret()) << c.key;
    }
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
    NodePtr node = createRender();
    ASSERT_TRUE(bool(node));
    const std::string name = node->getScriptName();
    U64 hash = node->getHashValue();
    for (const Case& c : cases()) {
        setKnob(node, c.scriptName, c.changed);
        const U64 changed = node->getHashValue();
        EXPECT_NE(hash, changed) << c.key;
        hash = changed;
    }
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
    EXPECT_TRUE(loadedSettings->orphans().empty());
}

// The values of a setting the delegate no longer declares, and of a renderer that is not
// installed, stay in the project across a load and a save, and come back with their knobs.
TEST_F(RenderSettingsKnobsTest, SettingsWithoutAKnobAreKept)
{
    RenderSettingsKnobs::DescriptorList both;
    both.push_back(descriptor("test:count", VtValue(3)));
    both.push_back(descriptor("test:gone", VtValue(1)));
    RenderSettingsKnobs::DescriptorList other;
    other.push_back(descriptor("test:gain", VtValue(0.5f)));
    RenderSettingsKnobs::setDescriptorsForTests(kRenderer, both);
    RenderSettingsKnobs::setDescriptorsForTests(kOtherRenderer, other);
    NodePtr node = createRender();
    ASSERT_TRUE(bool(node));
    const std::string name = node->getScriptName();
    setKnob(node, "natronTestSettingsRendererCount", VtValue(5));
    setKnob(node, "natronTestSettingsRendererGone", VtValue(9));
    setKnob(node, "natronTestOtherSettingsRendererGain", VtValue(0.25f));

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    RenderSettingsKnobs::DescriptorList countOnly;
    countOnly.push_back(descriptor("test:count", VtValue(3)));
    {
        ProjectPtr project = getApp()->getProject();
        QString saved;
        ASSERT_TRUE(project->saveProject(tmp.path() + QLatin1Char('/'), QString::fromUtf8("kept.ntpz"), &saved));
        project->reset(false, true);
        RenderSettingsKnobs::setDescriptorsForTests(kRenderer, countOnly);
        RenderSettingsKnobs::setDescriptorsForTests(kOtherRenderer, RenderSettingsKnobs::DescriptorList());
        ASSERT_TRUE(project->loadProject(tmp.path() + QLatin1Char('/'), QString::fromUtf8("kept.ntpz")));
    }
    NodePtr loaded = getApp()->getProject()->getNodeByName(name);
    ASSERT_TRUE(bool(loaded));
    const RenderSettingsKnobs* settings = settingsOf(loaded);
    ASSERT_TRUE(settings != nullptr);
    EXPECT_FALSE(bool(loaded->getKnobByName("natronTestSettingsRendererGone")));
    EXPECT_FALSE(bool(loaded->getKnobByName("natronTestOtherSettingsRendererGain")));
    RenderSettingsKnobs::Values sent = settings->values(kRenderer);
    ASSERT_EQ(1u, sent.size());
    EXPECT_EQ(VtValue(5), sent["test:count"]);
    const std::vector<std::vector<std::string>> orphans = settings->orphans();
    ASSERT_EQ(2u, orphans.size());
    EXPECT_EQ((std::vector<std::string> { kOtherRenderer, "test:gain", "0.25" }), orphans[0]);
    EXPECT_EQ((std::vector<std::string> { kRenderer, "test:gone", "9" }), orphans[1]);
    KnobString* store = dynamic_cast<KnobString*>(loaded->getKnobByName(kRenderSettingsParamStore).get());
    ASSERT_TRUE(store != nullptr);
    EXPECT_TRUE(store->getIsSecret());
    EXPECT_NE(std::string::npos, store->getValue().find("test:gone\t9")) << store->getValue();

    // Saved again without their knobs, then loaded where both are declared again.
    {
        ProjectPtr project = getApp()->getProject();
        QString saved;
        ASSERT_TRUE(project->saveProject(tmp.path() + QLatin1Char('/'), QString::fromUtf8("again.ntpz"), &saved));
        project->reset(false, true);
        RenderSettingsKnobs::setDescriptorsForTests(kRenderer, both);
        RenderSettingsKnobs::setDescriptorsForTests(kOtherRenderer, other);
        ASSERT_TRUE(project->loadProject(tmp.path() + QLatin1Char('/'), QString::fromUtf8("again.ntpz")));
    }
    loaded = getApp()->getProject()->getNodeByName(name);
    ASSERT_TRUE(bool(loaded));
    settings = settingsOf(loaded);
    ASSERT_TRUE(settings != nullptr);
    EXPECT_TRUE(settings->orphans().empty());
    sent = settings->values(kRenderer);
    EXPECT_EQ(VtValue(5), sent["test:count"]);
    EXPECT_EQ(VtValue(9), sent["test:gone"]);
    sent = settings->values(kOtherRenderer);
    EXPECT_EQ(VtValue(0.25f), sent["test:gain"]);
}

// Prints every installed delegate's declared settings: the list the curated groups are built from.
TEST_F(RenderSettingsKnobsTest, DumpEveryDelegatesDescriptors)
{
    const std::string dump = RenderSettingsKnobs::dump();

    std::printf("renderer\tkey\ttype\tdefault\tname\n%s", dump.c_str());
    EXPECT_FALSE(dump.empty());
}
