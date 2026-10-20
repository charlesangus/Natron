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
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#include <gtest/gtest.h>

#include <QString>
#include <QStringList>

#include "Tests/BaseTest.h"

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/LibraryBinary.h"
#include "Engine/Node.h"
#include "Engine/Nodes/KnobReader.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/NodeSpec.h"
#include "Engine/Plugin.h"
#include "Engine/PluginActionShortcut.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"

#define kNodeSpecTestPluginID "test.natron.built-in.NodeSpec"
#define kNodeSpecBakeTestPluginID "test.natron.built-in.NodeSpecBake"

NATRON_NAMESPACE_ENTER

namespace NodeSpecFixtures {
inline constexpr std::string_view kRfvReads[] = { "shot" };

inline constexpr InputSpec kInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Depth", .kind = eDataKindDeep, .role = InputRole::Aux, .optional = true, .time = InputTime::Any, .boundary = InputBoundary::Nearest, .layer = InputLayer::select("depthLayer") },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
    InputSpec { .label = "In", .role = InputRole::Variadic, .optional = true, .layer = InputLayer::pairWith(0), .count = 3, .firstNumber = 2 },
};

inline constexpr NodeSpec kSpec = {
    .id = kNodeSpecTestPluginID,
    .label = "Node Spec Test",
    .description = "A node described by a NodeSpec.",
    .grouping = "Test/Spec Nested",
    .majorVersion = 3,
    .minorVersion = 2,
    .isWriter = true,
    .outputKind = eDataKindPolymorphic,
    .rfvReads = kRfvReads,
    .inputs = kInputs,
};

static_assert(isValidNodeSpec(kSpec));
static_assert(kSpec.inputCount() == 6);
static_assert(kSpec.resolveInput(4).spec == &kInputs[3] && kSpec.resolveInput(4).indexInRun == 1);
static_assert(kSpec.resolveInput(6).spec == nullptr && kSpec.resolveInput(-1).spec == nullptr);

inline constexpr InputSpec kDeepMaskInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .kind = eDataKindDeep, .role = InputRole::Mask, .optional = true },
};
inline constexpr NodeSpec kDeepMaskSpec = { .id = "bad.deepMask", .label = "Bad", .inputs = kDeepMaskInputs };

inline constexpr InputSpec kVariadicFirstInputs[] = {
    InputSpec { .label = "In", .role = InputRole::Variadic, .count = 2 },
    InputSpec { .label = "Source" },
};
inline constexpr NodeSpec kVariadicFirstSpec = { .id = "bad.variadicFirst", .label = "Bad", .inputs = kVariadicFirstInputs };

inline constexpr InputSpec kSelfPairInputs[] = {
    InputSpec { .label = "Source", .layer = InputLayer::pairWith(0) },
};
inline constexpr NodeSpec kSelfPairSpec = { .id = "bad.selfPair", .label = "Bad", .inputs = kSelfPairInputs };

inline constexpr InputSpec kPairCycleInputs[] = {
    InputSpec { .label = "A", .layer = InputLayer::pairWith(2) },
    InputSpec { .label = "B", .layer = InputLayer::pairWith(0) },
    InputSpec { .label = "C", .layer = InputLayer::pairWith(1) },
};
inline constexpr NodeSpec kPairCycleSpec = { .id = "bad.pairCycle", .label = "Bad", .inputs = kPairCycleInputs };

inline constexpr InputSpec kPairTwoCycleInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "A", .layer = InputLayer::pairWith(2) },
    InputSpec { .label = "B", .layer = InputLayer::pairWith(1) },
};
inline constexpr NodeSpec kPairTwoCycleSpec = { .id = "bad.pairTwoCycle", .label = "Bad", .inputs = kPairTwoCycleInputs };

inline constexpr InputSpec kPairChainInputs[] = {
    InputSpec { .label = "A", .layer = InputLayer::pairWith(1) },
    InputSpec { .label = "B", .layer = InputLayer::pairWith(2) },
    InputSpec { .label = "C", .layer = InputLayer::select("layer") },
};
inline constexpr NodeSpec kPairChainSpec = { .id = "good.pairChain", .label = "Good", .inputs = kPairChainInputs };

static_assert(!nodeSpecMasksAreImages(kDeepMaskSpec) && !isValidNodeSpec(kDeepMaskSpec));
static_assert(!nodeSpecVariadicIsLast(kVariadicFirstSpec) && !isValidNodeSpec(kVariadicFirstSpec));
static_assert(!nodeSpecLayerPairsAreValid(kSelfPairSpec) && !isValidNodeSpec(kSelfPairSpec));
static_assert(!nodeSpecLayerPairsAreValid(kPairCycleSpec) && !isValidNodeSpec(kPairCycleSpec));
static_assert(!nodeSpecLayerPairsAreValid(kPairTwoCycleSpec) && !isValidNodeSpec(kPairTwoCycleSpec));
static_assert(nodeSpecLayerPairsAreValid(kPairChainSpec) && isValidNodeSpec(kPairChainSpec));

