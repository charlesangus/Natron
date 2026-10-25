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

#include <memory>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include <QtCore/QObject>

#include "BaseTest.h"

#include "Engine/AppManager.h"
#include "Engine/Curve.h"
#include "Engine/EffectInstance.h"
#include "Engine/Knob.h"
#include "Engine/KnobGuiI.h"
#include "Engine/KnobLinkGroup.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/RectD.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

KnobDoublePtr
makeDoubleKnob(const NodePtr& node,
               const std::string& name,
               int dimension = 1)
{
    KnobDoublePtr knob = AppManager::createKnob<KnobDouble>(node->getEffectInstance().get(), name, dimension, false);
    // An expression resolves thisParam as a Python attribute of the node, which a knob added after
    // node creation lacks until the node redeclares its fields.
    node->declarePythonFields();

    return knob;
}

/**
 * @brief A holder whose knobs can be made to queue their writes, as a node's do while it renders.
 **/
class RenderBusyHolder
    : public KnobHolder {
public:
    explicit RenderBusyHolder(const AppInstancePtr& app)
        : KnobHolder(app)
        , busy(false)
    {
    }

    bool busy;

protected:
    virtual bool canSetValue() const OVERRIDE
    {
        return !busy;
    }

    virtual void initializeKnobs() OVERRIDE
    {
    }
};

/**
 * @brief The engine side of a knob's GUI: one GUI curve per dimension, as a panel provides.
 **/
class CurveOnlyKnobGui
    : public KnobGuiI {
public:
    explicit CurveOnlyKnobGui(const KnobIPtr& knob)
    {
        for (int i = 0; i < knob->getDimension(); ++i) {
            CurvePtr curve = std::make_shared<Curve>(knob.get(), i);
            CurvePtr internal = knob->getCurve(ViewIdx(0), i, true);
            if (internal) {
                curve->clone(*internal);
            }
            _curves.push_back(curve);
        }
    }

    virtual void swapOpenGLBuffers() OVERRIDE { }
    virtual void redraw() OVERRIDE { }
    virtual void getViewportSize(double& width, double& height) const OVERRIDE { width = height = 0.; }
    virtual void getPixelScale(double& xScale, double& yScale) const OVERRIDE { xScale = yScale = 1.; }
    virtual double getScreenPixelRatio() const OVERRIDE { return 1.; }
    virtual void getBackgroundColour(double& r, double& g, double& b) const OVERRIDE { r = g = b = 0.; }
    virtual bool isGuiFrozenForPlayback() const OVERRIDE { return false; }
    virtual void saveOpenGLContext() OVERRIDE { }
    virtual void restoreOpenGLContext() OVERRIDE { }
    virtual CurvePtr getCurve(ViewSpec /*view*/, int dimension) const OVERRIDE
    {
        return (dimension >= 0 && dimension < (int)_curves.size()) ? _curves[dimension] : CurvePtr();
    }
    virtual bool getAllDimensionsVisible() const OVERRIDE { return true; }
    virtual RectD getViewportRect() const OVERRIDE { return RectD(); }
    virtual void getCursorPosition(double& x, double& y) const OVERRIDE { x = y = 0.; }
    virtual void toWidgetCoordinates(double* /*x*/, double* /*y*/) const OVERRIDE { }
    virtual void toCanonicalCoordinates(double* /*x*/, double* /*y*/) const OVERRIDE { }
    virtual int getWidgetFontHeight() const OVERRIDE { return 10; }
    virtual int getStringWidthForCurrentFont(const std::string& string) const OVERRIDE { return (int)string.size(); }

protected:
    virtual void setKnobGuiPointer() OVERRIDE { }

private:
    std::vector<CurvePtr> _curves;
};

} // namespace

class KnobLinkGroupTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        nodeA = createNode(_generatorPluginID);
        nodeB = createNode(_generatorPluginID);
        nodeC = createNode(_generatorPluginID);
        ASSERT_TRUE(nodeA && nodeB && nodeC);
        a = makeDoubleKnob(nodeA, "linkA");
        b = makeDoubleKnob(nodeB, "linkB");
        c = makeDoubleKnob(nodeC, "linkC");
        ASSERT_TRUE(a && b && c);
    }

    void linkThree()
    {
        ASSERT_TRUE(b->linkTo(-1, a, -1));
        ASSERT_TRUE(c->linkTo(-1, b, -1));
    }

    NodePtr nodeA, nodeB, nodeC;
    KnobDoublePtr a, b, c;
};

