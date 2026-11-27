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

#include <future>
#include <list>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "BaseTest.h"
#include "CacheStress.h"
#include "NativeParity.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Project.h"

NATRON_NAMESPACE_USING

namespace {

using namespace CacheStress;

const RectI kWindow(0, 0, 16, 16);

// The reader is created first, so a reset frees it before the driver whose listeners name its knobs.
struct ReaderOverDriver {
    NodePtr reader;
    NodePtr driver;
    KnobIPtr readerWhite;
    KnobIPtr readerGamma;
    KnobIPtr driverWhite;
    KnobIPtr driverGamma;
};

bool
build(const AppInstancePtr& app,
      ReaderOverDriver* g)
{
    app->getProject()->setOrAddProjectFormat(Format(0, 0, kWindow.width(), kWindow.height(), "resetListeners", 1.));
    g->reader = createIn(app, app->getProject(), PLUGINID_NATRON_GRADE, PLUGIN_MAJOR_NATRON_GRADE);
    g->driver = createIn(app, app->getProject(), PLUGINID_NATRON_GRADE, PLUGIN_MAJOR_NATRON_GRADE);
    const NodePtr constant = createConstantIn(app, app->getProject(), kWindow, { 0.5, 0.5, 0.5, 1. });
    if (!g->reader || !g->driver || !constant || !g->driver->connectInput(constant, 0) || !g->reader->connectInput(g->driver, 0)) {
        return false;
    }
    g->readerWhite = g->reader->getKnobByName(kGradeParamWhite);
    g->readerGamma = g->reader->getKnobByName(kGradeParamGamma);
    g->driverWhite = g->driver->getKnobByName(kGradeParamWhite);
    g->driverGamma = g->driver->getKnobByName(kGradeParamGamma);

    return g->readerWhite && g->readerGamma && g->driverWhite && g->driverGamma;
}

bool
listens(const KnobIPtr& driver,
        const KnobIPtr& reader,
        int dimension)
{
    KnobI::ListenerDimsMap listeners;
    driver->getListeners(listeners);
    KnobI::ListenerDimsMap::const_iterator found = listeners.find(reader);

    return (found != listeners.end()) && (dimension < (int)found->second.size()) && found->second[dimension].isListening;
}

// A knob may outlive its effect, but must then point at no holder rather than at freed memory. Every
// knob checked here belongs to an effect.
bool
holderIsLive(const KnobIPtr& knob)
{
    KnobHolder* holder = 0;

    return !knob->getHolder() || (EffectInstance::lockHolder(*knob, &holder) != nullptr);
}

std::string
readExpression(const NodePtr& driver,
               const char* knobName)
{
    return driver->getScriptName() + "." + knobName + ".getValue(0)";
}

} // namespace

class ProjectResetListenersTest
    : public BaseTest {
protected:
    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        appPTR->clearAllCaches();
        BaseTest::TearDown();
    }
};

TEST_F(ProjectResetListenersTest, KnobTakenOffItsHolderPointsAtNoHolder)
{
    ReaderOverDriver g;
    ASSERT_TRUE(build(getApp(), &g));
    EffectInstancePtr effect = g.reader->getEffectInstance();
    ASSERT_EQ(effect.get(), g.readerWhite->getHolder());

    effect->removeKnobFromList(g.readerWhite.get());
    EXPECT_EQ(nullptr, g.readerWhite->getHolder());
    effect->insertKnob(0, g.readerWhite);
    EXPECT_EQ(effect.get(), g.readerWhite->getHolder());

    KnobDoublePtr doomed = effect->createDoubleKnob("doomed", "Doomed", 1);
    ASSERT_TRUE(doomed != nullptr);
    effect->deleteKnob(doomed.get(), false);
    EXPECT_EQ(nullptr, doomed->getHolder());
}

// The driver's teardown walks its listeners after the reader is gone, reaching the reader's knob
// that something else still holds.
TEST_F(ProjectResetListenersTest, ResetFreesAReaderWhoseKnobLivesOn)
{
    ReaderOverDriver g;
    ASSERT_TRUE(build(getApp(), &g));
    ASSERT_NO_THROW(g.readerWhite->setExpression(0, readExpression(g.driver, kGradeParamWhite), false, true));
    ASSERT_TRUE(listens(g.driverWhite, g.readerWhite, 0));

    g.reader->getEffectInstance()->removeKnobFromList(g.readerWhite.get());
    const KnobIPtr detached = g.readerWhite;
    g = ReaderOverDriver();
    getApp()->getProject()->reset(false, true);
    EXPECT_TRUE(holderIsLive(detached));
}

