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
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/RFV/ReverseFlowVariableSet.h"
#include "Engine/Project.h"
#include "Engine/RFV.h"
#include "Engine/RenderContext.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kConstantPluginID = "net.sf.openfx.ConstantPlugin";
const char* const kGradePluginID = "net.sf.openfx.GradePlugin";

RenderContextName
plainName(const char* name)
{
    return RenderContextName::plain(name);
}

RenderContextReadSet
readsOf(const std::vector<RenderContextName>& names)
{
    RenderContextReadSet reads;

    for (const RenderContextName& name : names) {
        reads.add(name);
    }

    return reads;
}

class TestReadSource
    : public ContextReadSource {
public:
    explicit TestReadSource(const RenderContextReadSet& reads)
        : _reads(reads)
    {
    }

    void setReads(const RenderContextReadSet& reads)
    {
        _reads = reads;
        invalidateRenderContextReads();
    }

    virtual void appendContextReads(RenderContextReadSet* reads) const OVERRIDE FINAL
    {
        reads->merge(_reads);
    }

private:
    RenderContextReadSet _reads;
};

} // namespace

TEST(RenderContext, EmptyHashesToZero)
{
    EXPECT_EQ(0u, RenderContext::empty()->hash());
    EXPECT_TRUE(RenderContext::empty()->isEmpty());
    EXPECT_EQ(0u, RenderContext::Builder().build()->hash());

    RenderContextPtr one = RenderContext::Builder().set(plainName("t"), 1.).build();
    EXPECT_NE(0u, one->hash());
    EXPECT_EQ(0u, RenderContext::Builder(one).erase(plainName("t")).build()->hash());
}

TEST(RenderContext, HashIsStableAndOrderIndependent)
{
    RenderContextPtr a = RenderContext::Builder()
                             .set(plainName("gain"), 2.5)
                             .set(RenderContextName::loop("i"), std::int64_t(3))
                             .set(RenderContextName::loops("outer", "t"), std::string("x"))
                             .set(plainName("weights"), std::vector<double> { 1., 2. })
                             .set(plainName("ids"), std::vector<std::int64_t> { 4, 5 })
                             .set(plainName("names"), std::vector<std::string> { "a", "b" })
                             .set(plainName("on"), true)
                             .build();
    RenderContextPtr b = RenderContext::Builder()
                             .set(plainName("on"), true)
                             .set(plainName("names"), std::vector<std::string> { "a", "b" })
                             .set(plainName("ids"), std::vector<std::int64_t> { 4, 5 })
                             .set(plainName("weights"), std::vector<double> { 1., 2. })
                             .set(RenderContextName::loops("outer", "t"), std::string("x"))
                             .set(RenderContextName::loop("i"), std::int64_t(3))
                             .set(plainName("gain"), 2.5)
                             .build();

    EXPECT_EQ(a->hash(), b->hash());
    EXPECT_TRUE(*a == *b);
    EXPECT_EQ(a->hash(), a->hash());

    // Rebuilt from its own entries, through overlays in either grouping.
    RenderContextPtr rebuilt = RenderContext::Builder(a).build();
    EXPECT_EQ(a->hash(), rebuilt->hash());
    RenderContextPtr left = RenderContext::Builder().set(plainName("gain"), 2.5).build();
    RenderContextPtr right = RenderContext::Builder(a).erase(plainName("gain")).build();
    EXPECT_EQ(a->hash(), left->overlay(right)->hash());
    EXPECT_EQ(a->hash(), right->overlay(left)->hash());
}

TEST(RenderContext, HashSeesValueBitsAndTypes)
{
    const RenderContextName t = plainName("t");
    const U64 positiveZero = RenderContext::Builder().set(t, 0.).build()->hash();
    const U64 negativeZero = RenderContext::Builder().set(t, -0.).build()->hash();
    EXPECT_NE(positiveZero, negativeZero);

    const U64 asDouble = RenderContext::Builder().set(t, 1.).build()->hash();
    const U64 asInt = RenderContext::Builder().set(t, std::int64_t(1)).build()->hash();
    const U64 asBool = RenderContext::Builder().set(t, true).build()->hash();
    EXPECT_NE(asDouble, asInt);
    EXPECT_NE(asInt, asBool);

    const U64 nextDouble = RenderContext::Builder().set(t, 1.0000000000000002).build()->hash();
    EXPECT_NE(asDouble, nextDouble);

    const U64 scalar = RenderContext::Builder().set(t, 1.).build()->hash();
    const U64 vector = RenderContext::Builder().set(t, std::vector<double> { 1. }).build()->hash();
    EXPECT_NE(scalar, vector);
}