TEST_F(KnobLinkGroupTest, LinkingAdoptsTheOtherKnobsValue)
{
    a->setValue(4.);
    b->setValue(1.);
    ASSERT_TRUE(b->linkTo(-1, a, -1));
    EXPECT_DOUBLE_EQ(4., b->getValue(0));
    EXPECT_DOUBLE_EQ(4., a->getValue(0));
    EXPECT_TRUE(a->isLinked(0));
    EXPECT_TRUE(b->isLinked(0));
    EXPECT_EQ(a->getLinkGroup(0), b->getLinkGroup(0));
    ASSERT_EQ(1u, a->getLinkedKnobs().size());
    EXPECT_EQ(KnobIPtr(b), a->getLinkedKnobs()[0].knob);
}

TEST_F(KnobLinkGroupTest, ValueSetOnAnyMemberReadsBackOnAll)
{
    linkThree();
    a->setValue(2.);
    EXPECT_DOUBLE_EQ(2., b->getValue(0));
    EXPECT_DOUBLE_EQ(2., c->getValue(0));

    c->setValue(7.);
    EXPECT_DOUBLE_EQ(7., a->getValue(0));
    EXPECT_DOUBLE_EQ(7., b->getValue(0));

    b->setValue(-3.);
    EXPECT_DOUBLE_EQ(-3., a->getValue(0));
    EXPECT_DOUBLE_EQ(-3., c->getValue(0));
}

TEST_F(KnobLinkGroupTest, KeyframeAddedOnOneAppearsOnAll)
{
    linkThree();
    b->setValueAtTime(10, 5., ViewSpec::all(), 0);
    EXPECT_EQ(1, a->getKeyFramesCount(ViewIdx(0), 0));
    EXPECT_EQ(1, c->getKeyFramesCount(ViewIdx(0), 0));
    EXPECT_DOUBLE_EQ(5., a->getValueAtTime(10, 0));
    EXPECT_DOUBLE_EQ(5., c->getValueAtTime(10, 0));

    c->setValueAtTime(20, 8., ViewSpec::all(), 0);
    EXPECT_EQ(2, a->getKeyFramesCount(ViewIdx(0), 0));
    EXPECT_EQ(2, b->getKeyFramesCount(ViewIdx(0), 0));
}

TEST_F(KnobLinkGroupTest, ExpressionSetOnOneEvaluatesOnAll)
{
    linkThree();
    ASSERT_NO_THROW(a->setExpression(0, "1.5 * 2", false, true));
    EXPECT_EQ(std::string("1.5 * 2"), b->getExpression(0));
    EXPECT_EQ(std::string("1.5 * 2"), c->getExpression(0));
    EXPECT_DOUBLE_EQ(3., b->getValue(0));
    EXPECT_DOUBLE_EQ(3., c->getValue(0));

    b->clearExpression(0, true);
    EXPECT_TRUE(a->getExpression(0).empty());
    EXPECT_TRUE(c->getExpression(0).empty());
}

TEST_F(KnobLinkGroupTest, CrossDimensionLinkPropagatesBothWays)
{
    KnobDoublePtr a2 = makeDoubleKnob(nodeA, "linkA2", 2);
    ASSERT_NE(nullptr, a2);
    a2->setValue(1., ViewSpec::all(), 0);
    a2->setValue(6., ViewSpec::all(), 1);
    ASSERT_TRUE(b->linkTo(0, a2, 1));
    EXPECT_DOUBLE_EQ(6., b->getValue(0));
    EXPECT_TRUE(a2->isLinked(1));
    EXPECT_FALSE(a2->isLinked(0));

    a2->setValue(9., ViewSpec::all(), 1);
    EXPECT_DOUBLE_EQ(9., b->getValue(0));
    b->setValue(11.);
    EXPECT_DOUBLE_EQ(11., a2->getValue(1));
    EXPECT_DOUBLE_EQ(1., a2->getValue(0));
}

TEST_F(KnobLinkGroupTest, UnlinkKeepsTheValueAndStopsPropagation)
{
    linkThree();
    a->setValue(5.);
    ASSERT_TRUE(b->unlinkFromGroups());
    EXPECT_FALSE(b->isLinked(0));
    EXPECT_TRUE(b->hasRelinkableGroups());
    EXPECT_DOUBLE_EQ(5., b->getValue(0));

    a->setValue(6.);
    EXPECT_DOUBLE_EQ(5., b->getValue(0));
    EXPECT_DOUBLE_EQ(6., c->getValue(0));

    b->setValue(1.);
    EXPECT_DOUBLE_EQ(6., a->getValue(0));
    EXPECT_DOUBLE_EQ(6., c->getValue(0));
}

