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

#include <list>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QStringList>

#include "Tests/BaseTest.h"
#include "Tests/RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobTypes.h"
#include "Engine/LibraryBinary.h"
#include "Engine/Node.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Plugin.h"
#include "Engine/PluginActionShortcut.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

#include <ofxImageEffect.h>

#define kEngineHooksUnPremultPluginID "test.natron.built-in.EngineHooksUnPremult"
#define kEngineHooksPlaneRecorderPluginID "test.natron.built-in.EngineHooksPlaneRecorder"

// A colour plugin that stays OpenFX, so it still exercises the OpenFX hosting paths.
#define kOfxColorMatrixMajor 2

NATRON_NAMESPACE_ENTER

class EngineHooksUnPremultTestEffect
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new EngineHooksUnPremultTestEffect(n);
    }

    explicit EngineHooksUnPremultTestEffect(NodePtr n)
        : NativeEffectBase(n)
    {
    }

    virtual bool wantsHostUnPremultSelector() const OVERRIDE FINAL
    {
        return true;
    }

private:
    virtual NativePluginDescription getNativePluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        NativePluginDescription desc;

        desc.id = kEngineHooksUnPremultPluginID;
        desc.label = "Test Engine Hooks UnPremult";
        desc.description = "";
        desc.inputs.push_back(NativeInputDescription("Source", true, eDataKindImage));
        desc.outputKind = eDataKindImage;

        return desc;
    }
};

// Render clones may run render(), so what it sees is kept here rather than on the instance.
class PlaneRecorderLog {
public:
    static void clear()
    {
        std::lock_guard<std::mutex> lock(mutex());
        resolved().clear();
        failures() = 0;
    }

    static void note(const std::string& rendered,
                     const std::string& read)
    {
        std::lock_guard<std::mutex> lock(mutex());
        resolved()[rendered].insert(read);
    }

    static void noteFailure()
    {
        std::lock_guard<std::mutex> lock(mutex());
        ++failures();
    }

    static std::map<std::string, std::set<std::string>> snapshot(int* failureCount)
    {
        std::lock_guard<std::mutex> lock(mutex());
        *failureCount = failures();

        return resolved();
    }

private:
    static std::mutex& mutex()
    {
        static std::mutex m;

        return m;
    }

    static std::map<std::string, std::set<std::string>>& resolved()
    {
        static std::map<std::string, std::set<std::string>> r;

        return r;
    }

    static int& failures()
    {
        static int f = 0;

        return f;
    }
};

class EngineHooksPlaneRecorderTestEffect
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new EngineHooksPlaneRecorderTestEffect(n);
    }

    explicit EngineHooksPlaneRecorderTestEffect(NodePtr n)
        : NativeEffectBase(n)
    {
    }

private:
    virtual NativePluginDescription getNativePluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        NativePluginDescription desc;

        desc.id = kEngineHooksPlaneRecorderPluginID;
        desc.label = "Test Engine Hooks Plane Recorder";
        desc.description = "";
        desc.inputs.push_back(NativeInputDescription("Source", true, eDataKindImage));
        desc.outputKind = eDataKindImage;

        return desc;
    }

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        ImageLayerDesc rendered;
        ImageLayerDesc read;

        if (!getThreadLocalOutputLayerBeingRendered(&rendered) || !resolveInputPlaneForRender(0, args.time, args.view, &read, NULL)) {
            PlaneRecorderLog::noteFailure();
        } else {
            PlaneRecorderLog::note(rendered.getLayerID(), read.getLayerID());
        }

        for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
            if (it->second) {
                it->second->fillZero(args.roi);
            }
        }

        return eStatusOK;
    }
};

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING

namespace {

template <typename PLUGIN>
void
registerEngineHooksPlugin()
{
    EffectInstancePtr node(PLUGIN::BuildEffect(NodePtr()));
    std::map<std::string, void (*)()> functions;

    functions.insert(std::make_pair("BuildEffect", (void (*)())&PLUGIN::BuildEffect));
    LibraryBinary* binary = new LibraryBinary(functions);

    std::list<std::string> grouping;
    node->getPluginGrouping(&grouping);
    QStringList qgrouping;
    for (std::list<std::string>::iterator it = grouping.begin(); it != grouping.end(); ++it) {
        qgrouping.push_back(QString::fromUtf8(it->c_str()));
    }

    Plugin* p = appPTR->registerPlugin(QString(), qgrouping, QString::fromUtf8(node->getPluginID().c_str()), QString::fromUtf8(node->getPluginLabel().c_str()),
                                       QString::fromUtf8(""), QStringList(), node->isReader(), node->isWriter(), binary, node->renderThreadSafety() == eRenderSafetyUnsafe, node->getMajorVersion(), node->getMinorVersion(), false);
    // Registered after AppManager::load(), so the label-without-suffix the script name is made
    // from was never assigned.
    p->setLabelWithoutSuffix(Plugin::makeLabelWithoutSuffix(p->getPluginLabel()));

    std::list<PluginActionShortcut> shortcuts;
    node->getPluginShortcuts(&shortcuts);
    p->setShorcuts(shortcuts);
    p->setOpenGLRenderSupport(node->supportsOpenGLRender());
}

void
registerEngineHooksPluginsOnce()
{
    static std::once_flag once;

    std::call_once(once, []() {
        registerEngineHooksPlugin<EngineHooksUnPremultTestEffect>();
        registerEngineHooksPlugin<EngineHooksPlaneRecorderTestEffect>();
    });
}

int
inputNamed(const NodePtr& node,
           const std::string& label)
{
    for (int i = 0; i < node->getNInputs(); ++i) {
        if (node->getInputLabel(i) == label) {
            return i;
        }
    }

    return -1;
}

} // namespace

class EngineHooksTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        registerEngineHooksPluginsOnce();
        BaseTest::SetUp();
    }

    NodePtr createThreeLayerReader()
    {
        CreateNodeArgs readerArgs(_readPluginID.toStdString(), getApp()->getProject());

        readerArgs.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, std::string(NATRON_TESTS_FIXTURES_DIR "/flat-three-layers.exr"));

        return getApp()->createNode(readerArgs);
    }
};

TEST_F(EngineHooksTest, EffectAskingForTheHostUnPremultSelectorGetsIt)
{
    NodePtr asking = createNode(QString::fromUtf8(kEngineHooksUnPremultPluginID));
    ASSERT_TRUE(bool(asking));

    KnobIPtr knob = asking->getKnobByName(kUnPremultByKnobName);
    ASSERT_TRUE(bool(knob));
    EXPECT_TRUE(bool(std::dynamic_pointer_cast<KnobChannelSelect>(knob)));
    EXPECT_EQ(knob, KnobIPtr(asking->getUnPremultBySelector()));
    EXPECT_FALSE(knob->getIsSecret());
    EXPECT_FALSE(bool(asking->getKnobByName(kUnPremultByPluginKnobName)));
}

TEST_F(EngineHooksTest, EffectNotAskingGetsNoHostUnPremultSelector)
{
    NodePtr plain = createNode(QString::fromUtf8(kEngineHooksPlaneRecorderPluginID));
    ASSERT_TRUE(bool(plain));

    EXPECT_FALSE(bool(plain->getKnobByName(kUnPremultByKnobName)));
    EXPECT_FALSE(bool(plain->getUnPremultBySelector()));
}

TEST_F(EngineHooksTest, OfxPluginPairStillYieldsTheHostSelectorAndIsHidden)
{
    NodePtr colorMatrix = createNode(QString::fromUtf8("net.sf.openfx.ColorMatrixPlugin"), kOfxColorMatrixMajor);
    ASSERT_TRUE(bool(colorMatrix));

    EXPECT_TRUE(bool(colorMatrix->getUnPremultBySelector()));
    KnobIPtr pluginEnabled = colorMatrix->getKnobByName(kUnPremultByPluginKnobName);
    KnobIPtr pluginChannel = colorMatrix->getKnobByName(kUnPremultByChannelPluginKnobName);
    ASSERT_TRUE(bool(pluginEnabled));
    ASSERT_TRUE(bool(pluginChannel));
    EXPECT_TRUE(pluginEnabled->getIsSecret());
    EXPECT_TRUE(pluginChannel->getIsSecret());
}

TEST_F(EngineHooksTest, OfxColorMatrixOutsideRenderReadsTheSelectedNonColorLayer)
{
    NodePtr reader = createThreeLayerReader();
    ASSERT_TRUE(bool(reader));
    NodePtr colorMatrix = createNode(QString::fromUtf8("net.sf.openfx.ColorMatrixPlugin"), kOfxColorMatrixMajor);
    ASSERT_TRUE(bool(colorMatrix));
    connectNodes(reader, colorMatrix, 0, true);

    EffectInstancePtr effect = colorMatrix->getEffectInstance();
    ImageLayerDesc layer;
    int maskChannel = 42;

    ASSERT_TRUE(effect->resolveInputPlaneForRender(0, 0., ViewIdx(0), &layer, &maskChannel));
    EXPECT_TRUE(layer.isColorLayer()) << layer.getLayerID();
    EXPECT_EQ(-1, maskChannel);

    KnobChannelSetPtr channels = std::dynamic_pointer_cast<KnobChannelSet>(colorMatrix->getKnobByName(kNodeParamChannelSet));
    ASSERT_TRUE(bool(channels));
    channels->setLayer(0, "diffuse", NULL);

    maskChannel = 42;
    ASSERT_TRUE(effect->resolveInputPlaneForRender(0, 0., ViewIdx(0), &layer, &maskChannel));
    EXPECT_EQ(std::string("diffuse"), layer.getLayerID());
    EXPECT_EQ(-1, maskChannel);
}