TEST_F(ProjectResetListenersTest, DeletedKnobLeavesItsMasters)
{
    ReaderOverDriver g;
    ASSERT_TRUE(build(getApp(), &g));
    KnobDoublePtr doomed = g.reader->getEffectInstance()->createDoubleKnob("doomed", "Doomed", 1);
    ASSERT_TRUE(doomed != nullptr);
    ASSERT_TRUE(doomed->slaveTo(0, g.driverWhite, 0));
    ASSERT_TRUE(listens(g.driverWhite, doomed, 0));

    g.reader->getEffectInstance()->deleteKnob(doomed.get(), false);
    EXPECT_FALSE(listens(g.driverWhite, doomed, 0));
    EXPECT_TRUE(doomed->getMaster(0).second == nullptr);
}

// A listener reading a deleted knob that something else keeps alive must forget it, or its next
// expression would unregister from a knob that has no holder.
TEST_F(ProjectResetListenersTest, ListenerOfADeletedKnobTakesANewExpression)
{
    ReaderOverDriver g;
    ASSERT_TRUE(build(getApp(), &g));
    const EffectInstancePtr driverEffect = g.driver->getEffectInstance();
    KnobDoublePtr k = driverEffect->createDoubleKnob("k", "K", 1);
    ASSERT_TRUE(k != nullptr);
    ASSERT_NO_THROW(g.readerWhite->setExpression(0, readExpression(g.driver, "k"), false, true));
    ASSERT_TRUE(listens(k, g.readerWhite, 0));

    const KnobIPtr held = k;
    driverEffect->deleteKnob(k.get(), false);
    k.reset();

    std::string error;
    EXPECT_FALSE(g.readerWhite->isExpressionValid(0, &error));
    KnobI::ListenerDimsMap listeners;
    held->getListeners(listeners);
    EXPECT_TRUE(listeners.empty());
    std::list<Node::KnobLink> links;
    g.reader->getKnobsLinks(links);
    for (std::list<Node::KnobLink>::const_iterator it = links.begin(); it != links.end(); ++it) {
        EXPECT_TRUE(it->master.lock() != held);
    }

    const KnobColorPtr driverWhite = std::dynamic_pointer_cast<KnobColor>(g.driverWhite);
    const KnobColorPtr readerWhite = std::dynamic_pointer_cast<KnobColor>(g.readerWhite);
    ASSERT_TRUE(driverWhite != nullptr);
    ASSERT_TRUE(readerWhite != nullptr);
    driverWhite->setValue(0.75, ViewSpec::all(), 0);
    ASSERT_NO_THROW(g.readerWhite->setExpression(0, readExpression(g.driver, kGradeParamWhite), false, true));
    EXPECT_TRUE(g.readerWhite->isExpressionValid(0, &error)) << error;
    EXPECT_TRUE(listens(g.driverWhite, g.readerWhite, 0));
    EXPECT_DOUBLE_EQ(0.75, readerWhite->getValue(0));
}

