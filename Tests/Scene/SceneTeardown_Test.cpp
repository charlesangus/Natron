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

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include <pxr/base/tf/weakPtr.h>

#include "BaseTest.h"
#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/SceneTeardown.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/Project.h"
#include "Engine/SceneLoadSet.h"
#include "Engine/ScenePayload.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
struct Destruction {
    std::atomic<bool> done { false };
    std::thread::id thread;
};

// Records where and when it is destroyed; a moved-from probe records nothing.
class Probe {
public:
    explicit Probe(const std::shared_ptr<Destruction>& record)
        : _record(record)
    {
    }

    Probe(Probe&& other) noexcept
        : _record(std::move(other._record))
    {
    }

    ~Probe()
    {
        if (_record) {
            _record->thread = std::this_thread::get_id();
            _record->done = true;
        }
    }

private:
    std::shared_ptr<Destruction> _record;
};

// Holds the teardown thread inside its destructor until opened, so that whatever is released after
// it is still pending. Bounded, so a teardown that would wait on the test fails it instead of
// hanging it.
class Gate {
public:
    Gate()
        : _opened(_open.get_future().share())
    {
    }

    ~Gate()
    {
        open();
    }

    void close()
    {
        struct Blocker {
            std::shared_future<void> opened;

            explicit Blocker(const std::shared_future<void>& o)
                : opened(o)
            {
            }

            Blocker(Blocker&& other) noexcept
                : opened(std::move(other.opened))
            {
            }

            ~Blocker()
            {
                if (opened.valid()) {
                    (void)opened.wait_for(std::chrono::seconds(30));
                }
            }
        };

        SceneTeardown::release(Blocker(_opened));
    }

    void open()
    {
        if (!_isOpen) {
            _isOpen = true;
            _open.set_value();
        }
    }

private:
    std::promise<void> _open;
    std::shared_future<void> _opened;
    bool _isOpen = false;
};

// Defers for its lifetime, and leaves nothing pending behind it.
class DeferredScope {
public:
    DeferredScope()
        : _was(SceneTeardown::isDeferred())
    {
        SceneTeardown::setDeferred(true);
    }

    ~DeferredScope()
    {
        gate.open();
        EXPECT_TRUE(SceneTeardown::waitIdle(60000));
        SceneTeardown::setDeferred(_was);
    }

    Gate gate;

private:
    bool _was;
};

void
writeFile(const std::string& path,
          const std::string& content)
{
    QFile f(QString::fromStdString(path));

    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(content.c_str(), (qint64)content.size());
}

std::string
sceneOfXforms(int count)
{
    std::string scene = "#usda 1.0\n(\n    defaultPrim = \"World\"\n)\n\ndef Xform \"World\"\n{\n";
    for (int i = 0; i < count; ++i) {
        scene += "    def Xform \"X_" + std::to_string(i) + "\"\n    {\n    }\n";
    }
    scene += "}\n";

    return scene;
}
} // anonymous namespace

TEST(SceneTeardown, NotDeferredDestroysAtOnceOnTheCallingThread)
{
    ASSERT_FALSE(SceneTeardown::isDeferred());
    const std::shared_ptr<Destruction> record = std::make_shared<Destruction>();

    SceneTeardown::release(Probe(record));
    EXPECT_TRUE(record->done.load());
    EXPECT_EQ(std::this_thread::get_id(), record->thread);
}

TEST(SceneTeardown, DeferredReturnsBeforeDestroyingAndDestroysOnAnotherThreadInOrder)
{
    std::vector<std::shared_ptr<Destruction>> records;
    {
        DeferredScope deferred;
        deferred.gate.close();
        for (int i = 0; i < 3; ++i) {
            records.push_back(std::make_shared<Destruction>());
            SceneTeardown::release(Probe(records.back()));
        }
        for (const std::shared_ptr<Destruction>& record : records) {
            EXPECT_FALSE(record->done.load());
        }
        EXPECT_EQ((std::size_t)4, SceneTeardown::getPendingCount());
        deferred.gate.open();
        ASSERT_TRUE(SceneTeardown::waitIdle(60000));
    }
    for (const std::shared_ptr<Destruction>& record : records) {
        EXPECT_TRUE(record->done.load());
        EXPECT_NE(std::this_thread::get_id(), record->thread);
        EXPECT_EQ(records.front()->thread, record->thread);
    }
    EXPECT_EQ((std::size_t)0, SceneTeardown::getPendingCount());
}