TEST_F(KnobLinkGroupTest, RelinkAdoptsTheGroupsValue)
{
    linkThree();
    a->setValue(5.);
    ASSERT_TRUE(b->unlinkFromGroups());
    b->setValue(1.);
    c->setValue(8.);

    ASSERT_TRUE(b->relinkToGroups());
    EXPECT_TRUE(b->isLinked(0));
    EXPECT_FALSE(b->hasRelinkableGroups());
    EXPECT_DOUBLE_EQ(8., b->getValue(0));

    b->setValue(2.);
    EXPECT_DOUBLE_EQ(2., a->getValue(0));
}

TEST_F(KnobLinkGroupTest, AliasLinksAreTwoWayAndCannotBeUnlinked)
{
    a->setValue(3.);
    ASSERT_TRUE(b->linkAsAlias(a));
    EXPECT_TRUE(b->isLinkPermanent(0));
    EXPECT_TRUE(a->isLinkPermanent(0));
    EXPECT_DOUBLE_EQ(3., b->getValue(0));

    b->setValue(4.);
    EXPECT_DOUBLE_EQ(4., a->getValue(0));
    a->setValue(5.);
    EXPECT_DOUBLE_EQ(5., b->getValue(0));

    EXPECT_FALSE(b->unlinkFromGroups());
    EXPECT_FALSE(a->unlinkFromGroups());
    EXPECT_TRUE(b->isLinked(0));
}

TEST_F(KnobLinkGroupTest, PasteLinkFromAnAliasKnobIsRefused)
{
    KnobDoublePtr c = makeDoubleKnob(nodeB, "linkTarget", 1);
    ASSERT_NE(nullptr, c);
    a->setValue(3.);
    c->setValue(9.);
    ASSERT_TRUE(b->linkAsAlias(a));
    EXPECT_TRUE(KnobI::isAliasKnob(a.get()));
    EXPECT_TRUE(KnobI::isAliasKnob(b.get()));
    EXPECT_FALSE(KnobI::isAliasKnob(c.get()));

    EXPECT_FALSE(c->pasteFrom(a, eKnobClipBoardTypeCopyLink, -1, -1));
    EXPECT_FALSE(c->pasteFrom(b, eKnobClipBoardTypeCopyLink, -1, -1));
    EXPECT_FALSE(c->isLinked(0));
    EXPECT_DOUBLE_EQ(9., c->getValue(0));

    a->setValue(5.);
    EXPECT_DOUBLE_EQ(9., c->getValue(0));
    c->setValue(1.);
    EXPECT_DOUBLE_EQ(5., a->getValue(0));
}

TEST_F(KnobLinkGroupTest, DestroyingOneOfThreeLeavesTheOthersLinked)
{
    linkThree();
    KnobLinkGroupPtr group = a->getLinkGroup(0);
    ASSERT_NE(nullptr, group);
    nodeC->getEffectInstance()->deleteKnob(c.get(), false);
    c.reset();
    EXPECT_EQ(2, group->getLiveMembersCount());

    a->setValue(12.);
    EXPECT_DOUBLE_EQ(12., b->getValue(0));
    b->setValue(13.);
    EXPECT_DOUBLE_EQ(13., a->getValue(0));
}

TEST_F(KnobLinkGroupTest, TwoMemberGroupDissolvesWhenOneIsDestroyed)
{
    ASSERT_TRUE(b->linkTo(-1, a, -1));
    ASSERT_TRUE(b->unlinkFromGroups());
    nodeA->getEffectInstance()->deleteKnob(a.get(), false);
    a.reset();
    EXPECT_EQ(nullptr, b->getLinkGroup(0));
    EXPECT_FALSE(b->isLinked(0));
    EXPECT_FALSE(b->hasRelinkableGroups());

    {
        KnobDoublePtr holderless = AppManager::createKnob<KnobDouble>((KnobHolder*)NULL, std::string("holderless"), 1, false);
        ASSERT_TRUE(holderless->linkTo(-1, c, -1));
        EXPECT_NE(nullptr, c->getLinkGroup(0));
    }
    EXPECT_EQ(nullptr, c->getLinkGroup(0));
}

