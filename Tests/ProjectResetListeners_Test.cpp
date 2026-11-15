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

#include <string>
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

const RenderSchedulerModeEnum kModes[] = { eRenderSchedulerModeLegacy, eRenderSchedulerModeTaskGraph };

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

// A knob may outlive its effect, but must then point at no holder rather than at freed memory.
bool
holderIsLive(const KnobIPtr& knob)
{
    KnobHolder* holder = knob->getHolder();

    return !holder || (EffectInstance::lockLiveEffect(holder) != nullptr);
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

// Cross-node expressions, a frame expression and a link, rendered then reset many times while the
// knobs are held across each reset, as the cache stress harness holds them.
TEST_F(ProjectResetListenersTest, RepeatedResetsWithCrossNodeReaders)
{
    const AppInstancePtr app = getApp();

    for (RenderSchedulerModeEnum mode : kModes) {
        for (int iteration = 0; iteration < 24; ++iteration) {
            ReaderOverDriver g;
            ASSERT_TRUE(build(app, &g)) << "mode " << (int)mode << " iteration " << iteration;
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
            EXPECT_TRUE(renderProbe(g.reader, 1. + (iteration % 3), 0, kWindow, mode, false, &plane, &error))
                << "mode " << (int)mode << " iteration " << iteration << ": " << error;

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
                EXPECT_TRUE(holderIsLive(held[i])) << "mode " << (int)mode << " iteration " << iteration << " knob " << held[i]->getName();
            }
        }
    }
}