TEST(SceneTeardown, IdleQueryStagesCloseAfterReleaseIdleReturns)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = dir.filePath(QString::fromUtf8("pool.usda")).toStdString();
    writeFile(path, sceneOfXforms(64));
    const PXR_NS::SdfLayerRefPtr file = PXR_NS::SdfLayer::FindOrOpen(path);
    ASSERT_TRUE(bool(file));
    const ScenePayload payload = SceneOps::appendLayer(ScenePayload(), file, 1);
    ASSERT_FALSE(payload.isEmpty());

    SceneQueryPool pool(1);
    PXR_NS::UsdStagePtr stage;
    {
        std::string error;
        SceneQueryPool::Lease lease = pool.borrow(payload, SceneLoadSet::everything(), &error);
        ASSERT_TRUE(lease.isValid()) << error;
        stage = lease.getStage();
    }
    ASSERT_EQ((std::size_t)1, pool.getNumIdle());
    ASSERT_TRUE(bool(stage));

    DeferredScope deferred;
    deferred.gate.close();
    pool.releaseIdle();
    EXPECT_EQ((std::size_t)0, pool.getNumIdle());
    EXPECT_FALSE(stage.IsExpired()) << "the stage closed on the thread that let it go";
    deferred.gate.open();
    ASSERT_TRUE(SceneTeardown::waitIdle(60000));
    EXPECT_TRUE(stage.IsExpired()) << "the stage outlived the teardown";
}

class SceneTeardownProjectTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        KnobDouble* metersPerUnit = dynamic_cast<KnobDouble*>(getApp()->getProject()->getKnobByName("metersPerUnit").get());
        ASSERT_TRUE(metersPerUnit != NULL);
        metersPerUnit->setValue(0.01);
    }

    virtual void TearDown() OVERRIDE
    {
        SceneQueryPool::instance().releaseIdle();

        BaseTest::TearDown();
    }
};

// Resetting a project that reads a USD file returns while the file's layer and the stages
// composing it are still to be closed, and they are closed once the teardown runs.
TEST_F(SceneTeardownProjectTest, ResetReturnsBeforeTheReadSceneIsClosed)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = dir.filePath(QString::fromUtf8("reset.usda")).toStdString();
    writeFile(path, sceneOfXforms(256));

    PXR_NS::UsdStagePtr stage;
    {
        NodePtr reader = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));
        ASSERT_TRUE(bool(reader));
        KnobFile* file = dynamic_cast<KnobFile*>(reader->getKnobByName(kUSDReadParamFile).get());
        ASSERT_TRUE(file != NULL);
        file->setValue(path);

        ScenePayload payload;
        std::string error;
        ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk,
                  reader->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0)), &payload, &error))
            << error;
        SceneQueryPool::Lease lease = SceneQueryPool::instance().borrow(payload, SceneLoadSet::everything(), &error);
        ASSERT_TRUE(lease.isValid()) << error;
        ASSERT_TRUE(lease.getStage()->GetPrimAtPath(PXR_NS::SdfPath("/World/X_255")).IsValid());
        stage = lease.getStage();
    }
    ASSERT_TRUE(bool(PXR_NS::SdfLayer::Find(path)));

    DeferredScope deferred;
    deferred.gate.close();
    getApp()->getProject()->reset(false, true);
    EXPECT_TRUE(bool(PXR_NS::SdfLayer::Find(path))) << "the reset closed the file's layer itself";
    EXPECT_FALSE(stage.IsExpired()) << "the reset closed the pooled stage itself";
    EXPECT_GT(SceneTeardown::getPendingCount(), (std::size_t)1);

    deferred.gate.open();
    ASSERT_TRUE(SceneTeardown::waitIdle(60000));
    EXPECT_TRUE(stage.IsExpired()) << "the pooled stage outlived the teardown";
    EXPECT_FALSE(bool(PXR_NS::SdfLayer::Find(path))) << "the file's layer outlived the teardown";
}