TEST_F(KnobLinkGroupTest, IncompatibleTypesAreRefused)
{
    KnobStringPtr s = AppManager::createKnob<KnobString>(nodeB->getEffectInstance().get(), std::string("linkString"), 1, false);
    ASSERT_NE(nullptr, s);
    EXPECT_FALSE(s->linkTo(-1, a, -1));
    EXPECT_FALSE(a->linkTo(-1, s, -1));
    EXPECT_EQ(nullptr, a->getLinkGroup(0));
    EXPECT_EQ(nullptr, s->getLinkGroup(0));
}

TEST_F(KnobLinkGroupTest, MemberOnARenderingHolderGetsChangesThroughItsQueue)
{
    std::shared_ptr<RenderBusyHolder> holder = std::make_shared<RenderBusyHolder>(getApp());
    KnobDoublePtr member = AppManager::createKnob<KnobDouble>(holder.get(), std::string("linkBusy"), 1, false);
    ASSERT_NE(nullptr, member);
    std::shared_ptr<CurveOnlyKnobGui> gui = std::make_shared<CurveOnlyKnobGui>(member);
    member->setKnobGuiPointer(gui);

    a->setValue(1.);
    ASSERT_TRUE(member->linkTo(-1, a, -1));
    EXPECT_DOUBLE_EQ(1., member->getRawValue(0));

    holder->busy = true;
    a->setValue(4.);
    // The render keeps the value it started with, and the GUI already shows the new one.
    EXPECT_DOUBLE_EQ(1., member->getRawValue(0));
    EXPECT_DOUBLE_EQ(4., member->getValue(0));

    a->setValueAtTime(10, 6., ViewSpec::all(), 0);
    EXPECT_EQ(0, member->getCurve(ViewIdx(0), 0, true)->getKeyFramesCount());
    EXPECT_EQ(1, gui->getCurve(ViewIdx(0), 0)->getKeyFramesCount());

    holder->busy = false;
    holder->dequeueValuesSet();
    EXPECT_DOUBLE_EQ(4., member->getRawValue(0));
    ASSERT_EQ(1, member->getCurve(ViewIdx(0), 0, true)->getKeyFramesCount());
    EXPECT_DOUBLE_EQ(6., member->getValueAtTime(10, 0));

    // Applying the queued writes leaves the other members as they were.
    EXPECT_EQ(1, a->getKeyFramesCount(ViewIdx(0), 0));
    EXPECT_DOUBLE_EQ(6., a->getValueAtTime(10, 0));

    // Once the render is over, edits on the member reach the others again.
    member->setValueAtTime(20, 2., ViewSpec::all(), 0);
    EXPECT_EQ(2, a->getKeyFramesCount(ViewIdx(0), 0));
}

TEST_F(KnobLinkGroupTest, ApplyingQueuedLinkWritesDoesNotPushThemBack)
{
    std::shared_ptr<RenderBusyHolder> holder = std::make_shared<RenderBusyHolder>(getApp());
    // The double member comes first, so its queue is applied before the integer member's.
    KnobDoublePtr busyDouble = AppManager::createKnob<KnobDouble>(holder.get(), std::string("linkBusyDouble"), 1, false);
    KnobIntPtr busyInt = AppManager::createKnob<KnobInt>(holder.get(), std::string("linkBusyInt"), 1, false);
    ASSERT_TRUE(busyDouble && busyInt);
    std::shared_ptr<CurveOnlyKnobGui> doubleGui = std::make_shared<CurveOnlyKnobGui>(busyDouble);
    std::shared_ptr<CurveOnlyKnobGui> intGui = std::make_shared<CurveOnlyKnobGui>(busyInt);
    busyDouble->setKnobGuiPointer(doubleGui);
    busyInt->setKnobGuiPointer(intGui);
    ASSERT_TRUE(busyDouble->linkTo(-1, a, -1));
    ASSERT_TRUE(busyInt->linkTo(-1, a, -1));
    KnobLinkGroupPtr group = a->getLinkGroup(0);
    ASSERT_NE(nullptr, group);

    holder->busy = true;
    a->setValueAtTime(10, 2.4, ViewSpec::all(), 0);
    ASSERT_EQ(1, doubleGui->getCurve(ViewIdx(0), 0)->getKeyFramesCount());
    ASSERT_EQ(1, intGui->getCurve(ViewIdx(0), 0)->getKeyFramesCount());
    KeyFrame stateKey;
    ASSERT_TRUE(group->getStateKnob()->getCurve(ViewIdx(0), 0, true)->getKeyFrameWithTime(10, &stateKey));
    EXPECT_DOUBLE_EQ(2.4, stateKey.getValue());

    holder->busy = false;
    holder->dequeueValuesSet();
    // Pushed back, the integer member's rounded key would replace the group's.
    ASSERT_TRUE(group->getStateKnob()->getCurve(ViewIdx(0), 0, true)->getKeyFrameWithTime(10, &stateKey));
    EXPECT_DOUBLE_EQ(2.4, stateKey.getValue());
    EXPECT_DOUBLE_EQ(2.4, a->getValueAtTime(10, 0));
    ASSERT_EQ(1, busyDouble->getCurve(ViewIdx(0), 0, true)->getKeyFramesCount());
    EXPECT_DOUBLE_EQ(2.4, busyDouble->getValueAtTime(10, 0));
    ASSERT_EQ(1, busyInt->getCurve(ViewIdx(0), 0, true)->getKeyFramesCount());
    EXPECT_EQ(2, busyInt->getValueAtTime(10, 0));
}