TEST_F(EngineHooksTest, OfxColorMatrixMaskInputReadsTheSelectedMaskChannel)
{
    NodePtr reader = createThreeLayerReader();
    ASSERT_TRUE(bool(reader));
    NodePtr colorMatrix = createNode(QString::fromUtf8("net.sf.openfx.ColorMatrixPlugin"), kOfxColorMatrixMajor);
    ASSERT_TRUE(bool(colorMatrix));
    connectNodes(reader, colorMatrix, 0, true);

    const int maskInput = inputNamed(colorMatrix, "Mask");
    ASSERT_GE(maskInput, 0);
    connectNodes(reader, colorMatrix, maskInput, true);

    KnobBool* maskEnabled = dynamic_cast<KnobBool*>(colorMatrix->getKnobByName("enableMask_Mask").get());
    ASSERT_TRUE(maskEnabled != NULL);
    maskEnabled->setValue(true);
    KnobChannelSelect* maskSelect = dynamic_cast<KnobChannelSelect*>(colorMatrix->getKnobByName("maskChannel_Mask").get());
    ASSERT_TRUE(maskSelect != NULL);
    maskSelect->set("specular.G");

    EffectInstancePtr effect = colorMatrix->getEffectInstance();
    ImageLayerDesc layer;
    int maskChannel = -1;

    ASSERT_TRUE(effect->resolveInputPlaneForRender(maskInput, 0., ViewIdx(0), &layer, &maskChannel));
    EXPECT_EQ(std::string("specular"), layer.getLayerID());
    EXPECT_EQ(1, maskChannel);

    ImageLayerDesc layerWithoutChannel;
    ASSERT_TRUE(effect->resolveInputPlaneForRender(maskInput, 0., ViewIdx(0), &layerWithoutChannel, NULL));
    EXPECT_EQ(std::string("specular"), layerWithoutChannel.getLayerID());
}

// Rendering every plane of a three-layer source, each render call must read the source's plane
// equivalent to the one it writes, in both scheduler modes.
TEST_F(EngineHooksTest, RenderReadsTheInputPlaneEquivalentToThePlaneBeingRendered)
{
    NodePtr reader = createThreeLayerReader();
    ASSERT_TRUE(bool(reader));
    NodePtr recorder = createNode(QString::fromUtf8(kEngineHooksPlaneRecorderPluginID));
    ASSERT_TRUE(bool(recorder));
    connectNodes(reader, recorder, 0, true);

    KnobChannelSetPtr channels = std::dynamic_pointer_cast<KnobChannelSet>(recorder->getKnobByName(kNodeParamChannelSet));
    ASSERT_TRUE(bool(channels));
    channels->setAll();

    NodePtr writer = createNode(_writeOIIOPluginID);
    ASSERT_TRUE(bool(writer));
    connectNodes(recorder, writer, 0, true);
    KnobChoice* partSplitting = dynamic_cast<KnobChoice*>(writer->getKnobByName("partSplitting").get());
    ASSERT_TRUE(partSplitting != NULL);
    partSplitting->setValueFromID("single", 0);
    KnobChannelSet* writerChannels = dynamic_cast<KnobChannelSet*>(writer->getKnobByName(kNodeParamChannelSet).get());
    ASSERT_TRUE(writerChannels != NULL);
    writerChannels->setAll();

    PlaneRecorderLog::clear();
    std::vector<int> poolSizes;
    poolSizes.push_back(1);
    poolSizes.push_back(4);
    const RenderMismatch m = renderBothWays(writer, 1, 1, poolSizes);
    EXPECT_FALSE(m.any) << describe(m);

    int failures = 0;
    const std::map<std::string, std::set<std::string>> resolved = PlaneRecorderLog::snapshot(&failures);
    EXPECT_EQ(0, failures);

    const char* const planes[3] = { kNatronColorLayerID, "diffuse", "specular" };
    for (int i = 0; i < 3; ++i) {
        std::map<std::string, std::set<std::string>>::const_iterator found = resolved.find(planes[i]);
        ASSERT_NE(resolved.end(), found) << "plane " << planes[i] << " was never rendered";
        EXPECT_EQ(std::set<std::string>({ std::string(planes[i]) }), found->second) << "rendering " << planes[i];
    }
    EXPECT_EQ(3u, resolved.size());
}