TEST(RenderContext, NamespacesAreSeparate)
{
    const RenderContextName loopT = RenderContextName::loop("t");
    const RenderContextName plainT = plainName("t");
    const RenderContextName namedLoopT = RenderContextName::loops("a", "t");
    const RenderContextName dottedPlain = plainName("loop.t");

    RenderContextPtr ctx = RenderContext::Builder().set(loopT, 1.).set(plainT, 2.).build();
    ASSERT_EQ(2u, ctx->size());
    ASSERT_TRUE(ctx->find(loopT));
    ASSERT_TRUE(ctx->find(plainT));
    EXPECT_EQ(1., std::get<double>(*ctx->find(loopT)));
    EXPECT_EQ(2., std::get<double>(*ctx->find(plainT)));
    EXPECT_FALSE(ctx->find(namedLoopT));
    EXPECT_FALSE(ctx->find(dottedPlain));

    const U64 inLoop = RenderContext::Builder().set(loopT, 1.).build()->hash();
    const U64 inPlain = RenderContext::Builder().set(plainT, 1.).build()->hash();
    const U64 inNamedLoop = RenderContext::Builder().set(namedLoopT, 1.).build()->hash();
    const U64 inDottedPlain = RenderContext::Builder().set(dottedPlain, 1.).build()->hash();
    EXPECT_NE(inLoop, inPlain);
    EXPECT_NE(inLoop, inNamedLoop);
    EXPECT_NE(inLoop, inDottedPlain);
    EXPECT_NE(inPlain, inNamedLoop);

    EXPECT_EQ(0u, ctx->restrictTo(readsOf({ namedLoopT, dottedPlain }))->hash());
    RenderContextPtr loopOnly = ctx->restrictTo(readsOf({ loopT }));
    ASSERT_EQ(1u, loopOnly->size());
    EXPECT_TRUE(loopOnly->find(loopT));
}

TEST(RenderContext, OverlayPrecedence)
{
    const RenderContextName a = plainName("a");
    const RenderContextName b = plainName("b");
    const RenderContextName c = plainName("c");
    RenderContextPtr base = RenderContext::Builder().set(a, 1.).set(b, 2.).build();
    RenderContextPtr over = RenderContext::Builder().set(b, 20.).set(c, 30.).build();

    RenderContextPtr merged = base->overlay(over);
    ASSERT_EQ(3u, merged->size());
    EXPECT_EQ(1., std::get<double>(*merged->find(a)));
    EXPECT_EQ(20., std::get<double>(*merged->find(b)));
    EXPECT_EQ(30., std::get<double>(*merged->find(c)));

    // Contexts are immutable: neither side changed.
    EXPECT_EQ(2., std::get<double>(*base->find(b)));
    EXPECT_FALSE(base->find(c));
    EXPECT_FALSE(over->find(a));

    RenderContextPtr reversed = over->overlay(base);
    EXPECT_EQ(2., std::get<double>(*reversed->find(b)));
    EXPECT_NE(merged->hash(), reversed->hash());

    EXPECT_EQ(base.get(), base->overlay(RenderContext::empty()).get());
    EXPECT_EQ(base.get(), base->overlay(RenderContextPtr()).get());
    EXPECT_EQ(over.get(), RenderContext::empty()->overlay(over).get());
}

TEST(RenderContext, Restriction)
{
    const RenderContextName a = plainName("a");
    const RenderContextName b = plainName("b");
    const RenderContextName c = plainName("c");
    RenderContextPtr ctx = RenderContext::Builder().set(a, 1.).set(b, std::string("two")).set(c, true).build();

    RenderContextPtr ab = ctx->restrictTo(readsOf({ a, b }));
    ASSERT_EQ(2u, ab->size());
    EXPECT_TRUE(ab->find(a));
    EXPECT_TRUE(ab->find(b));
    EXPECT_FALSE(ab->find(c));
    EXPECT_EQ(RenderContext::Builder().set(b, std::string("two")).set(a, 1.).build()->hash(), ab->hash());

    // A read of an unset name restricts to nothing for that name.
    RenderContextPtr missing = ctx->restrictTo(readsOf({ plainName("z") }));
    EXPECT_TRUE(missing->isEmpty());
    EXPECT_EQ(0u, missing->hash());

    EXPECT_EQ(0u, ctx->restrictTo(RenderContextReadSet())->hash());
    EXPECT_EQ(ctx.get(), ctx->restrictTo(RenderContextReadSet::all()).get());
    EXPECT_EQ(ctx.get(), ctx->restrictTo(readsOf({ a, b, c })).get());

    // Two contexts that differ only outside the read-set restrict to the same key.
    RenderContextPtr other = RenderContext::Builder(ctx).set(c, false).build();
    EXPECT_NE(ctx->hash(), other->hash());
    EXPECT_EQ(ctx->restrictTo(readsOf({ a, b }))->hash(), other->restrictTo(readsOf({ a, b }))->hash());
}