TEST_F(KnobLinkGroupTest, EditDuringRenderAfterALinkWriteIsPushedWhenApplied)
{
    std::shared_ptr<RenderBusyHolder> holder = std::make_shared<RenderBusyHolder>(getApp());
    KnobDoublePtr member = AppManager::createKnob<KnobDouble>(holder.get(), std::string("linkBusyEdited"), 1, false);
    ASSERT_NE(nullptr, member);
    std::shared_ptr<CurveOnlyKnobGui> gui = std::make_shared<CurveOnlyKnobGui>(member);
    member->setKnobGuiPointer(gui);
    ASSERT_TRUE(member->linkTo(-1, a, -1));
    a->setValueAtTime(10, 1., ViewSpec::all(), 0);
    a->setValueAtTime(20, 2., ViewSpec::all(), 0);
    ASSERT_EQ(2, member->getCurve(ViewIdx(0), 0, true)->getKeyFramesCount());

    holder->busy = true;
    a->setValueAtTime(30, 3., ViewSpec::all(), 0);
    ASSERT_EQ(3, gui->getCurve(ViewIdx(0), 0)->getKeyFramesCount());
    KnobIPtr memberKnob = member;
    memberKnob->deleteValueAtTime(eCurveChangeReasonInternal, 20, ViewSpec::all(), 0, false);
    EXPECT_EQ(2, gui->getCurve(ViewIdx(0), 0)->getKeyFramesCount());
    EXPECT_EQ(3, a->getKeyFramesCount(ViewIdx(0), 0));

    holder->busy = false;
    holder->dequeueValuesSet();
    EXPECT_EQ(2, member->getCurve(ViewIdx(0), 0, true)->getKeyFramesCount());
    EXPECT_EQ(2, a->getKeyFramesCount(ViewIdx(0), 0));
    KeyFrame k;
    EXPECT_FALSE(a->getCurve(ViewIdx(0), 0, true)->getKeyFrameWithTime(20, &k));
    EXPECT_TRUE(a->getCurve(ViewIdx(0), 0, true)->getKeyFrameWithTime(30, &k));
}

TEST_F(KnobLinkGroupTest, MembersOfAnotherTypeGetConvertedValues)
{
    KnobIntPtr i = AppManager::createKnob<KnobInt>(nodeB->getEffectInstance().get(), std::string("linkInt"), 1, false);
    ASSERT_NE(nullptr, i);
    ASSERT_TRUE(i->linkTo(-1, a, -1));

    a->setValue(2.6);
    EXPECT_EQ(3, i->getValue(0));

    a->setValueAtTime(10, 4.4, ViewSpec::all(), 0);
    ASSERT_EQ(1, i->getKeyFramesCount(ViewIdx(0), 0));
    EXPECT_EQ(4, i->getValueAtTime(10, 0));

    i->setValue(7);
    EXPECT_DOUBLE_EQ(7., a->getValue(0));
}