// Deleting a group deletes its children through the holder, each with its own recheck.
TEST_F(ProjectResetListenersTest, ListenerOfADeletedGroupsChildTakesANewExpression)
{
    ReaderOverDriver g;
    ASSERT_TRUE(build(getApp(), &g));
    const EffectInstancePtr driverEffect = g.driver->getEffectInstance();
    KnobGroupPtr group = driverEffect->createGroupKnob("grp", "Grp");
    KnobDoublePtr k = driverEffect->createDoubleKnob("k", "K", 1);
    ASSERT_TRUE(group != nullptr);
    ASSERT_TRUE(k != nullptr);
    ASSERT_TRUE(group->addKnob(k));
    ASSERT_NO_THROW(g.readerWhite->setExpression(0, readExpression(g.driver, "k"), false, true));
    ASSERT_TRUE(listens(k, g.readerWhite, 0));

    const KnobIPtr held = k;
    driverEffect->deleteKnob(group.get(), false);
    group.reset();
    k.reset();

    EXPECT_TRUE(driverEffect->getKnobByName("k") == nullptr);
    EXPECT_TRUE(driverEffect->getKnobByName("grp") == nullptr);
    std::string error;
    EXPECT_FALSE(g.readerWhite->isExpressionValid(0, &error));
    KnobI::ListenerDimsMap listeners;
    held->getListeners(listeners);
    EXPECT_TRUE(listeners.empty());
    std::list<Node::KnobLink> links;
    g.reader->getKnobsLinks(links);
    for (std::list<Node::KnobLink>::const_iterator it = links.begin(); it != links.end(); ++it) {
        EXPECT_TRUE(it->master.lock() != held);
    }

    const KnobColorPtr driverWhite = std::dynamic_pointer_cast<KnobColor>(g.driverWhite);
    const KnobColorPtr readerWhite = std::dynamic_pointer_cast<KnobColor>(g.readerWhite);
    ASSERT_TRUE(driverWhite != nullptr);
    ASSERT_TRUE(readerWhite != nullptr);
    driverWhite->setValue(0.75, ViewSpec::all(), 0);
    ASSERT_NO_THROW(g.readerWhite->setExpression(0, readExpression(g.driver, kGradeParamWhite), false, true));
    EXPECT_TRUE(g.readerWhite->isExpressionValid(0, &error)) << error;
    EXPECT_TRUE(listens(g.driverWhite, g.readerWhite, 0));
    EXPECT_DOUBLE_EQ(0.75, readerWhite->getValue(0));
}

TEST_F(ProjectResetListenersTest, UnslavingOneDimensionKeepsTheOthersListening)
{
    ReaderOverDriver g;
    ASSERT_TRUE(build(getApp(), &g));
    ASSERT_GE(g.readerWhite->getDimension(), 2);
    ASSERT_TRUE(g.readerWhite->slaveTo(0, g.driverWhite, 0));
    ASSERT_TRUE(g.readerWhite->slaveTo(1, g.driverWhite, 1));

    g.readerWhite->unSlave(0, false);
    EXPECT_TRUE(g.readerWhite->getMaster(0).second == nullptr);
    EXPECT_TRUE(listens(g.driverWhite, g.readerWhite, 1));
}

// A render thread holds a reader while the driver's teardown edits it. Inside that edit the reader
// queues value changes, its node is destroyed and every other owner lets go of it, so the edit is all
// that keeps the effect and node alive when endChanges() reaches them, with changes still pending.
TEST_F(ProjectResetListenersTest, ReaderDestroyedInsideTheDriversTeardownEdit)
{
    const AppInstancePtr app = getApp();
    NodePtr reader = createIn(app, app->getProject(), PLUGINID_NATRON_GRADE, PLUGIN_MAJOR_NATRON_GRADE);
    NodePtr driver = createIn(app, app->getProject(), PLUGINID_NATRON_GRADE, PLUGIN_MAJOR_NATRON_GRADE);
    ASSERT_TRUE(reader && driver);
    const KnobIPtr readerWhite = reader->getKnobByName(kGradeParamWhite);
    const KnobColorPtr readerGamma = std::dynamic_pointer_cast<KnobColor>(reader->getKnobByName(kGradeParamGamma));
    const KnobIPtr driverWhite = driver->getKnobByName(kGradeParamWhite);
    ASSERT_TRUE(readerWhite && readerGamma && driverWhite);
    ASSERT_NO_THROW(readerWhite->setExpression(0, readExpression(driver, kGradeParamWhite), false, true));
    ASSERT_TRUE(listens(driverWhite, readerWhite, 0));

    const EffectInstanceWPtr readerEffect = reader->getEffectInstance();
    const NodeWPtr readerWeak = reader;
    std::promise<void> release;
    std::promise<void> released;
    std::thread render([held = reader, heldEffect = reader->getEffectInstance(), go = release.get_future(), done = &released]() mutable {
        go.wait();
        held.reset();
        heldEffect.reset();
        done->set_value();
    });

    bool armed = true;
    bool hookRan = false;
    bool evaluationBlockedInside = false;
    bool effectHeldByTheEdit = false;
    bool nodeHeldByTheEdit = false;
    const KnobSignalSlotHandlerPtr handler = readerWhite->getSignalSlotHandler();
    ASSERT_TRUE(handler != nullptr);
    const QMetaObject::Connection connection = QObject::connect(handler.get(), &KnobSignalSlotHandler::expressionChanged, [&](int) {
        if (!armed) {
            return;
        }
        armed = false;
        hookRan = true;
        {
            const EffectInstancePtr effect = readerEffect.lock();
            evaluationBlockedInside = effect && effect->isEvaluationBlocked();
            readerGamma->setValue(2.);
            if (effect) {
                effect->appendValueChange(readerGamma, 0, true, 0., ViewIdx(0), eValueChangedReasonUserEdited, eValueChangedReasonUserEdited);
            }
        }
        reader->destroyNode(true, false);
        release.set_value();
        released.get_future().wait();
        reader.reset();
        effectHeldByTheEdit = !readerEffect.expired();
        nodeHeldByTheEdit = !readerWeak.expired();
    });

    driver->destroyNode(true, false);
    QObject::disconnect(connection);
    if (armed) {
        release.set_value();
    }
    render.join();

    EXPECT_TRUE(hookRan);
    EXPECT_TRUE(evaluationBlockedInside);
    EXPECT_TRUE(effectHeldByTheEdit);
    EXPECT_TRUE(nodeHeldByTheEdit);
    EXPECT_TRUE(readerEffect.expired());
    EXPECT_TRUE(readerWeak.expired());
    EXPECT_TRUE(holderIsLive(readerWhite));
    EXPECT_EQ(nullptr, readerWhite->getHolder());
}