inline constexpr InputSpec kBakeInputs[] = {
    InputSpec { .label = "Source", .optional = true },
};

inline constexpr NodeSpec kBakeSpec = {
    .id = kNodeSpecBakeTestPluginID,
    .label = "Node Spec Bake Test",
    .inputs = kBakeInputs,
};
} // namespace NodeSpecFixtures

class SpecTestEffect
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new SpecTestEffect(n);
    }

    explicit SpecTestEffect(NodePtr n)
        : NativeEffectBase(n)
    {
    }

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<NodeSpecFixtures::kSpec>();
    }

    virtual Route route(double /*time*/,
                        ViewIdx /*view*/) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return nextRoute;
    }

    bool callIsIdentity(double time,
                        ViewIdx view,
                        double* inputTime,
                        ViewIdx* inputView,
                        int* inputNb)
    {
        return isIdentity(time, RenderScale::identity, RectI(0, 0, 16, 16), view, inputTime, inputView, inputNb);
    }

    Route nextRoute;

private:
    virtual void initializeKnobs() OVERRIDE FINAL
    {
    }
};

class BakeTestEffect
    : public NativeEffectBase {
public:
    struct Params {
        double gain;
        int count;
        bool enabled;
        double offset[2];
    };

    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new BakeTestEffect(n);
    }

    explicit BakeTestEffect(NodePtr n)
        : NativeEffectBase(n)
    {
    }

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<NodeSpecFixtures::kBakeSpec>();
    }

    Params bake(const KnobReader& reader) const
    {
        Params p;

        p.gain = reader.value(_gain);
        p.count = reader.value(_count);
        p.enabled = reader.value(_enabled);
        p.offset[0] = reader.value(_offset, 0);
        p.offset[1] = reader.value(_offset, 1);

        return p;
    }

private:
    virtual void initializeKnobs() OVERRIDE FINAL
    {
        KnobPagePtr page = createKnob<KnobPage>(std::string("Controls"));

        KnobDoublePtr gain = createKnob<KnobDouble>(std::string("Gain"));
        gain->setName("gain");
        page->addKnob(gain);
        _gain = gain;

        KnobIntPtr count = createKnob<KnobInt>(std::string("Count"));
        count->setName("count");
        page->addKnob(count);
        _count = count;

        KnobBoolPtr enabled = createKnob<KnobBool>(std::string("Enabled"));
        enabled->setName("enabled");
        page->addKnob(enabled);
        _enabled = enabled;

        KnobDoublePtr offset = createKnob<KnobDouble>(std::string("Offset"), 2);
        offset->setName("offset");
        page->addKnob(offset);
        _offset = offset;
    }

    KnobDoubleWPtr _gain;
    KnobIntWPtr _count;
    KnobBoolWPtr _enabled;
    KnobDoubleWPtr _offset;
};

static_assert(BakesParams<BakeTestEffect>);
static_assert(std::is_trivially_copyable_v<BakeTestEffect::Params>);

// Each class below declares an invalid spec, so this block must fail to compile; define the macro
// to see the diagnostic staticNodeSpec() gives.
#if defined(NATRON_NODESPEC_COMPILE_FAIL_TESTS)
class DeepMaskSpecEffect
    : public NativeEffectBase {
public:
    explicit DeepMaskSpecEffect(NodePtr n)
        : NativeEffectBase(n)
    {
    }

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<NodeSpecFixtures::kDeepMaskSpec>();
    }
};

class VariadicFirstSpecEffect
    : public NativeEffectBase {
public:
    explicit VariadicFirstSpecEffect(NodePtr n)
        : NativeEffectBase(n)
    {
    }

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<NodeSpecFixtures::kVariadicFirstSpec>();
    }
};
#endif

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING

namespace {

template <typename PLUGIN>
void
registerNodeSpecTestPlugin()
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
registerNodeSpecTestPluginsOnce()
{
    static std::once_flag once;

    std::call_once(once, []() {
        registerNodeSpecTestPlugin<BakeTestEffect>();
    });
}

} // namespace

class NodeSpecTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        registerNodeSpecTestPluginsOnce();
        BaseTest::SetUp();
    }
};