TEST_F(KnobLinkGroupTest, CloningFromAKnobWithoutCurveKeepsTheGuiCurve)
{
    KnobBoolPtr flag = AppManager::createKnob<KnobBool>(nodeA->getEffectInstance().get(), std::string("linkFlag"), 1, false);
    KnobButtonPtr press = AppManager::createKnob<KnobButton>(nodeB->getEffectInstance().get(), std::string("linkPress"), 1, false);
    ASSERT_TRUE(flag && press);
    ASSERT_EQ(nullptr, press->getCurve(ViewIdx(0), 0, true));

    std::shared_ptr<CurveOnlyKnobGui> gui = std::make_shared<CurveOnlyKnobGui>(flag);
    flag->setKnobGuiPointer(gui);
    flag->setValueAtTime(5, true, ViewSpec::all(), 0);
    ASSERT_EQ(1, gui->getCurve(ViewIdx(0), 0)->getKeyFramesCount());

    flag->cloneAndUpdateGui(press.get());
    EXPECT_EQ(1, flag->getCurve(ViewIdx(0), 0, true)->getKeyFramesCount());
    EXPECT_EQ(1, gui->getCurve(ViewIdx(0), 0)->getKeyFramesCount());
}

TEST_F(KnobLinkGroupTest, GroupAliasIsTwoWayPermanentAndEndsWithTheGroupKnob)
{
    // a plays the Group's user knob and b the inner knob made an alias of it.
    a->setValue(3.);
    b->setValue(1.);
    ASSERT_TRUE(b->setKnobAsAliasOfThis(a, true));
    EXPECT_EQ(KnobIPtr(a), b->getAliasMaster());
    ASSERT_EQ(1u, a->getAliasesOfThis().size());
    EXPECT_EQ(KnobIPtr(b), a->getAliasesOfThis()[0]);
    EXPECT_TRUE(b->isLinkPermanent(0));
    EXPECT_DOUBLE_EQ(3., b->getValue(0));

    b->setValue(4.);
    EXPECT_DOUBLE_EQ(4., a->getValue(0));
    a->setValue(5.);
    EXPECT_DOUBLE_EQ(5., b->getValue(0));

    EXPECT_FALSE(b->unlinkFromGroups());
    EXPECT_FALSE(a->unlinkFromGroups());
    EXPECT_TRUE(b->isLinked(0));

    nodeA->getEffectInstance()->deleteKnob(a.get(), false);
    a.reset();
    EXPECT_EQ(nullptr, b->getAliasMaster());
    EXPECT_FALSE(b->isLinked(0));
    EXPECT_EQ(nullptr, b->getLinkGroup(0));
    EXPECT_DOUBLE_EQ(5., b->getValue(0));
    b->setValue(6.);
    EXPECT_DOUBLE_EQ(6., b->getValue(0));
}

TEST_F(KnobLinkGroupTest, SingleValueLinksToEveryComponentOfAFoldedKnob)
{
    KnobDoublePtr size = makeDoubleKnob(nodeB, "linkSize", 2);
    ASSERT_NE(nullptr, size);
    a->setValue(4.);
    size->setValue(1., ViewSpec::all(), 0);
    size->setValue(2., ViewSpec::all(), 1);

    // A folded knob pastes on all dimensions.
    ASSERT_TRUE(size->pasteFrom(a, eKnobClipBoardTypeCopyLink, -1, -1));
    EXPECT_DOUBLE_EQ(4., size->getValue(0));
    EXPECT_DOUBLE_EQ(4., size->getValue(1));

    a->setValue(6.);
    EXPECT_DOUBLE_EQ(6., size->getValue(0));
    EXPECT_DOUBLE_EQ(6., size->getValue(1));

    // Unfolded, each component is still linked to the source on its own.
    EXPECT_TRUE(size->isLinked(0));
    EXPECT_TRUE(size->isLinked(1));
    size->setValue(7., ViewSpec::all(), 1);
    EXPECT_DOUBLE_EQ(7., a->getValue(0));
    EXPECT_DOUBLE_EQ(7., size->getValue(0));

    ASSERT_TRUE(size->unlinkFromGroups());
    EXPECT_DOUBLE_EQ(7., size->getValue(0));
    EXPECT_DOUBLE_EQ(7., size->getValue(1));
    a->setValue(8.);
    EXPECT_DOUBLE_EQ(7., size->getValue(0));
    EXPECT_DOUBLE_EQ(7., size->getValue(1));
}

TEST_F(KnobLinkGroupTest, PasteSingleValueOntoOneComponent)
{
    KnobDoublePtr v3 = makeDoubleKnob(nodeB, "linkV3", 3);
    ASSERT_NE(nullptr, v3);
    a->setValue(5.);
    for (int i = 0; i < 3; ++i) {
        v3->setValue(i + 1., ViewSpec::all(), i);
    }
    ASSERT_TRUE(v3->pasteFrom(a, eKnobClipBoardTypeCopyValue, -1, 1));
    EXPECT_DOUBLE_EQ(1., v3->getValue(0));
    EXPECT_DOUBLE_EQ(5., v3->getValue(1));
    EXPECT_DOUBLE_EQ(3., v3->getValue(2));
}