// Cross-node expressions, a frame expression and a link, rendered then reset many times while the
// knobs are held across each reset, as the cache stress harness holds them.
TEST_F(ProjectResetListenersTest, RepeatedResetsWithCrossNodeReaders)
{
    const AppInstancePtr app = getApp();

    for (bool scheduled : { false, true }) {
        for (int iteration = 0; iteration < 24; ++iteration) {
            ReaderOverDriver g;
            ASSERT_TRUE(build(app, &g)) << (scheduled ? "scheduled" : "direct") << " iteration " << iteration;
            ASSERT_NO_THROW(g.driverWhite->setExpression(0, "frame*0.1", false, true));
            ASSERT_NO_THROW(g.readerWhite->setExpression(0, readExpression(g.driver, kGradeParamWhite), false, true));
            ASSERT_TRUE(g.readerGamma->slaveTo(0, g.driverGamma, 0));
            KnobDoublePtr extra = g.reader->getEffectInstance()->createDoubleKnob("extra", "Extra", 1);
            ASSERT_TRUE(extra != nullptr);
            ASSERT_NO_THROW(extra->setExpression(0, readExpression(g.driver, kGradeParamWhite), false, true));

            std::vector<KnobIPtr> held;
            held.push_back(g.readerWhite);
            held.push_back(g.readerGamma);
            held.push_back(g.driverWhite);
            held.push_back(g.driverGamma);
            held.push_back(extra);

            RenderedPlane plane;
            std::string error;
            EXPECT_TRUE((scheduled ? renderProbeScheduled : renderProbeDirect)(g.reader, 1. + (iteration % 3), 0, kWindow, false, &plane, &error))
                << (scheduled ? "scheduled" : "direct") << " iteration " << iteration << ": " << error;

            switch (iteration % 4) {
            case 1:
                g.reader->getEffectInstance()->deleteKnob(extra.get(), false);
                break;
            case 2:
                g.reader->getEffectInstance()->removeKnobFromList(g.readerWhite.get());
                break;
            case 3:
                g.reader->destroyNode(true, false);
                break;
            default:
                break;
            }

            g = ReaderOverDriver();
            app->getProject()->reset(false, true);
            for (std::size_t i = 0; i < held.size(); ++i) {
                EXPECT_TRUE(holderIsLive(held[i])) << (scheduled ? "scheduled" : "direct") << " iteration " << iteration << " knob " << held[i]->getName();
            }
        }
    }
}