TEST(RenderContext, ReadSetMerge)
{
    RenderContextReadSet reads = readsOf({ plainName("b"), plainName("a") });
    reads.add(plainName("a"));
    ASSERT_EQ(2u, reads.names().size());
    EXPECT_EQ(plainName("a"), reads.names()[0]);
    EXPECT_TRUE(reads.contains(plainName("b")));
    EXPECT_FALSE(reads.contains(RenderContextName::loop("b")));

    reads.merge(readsOf({ plainName("c"), plainName("a") }));
    EXPECT_EQ(3u, reads.names().size());

    reads.merge(RenderContextReadSet::all());
    EXPECT_TRUE(reads.readsAll());
    EXPECT_FALSE(reads.empty());
    EXPECT_TRUE(reads.contains(plainName("anything")));
    reads.add(plainName("d"));
    EXPECT_TRUE(reads.names().empty());

    EXPECT_TRUE(RenderContextReadSet().empty());
}

class ContextReadSetGraph
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
    }

    static RenderContextReadSet upstream(const NodePtr& node)
    {
        RenderContextReadSetConstPtr reads = node->getUpstreamContextReadSet();
        EXPECT_TRUE(bool(reads));

        return reads ? *reads : RenderContextReadSet();
    }
};

// head -> middle -> tail, with tail's offset reading side through an expression.
TEST_F(ContextReadSetGraph, ChainAndExpressionSideBranch)
{
    NodePtr head = createNode(QString::fromUtf8(kConstantPluginID));
    NodePtr middle = createNode(QString::fromUtf8(kGradePluginID));
    NodePtr tail = createNode(QString::fromUtf8(kGradePluginID));
    NodePtr side = createNode(QString::fromUtf8(kConstantPluginID));
    ASSERT_TRUE(head && middle && tail && side);
    connectNodes(head, middle, 0, true);
    connectNodes(middle, tail, 0, true);

    KnobIPtr offset = tail->getKnobByName("offset");
    ASSERT_TRUE(bool(offset));
    ASSERT_NO_THROW(offset->setExpression(0, side->getScriptName() + ".color.getValue(0)", false, true));

    for (const NodePtr& node : { head, middle, tail, side }) {
        EXPECT_TRUE(upstream(node).empty());
    }

    KnobIPtr headColor = head->getKnobByName("color");
    KnobIPtr sideColor = side->getKnobByName("color");
    ASSERT_TRUE(headColor && sideColor);

    const RenderContextName loopT = RenderContextName::loop("t");
    const RenderContextName gain = plainName("gain");
    std::shared_ptr<TestReadSource> headSource = std::make_shared<TestReadSource>(readsOf({ loopT }));
    std::shared_ptr<TestReadSource> sideSource = std::make_shared<TestReadSource>(readsOf({ gain }));
    headColor->addContextReadSource(headSource);
    sideColor->addContextReadSource(sideSource);
    EXPECT_TRUE(headColor->hasContextReadSources());
    EXPECT_EQ(readsOf({ loopT }), headColor->getContextReadSet());
    EXPECT_TRUE(tail->getKnobByName("multiply")->getContextReadSet().empty());

    EXPECT_EQ(readsOf({ loopT }), upstream(head));
    EXPECT_EQ(readsOf({ loopT }), upstream(middle));
    EXPECT_EQ(readsOf({ loopT, gain }), upstream(tail));
    EXPECT_EQ(readsOf({ gain }), upstream(side));

    // Memoized: asked again with nothing changed, the same set comes back.
    EXPECT_EQ(tail->getUpstreamContextReadSet().get(), tail->getUpstreamContextReadSet().get());

    headColor->removeContextReadSource(headSource);
    EXPECT_FALSE(headColor->hasContextReadSources());
    EXPECT_TRUE(upstream(head).empty());
    EXPECT_TRUE(upstream(middle).empty());
    EXPECT_EQ(readsOf({ gain }), upstream(tail));

    // A source whose reads change.
    sideSource->setReads(readsOf({ gain, RenderContextName::loops("outer", "i") }));
    EXPECT_EQ(readsOf({ gain, RenderContextName::loops("outer", "i") }), upstream(tail));

    sideSource->setReads(RenderContextReadSet::all());
    EXPECT_TRUE(upstream(tail).readsAll());
    EXPECT_TRUE(upstream(middle).empty());

    sideColor->removeContextReadSource(sideSource);
    for (const NodePtr& node : { head, middle, tail, side }) {
        EXPECT_TRUE(upstream(node).empty());
    }
}