TEST_F(KnobLinkGroupTest, PasteSingleValueOntoAllComponents)
{
    KnobDoublePtr v3 = makeDoubleKnob(nodeB, "linkV3", 3);
    ASSERT_NE(nullptr, v3);
    a->setValue(5.);
    ASSERT_TRUE(v3->pasteFrom(a, eKnobClipBoardTypeCopyValue, -1, -1));
    for (int i = 0; i < 3; ++i) {
        EXPECT_DOUBLE_EQ(5., v3->getValue(i));
    }
}

TEST_F(KnobLinkGroupTest, PasteMultiValueOntoSingleValuePastesTheFirstComponent)
{
    KnobDoublePtr v3 = makeDoubleKnob(nodeA, "linkV3", 3);
    ASSERT_NE(nullptr, v3);
    for (int i = 0; i < 3; ++i) {
        v3->setValue(i + 2., ViewSpec::all(), i);
    }
    b->setValue(0.);
    ASSERT_TRUE(b->pasteFrom(v3, eKnobClipBoardTypeCopyValue, -1, -1));
    EXPECT_DOUBLE_EQ(2., b->getValue(0));
}

TEST_F(KnobLinkGroupTest, PasteBetweenDifferentCountsPastesTheSharedComponents)
{
    KnobDoublePtr v2 = makeDoubleKnob(nodeA, "linkV2", 2);
    KnobDoublePtr v3 = makeDoubleKnob(nodeB, "linkV3", 3);
    ASSERT_TRUE(v2 && v3);
    v2->setValue(10., ViewSpec::all(), 0);
    v2->setValue(20., ViewSpec::all(), 1);
    for (int i = 0; i < 3; ++i) {
        v3->setValue(i + 1., ViewSpec::all(), i);
    }
    ASSERT_TRUE(v3->pasteFrom(v2, eKnobClipBoardTypeCopyValue, -1, -1));
    EXPECT_DOUBLE_EQ(10., v3->getValue(0));
    EXPECT_DOUBLE_EQ(20., v3->getValue(1));
    EXPECT_DOUBLE_EQ(3., v3->getValue(2));
}

TEST_F(KnobLinkGroupTest, AnotherThreadsSyncIsDeferredToTheSyncingThread)
{
    linkThree();
    KnobLinkGroupPtr group = a->getLinkGroup(0);
    ASSERT_NE(nullptr, group);

    KnobLinkGroup::SyncRequest request;
    request.knob = b;
    request.dimension = 0;
    request.time = 0.;
    request.view = ViewSpec::all();
    request.reason = eValueChangedReasonUserEdited;

    ASSERT_EQ(KnobLinkGroup::eSyncStateAcquired, group->beginSync(NULL));
    EXPECT_EQ(KnobLinkGroup::eSyncStateReentrant, group->beginSync(&request));
    KnobLinkGroup::SyncStateEnum other = KnobLinkGroup::eSyncStateAcquired;
    std::thread thread([&]() {
        other = group->beginSync(&request);
    });
    thread.join();
    EXPECT_EQ(KnobLinkGroup::eSyncStateDeferred, other);

    KnobLinkGroup::SyncRequest next;
    ASSERT_TRUE(group->endSync(&next));
    EXPECT_EQ(KnobIPtr(b), next.knob.lock());
    EXPECT_EQ(0, next.dimension);
    EXPECT_FALSE(group->endSync(&next));

    // Released: the next thread to sync owns the group.
    std::thread after([&]() {
        other = group->beginSync(NULL);
        if (other == KnobLinkGroup::eSyncStateAcquired) {
            KnobLinkGroup::SyncRequest none;
            EXPECT_FALSE(group->endSync(&none));
        }
    });
    after.join();
    EXPECT_EQ(KnobLinkGroup::eSyncStateAcquired, other);
}

