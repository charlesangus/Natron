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
#include <list>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QStringList>

#include <OpenColorIO/OpenColorIO.h>

#include "Tests/BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/LibraryBinary.h"
#include "Engine/Node.h"
#include "Engine/Nodes/IO/IoColorSpace.h"
#include "Engine/Nodes/IO/OcioSpaceKnobs.h"
#include "Engine/Nodes/Image/Op.h"
#include "Engine/Nodes/Image/PixelKernel.h"
#include "Engine/Nodes/KnobReader.h"
#include "Engine/Nodes/NodeSpec.h"
#include "Engine/Nodes/OCIO/OcioNodeBase.h"
#include "Engine/Plugin.h"
#include "Engine/Project.h"
#include "Engine/ProjectColorManagement.h"
#include "Engine/ViewIdx.h"

#define kOcioBaseTestPluginID "test.natron.built-in.OcioBaseTest"

NATRON_NAMESPACE_ENTER

namespace {

constexpr InputSpec kOcioBaseTestInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

/**
 * @brief An OCIO op converting between two spaces, in_colorspace and out_colorspace, as the
 * smallest user of the base with two spaces.
 **/
struct OcioBaseTestOp
    : public OcioOp<OcioBaseTestOp> {
    static constexpr NodeSpec kNodeSpec = {
        .id = kOcioBaseTestPluginID,
        .label = "Test OCIO Base",
        .grouping = "Other/Test",
        .inputs = kOcioBaseTestInputs,
    };

    void createKnobs(KnobHolder* holder,
                     const KnobPagePtr& page)
    {
        OcioSpaceKnobs::Description in;
        in.prefix = "in_colorspace";
        in.label = QString::fromUtf8("Input Colorspace");
        in.setLabel = QString::fromUtf8("Input Colorspace Set");
        ocio.addSpace(holder, page, in, []() { return std::optional<std::string>(); });

        OcioSpaceKnobs::Description out;
        out.prefix = "out_colorspace";
        out.label = QString::fromUtf8("Output Colorspace");
        out.setLabel = QString::fromUtf8("Output Colorspace Set");
        ocio.addSpace(holder, page, out, []() { return std::optional<std::string>(); });
    }

    OCIO_NAMESPACE::ConstTransformRcPtr transform(const KnobReader& /*reader*/) const
    {
        return ocio.spaceTransform(ocio.space(0), ocio.space(1));
    }
};

static_assert(PointOp<OcioBaseTestOp>);

class OcioBaseTestNode
    : public OcioNode<OcioBaseTestOp> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new OcioBaseTestNode(node);
    }

    explicit OcioBaseTestNode(NodePtr node)
        : OcioNode<OcioBaseTestOp>(node)
    {
    }
};

} // namespace

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING

namespace {

const char* const kStudioURI = "ocio://studio-config-v4.0.0_aces-v2.0_ocio-v2.5";
const char* const kCGURI = "ocio://cg-config-v4.0.0_aces-v2.0_ocio-v2.5";
const char* const kSRGB = "sRGB Encoded Rec.709 (sRGB)";

void
registerOcioBaseTestOnce()
{
    static std::once_flag once;

    std::call_once(once, []() {
        EffectInstancePtr node(OcioBaseTestNode::BuildEffect(NodePtr()));
        std::map<std::string, void (*)()> functions;

        functions.insert(std::make_pair("BuildEffect", (void (*)())&OcioBaseTestNode::BuildEffect));
        LibraryBinary* binary = new LibraryBinary(functions);

        std::list<std::string> grouping;
        node->getPluginGrouping(&grouping);
        QStringList qgrouping;
        for (std::list<std::string>::iterator it = grouping.begin(); it != grouping.end(); ++it) {
            qgrouping.push_back(QString::fromUtf8(it->c_str()));
        }

        Plugin* p = appPTR->registerPlugin(QString(), qgrouping, QString::fromUtf8(node->getPluginID().c_str()), QString::fromUtf8(node->getPluginLabel().c_str()),
                                           QString::fromUtf8(""), QStringList(), node->isReader(), node->isWriter(), binary, node->renderThreadSafety() == eRenderSafetyUnsafe, node->getMajorVersion(), node->getMinorVersion(), false);
        // Registered after AppManager::load(), so the label-without-suffix the script name is
        // made from was never assigned.
        p->setLabelWithoutSuffix(Plugin::makeLabelWithoutSuffix(p->getPluginLabel()));
    });
}

int
countKnobsNamed(const NodePtr& node,
                const std::string& name)
{
    const std::vector<KnobIPtr>& knobs = node->getKnobs();
    int count = 0;

    for (std::vector<KnobIPtr>::const_iterator it = knobs.begin(); it != knobs.end(); ++it) {
        if ((*it)->getName() == name) {
            ++count;
        }
    }

    return count;
}

} // namespace

class NativeOcioBaseTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        registerOcioBaseTestOnce();
        BaseTest::SetUp();
        project()->reset(false, true);
        setConfig(kStudioURI);
    }

    virtual void TearDown() OVERRIDE
    {
        project()->reset(false, true);
        BaseTest::TearDown();
    }

    ProjectPtr project() const
    {
        return getApp()->getProject();
    }

    void setConfig(const char* uri)
    {
        KnobChoicePtr config = project()->getKnobByNameAndType<KnobChoice>("ocioConfig");

        ASSERT_TRUE(bool(config));
        config->setValueFromID(uri, 0);
        ASSERT_EQ(std::string(uri), project()->getColorManagement()->getConfigSource());
    }

    NodePtr createOcioNode()
    {
        NodePtr node = createNode(QString::fromUtf8(kOcioBaseTestPluginID));

        EXPECT_TRUE(bool(node));
        EXPECT_TRUE(node && effect(node));

        return node;
    }

    static OcioBaseTestNode* effect(const NodePtr& node)
    {
        return dynamic_cast<OcioBaseTestNode*>(node->getEffectInstance().get());
    }

    static void setString(const NodePtr& node,
                          const char* name,
                          const std::string& value)
    {
        KnobStringPtr knob = std::dynamic_pointer_cast<KnobString>(node->getKnobByName(name));

        ASSERT_TRUE(bool(knob)) << name;
        knob->setValue(value);
    }

    static std::string stringValue(const NodePtr& node,
                                   const char* name)
    {
        KnobStringBasePtr knob = std::dynamic_pointer_cast<KnobStringBase>(node->getKnobByName(name));

        EXPECT_TRUE(bool(knob)) << name;

        return knob ? knob->getValue() : std::string();
    }

    static QString persistentMessage(const NodePtr& node)
    {
        QString message;
        int type = 0;

        node->getPersistentMessage(&message, &type, false);

        return message;
    }
};

TEST_F(NativeOcioBaseTest, TwoSpacesShareOneHiddenConfigPairAndContext)
{
    NodePtr node = createOcioNode();
    ASSERT_TRUE(bool(node));

    EXPECT_EQ(1, countKnobsNamed(node, "ocioConfigFile"));
    EXPECT_EQ(1, countKnobsNamed(node, "ocioWorkingSpace"));
    EXPECT_EQ(1, countKnobsNamed(node, "Context"));
    EXPECT_EQ(1, countKnobsNamed(node, "key1"));
    EXPECT_EQ(1, countKnobsNamed(node, "value4"));
    EXPECT_EQ(1, countKnobsNamed(node, "in_colorspaceIndex"));
    EXPECT_EQ(1, countKnobsNamed(node, "out_colorspaceIndex"));

    // The project pushes to the one pair.
    EXPECT_EQ(project()->getOCIOConfigSource(), stringValue(node, "ocioConfigFile"));
    EXPECT_EQ(project()->getWorkingColorSpace(), stringValue(node, "ocioWorkingSpace"));

    // The second space goes before the shared Context group.
    KnobIPtr context = node->getKnobByName("Context");
    KnobIPtr outMenu = node->getKnobByName("out_colorspaceIndex");
    ASSERT_TRUE(bool(context));
    ASSERT_TRUE(bool(outMenu));
    KnobPagePtr page = std::dynamic_pointer_cast<KnobPage>(context->getParentKnob());
    ASSERT_TRUE(bool(page));
    const std::vector<KnobIPtr> children = page->getChildren();
    int contextIndex = -1;
    int outMenuIndex = -1;
    for (std::size_t i = 0; i < children.size(); ++i) {
        if (children[i] == context) {
            contextIndex = (int)i;
        } else if (children[i] == outMenu) {
            outMenuIndex = (int)i;
        }
    }
    ASSERT_GE(contextIndex, 0);
    ASSERT_GE(outMenuIndex, 0);
    EXPECT_LT(outMenuIndex, contextIndex);

    // Both spaces read the shared Context variables.
    setString(node, "key1", "SHOT");
    setString(node, "value1", "sh010");
    const std::vector<std::pair<std::string, std::string>> variables = effect(node)->ocioKnobs().contextVariables(1.);
    ASSERT_EQ(1u, variables.size());
    EXPECT_EQ(std::string("SHOT"), variables[0].first);
    EXPECT_EQ(std::string("sh010"), variables[0].second);
}

TEST_F(NativeOcioBaseTest, EqualSpacesAreAnIdentityWithNoProcessor)
{
    NodePtr node = createOcioNode();
    ASSERT_TRUE(bool(node));

    setString(node, "in_colorspace", "ACEScg");
    setString(node, "out_colorspace", "ACEScg");
    OcioProcessor baked = effect(node)->bakeOcio(1., ViewIdx(0));
    EXPECT_FALSE(bool(baked.processor));
    EXPECT_FALSE(bool(baked.cpu));
    EXPECT_TRUE(baked.error.empty()) << baked.error;

    // A role names the same space as the space it designates.
    const std::string sceneLinear = IoColorSpace::nameInConfig(project()->getColorManagement()->getConfig(), OCIO_NAMESPACE::ROLE_SCENE_LINEAR);
    ASSERT_FALSE(sceneLinear.empty());
    setString(node, "in_colorspace", OCIO_NAMESPACE::ROLE_SCENE_LINEAR);
    setString(node, "out_colorspace", sceneLinear);
    EXPECT_FALSE(bool(effect(node)->ocioKnobs().spaceTransform(OCIO_NAMESPACE::ROLE_SCENE_LINEAR, sceneLinear)));
    baked = effect(node)->bakeOcio(1., ViewIdx(0));
    EXPECT_FALSE(bool(baked.cpu));
    EXPECT_TRUE(baked.error.empty()) << baked.error;
}