TEST_F(ContextReadSetGraph, FollowsGraphEdits)
{
    NodePtr head = createNode(QString::fromUtf8(kConstantPluginID));
    NodePtr middle = createNode(QString::fromUtf8(kGradePluginID));
    NodePtr tail = createNode(QString::fromUtf8(kGradePluginID));
    NodePtr side = createNode(QString::fromUtf8(kConstantPluginID));
    ASSERT_TRUE(head && middle && tail && side);
    connectNodes(head, middle, 0, true);
    connectNodes(middle, tail, 0, true);

    const RenderContextName loopT = RenderContextName::loop("t");
    const RenderContextName gain = plainName("gain");
    head->getKnobByName("color")->addContextReadSource(std::make_shared<TestReadSource>(readsOf({ loopT })));
    side->getKnobByName("color")->addContextReadSource(std::make_shared<TestReadSource>(readsOf({ gain })));

    EXPECT_EQ(readsOf({ loopT }), upstream(tail));

    KnobIPtr offset = tail->getKnobByName("offset");
    ASSERT_TRUE(bool(offset));
    ASSERT_NO_THROW(offset->setExpression(0, side->getScriptName() + ".color.getValue(0)", false, true));
    EXPECT_EQ(readsOf({ loopT, gain }), upstream(tail));

    offset->clearExpression(0, true);
    EXPECT_EQ(readsOf({ loopT }), upstream(tail));

    disconnectNodes(head, middle, true);
    EXPECT_TRUE(upstream(middle).empty());
    EXPECT_TRUE(upstream(tail).empty());
    EXPECT_EQ(readsOf({ loopT }), upstream(head));
}

// A binding's identity stands for the variable's value at every time: the same knob, name and
// captured context give the same identity, and an edit of the knob's curve or expression moves it.
TEST_F(ContextReadSetGraph, BindingIdentityFollowsTheKnob)
{
    NodePtr probe = createNode(QString::fromUtf8(kGradePluginID));
    ASSERT_TRUE(bool(probe));
    KnobDoublePtr offset = std::dynamic_pointer_cast<KnobDouble>(probe->getKnobByName("offset"));
    ASSERT_TRUE(bool(offset));
    const RenderContextPtr captured = RenderContext::Builder().set(plainName("outer"), 1.).build();

    offset->setValueAtTime(1., 5., ViewSpec::all(), 0);
    const RenderContextBinding first = RenderContextBinding::create(offset, "v", captured);
    const RenderContextBinding same = RenderContextBinding::create(offset, "v", captured);
    EXPECT_EQ(first.identity, same.identity);
    EXPECT_TRUE(renderContextValuesEqual(RenderContextValue(first), RenderContextValue(same)));
    const RenderContextPtr withFirst = RenderContext::Builder().set(plainName("v"), first).build();
    const RenderContextPtr withSame = RenderContext::Builder().set(plainName("v"), same).build();
    EXPECT_EQ(withFirst->hash(), withSame->hash());
    EXPECT_TRUE(*withFirst == *withSame);

    EXPECT_NE(first.identity, RenderContextBinding::create(offset, "w", captured).identity);
    EXPECT_NE(first.identity, RenderContextBinding::create(offset, "v", RenderContext::empty()).identity);
    EXPECT_NE(first.identity, RenderContextBinding::create(offset, "v", RenderContext::Builder().set(plainName("outer"), 2.).build()).identity);

    offset->setValueAtTime(1., 6., ViewSpec::all(), 0);
    const RenderContextBinding keyEdited = RenderContextBinding::create(offset, "v", captured);
    EXPECT_NE(first.identity, keyEdited.identity);
    offset->setValueAtTime(2., 7., ViewSpec::all(), 0);
    const RenderContextBinding keyAdded = RenderContextBinding::create(offset, "v", captured);
    EXPECT_NE(keyEdited.identity, keyAdded.identity);

    ASSERT_NO_THROW(offset->setExpression(0, "frame * 2", false, true));
    const RenderContextBinding expression = RenderContextBinding::create(offset, "v", captured);
    EXPECT_NE(keyAdded.identity, expression.identity);
    ASSERT_NO_THROW(offset->setExpression(0, "frame * 3", false, true));
    const RenderContextBinding expressionEdited = RenderContextBinding::create(offset, "v", captured);
    EXPECT_NE(expression.identity, expressionEdited.identity);
    EXPECT_NE(withFirst->hash(), RenderContext::Builder().set(plainName("v"), expressionEdited).build()->hash());

    // Restriction keeps a binding as it is.
    const RenderContextPtr both = RenderContext::Builder(withFirst).set(plainName("other"), 3.).build();
    const RenderContextPtr kept = both->restrictTo(readsOf({ plainName("v") }));
    ASSERT_EQ(1u, kept->size());
    const RenderContextBinding* keptBinding = std::get_if<RenderContextBinding>(kept->find(plainName("v")));
    ASSERT_TRUE(keptBinding != 0);
    EXPECT_EQ(first.identity, keptBinding->identity);
    EXPECT_EQ(offset.get(), keptBinding->knob.get());
    EXPECT_EQ(captured.get(), keptBinding->captured.get());
    EXPECT_EQ(withFirst->hash(), kept->hash());
}