TEST_F(KnobLinkGroupTest, AnEditFromAnotherThreadDuringASyncReachesEveryMember)
{
    linkThree();
    KnobLinkGroupPtr group = a->getLinkGroup(0);
    ASSERT_NE(nullptr, group);

    // While a's edit is copied to the others, b is edited and another thread asks for b's state to be
    // copied, as b's own push does when it runs on that thread.
    bool fired = false;
    QObject::connect(c->getSignalSlotHandler().get(), &KnobSignalSlotHandler::valueChanged, [&](ViewSpec, int, int) {
        if (fired) {
            return;
        }
        fired = true;
        b->setValue(9.);
        KnobLinkGroup::SyncRequest request;
        request.knob = b;
        request.dimension = 0;
        request.time = 0.;
        request.view = ViewSpec::all();
        request.reason = eValueChangedReasonUserEdited;
        KnobLinkGroup::SyncStateEnum state = KnobLinkGroup::eSyncStateAcquired;
        std::thread thread([&]() {
            state = group->beginSync(&request);
            if (state == KnobLinkGroup::eSyncStateAcquired) {
                // The slot ran after the sync: release what this thread took.
                KnobLinkGroup::SyncRequest none;
                group->endSync(&none);
            }
        });
        thread.join();
        EXPECT_EQ(KnobLinkGroup::eSyncStateDeferred, state);
    });

    a->setValue(5.);
    ASSERT_TRUE(fired);
    EXPECT_DOUBLE_EQ(9., a->getValue(0));
    EXPECT_DOUBLE_EQ(9., b->getValue(0));
    EXPECT_DOUBLE_EQ(9., c->getValue(0));
}

TEST_F(KnobLinkGroupTest, RemovingAnAliasFromAPasteLinkGroupLetsItsMembersUnlinkAgain)
{
    // c is Paste-Linked to a, then b, the inner knob of a Group, becomes an alias of a.
    a->setValue(2.);
    ASSERT_TRUE(c->linkTo(-1, a, -1));
    EXPECT_FALSE(c->isLinkPermanent(0));
    ASSERT_TRUE(b->setKnobAsAliasOfThis(a, true));
    EXPECT_EQ(a->getLinkGroup(0), b->getLinkGroup(0));
    EXPECT_TRUE(c->isLinkPermanent(0));
    b->setValue(3.);
    EXPECT_DOUBLE_EQ(3., c->getValue(0));

    ASSERT_TRUE(b->setKnobAsAliasOfThis(a, false));
    EXPECT_EQ(nullptr, b->getLinkGroup(0));
    EXPECT_DOUBLE_EQ(3., b->getValue(0));
    ASSERT_TRUE(c->isLinked(0));
    EXPECT_FALSE(c->isLinkPermanent(0));
    EXPECT_FALSE(a->getLinkGroup(0)->isPermanent());
    EXPECT_DOUBLE_EQ(3., a->getValue(0));
    EXPECT_DOUBLE_EQ(3., c->getValue(0));
    a->setValue(4.);
    EXPECT_DOUBLE_EQ(4., c->getValue(0));
    EXPECT_DOUBLE_EQ(3., b->getValue(0));

    EXPECT_TRUE(c->unlinkFromGroups());
    a->setValue(5.);
    EXPECT_DOUBLE_EQ(4., c->getValue(0));
}

TEST_F(KnobLinkGroupTest, RemovingAnAliasByDeletingItLetsTheOthersUnlinkAgain)
{
    ASSERT_TRUE(c->linkTo(-1, a, -1));
    ASSERT_TRUE(b->setKnobAsAliasOfThis(a, true));
    EXPECT_TRUE(c->isLinkPermanent(0));
    a->setValue(6.);

    nodeB->getEffectInstance()->deleteKnob(b.get(), false);
    b.reset();
    EXPECT_FALSE(c->isLinkPermanent(0));
    EXPECT_DOUBLE_EQ(6., c->getValue(0));
    EXPECT_TRUE(c->unlinkFromGroups());
}

TEST_F(KnobLinkGroupTest, DeletingALinkedKnobNotifiesEveryLinkedNode)
{
    linkThree();
    int changedA = 0;
    int changedB = 0;
    int changedC = 0;
    QObject::connect(nodeA.get(), &Node::linksChanged, [&]() { ++changedA; });
    QObject::connect(nodeB.get(), &Node::linksChanged, [&]() { ++changedB; });
    QObject::connect(nodeC.get(), &Node::linksChanged, [&]() { ++changedC; });

    nodeB->getEffectInstance()->deleteKnob(b.get(), false);
    b.reset();
    EXPECT_GE(changedA, 1);
    EXPECT_GE(changedB, 1);
    EXPECT_GE(changedC, 1);
}