TEST_F(NativeOcioBaseTest, ProcessorMatchesOcioAndRowLeavesAlpha)
{
    NodePtr node = createOcioNode();
    ASSERT_TRUE(bool(node));

    setString(node, "in_colorspace", "ACEScg");
    setString(node, "out_colorspace", kSRGB);
    const OcioProcessor baked = effect(node)->bakeOcio(1., ViewIdx(0));
    ASSERT_TRUE(baked.cpu != nullptr) << baked.error;
    ASSERT_TRUE(baked.processor != nullptr);

    OCIO_NAMESPACE::ConstCPUProcessorRcPtr direct = project()->getColorManagement()->getConfig()->getProcessor("ACEScg", kSRGB)->getDefaultCPUProcessor();
    float expected[3] = { 0.18f, 0.5f, 0.9f };
    direct->applyRGB(expected);

    const float src[8] = { 0.18f, 0.5f, 0.9f, 0.25f, 0.18f, 0.5f, 0.9f, 0.75f };
    float dst[8] = { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f };
    RowIO io;
    io.src[0] = src;
    io.nSrc = 1;
    io.dst = dst;
    io.width = 2;
    io.nComps = 4;
    io.channels.set();
    OcioNodeKnobs::applyRow(baked, io);
    for (int x = 0; x < 2; ++x) {
        for (int c = 0; c < 3; ++c) {
            EXPECT_NEAR(expected[c], dst[x * 4 + c], 1e-5f) << "pixel " << x << " channel " << c;
        }
        EXPECT_EQ(src[x * 4 + 3], dst[x * 4 + 3]) << "pixel " << x;
    }
}

TEST_F(NativeOcioBaseTest, ConfigChangeRebuildsTheProcessorAndRefreshesMenus)
{
    NodePtr node = createOcioNode();
    ASSERT_TRUE(bool(node));

    setString(node, "in_colorspace", "ACEScg");
    setString(node, "out_colorspace", kSRGB);
    const OcioProcessor before = effect(node)->bakeOcio(1., ViewIdx(0));
    ASSERT_TRUE(before.processor != nullptr) << before.error;
    // The same transform comes from the project's cache while the config stays.
    EXPECT_EQ(before.processor, effect(node)->bakeOcio(1., ViewIdx(0)).processor);

    setConfig(kCGURI);
    EXPECT_EQ(std::string(kCGURI), stringValue(node, "ocioConfigFile"));
    const OcioProcessor after = effect(node)->bakeOcio(1., ViewIdx(0));
    ASSERT_TRUE(after.processor != nullptr) << after.error;
    EXPECT_NE(before.processor, after.processor);

    KnobChoicePtr menu = std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName("out_colorspaceIndex"));
    ASSERT_TRUE(bool(menu));
    EXPECT_EQ((int)Project::colorSpaceOptions(*project()->getColorManagement()).size(), menu->getNumEntries());
    ASSERT_GE(menu->getValue(), 0);
    EXPECT_EQ(std::string(kSRGB), menu->getEntry(menu->getValue()).id);
}

TEST_F(NativeOcioBaseTest, MissingSpaceShowsAnErrorAndPassesThrough)
{
    NodePtr node = createOcioNode();
    ASSERT_TRUE(bool(node));

    setString(node, "in_colorspace", "ACEScg");
    setString(node, "out_colorspace", "no such colorspace");
    const OcioProcessor broken = effect(node)->bakeOcio(1., ViewIdx(0));
    EXPECT_FALSE(bool(broken.cpu));
    EXPECT_FALSE(broken.error.empty());
    EXPECT_FALSE(persistentMessage(node).isEmpty());

    const float src[4] = { 0.18f, 0.5f, 0.9f, 0.25f };
    float dst[4] = { 0.f, 0.f, 0.f, 0.f };
    RowIO io;
    io.src[0] = src;
    io.nSrc = 1;
    io.dst = dst;
    io.width = 1;
    io.nComps = 4;
    io.channels.set();
    OcioNodeKnobs::applyRow(broken, io);
    for (int c = 0; c < 4; ++c) {
        EXPECT_EQ(src[c], dst[c]) << "channel " << c;
    }

    setString(node, "out_colorspace", kSRGB);
    EXPECT_TRUE(persistentMessage(node).isEmpty()) << persistentMessage(node).toStdString();
}