// A Set's constant variable is stored as its value; an animated one or one with an expression as a
// binding capturing what the knob reads of the incoming context, the same at every request time.
TEST_F(ContextReadSetGraph, SetStoresConstantsAsValuesAndTimeVaryingAsBindings)
{
    NodePtr set = createNode(QString::fromUtf8(PLUGINID_NATRON_REVERSEFLOWVARIABLESET));
    ASSERT_TRUE(bool(set));
    EffectInstancePtr effect = set->getEffectInstance();
    KnobGroupPtr variables = std::dynamic_pointer_cast<KnobGroup>(set->getKnobByName(kReverseFlowVariableSetParamVariables));
    ASSERT_TRUE(effect && variables);
    KnobDoublePtr constant = effect->createDoubleKnob("constant", "constant", 1);
    KnobDoublePtr animated = effect->createDoubleKnob("animated", "animated", 1);
    KnobDoublePtr expression = effect->createDoubleKnob("expression", "expression", 1);
    ASSERT_TRUE(variables->addKnob(constant));
    ASSERT_TRUE(variables->addKnob(animated));
    ASSERT_TRUE(variables->addKnob(expression));
    effect->recreateUserKnobs(false);
    constant->setValue(4.);
    animated->setValueAtTime(1., 10., ViewSpec::all(), 0);
    animated->setValueAtTime(2., 20., ViewSpec::all(), 0);
    ASSERT_NO_THROW(expression->setExpression(0, "rfv.outer or 0", false, true));
    EXPECT_FALSE(RFV::isTimeVarying(constant));
    EXPECT_TRUE(RFV::isTimeVarying(animated));
    EXPECT_TRUE(RFV::isTimeVarying(expression));

    const RenderContextPtr incoming = RenderContext::Builder().set(plainName("outer"), 1.).set(plainName("unread"), 2.).build();
    const RenderContextPtr atOne = RFV::overlayDeclaredVariables(effect.get(), 1., ViewIdx(0), incoming);
    const RenderContextPtr atTwo = RFV::overlayDeclaredVariables(effect.get(), 2., ViewIdx(0), incoming);

    const RenderContextValue* constantValue = atOne->find(plainName("constant"));
    ASSERT_TRUE(constantValue != 0);
    ASSERT_TRUE(std::holds_alternative<double>(*constantValue));
    EXPECT_EQ(4., std::get<double>(*constantValue));

    const RenderContextBinding* animatedBinding = std::get_if<RenderContextBinding>(atOne->find(plainName("animated")));
    ASSERT_TRUE(animatedBinding != 0);
    EXPECT_EQ(animated.get(), animatedBinding->knob.get());
    EXPECT_TRUE(animatedBinding->captured->isEmpty());

    const RenderContextBinding* expressionBinding = std::get_if<RenderContextBinding>(atOne->find(plainName("expression")));
    ASSERT_TRUE(expressionBinding != 0);
    ASSERT_EQ(1u, expressionBinding->captured->size());
    EXPECT_TRUE(expressionBinding->captured->find(plainName("outer")) != 0);

    // Bindings do not depend on the request time, so the overlay keys the same at every frame.
    EXPECT_EQ(atOne->hash(), atTwo->hash());

    // A constant whose value changes is still a value, and gets a new key.
    constant->setValue(5.);
    const RenderContextPtr edited = RFV::overlayDeclaredVariables(effect.get(), 1., ViewIdx(0), incoming);
    EXPECT_NE(atOne->hash(), edited->hash());
    EXPECT_TRUE(std::holds_alternative<double>(*edited->find(plainName("constant"))));
}