TEST_F(NodeSpecTest, SpecAnswersEveryDescriptionQuery)
{
    std::shared_ptr<SpecTestEffect> spec(new SpecTestEffect(NodePtr()));

    ASSERT_NE(nullptr, spec->nodeSpec());

    EXPECT_EQ(std::string(kNodeSpecTestPluginID), spec->getPluginID());
    EXPECT_EQ(std::string("Node Spec Test"), spec->getPluginLabel());
    EXPECT_EQ(std::string("A node described by a NodeSpec."), spec->getPluginDescription());
    EXPECT_EQ(3, spec->getMajorVersion());
    EXPECT_EQ(2, spec->getMinorVersion());
    EXPECT_TRUE(spec->isWriter());
    EXPECT_TRUE(spec->isOutput());
    EXPECT_EQ(eDataKindPolymorphic, spec->getOutputDataKind());

    std::list<std::string> grouping;
    spec->getPluginGrouping(&grouping);
    ASSERT_EQ(2u, grouping.size());
    EXPECT_EQ(std::string("Test"), grouping.front());
    EXPECT_EQ(std::string("Spec Nested"), grouping.back());

    ASSERT_EQ(6, spec->getNInputs());

    const char* const labels[] = { "Source", "Depth", "Mask", "In2", "In3", "In4" };
    const bool optional[] = { false, true, true, true, true, true };
    const bool mask[] = { false, false, true, false, false, false };
    const DataKindEnum kinds[] = { eDataKindImage, eDataKindDeep, eDataKindImage, eDataKindImage, eDataKindImage, eDataKindImage };
    for (int i = 0; i < 6; ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(std::string(labels[i]), spec->getInputLabel(i));
        EXPECT_EQ(optional[i], spec->isInputOptional(i));
        EXPECT_EQ(mask[i], spec->isInputMask(i));
        EXPECT_EQ(kinds[i], spec->getInputDataKind(i));
    }
}

TEST_F(NodeSpecTest, IsIdentityAnswersFromTheRoute)
{
    std::shared_ptr<SpecTestEffect> effect(new SpecTestEffect(NodePtr()));
    double inputTime = 0.;
    ViewIdx inputView(0);
    int inputNb = -3;

    effect->nextRoute = Route::none();
    EXPECT_FALSE(effect->callIsIdentity(5., ViewIdx(1), &inputTime, &inputView, &inputNb));

    effect->nextRoute = Route::input(3, 7.);
    inputNb = -3;
    EXPECT_TRUE(effect->callIsIdentity(5., ViewIdx(1), &inputTime, &inputView, &inputNb));
    EXPECT_EQ(3, inputNb);
    EXPECT_EQ(7., inputTime);
    EXPECT_EQ(1, inputView.value());

    effect->nextRoute = Route::black();
    inputNb = -3;
    EXPECT_TRUE(effect->callIsIdentity(5., ViewIdx(1), &inputTime, &inputView, &inputNb));
    EXPECT_EQ(-1, inputNb);
    EXPECT_EQ(5., inputTime);
    EXPECT_EQ(1, inputView.value());
}

TEST_F(NodeSpecTest, BakeReadsEachKnobOnce)
{
    NodePtr node = createNode(QString::fromUtf8(kNodeSpecBakeTestPluginID));
    ASSERT_TRUE(bool(node));
    std::shared_ptr<BakeTestEffect> effect = std::dynamic_pointer_cast<BakeTestEffect>(node->getEffectInstance());
    ASSERT_TRUE(bool(effect));

    KnobDoublePtr gain = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName("gain"));
    KnobIntPtr count = std::dynamic_pointer_cast<KnobInt>(node->getKnobByName("count"));
    KnobBoolPtr enabled = std::dynamic_pointer_cast<KnobBool>(node->getKnobByName("enabled"));
    KnobDoublePtr offset = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName("offset"));
    ASSERT_TRUE(gain && count && enabled && offset);

    gain->setValue(2.5);
    count->setValue(7);
    enabled->setValue(true);
    offset->setValue(-1.5, ViewSpec::all(), 0);
    offset->setValue(4., ViewSpec::all(), 1);

    const KnobReader reader(1., ViewIdx(0));
    const BakeTestEffect::Params params = bakeParams(*effect, reader);

    EXPECT_EQ(2.5, params.gain);
    EXPECT_EQ(7, params.count);
    EXPECT_TRUE(params.enabled);
    EXPECT_EQ(-1.5, params.offset[0]);
    EXPECT_EQ(4., params.offset[1]);
    EXPECT_EQ(5, reader.readCount());

    const KnobReader second(1., ViewIdx(0));
    EXPECT_EQ(7, bakeParams(*effect, second).count);
    EXPECT_EQ(5, second.readCount());
}

TEST_F(NodeSpecTest, ReaderRejectsAMissingKnob)
{
    const KnobReader reader(0., ViewIdx(0));
    const KnobDoubleWPtr missing;

    EXPECT_THROW((void)reader.value(missing), std::invalid_argument);
    EXPECT_EQ(3., reader.valueOr(missing, 3.));
    EXPECT_EQ(0, reader.readCount());
}
