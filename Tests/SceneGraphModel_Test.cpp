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

#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <unistd.h>

#include <gtest/gtest.h>

#include "Gui/SceneGraphModel.h"

#include <QAbstractItemModelTester>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFont>
#include <QMimeData>
#include <QPersistentModelIndex>
#include <QSignalSpy>
#include <QStringList>

#include "Engine/Nodes/Scene/SceneDrawStates.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneStage.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"

NATRON_NAMESPACE_USING

namespace {

// A stage the test syncs itself, read under its own lock as the viewer's is.
class TestStageSource
    : public SceneGraphStageSource {
public:
    explicit TestStageSource(PXR_NS::UsdStage::InitialLoadSet load = PXR_NS::UsdStage::LoadAll)
        : stage(load)
    {
    }

    void readStage(const std::function<void(const SceneStage* stage)>& read) const override
    {
        std::lock_guard<std::mutex> lock(mutex);

        read(stage.isValid() && !stage.getPayload().isEmpty() ? &stage : nullptr);
    }

    bool tryReadStage(const std::function<void(const SceneStage* stage)>& read) const override
    {
        if (busy) {
            return false;
        }
        readStage(read);

        return true;
    }

    U64 getEditCount() const override
    {
        return stage.getEditCount();
    }

    void sync(const ScenePayload& payload)
    {
        std::lock_guard<std::mutex> lock(mutex);

        stage.sync(payload);
    }

    void serviceLoads()
    {
        const SceneStage::LockedSection locked = [this](const std::function<void()>& section) {
            std::lock_guard<std::mutex> lock(mutex);
            section();
        };

        stage.serviceLoads(locked, locked);
    }

    mutable std::mutex mutex;
    SceneStage stage;
    // As if a commit held the stage's lock.
    bool busy = false;
};

// A TestStageSource whose reads off the thread that made it wait while it is closed, as it is
// until open() is first called.
class GatedStageSource
    : public TestStageSource {
public:
    GatedStageSource()
        : _mainThread(std::this_thread::get_id())
    {
    }

    ~GatedStageSource() override
    {
        open();
    }

    void readStage(const std::function<void(const SceneStage* stage)>& read) const override
    {
        const bool onMain = (std::this_thread::get_id() == _mainThread);

        if (!onMain) {
            std::unique_lock<std::mutex> lock(_gateMutex);
            // Bounded, so that a read the test never lets through fails it instead of hanging it.
            (void)_gate.wait_for(lock, std::chrono::seconds(30), [this]() { return _isOpen; });
        }
        TestStageSource::readStage(read);
        ++(onMain ? mainReads : workerReads);
    }

    void open()
    {
        {
            std::lock_guard<std::mutex> lock(_gateMutex);
            _isOpen = true;
        }
        _gate.notify_all();
    }

    // A read already past the gate is not held back.
    void close()
    {
        std::lock_guard<std::mutex> lock(_gateMutex);

        _isOpen = false;
    }

    mutable std::atomic<int> workerReads { 0 };
    mutable std::atomic<int> mainReads { 0 };

private:
    mutable std::mutex _gateMutex;
    mutable std::condition_variable _gate;
    bool _isOpen = false;
    std::thread::id _mainThread;
};

template <typename Done>
bool
processEventsUntil(Done done)
{
    QElapsedTimer timer;

    timer.start();
    while (!done()) {
        if (timer.elapsed() > 30000) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return true;
}

// On disk, /World holding the payload prims B0 and B1, and the group Other holding the payload prim
// B2. B1 also has a child in the root layer, composed whether or not its payload is loaded.
class PayloadScene {
public:
    PayloadScene()
    {
        static std::atomic<int> counter(0);

        _dir = std::filesystem::temp_directory_path() / ("scene_graph_payloads_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
        std::filesystem::create_directories(_dir);
        std::ofstream root((_dir / "root.usda").string());
        root << "#usda 1.0\n(\n    defaultPrim = \"World\"\n)\ndef Xform \"World\"\n{\n"
             << "    def Xform \"B0\" (\n        prepend payload = @./b0.usda@\n    )\n    {\n    }\n"
             << "    def Xform \"B1\" (\n        prepend payload = @./b1.usda@\n    )\n    {\n        def Xform \"Own\"\n        {\n        }\n    }\n"
             << "    def Xform \"Other\"\n    {\n        def Xform \"B2\" (\n            prepend payload = @./b2.usda@\n        )\n        {\n        }\n    }\n"
             << "}\n";
        for (const char* name : { "b0.usda", "b1.usda", "b2.usda" }) {
            std::ofstream building((_dir / name).string());
            building << "#usda 1.0\n(\n    defaultPrim = \"Building\"\n)\ndef Xform \"Building\"\n{\n    def Cube \"Geo\"\n    {\n    }\n}\n";
        }
    }

    ~PayloadScene()
    {
        std::error_code ignored;
        std::filesystem::remove_all(_dir, ignored);
    }

    void removeBuilding(const std::string& name) const
    {
        std::filesystem::remove(_dir / name);
    }

    ScenePayload payload() const
    {
        const PXR_NS::SdfLayerRefPtr root = PXR_NS::SdfLayer::FindOrOpen((_dir / "root.usda").string());

        EXPECT_TRUE(bool(root));
        _root = root;

        return SceneOps::appendLayer(ScenePayload(), root, 0xb011d1ULL);
    }

private:
    std::filesystem::path _dir;
    mutable PXR_NS::SdfLayerRefPtr _root;
};

const QString kB0 = QString::fromUtf8("/World/B0");
const QString kB1 = QString::fromUtf8("/World/B1");
const QString kB2 = QString::fromUtf8("/World/Other/B2");

// The fixture as a USDRead node would hand it on.
ScenePayload
referenceScene()
{
    const std::string path = NATRON_TESTS_FIXTURES_DIR "/scene-reference.usda";
    PXR_NS::SdfLayerRefPtr file = PXR_NS::SdfLayer::FindOrOpen(path);

    if (!file) {
        ADD_FAILURE() << "cannot open " << path;

        return ScenePayload();
    }
    PXR_NS::SdfLayerRefPtr layer = SceneOps::createNodeLayer("SceneGraphModelTest");
    layer->InsertSubLayerPath(file->GetIdentifier());

    return SceneOps::appendLayer(ScenePayload(), layer, 0x5ce9e0ULL);
}

// A node layer defining root with an Xform child per name.
PXR_NS::SdfLayerRefPtr
groupLayer(const std::string& root,
           const std::vector<std::string>& children)
{
    PXR_NS::SdfLayerRefPtr layer = SceneOps::createNodeLayer("SceneGraphModelTest");
    PXR_NS::SdfPrimSpecHandle group = PXR_NS::SdfPrimSpec::New(layer->GetPseudoRoot(), root, PXR_NS::SdfSpecifierDef, "Xform");

    for (const std::string& child : children) {
        PXR_NS::SdfPrimSpec::New(group, child, PXR_NS::SdfSpecifierDef, "Xform");
    }

    return layer;
}

// Fails the test on any report of the model tester.
QStringList modelTestMessages;

void
collectModelTestMessages(QtMsgType type,
                         const QMessageLogContext& context,
                         const QString& message)
{
    if (context.category && (std::string(context.category) == "qt.modeltest")) {
        modelTestMessages << message;

        return;
    }
    if (type != QtDebugMsg) {
        std::fprintf(stderr, "%s\n", message.toLocal8Bit().constData());
    }
}

class ModelTestMessageScope {
public:
    ModelTestMessageScope()
    {
        modelTestMessages.clear();
        _previous = qInstallMessageHandler(collectModelTestMessages);
    }

    ~ModelTestMessageScope()
    {
        qInstallMessageHandler(_previous);
    }

private:
    QtMessageHandler _previous;
};

QStringList
childNames(const SceneGraphModel& model,
           const QModelIndex& parent)
{
    QStringList names;

    for (int row = 0; row < model.rowCount(parent); ++row) {
        names << model.index(row, SceneGraphModel::eColumnName, parent).data().toString();
    }

    return names;
}

QModelIndex
childNamed(const SceneGraphModel& model,
           const QModelIndex& parent,
           const QString& name)
{
    for (int row = 0; row < model.rowCount(parent); ++row) {
        const QModelIndex child = model.index(row, SceneGraphModel::eColumnName, parent);
        if (child.data().toString() == name) {
            return child;
        }
    }

    return QModelIndex();
}

// The row of path, asked for until the reads on the worker that bring it in land.
QModelIndex
waitForPath(SceneGraphModel& model,
            const QString& path)
{
    QModelIndex index;

    (void)processEventsUntil([&]() {
        index = model.indexOfPath(path, true);

        return index.isValid();
    });
    (void)processEventsUntil([&]() { return !model.hasOwedReads(); });

    return model.indexOfPath(path, false);
}

// Children of every kind the walk tells apart; listed gets the paths it should list, in order.
PXR_NS::SdfLayerRefPtr
walkFixtureLayer(QStringList* listed)
{
    using namespace PXR_NS;

    SdfLayerRefPtr layer = SceneOps::createNodeLayer("SceneGraphPrimWalkTest");
    SdfPrimSpecHandle root = SdfPrimSpec::New(layer->GetPseudoRoot(), "Root", SdfSpecifierDef, "Xform");
    SdfPrimSpecHandle a = SdfPrimSpec::New(root, "A", SdfSpecifierDef, "Xform");
    SdfPrimSpec::New(a, "Inner", SdfSpecifierDef, "Xform");
    SdfPrimSpec::New(root, "B", SdfSpecifierDef, "Xform");
    SdfPrimSpecHandle gone = SdfPrimSpec::New(root, "Gone", SdfSpecifierDef, "Xform");
    gone->SetActive(false);
    SdfPrimSpec::New(gone, "UnderGone", SdfSpecifierDef, "Xform");
    SdfPrimSpecHandle undefined = SdfPrimSpec::New(root, "Undefined", SdfSpecifierOver, "");
    SdfPrimSpec::New(undefined, "UnderUndefined", SdfSpecifierDef, "Xform");
    SdfPrimSpec::New(root, "Abstract", SdfSpecifierClass, "");
    SdfPrimSpecHandle instance = SdfPrimSpec::New(root, "Inst", SdfSpecifierDef, "Xform");
    instance->SetInstanceable(true);
    instance->GetReferenceList().Prepend(SdfReference(std::string(), SdfPath("/Proto")));
    SdfPrimSpec::New(root, "Last", SdfSpecifierDef, "Xform");
    SdfPrimSpecHandle proto = SdfPrimSpec::New(layer->GetPseudoRoot(), "Proto", SdfSpecifierDef, "Xform");
    SdfPrimSpec::New(proto, "Kid", SdfSpecifierDef, "Xform");
    SdfPrimSpecHandle trailing = SdfPrimSpec::New(layer->GetPseudoRoot(), "Trailing", SdfSpecifierDef, "Xform");
    trailing->SetActive(false);

    *listed = QStringList { QString::fromUtf8("/Root"), QString::fromUtf8("/Root/A"), QString::fromUtf8("/Root/A/Inner"),
                            QString::fromUtf8("/Root/B"), QString::fromUtf8("/Root/Inst"), QString::fromUtf8("/Root/Inst/Kid"),
                            QString::fromUtf8("/Root/Last"), QString::fromUtf8("/Proto"), QString::fromUtf8("/Proto/Kid") };

    return layer;
}

void
stepWalk(const TestStageSource& source,
         SceneGraphPrimWalk* walk,
         int budgetMs,
         int cap,
         QStringList* found)
{
    source.readStage([&](const SceneStage* stage) {
        walk->step(stage, budgetMs, cap, [](const QString&) { return true; }, found);
    });
}

// Steps to the end; returns how many slices it took.
int
finishWalk(const TestStageSource& source,
           SceneGraphPrimWalk* walk,
           int budgetMs,
           QStringList* found)
{
    int slices = 0;

    while (!walk->isOver()) {
        stepWalk(source, walk, budgetMs, INT_MAX, found);
        ++slices;
    }

    return slices;
}

} // anon namespace

TEST(SceneGraphModel, ShowsTheReferenceTree)
{
    ModelTestMessageScope messages;
    std::shared_ptr<TestStageSource> source = std::make_shared<TestStageSource>();
    source->sync(referenceScene());
    SceneGraphModel model;
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Warning);

    // The tester reads every row it can on its own; the model must answer the same either way.
    model.setSource(source);
    if (model.canFetchMore(QModelIndex())) {
        model.fetchMore(QModelIndex());
    }
    ASSERT_EQ(QStringList() << QString::fromUtf8("World"), childNames(model, QModelIndex()));
    const QModelIndex world = model.index(0, SceneGraphModel::eColumnName);
    EXPECT_EQ(QString::fromUtf8("Xform"), model.index(0, SceneGraphModel::eColumnType).data().toString());
    EXPECT_TRUE(model.hasChildren(world));
    if (model.canFetchMore(world)) {
        model.fetchMore(world);
    }
    ASSERT_EQ(QStringList() << QString::fromUtf8("Box") << QString::fromUtf8("Ball") << QString::fromUtf8("Cam"), childNames(model, world));
    EXPECT_FALSE(model.canFetchMore(world));
    EXPECT_EQ(QString::fromUtf8("Cube"), model.index(0, SceneGraphModel::eColumnType, world).data().toString());
    EXPECT_EQ(QString::fromUtf8("Sphere"), model.index(1, SceneGraphModel::eColumnType, world).data().toString());
    EXPECT_EQ(QString::fromUtf8("Camera"), model.index(2, SceneGraphModel::eColumnType, world).data().toString());
    EXPECT_EQ(QString::fromUtf8("/World/Cam"), model.index(2, 0, world).data(SceneGraphModel::ePrimPathRole).toString());
    EXPECT_FALSE(model.hasChildren(model.index(0, 0, world)));
    EXPECT_EQ(4, model.getItemCount());

    const QModelIndex cam = model.indexOfPath(QString::fromUtf8("/World/Cam"), false);
    ASSERT_TRUE(cam.isValid());
    EXPECT_EQ(2, cam.row());
    EXPECT_EQ(QString::fromUtf8("/World/Cam"), model.pathOf(cam));
    EXPECT_FALSE(model.indexOfPath(QString::fromUtf8("/World/Nothing"), true).isValid());

    std::unique_ptr<QMimeData> mime(model.mimeData(QModelIndexList() << model.index(0, 0, world) << model.index(0, 1, world) << cam));
    ASSERT_TRUE(bool(mime));
    EXPECT_EQ(QByteArray("/World/Box\n/World/Cam"), mime->data(SceneGraphModel::primPathsMimeType()));

    EXPECT_TRUE(modelTestMessages.isEmpty()) << modelTestMessages.join(QLatin1Char('\n')).toStdString();
}

TEST(SceneGraphModel, FindsAPathByReadingTheWayDown)
{
    std::shared_ptr<TestStageSource> source = std::make_shared<TestStageSource>();
    source->sync(referenceScene());
    SceneGraphModel model;

    model.setSource(source);
    EXPECT_FALSE(model.indexOfPath(QString::fromUtf8("/World/Ball"), false).isValid());
    const QModelIndex ball = model.indexOfPath(QString::fromUtf8("/World/Ball"), true);
    ASSERT_TRUE(ball.isValid());
    EXPECT_EQ(QString::fromUtf8("Ball"), ball.data().toString());
    EXPECT_EQ(QString::fromUtf8("World"), ball.parent().data().toString());
}

TEST(SceneGraphModel, AddsRowsAWorkerReadForAPath)
{
    std::shared_ptr<TestStageSource> source = std::make_shared<TestStageSource>();
    source->sync(referenceScene());
    SceneGraphModel model;
    const QString path = QString::fromUtf8("/World/Ball");

    model.setSource(source);
    SceneGraphPathRowsBuilder builder({ path, QString::fromUtf8("/World/Nothing") });
    while (!builder.isDone()) {
        source->readStage([&builder](const SceneStage* stage) { builder.readBatch(stage); });
    }
    model.addPathRows(builder.take());

    const QModelIndex ball = model.indexOfPath(path, false);
    ASSERT_TRUE(ball.isValid());
    EXPECT_EQ(QString::fromUtf8("Ball"), ball.data().toString());
    EXPECT_EQ(QString::fromUtf8("World"), ball.parent().data().toString());
    EXPECT_FALSE(model.indexOfPath(QString::fromUtf8("/World/Nothing"), true).isValid());
}

// Swapping one node's layer for another resyncs only what the two author: rows elsewhere keep
// their indexes, and the model is not reset.
TEST(SceneGraphModel, AnEditResyncsOnlyItsSubtree)
{
    ModelTestMessageScope messages;
    std::shared_ptr<TestStageSource> source = std::make_shared<TestStageSource>();
    PXR_NS::SdfLayerRefPtr kept = groupLayer("Kept", { "K0", "K1", "K2" });
    PXR_NS::SdfLayerRefPtr edited = groupLayer("Edited", { "Old", "Same", "Other" });
    ScenePayload base = SceneOps::appendLayer(ScenePayload(), kept, 1);
    source->sync(SceneOps::appendLayer(base, edited, 2));

    SceneGraphModel model;
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Warning);
    model.setSource(source);
    const QModelIndex keptIndex = model.indexOfPath(QString::fromUtf8("/Kept/K2"), true).parent();
    const QModelIndex editedIndex = model.indexOfPath(QString::fromUtf8("/Edited/Other"), true).parent();
    ASSERT_TRUE(keptIndex.isValid());
    ASSERT_TRUE(editedIndex.isValid());
    ASSERT_EQ(3, model.rowCount(keptIndex));
    ASSERT_EQ(3, model.rowCount(editedIndex));
    const int resets = model.getResetCount();
    const QPersistentModelIndex keptGroup(keptIndex);
    const QPersistentModelIndex k1(model.index(1, 0, keptIndex));
    const QPersistentModelIndex same(childNamed(model, editedIndex, QString::fromUtf8("Same")));
    const QPersistentModelIndex old(childNamed(model, editedIndex, QString::fromUtf8("Old")));

    QStringList changedParents;
    QObject::connect(&model, &QAbstractItemModel::rowsInserted, [&](const QModelIndex& parent, int, int) { changedParents << model.pathOf(parent); });
    QObject::connect(&model, &QAbstractItemModel::rowsRemoved, [&](const QModelIndex& parent, int, int) { changedParents << model.pathOf(parent); });

    source->sync(SceneOps::appendLayer(base, groupLayer("Edited", { "Same", "New", "Other" }), 3));
    model.refresh();

    EXPECT_EQ(resets, model.getResetCount());
    EXPECT_EQ(QStringList() << QString::fromUtf8("Same") << QString::fromUtf8("New") << QString::fromUtf8("Other"), childNames(model, editedIndex));
    for (const QString& parent : changedParents) {
        EXPECT_EQ(QString::fromUtf8("/Edited"), parent);
    }
    EXPECT_FALSE(changedParents.isEmpty());
    ASSERT_TRUE(keptGroup.isValid());
    ASSERT_TRUE(k1.isValid());
    EXPECT_EQ(1, k1.row());
    EXPECT_EQ(QString::fromUtf8("/Kept/K1"), model.pathOf(k1));
    ASSERT_TRUE(same.isValid());
    EXPECT_EQ(0, same.row());
    EXPECT_FALSE(old.isValid());

    // Nothing edited since: nothing is read again.
    changedParents.clear();
    model.refresh();
    EXPECT_TRUE(changedParents.isEmpty());

    // A changed type is a change of the prim's data, not of its row.
    PXR_NS::SdfLayerRefPtr retyped = groupLayer("Edited", { "Same", "New", "Other" });
    retyped->GetPrimAtPath(PXR_NS::SdfPath("/Edited/New"))->SetTypeName("Scope");
    source->sync(SceneOps::appendLayer(base, retyped, 4));
    model.refresh();
    EXPECT_EQ(resets, model.getResetCount());
    EXPECT_EQ(QString::fromUtf8("Scope"), model.index(1, SceneGraphModel::eColumnType, editedIndex).data().toString());
    EXPECT_TRUE(k1.isValid());

    EXPECT_TRUE(modelTestMessages.isEmpty()) << modelTestMessages.join(QLatin1Char('\n')).toStdString();
}

TEST(SceneGraphModel, OnlyAnEditOfTheStageSendsNotices)
{
    ModelTestMessageScope messages;
    std::shared_ptr<TestStageSource> source = std::make_shared<TestStageSource>();
    ScenePayload base = SceneOps::appendLayer(ScenePayload(), groupLayer("Group", { "A", "B" }), 1);
    source->sync(base);

    SceneGraphModel model;
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Warning);
    model.setSource(source);
    const U64 notices = model.getStageNoticeCount();

    const QModelIndex group = model.indexOfPath(QString::fromUtf8("/Group/B"), true).parent();
    ASSERT_TRUE(group.isValid());
    EXPECT_EQ(2, model.rowCount(group));
    model.refresh();
    EXPECT_EQ(notices, model.getStageNoticeCount());

    source->sync(SceneOps::appendLayer(base, groupLayer("Other", { "C" }), 2));
    EXPECT_GT(model.getStageNoticeCount(), notices);

    EXPECT_TRUE(modelTestMessages.isEmpty()) << modelTestMessages.join(QLatin1Char('\n')).toStdString();
}

// Read on the worker, an edit's rows change from the event loop, as they would under the main
// thread's read.
TEST(SceneGraphModel, AnEditReadOnTheWorkerAppliesFromTheEventLoop)
{
    ModelTestMessageScope messages;
    std::shared_ptr<GatedStageSource> source = std::make_shared<GatedStageSource>();
    PXR_NS::SdfLayerRefPtr kept = groupLayer("Kept", { "K0", "K1", "K2" });
    ScenePayload base = SceneOps::appendLayer(ScenePayload(), kept, 1);
    source->sync(SceneOps::appendLayer(base, groupLayer("Edited", { "Old", "Same", "Other" }), 2));

    SceneGraphModel model;
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Warning);
    model.setReadsOnWorker(true);
    model.setSource(source);
    source->open();
    const QModelIndex keptIndex = waitForPath(model, QString::fromUtf8("/Kept/K2")).parent();
    const QModelIndex editedIndex = waitForPath(model, QString::fromUtf8("/Edited/Other")).parent();
    ASSERT_TRUE(keptIndex.isValid());
    ASSERT_TRUE(editedIndex.isValid());
    source->close();
    const int resets = model.getResetCount();
    const QPersistentModelIndex k1(model.index(1, 0, keptIndex));
    const QPersistentModelIndex same(childNamed(model, editedIndex, QString::fromUtf8("Same")));
    const QStringList before = QStringList() << QString::fromUtf8("Old") << QString::fromUtf8("Same") << QString::fromUtf8("Other");
    const QStringList after = QStringList() << QString::fromUtf8("Same") << QString::fromUtf8("New") << QString::fromUtf8("Other");

    source->sync(SceneOps::appendLayer(base, groupLayer("Edited", { "Same", "New", "Other" }), 3));
    model.refresh();
    EXPECT_EQ(before, childNames(model, editedIndex));
    source->open();
    EXPECT_TRUE(processEventsUntil([&]() { return childNames(model, editedIndex) == after; }));

    EXPECT_EQ(resets, model.getResetCount());
    EXPECT_GT(source->workerReads.load(), 0);
    ASSERT_TRUE(k1.isValid());
    EXPECT_EQ(QString::fromUtf8("/Kept/K1"), model.pathOf(k1));
    ASSERT_TRUE(same.isValid());
    EXPECT_EQ(0, same.row());

    EXPECT_TRUE(modelTestMessages.isEmpty()) << modelTestMessages.join(QLatin1Char('\n')).toStdString();
}

// What the worker read for a stage the model no longer shows changes none of its rows, even
// where the new stage has the same paths.
TEST(SceneGraphModel, AnEditReadForAReplacedStageIsDropped)
{
    ModelTestMessageScope messages;
    std::shared_ptr<GatedStageSource> replaced = std::make_shared<GatedStageSource>();
    std::shared_ptr<TestStageSource> shown = std::make_shared<TestStageSource>();
    const ScenePayload original = SceneOps::appendLayer(ScenePayload(), groupLayer("Edited", { "Old", "Same", "Other" }), 1);
    replaced->sync(original);
    shown->sync(original);

    SceneGraphModel model;
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Warning);
    model.setReadsOnWorker(true);
    model.setSource(replaced);
    replaced->open();
    EXPECT_TRUE(waitForPath(model, QString::fromUtf8("/Edited/Other")).isValid());
    replaced->close();

    replaced->sync(SceneOps::appendLayer(ScenePayload(), groupLayer("Edited", { "Same", "New", "Other" }), 2));
    model.refresh();
    // The worker is held by the replaced stage's gather: the shown stage's rows are read under
    // the main thread's read, and its edit on the worker.
    model.setReadsOnWorker(false);
    model.setSource(shown);
    const QModelIndex edited = model.indexOfPath(QString::fromUtf8("/Edited/Other"), true).parent();
    EXPECT_TRUE(edited.isValid());
    model.setReadsOnWorker(true);
    const int resets = model.getResetCount();
    QStringList inserted;
    QObject::connect(&model, &QAbstractItemModel::rowsInserted, [&](const QModelIndex& parent, int first, int last) {
        for (int row = first; row <= last; ++row) {
            inserted << model.index(row, 0, parent).data().toString();
        }
    });
    // Gathered after the replaced stage's edit, so applied after it would have been.
    shown->sync(SceneOps::appendLayer(ScenePayload(), groupLayer("Edited", { "Old", "Extra", "Same", "Other" }), 3));
    model.refresh();
    replaced->open();
    const QStringList extended = QStringList() << QString::fromUtf8("Old") << QString::fromUtf8("Extra") << QString::fromUtf8("Same") << QString::fromUtf8("Other");
    EXPECT_TRUE(processEventsUntil([&]() { return childNames(model, model.indexOfPath(QString::fromUtf8("/Edited"), false)) == extended; }));

    EXPECT_EQ(resets, model.getResetCount());
    EXPECT_EQ(QStringList(QString::fromUtf8("Extra")), inserted);

    EXPECT_TRUE(modelTestMessages.isEmpty()) << modelTestMessages.join(QLatin1Char('\n')).toStdString();
}

// With reads on the worker, a twirl asks nothing of the stage on the thread that twirls: the
// row shows the loading row until its children land from the event loop.
TEST(SceneGraphModel, AnExpandOnTheWorkerReadsNothingOnTheCallingThread)
{
    ModelTestMessageScope messages;
    std::shared_ptr<GatedStageSource> source = std::make_shared<GatedStageSource>();
    source->sync(referenceScene());
    SceneGraphModel model;
    model.setReadsOnWorker(true);
    model.setSource(source);

    int mainReads = source->mainReads.load();
    ASSERT_TRUE(model.canFetchMore(QModelIndex()));
    model.fetchMore(QModelIndex());
    EXPECT_EQ(mainReads, source->mainReads.load());
    EXPECT_EQ(0, model.rowCount());
    EXPECT_TRUE(model.hasOwedReads());
    EXPECT_FALSE(model.canFetchMore(QModelIndex()));
    source->open();
    EXPECT_TRUE(processEventsUntil([&]() { return childNames(model, QModelIndex()) == QStringList(QString::fromUtf8("World")); }));
    EXPECT_TRUE(processEventsUntil([&]() { return !model.hasOwedReads(); }));
    const int workerReads = source->workerReads.load();
    EXPECT_GT(workerReads, 0);

    source->close();
    const QModelIndex world = model.index(0, 0);
    mainReads = source->mainReads.load();
    QSignalSpy done(&model, &SceneGraphModel::deferredReadsDone);
    ASSERT_TRUE(model.canFetchMore(world));
    model.fetchMore(world);
    EXPECT_EQ(mainReads, source->mainReads.load());
    ASSERT_EQ(1, model.rowCount(world));
    EXPECT_FALSE(model.flags(model.index(0, 0, world)).testFlag(Qt::ItemIsEnabled));
    EXPECT_FALSE(model.canFetchMore(world));
    bool busy = false;
    EXPECT_FALSE(model.indexOfPath(QString::fromUtf8("/World/Ball"), true, &busy).isValid());
    EXPECT_TRUE(busy);
    EXPECT_EQ(mainReads, source->mainReads.load());

    source->open();
    const QStringList children = QStringList() << QString::fromUtf8("Box") << QString::fromUtf8("Ball") << QString::fromUtf8("Cam");
    EXPECT_TRUE(processEventsUntil([&]() { return childNames(model, world) == children; }));
    EXPECT_TRUE(processEventsUntil([&]() { return done.count() > 0; }));
    EXPECT_GT(source->workerReads.load(), workerReads);
    EXPECT_FALSE(model.canFetchMore(world));
    EXPECT_TRUE(model.indexOfPath(QString::fromUtf8("/World/Ball"), false).isValid());
    EXPECT_EQ(4, model.getItemCount());

    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Warning);
    EXPECT_TRUE(modelTestMessages.isEmpty()) << modelTestMessages.join(QLatin1Char('\n')).toStdString();
}

// The rows a twirl read on the worker for a stage the model no longer shows are never added.
TEST(SceneGraphModel, AnExpandReadForAReplacedStageIsDropped)
{
    ModelTestMessageScope messages;
    std::shared_ptr<GatedStageSource> replaced = std::make_shared<GatedStageSource>();
    std::shared_ptr<TestStageSource> shown = std::make_shared<TestStageSource>();
    replaced->sync(SceneOps::appendLayer(ScenePayload(), groupLayer("Group", { "Gone0", "Gone1" }), 1));
    shown->sync(SceneOps::appendLayer(ScenePayload(), groupLayer("Group", { "Kept" }), 2));

    SceneGraphModel model;
    model.setReadsOnWorker(true);
    model.setSource(replaced);
    replaced->open();
    EXPECT_TRUE(processEventsUntil([&]() { return model.indexOfPath(QString::fromUtf8("/Group"), true).isValid(); }));
    EXPECT_TRUE(processEventsUntil([&]() { return !model.hasOwedReads(); }));
    replaced->close();
    const QModelIndex replacedGroup = model.index(0, 0);
    ASSERT_TRUE(model.canFetchMore(replacedGroup));
    model.fetchMore(replacedGroup);
    EXPECT_EQ(1, model.rowCount(replacedGroup));

    model.setSource(shown);
    QStringList inserted;
    QObject::connect(&model, &QAbstractItemModel::rowsInserted, [&](const QModelIndex& parent, int first, int last) {
        for (int row = first; row <= last; ++row) {
            inserted << model.index(row, 0, parent).data().toString();
        }
    });
    // Queued behind the read the replaced stage holds.
    bool busy = false;
    EXPECT_FALSE(model.indexOfPath(QString::fromUtf8("/Group/Kept"), true, &busy).isValid());
    EXPECT_TRUE(busy);
    replaced->open();
    EXPECT_TRUE(processEventsUntil([&]() { return model.indexOfPath(QString::fromUtf8("/Group/Kept"), true).isValid(); }));
    EXPECT_TRUE(processEventsUntil([&]() { return !model.hasOwedReads(); }));

    EXPECT_GT(replaced->workerReads.load(), 0);
    EXPECT_EQ(QStringList(QString::fromUtf8("Kept")), childNames(model, model.indexOfPath(QString::fromUtf8("/Group"), false)));
    for (const QString& name : inserted) {
        EXPECT_FALSE(name.startsWith(QString::fromUtf8("Gone"))) << name.toStdString();
    }
    EXPECT_EQ(2, model.getItemCount());

    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Warning);
    EXPECT_TRUE(modelTestMessages.isEmpty()) << modelTestMessages.join(QLatin1Char('\n')).toStdString();
}

// Gathered edits touching many rows are applied over several turns of the event loop, the model
// whole between them.
TEST(SceneGraphModel, ALargeGatheredEditAppliesInSlices)
{
    ModelTestMessageScope messages;
    std::shared_ptr<TestStageSource> source = std::make_shared<TestStageSource>();
    const int groupCount = 8;
    const auto payloadOf = [groupCount](const std::vector<std::string>& children, U64 salt) {
        ScenePayload payload;
        for (int i = 0; i < groupCount; ++i) {
            payload = SceneOps::appendLayer(payload, groupLayer("G" + std::to_string(i), children), salt + (U64)i);
        }

        return payload;
    };
    source->sync(payloadOf({ "A", "B", "C" }, 10));

    SceneGraphModel model;
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Warning);
    model.setReadsOnWorker(true);
    model.setSource(source);
    for (int i = 0; i < groupCount; ++i) {
        ASSERT_TRUE(waitForPath(model, QString::fromStdString("/G" + std::to_string(i) + "/C")).isValid());
    }
    const int resets = model.getResetCount();
    const int slices = model.getApplySliceCount();

    model.setApplySliceMs(0);
    source->sync(payloadOf({ "B", "C", "D" }, 100));
    model.refresh();
    const QStringList after = QStringList() << QString::fromUtf8("B") << QString::fromUtf8("C") << QString::fromUtf8("D");
    const auto allEdited = [&]() {
        for (int i = 0; i < groupCount; ++i) {
            if (childNames(model, model.indexOfPath(QString::fromStdString("/G" + std::to_string(i)), false)) != after) {
                return false;
            }
        }

        return true;
    };
    EXPECT_TRUE(processEventsUntil(allEdited));
    EXPECT_TRUE(processEventsUntil([&]() { return !model.hasOwedReads(); }));

    EXPECT_GT(model.getApplySliceCount() - slices, 1);
    EXPECT_EQ(resets, model.getResetCount());
    EXPECT_EQ(groupCount * 4, model.getItemCount());

    EXPECT_TRUE(modelTestMessages.isEmpty()) << modelTestMessages.join(QLatin1Char('\n')).toStdString();
}

// After a reset the model holds the root alone; the rows a remembered path needs come in from the
// event loop, and deferredReadsDone() says when to ask for it again.
TEST(SceneGraphModel, RestoringAPathAfterAResetArrivesAsynchronously)
{
    ModelTestMessageScope messages;
    std::shared_ptr<GatedStageSource> source = std::make_shared<GatedStageSource>();
    std::shared_ptr<TestStageSource> other = std::make_shared<TestStageSource>();
    source->sync(referenceScene());
    other->sync(SceneOps::appendLayer(ScenePayload(), groupLayer("Group", { "A" }), 1));
    const QString ball = QString::fromUtf8("/World/Ball");

    SceneGraphModel model;
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Warning);
    model.setReadsOnWorker(true);
    model.setSource(source);
    source->open();
    ASSERT_TRUE(waitForPath(model, ball).isValid());
    source->close();

    model.setSource(other);
    EXPECT_TRUE(processEventsUntil([&]() { return !model.hasOwedReads(); }));
    const int resets = model.getResetCount();
    model.setSource(source);
    EXPECT_EQ(resets + 1, model.getResetCount());
    EXPECT_EQ(0, model.rowCount());
    EXPECT_EQ(0, model.getItemCount());

    QSignalSpy done(&model, &SceneGraphModel::deferredReadsDone);
    const int mainReads = source->mainReads.load();
    bool busy = false;
    EXPECT_FALSE(model.indexOfPath(ball, true, &busy).isValid());
    EXPECT_TRUE(busy);
    EXPECT_EQ(mainReads, source->mainReads.load());
    source->open();
    EXPECT_TRUE(processEventsUntil([&]() { return done.count() > 0; }));
    const QModelIndex restored = model.indexOfPath(ball, false);
    ASSERT_TRUE(restored.isValid());
    EXPECT_EQ(QString::fromUtf8("World"), restored.parent().data().toString());
    EXPECT_EQ(resets + 1, model.getResetCount());

    EXPECT_TRUE(modelTestMessages.isEmpty()) << modelTestMessages.join(QLatin1Char('\n')).toStdString();
}

TEST(SceneGraphModel, NoSceneEmptiesTheModel)
{
    std::shared_ptr<TestStageSource> source = std::make_shared<TestStageSource>();
    source->sync(referenceScene());
    SceneGraphModel model;

    model.setSource(source);
    model.fetchMore(QModelIndex());
    ASSERT_EQ(1, model.rowCount());
    source->sync(ScenePayload());
    model.refresh();
    EXPECT_EQ(0, model.rowCount());
    EXPECT_FALSE(model.hasChildren());
    source->sync(referenceScene());
    model.refresh();
    model.fetchMore(QModelIndex());
    EXPECT_EQ(1, model.rowCount());
}

TEST(SceneGraphModel, ALargeRootPopulatesQuickly)
{
    const int kPrims = 100000;
    PXR_NS::SdfLayerRefPtr layer = SceneOps::createNodeLayer("SceneGraphModelTest");
    {
        PXR_NS::SdfChangeBlock block;
        char name[32];
        for (int i = 0; i < kPrims; ++i) {
            std::snprintf(name, sizeof(name), "P%06d", i);
            PXR_NS::SdfPrimSpec::New(layer->GetPseudoRoot(), name, PXR_NS::SdfSpecifierDef, "Xform");
        }
    }
    std::shared_ptr<TestStageSource> source = std::make_shared<TestStageSource>();
    source->sync(SceneOps::appendLayer(ScenePayload(), layer, 5));
    SceneGraphModel model;

    QElapsedTimer timer;
    timer.start();
    model.setSource(source);
    model.fetchMore(QModelIndex());
    const qint64 elapsed = timer.elapsed();

    EXPECT_EQ(SceneGraphModel::kFetchBatchSize, model.rowCount());
    EXPECT_TRUE(model.canFetchMore(QModelIndex()));
    // Debug builds on loaded runners take tens of ms; an eager walk of the stage takes seconds.
    EXPECT_LT(elapsed, 500) << "populating the root of " << kPrims << " prims took " << elapsed << " ms";

    // A prim far down the list is found by reading the rows up to it.
    const QModelIndex last = model.indexOfPath(QString::fromUtf8("/P099999"), true);
    ASSERT_TRUE(last.isValid());
    EXPECT_EQ(kPrims - 1, last.row());
}

TEST(SceneGraphPrimWalk, ASliceEndsInsideALongChildList)
{
    const int kChildren = 100000;
    std::vector<std::string> names;
    char name[32];
    for (int i = 0; i < kChildren; ++i) {
        std::snprintf(name, sizeof(name), "P%06d", i);
        names.push_back(name);
    }
    PXR_NS::SdfLayerRefPtr layer;
    {
        PXR_NS::SdfChangeBlock block;
        layer = groupLayer("Group", names);
    }
    std::shared_ptr<TestStageSource> source = std::make_shared<TestStageSource>();
    source->sync(SceneOps::appendLayer(ScenePayload(), layer, 7));

    // A spent budget stops the slice at the first read of the clock, whatever the machine's speed.
    // The pseudo-root counts as a prim looked at but is never listed.
    SceneGraphPrimWalk walk;
    QStringList found;
    walk.start();
    stepWalk(*source, &walk, 0, INT_MAX, &found);
    EXPECT_FALSE(walk.isOver());
    EXPECT_EQ(SceneGraphPrimWalk::kPrimsPerClockRead - 1, found.size());

    QElapsedTimer timer;
    timer.start();
    stepWalk(*source, &walk, 8, INT_MAX, &found);
    const qint64 elapsed = timer.elapsed();
    EXPECT_LT(elapsed, 500) << "one slice over " << kChildren << " children took " << elapsed << " ms";

    const int slices = finishWalk(*source, &walk, 0, &found);
    EXPECT_GT(slices, 1);
    ASSERT_EQ(kChildren + 1, found.size());
    EXPECT_EQ(QString::fromUtf8("/Group"), found.front());
    for (int i = 0; i < kChildren; i += 9973) {
        std::snprintf(name, sizeof(name), "/Group/P%06d", i);
        EXPECT_EQ(QString::fromUtf8(name), found[i + 1]);
    }
    EXPECT_EQ(QString::fromUtf8("/Group/P099999"), found.back());
}

TEST(SceneGraphPrimWalk, ResumingAtEveryPrimKeepsTheOrder)
{
    QStringList listed;
    PXR_NS::SdfLayerRefPtr layer = walkFixtureLayer(&listed);
    std::shared_ptr<TestStageSource> source = std::make_shared<TestStageSource>();
    source->sync(SceneOps::appendLayer(ScenePayload(), layer, 8));

    SceneGraphPrimWalk whole;
    QStringList found;
    whole.start();
    EXPECT_EQ(1, finishWalk(*source, &whole, 1000, &found));
    EXPECT_EQ(listed, found);

    // One prim a slice: the lock could go between any two prims, instance proxies among them.
    SceneGraphPrimWalk sliced(1);
    found.clear();
    sliced.start();
    EXPECT_GE(finishWalk(*source, &sliced, 0, &found), listed.size());
    EXPECT_EQ(listed, found);
}

TEST(SceneGraphPrimWalk, TheCapLeavesTheWalkResumable)
{
    QStringList listed;
    PXR_NS::SdfLayerRefPtr layer = walkFixtureLayer(&listed);
    std::shared_ptr<TestStageSource> source = std::make_shared<TestStageSource>();
    source->sync(SceneOps::appendLayer(ScenePayload(), layer, 9));

    SceneGraphPrimWalk walk;
    QStringList found;
    walk.start();
    stepWalk(*source, &walk, 1000, 2, &found);
    EXPECT_EQ(listed.mid(0, 2), found);
    EXPECT_FALSE(walk.isOver());

    stepWalk(*source, &walk, 1000, 2, &found);
    EXPECT_EQ(2, found.size()) << "still at the cap";

    // Reaching the cap on the last listed prim leaves no more, past the inactive prim after it.
    stepWalk(*source, &walk, 1000, listed.size(), &found);
    EXPECT_EQ(listed, found);
    EXPECT_TRUE(walk.isOver());
}

TEST(SceneGraphPrimWalk, AnEditBetweenSlicesStartsOver)
{
    std::shared_ptr<TestStageSource> source = std::make_shared<TestStageSource>();
    source->sync(SceneOps::appendLayer(ScenePayload(), groupLayer("Root", { "A", "B", "C" }), 10));

    SceneGraphPrimWalk walk(1);
    QStringList found;
    walk.start();
    stepWalk(*source, &walk, 0, INT_MAX, &found);
    stepWalk(*source, &walk, 0, INT_MAX, &found);
    stepWalk(*source, &walk, 0, INT_MAX, &found);
    EXPECT_EQ((QStringList { QString::fromUtf8("/Root"), QString::fromUtf8("/Root/A") }), found);

    source->sync(SceneOps::appendLayer(ScenePayload(), groupLayer("Other", { "X" }), 11));
    finishWalk(*source, &walk, 0, &found);
    EXPECT_EQ((QStringList { QString::fromUtf8("/Other"), QString::fromUtf8("/Other/X") }), found);
}

TEST(SceneGraphPrimWalk, ARecompositionTheEditCountMissesIsSurvived)
{
    PXR_NS::SdfLayerRefPtr layer = groupLayer("Root", { "A", "B", "C" });
    PXR_NS::SdfPrimSpec::New(layer->GetPseudoRoot(), "Other", PXR_NS::SdfSpecifierDef, "Xform");
    std::shared_ptr<TestStageSource> source = std::make_shared<TestStageSource>();
    source->sync(SceneOps::appendLayer(ScenePayload(), layer, 12));
    const auto setActive = [&](const char* path, bool active) {
        source->readStage([&](const SceneStage* stage) {
            stage->getStage()->GetPrimAtPath(PXR_NS::SdfPath(path)).SetActive(active);
        });
    };

    SceneGraphPrimWalk walk(1);
    QStringList found;
    walk.start();
    stepWalk(*source, &walk, 0, INT_MAX, &found);
    stepWalk(*source, &walk, 0, INT_MAX, &found);
    stepWalk(*source, &walk, 0, INT_MAX, &found);
    const U64 editCount = source->getEditCount();

    // Recomposes a prim the walk has passed: it goes on from where it was.
    setActive("/Root/A", false);
    EXPECT_EQ(editCount, source->getEditCount());
    stepWalk(*source, &walk, 0, INT_MAX, &found);
    EXPECT_EQ((QStringList { QString::fromUtf8("/Root"), QString::fromUtf8("/Root/A"), QString::fromUtf8("/Root/B") }), found);

    // Takes the next prim away: there is no position to go back to.
    setActive("/Root", false);
    EXPECT_EQ(editCount, source->getEditCount());
    finishWalk(*source, &walk, 0, &found);
    EXPECT_EQ((QStringList { QString::fromUtf8("/Other") }), found);
}

namespace {

SceneChangedHereRowsPtr
buildChangedHereRows(const TestStageSource& source,
                     const QSet<QString>& paths)
{
    SceneChangedHereRowsBuilder builder(paths);

    while (!builder.isDone()) {
        source.readStage([&builder](const SceneStage* stage) { builder.readBatch(stage); });
    }

    return builder.take();
}

} // anonymous namespace

TEST(SceneChangedHereRows, HoldsOnlyTheMarkedPathsAndTheirAncestors)
{
    std::shared_ptr<TestStageSource> source = std::make_shared<TestStageSource>();
    source->sync(SceneOps::appendLayer(ScenePayload(), groupLayer("Root", { "A", "B", "C" }), 1));

    const SceneChangedHereRowsPtr rows = buildChangedHereRows(*source, { QString::fromUtf8("/Root"), QString::fromUtf8("/Root/C"), QString::fromUtf8("/Root/A") });

    SceneChangedHereModel model;
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Warning);

    model.setRows(rows);
    EXPECT_EQ(3, model.getRowCount());
    EXPECT_TRUE(model.contains(QString::fromUtf8("/Root/A")));
    EXPECT_FALSE(model.contains(QString::fromUtf8("/Root/B")));

    ASSERT_EQ(1, model.rowCount());
    const QModelIndex root = model.index(0, 0);
    EXPECT_EQ(QString::fromUtf8("Root"), model.data(root).toString());
    ASSERT_EQ(2, model.rowCount(root));
    const QModelIndex a = model.index(0, 0, root);
    EXPECT_EQ(QString::fromUtf8("A"), model.data(a).toString());
    EXPECT_EQ(QString::fromUtf8("C"), model.data(model.index(1, 0, root)).toString());
    EXPECT_EQ(root, model.parent(a));
    EXPECT_EQ(a, model.indexOfPath(QString::fromUtf8("/Root/A")));
    EXPECT_FALSE(model.indexOfPath(QString::fromUtf8("/Root/B")).isValid());

    model.setRows(SceneChangedHereRowsPtr());
    EXPECT_EQ(0, model.rowCount());
}

TEST(SceneChangedHereRows, DropsAPathTheSceneDoesNotList)
{
    std::shared_ptr<TestStageSource> source = std::make_shared<TestStageSource>();
    source->sync(SceneOps::appendLayer(ScenePayload(), groupLayer("Root", { "A" }), 1));

    const SceneChangedHereRowsPtr rows = buildChangedHereRows(*source, { QString::fromUtf8("/Root"), QString::fromUtf8("/Root/Gone"), QString::fromUtf8("/Root/Gone/Below") });

    ASSERT_EQ(1u, rows->rows.size());
    EXPECT_EQ(QString::fromUtf8("/Root"), rows->rows[0].path);
}

TEST(SceneChangedHereRows, ReadsNoPrimWithoutAStage)
{
    SceneChangedHereRowsBuilder builder({ QString::fromUtf8("/Root") });

    while (!builder.isDone()) {
        builder.readBatch(nullptr);
    }
    EXPECT_TRUE(builder.take()->rows.empty());
}

TEST(SceneChangedHereRows, ComparesByWhatTheyShow)
{
    std::shared_ptr<TestStageSource> source = std::make_shared<TestStageSource>();
    source->sync(SceneOps::appendLayer(ScenePayload(), groupLayer("Root", { "A", "B" }), 1));

    const QSet<QString> paths { QString::fromUtf8("/Root"), QString::fromUtf8("/Root/A") };
    const SceneChangedHereRowsPtr first = buildChangedHereRows(*source, paths);

    EXPECT_TRUE(first->hasSameRows(*buildChangedHereRows(*source, paths)));
    EXPECT_FALSE(first->hasSameRows(*buildChangedHereRows(*source, { QString::fromUtf8("/Root"), QString::fromUtf8("/Root/B") })));
}

namespace {

// The model over a PayloadScene opened unloaded, /World and Other read as a view would have them.
struct PayloadModel {
    PayloadModel()
        : source(std::make_shared<TestStageSource>(PXR_NS::UsdStage::LoadNone))
        , tester(&model, QAbstractItemModelTester::FailureReportingMode::Warning)
    {
        source->sync(scene.payload());
        model.setSource(source);
        model.indexOfPath(kB2, true);
        model.indexOfPath(kB1, true);
    }

    QModelIndex row(const QString& path)
    {
        return model.indexOfPath(path, true);
    }

    // What the panel does when a row opens: the view fetches, then the load is asked for.
    void twirl(const QString& path)
    {
        const QModelIndex index = row(path);

        while (model.canFetchMore(index)) {
            model.fetchMore(index);
        }
        source->stage.requestLoad({ PXR_NS::SdfPath(path.toStdString()) }, false);
        model.markLoading(QStringList(path));
    }

    // What the viewport's load thread and the panel do once a request is in.
    void service()
    {
        source->serviceLoads();
        model.settleLoads();
    }

    ModelTestMessageScope messages;
    PayloadScene scene;
    std::shared_ptr<TestStageSource> source;
    SceneGraphModel model;
    QAbstractItemModelTester tester;
};

} // anonymous namespace

TEST(SceneGraphModelPayloads, AnUnloadedRowCanHaveChildrenAndReadsNone)
{
    PayloadModel fixture;
    const QModelIndex b0 = fixture.row(kB0);

    ASSERT_TRUE(b0.isValid());
    EXPECT_EQ(SceneGraphModel::ePayloadStateUnloaded, b0.data(SceneGraphModel::ePayloadStateRole).toInt());
    EXPECT_EQ(SceneGraphModel::ePayloadStateNone, fixture.row(QString::fromUtf8("/World/Other")).data(SceneGraphModel::ePayloadStateRole).toInt());
    EXPECT_TRUE(fixture.model.hasChildren(b0));
    EXPECT_TRUE(fixture.model.canFetchMore(b0));
    fixture.model.fetchMore(b0);
    EXPECT_EQ(0, fixture.model.rowCount(b0));
    EXPECT_TRUE(fixture.model.hasChildren(b0));
    EXPECT_TRUE(modelTestMessages.isEmpty()) << modelTestMessages.join(QLatin1Char('\n')).toStdString();
}

TEST(SceneGraphModelPayloads, LoadingATwirledRowChangesOnlyItsRows)
{
    PayloadModel fixture;
    const QPersistentModelIndex other(fixture.row(QString::fromUtf8("/World/Other")));
    const QPersistentModelIndex b2(fixture.row(kB2));
    const QPersistentModelIndex b0(fixture.row(kB0));
    const QModelIndex b1 = fixture.row(kB1);
    // The tester reads every row it can on the first change signal outside a model change; B1 is
    // read here so that read is not counted among the rows the load changes.
    while (fixture.model.canFetchMore(b1)) {
        fixture.model.fetchMore(b1);
    }
    const int resets = fixture.model.getResetCount();
    QStringList changedParents;
    int layouts = 0;
    QObject::connect(&fixture.model, &QAbstractItemModel::rowsInserted, [&](const QModelIndex& parent, int, int) { changedParents << fixture.model.pathOf(parent); });
    QObject::connect(&fixture.model, &QAbstractItemModel::rowsRemoved, [&](const QModelIndex& parent, int, int) { changedParents << fixture.model.pathOf(parent); });
    QObject::connect(&fixture.model, &QAbstractItemModel::layoutChanged, [&]() { ++layouts; });

    fixture.twirl(kB0);

    EXPECT_EQ(SceneGraphModel::ePayloadStateLoading, QModelIndex(b0).data(SceneGraphModel::ePayloadStateRole).toInt());
    ASSERT_EQ(1, fixture.model.rowCount(b0));
    const QModelIndex loading = fixture.model.index(0, 0, b0);
    EXPECT_EQ(QModelIndex(b0), fixture.model.parent(loading));
    EXPECT_EQ(int(Qt::NoItemFlags), int(fixture.model.flags(loading)));
    EXPECT_TRUE(fixture.model.pathOf(loading).isEmpty());
    EXPECT_FALSE(fixture.model.hasChildren(loading));
    EXPECT_FALSE(fixture.model.canFetchMore(loading));

    fixture.service();

    EXPECT_EQ(SceneGraphModel::ePayloadStateLoaded, QModelIndex(b0).data(SceneGraphModel::ePayloadStateRole).toInt());
    EXPECT_EQ(QStringList(QString::fromUtf8("Geo")), childNames(fixture.model, b0));
    EXPECT_EQ(resets, fixture.model.getResetCount());
    EXPECT_EQ(0, layouts);
    EXPECT_EQ(QStringList({ kB0, kB0, kB0 }), changedParents);
    EXPECT_TRUE(other.isValid());
    EXPECT_TRUE(b2.isValid());
    EXPECT_EQ(SceneGraphModel::ePayloadStateUnloaded, fixture.model.getPayloadState(kB2));
    EXPECT_EQ(0, fixture.model.rowCount(b2));
    EXPECT_TRUE(modelTestMessages.isEmpty()) << modelTestMessages.join(QLatin1Char('\n')).toStdString();
}

TEST(SceneGraphModelPayloads, TheLoadingRowFollowsAPrimsOwnChildren)
{
    PayloadModel fixture;
    const QPersistentModelIndex b1(fixture.row(kB1));
    fixture.source->stage.requestLoad({ PXR_NS::SdfPath(kB1.toStdString()) }, false);
    fixture.model.markLoading(QStringList(kB1));

    // Marked before the row was read: the tester reads it on markLoading's dataChanged, as a view
    // would, and the row it fetches goes before the loading one.
    while (fixture.model.canFetchMore(b1)) {
        fixture.model.fetchMore(b1);
    }
    EXPECT_EQ(QStringList({ QString::fromUtf8("Own"), SceneGraphModel::tr("Loading...") }), childNames(fixture.model, b1));
    EXPECT_EQ(fixture.model.parent(fixture.model.index(1, 0, b1)), QModelIndex(b1));

    fixture.service();

    QStringList names = childNames(fixture.model, b1);
    names.sort();
    EXPECT_EQ(QStringList({ QString::fromUtf8("Geo"), QString::fromUtf8("Own") }), names);
    EXPECT_TRUE(modelTestMessages.isEmpty()) << modelTestMessages.join(QLatin1Char('\n')).toStdString();
}

TEST(SceneGraphModelPayloads, AnOpenFailureBadgesItsRowAlone)
{
    PayloadModel fixture;
    fixture.scene.removeBuilding("b0.usda");

    fixture.twirl(kB0);
    fixture.twirl(kB2);
    fixture.service();

    const QModelIndex b0 = fixture.row(kB0);
    EXPECT_EQ(SceneGraphModel::ePayloadStateFailed, fixture.model.getPayloadState(kB0));
    EXPECT_EQ(0, fixture.model.rowCount(b0));
    EXPECT_TRUE(b0.data(Qt::ToolTipRole).toString().contains(QString::fromUtf8("b0.usda")));
    EXPECT_TRUE(fixture.model.hasChildren(b0));
    EXPECT_EQ(SceneGraphModel::ePayloadStateLoaded, fixture.model.getPayloadState(kB2));

    // Asking again shows it as loading again, and it fails again with the file still gone.
    fixture.twirl(kB0);
    EXPECT_EQ(SceneGraphModel::ePayloadStateLoading, fixture.model.getPayloadState(kB0));
    fixture.service();
    EXPECT_EQ(SceneGraphModel::ePayloadStateFailed, fixture.model.getPayloadState(kB0));
    EXPECT_TRUE(modelTestMessages.isEmpty()) << modelTestMessages.join(QLatin1Char('\n')).toStdString();
}

TEST(SceneGraphModelPayloads, CancellingEndsTheLoadingRow)
{
    PayloadModel fixture;

    fixture.twirl(kB0);
    fixture.source->stage.cancelPendingLoads();
    fixture.model.settleLoads();

    EXPECT_EQ(SceneGraphModel::ePayloadStateUnloaded, fixture.model.getPayloadState(kB0));
    EXPECT_EQ(0, fixture.model.rowCount(fixture.row(kB0)));
    EXPECT_TRUE(modelTestMessages.isEmpty()) << modelTestMessages.join(QLatin1Char('\n')).toStdString();
}

TEST(SceneGraphModelPayloads, TwirlingALoadedRowAsksNothing)
{
    PayloadModel fixture;

    fixture.twirl(kB0);
    fixture.service();
    const QModelIndex b0 = fixture.row(kB0);
    const int rows = fixture.model.rowCount(b0);

    fixture.model.markLoading(QStringList(kB0));

    EXPECT_EQ(SceneGraphModel::ePayloadStateLoaded, fixture.model.getPayloadState(kB0));
    EXPECT_EQ(rows, fixture.model.rowCount(b0));
}

TEST(SceneGraphModelPayloads, ATwirlDuringACommitShowsLoadingAndReadsWhenItEnds)
{
    PayloadModel fixture;
    const QPersistentModelIndex b1(fixture.row(kB1));
    QSignalSpy done(&fixture.model, &SceneGraphModel::deferredReadsDone);

    fixture.source->busy = true;
    ASSERT_TRUE(fixture.model.canFetchMore(b1));
    fixture.model.fetchMore(b1);
    EXPECT_FALSE(fixture.model.canFetchMore(b1));
    EXPECT_TRUE(fixture.model.hasOwedReads());
    ASSERT_EQ(1, fixture.model.rowCount(b1));
    EXPECT_EQ(QString::fromUtf8("Loading..."), fixture.model.index(0, 0, b1).data().toString());

    // Still busy: nothing moves, and nothing is lost.
    fixture.model.settleLoads();
    EXPECT_TRUE(fixture.model.hasOwedReads());
    EXPECT_EQ(0, done.count());

    fixture.source->busy = false;
    fixture.model.settleLoads();
    EXPECT_FALSE(fixture.model.hasOwedReads());
    EXPECT_EQ(1, done.count());
    ASSERT_EQ(1, fixture.model.rowCount(b1));
    EXPECT_EQ(QString::fromUtf8("Own"), fixture.model.index(0, 0, b1).data().toString());
}

TEST(SceneGraphModelPayloads, ACommitInTheWayKeepsALoadingRowUntilSettled)
{
    PayloadModel fixture;

    fixture.twirl(kB0);
    fixture.source->serviceLoads();
    fixture.source->busy = true;
    fixture.model.settleLoads();
    EXPECT_TRUE(fixture.model.hasOwedReads());
    EXPECT_EQ(SceneGraphModel::ePayloadStateLoading, fixture.model.getPayloadState(kB0));

    // Whichever edit was in the way, the refresh it ends with takes the outcome in.
    fixture.source->busy = false;
    fixture.model.refresh();
    EXPECT_FALSE(fixture.model.hasOwedReads());
    EXPECT_EQ(SceneGraphModel::ePayloadStateLoaded, fixture.model.getPayloadState(kB0));
}

TEST(SceneGraphModelPayloads, IndexOfPathReportsABusyStage)
{
    PayloadModel fixture;
    QSignalSpy done(&fixture.model, &SceneGraphModel::deferredReadsDone);
    const QString own = kB1 + QString::fromUtf8("/Own");
    bool busy = false;

    fixture.source->busy = true;
    EXPECT_FALSE(fixture.model.indexOfPath(own, true, &busy).isValid());
    EXPECT_TRUE(busy);

    fixture.source->busy = false;
    fixture.model.refresh();
    EXPECT_EQ(1, done.count());
    busy = false;
    EXPECT_TRUE(fixture.model.indexOfPath(own, false, &busy).isValid());
    EXPECT_FALSE(busy);
}

TEST(SceneGraphModelPayloads, UnloadRemovesRowsAndKeepsTheBadge)
{
    PayloadModel fixture;

    fixture.twirl(kB0);
    fixture.service();
    const QPersistentModelIndex b0(fixture.row(kB0));
    const QPersistentModelIndex geo(fixture.row(kB0 + QString::fromUtf8("/Geo")));
    ASSERT_TRUE(geo.isValid());
    const int resets = fixture.model.getResetCount();

    fixture.source->stage.requestUnload({ PXR_NS::SdfPath(kB0.toStdString()) });
    fixture.service();

    EXPECT_FALSE(geo.isValid());
    EXPECT_TRUE(b0.isValid());
    EXPECT_EQ(0, fixture.model.rowCount(b0));
    EXPECT_EQ(SceneGraphModel::ePayloadStateUnloaded, fixture.model.getPayloadState(kB0));
    EXPECT_TRUE(fixture.model.hasChildren(b0));
    EXPECT_EQ(resets, fixture.model.getResetCount());

    // The row twirls open as it did the first time.
    fixture.twirl(kB0);
    fixture.service();
    EXPECT_EQ(QStringList(QString::fromUtf8("Geo")), childNames(fixture.model, b0));
    EXPECT_TRUE(modelTestMessages.isEmpty()) << modelTestMessages.join(QLatin1Char('\n')).toStdString();
}

namespace {

// Stands in for the viewer the panel is linked to: its draw states, and the edits the column asks of it.
struct DrawStatesViewer {
    SceneDrawStates states;
    // As if an edit of many rows held them.
    bool busy = false;
    QStringList writes;
    SceneGraphModel* model = nullptr;

    SceneGraphModel::DrawStatesReader reader()
    {
        return [this](const std::function<void(const SceneDrawStates&)>& read) {
            if (busy) {
                return false;
            }
            read(states);

            return true;
        };
    }

    SceneGraphModel::DrawStateWriter writer()
    {
        return [this](const QString& path, int state) {
            writes << QString::fromUtf8("%1=%2").arg(path).arg(state);
            const std::string p = path.toStdString();
            if (state < 0) {
                states.clearStatesBelow(p);
                states.clearState(p);
            } else {
                states.setState(p, (SceneDrawStateEnum)state);
            }
            model->refreshDrawStates();
        };
    }
};

QModelIndex
drawCell(const QModelIndex& row)
{
    return row.sibling(row.row(), SceneGraphModel::eColumnDrawState);
}

} // anon namespace

TEST(SceneGraphModel, TheDrawStateColumnShowsTheViewersStatesAndEditsThem)
{
    ModelTestMessageScope messages;
    std::shared_ptr<TestStageSource> source = std::make_shared<TestStageSource>();
    source->sync(referenceScene());
    SceneGraphModel model;
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Warning);
    model.setSource(source);
    if (model.canFetchMore(QModelIndex())) {
        model.fetchMore(QModelIndex());
    }
    const QModelIndex world = model.index(0, SceneGraphModel::eColumnName);
    if (model.canFetchMore(world)) {
        model.fetchMore(world);
    }
    ASSERT_EQ(QStringList() << QString::fromUtf8("Box") << QString::fromUtf8("Ball") << QString::fromUtf8("Cam"), childNames(model, world));
    EXPECT_EQ(QString::fromUtf8("Draw State"), model.headerData(SceneGraphModel::eColumnDrawState, Qt::Horizontal).toString());

    // Without a viewer the column is empty and cannot be edited.
    EXPECT_FALSE(drawCell(world).data().isValid());
    EXPECT_FALSE(model.flags(drawCell(world)) & Qt::ItemIsEditable);
    EXPECT_FALSE(model.setData(drawCell(world), (int)eSceneDrawStateBBox));

    DrawStatesViewer viewer;
    viewer.model = &model;
    const U64 editCount = source->getEditCount();
    QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
    model.setDrawStateAccess(viewer.reader(), viewer.writer());
    EXPECT_FALSE(changed.isEmpty());
    EXPECT_TRUE(model.flags(drawCell(world)) & Qt::ItemIsEditable);
    EXPECT_FALSE(model.flags(world) & Qt::ItemIsEditable);

    // Nothing set: every row draws in full, as the default has it.
    const QModelIndex box = model.index(0, 0, world);
    const QModelIndex ball = model.index(1, 0, world);
    const QModelIndex cam = model.index(2, 0, world);
    EXPECT_EQ(QString::fromUtf8("Full"), drawCell(box).data().toString());
    EXPECT_EQ((int)eSceneDrawStateFull, box.data(SceneGraphModel::eDrawStateRole).toInt());
    EXPECT_TRUE(box.data(SceneGraphModel::eDrawStateInheritedRole).toBool());
    EXPECT_TRUE(drawCell(box).data(Qt::FontRole).value<QFont>().italic());

    // A state set on a row reads as set; the rows below it inherit what it implies.
    EXPECT_TRUE(model.setData(drawCell(world), (int)eSceneDrawStateChildren));
    EXPECT_EQ(QStringList(QString::fromUtf8("/World=%1").arg((int)eSceneDrawStateChildren)), viewer.writes);
    EXPECT_EQ(QString::fromUtf8("Children"), drawCell(world).data().toString());
    EXPECT_FALSE(world.data(SceneGraphModel::eDrawStateInheritedRole).toBool());
    EXPECT_FALSE(drawCell(world).data(Qt::FontRole).value<QFont>().italic());
    EXPECT_EQ(QString::fromUtf8("BBox"), drawCell(ball).data().toString());
    EXPECT_TRUE(ball.data(SceneGraphModel::eDrawStateInheritedRole).toBool());

    // Full deep down switches the boxing ancestor to Children; the siblings stay boxed.
    EXPECT_TRUE(model.setData(drawCell(world), (int)eSceneDrawStateBBox));
    EXPECT_EQ(QString::fromUtf8("BBox"), drawCell(world).data().toString());
    EXPECT_TRUE(model.setData(drawCell(ball), (int)eSceneDrawStateFull));
    EXPECT_EQ(QString::fromUtf8("Children"), drawCell(world).data().toString());
    EXPECT_EQ(QString::fromUtf8("Full"), drawCell(ball).data().toString());
    EXPECT_FALSE(ball.data(SceneGraphModel::eDrawStateInheritedRole).toBool());
    EXPECT_EQ(QString::fromUtf8("BBox"), drawCell(box).data().toString());
    EXPECT_EQ(QString::fromUtf8("BBox"), drawCell(cam).data().toString());

    EXPECT_TRUE(model.setData(drawCell(cam), (int)eSceneDrawStateLoadAll));
    EXPECT_EQ(QString::fromUtf8("Load All"), drawCell(cam).data().toString());
    EXPECT_TRUE(model.setData(drawCell(box), (int)eSceneDrawStateHidden));
    EXPECT_EQ(QString::fromUtf8("Hidden"), drawCell(box).data().toString());

    // Out of range is refused without reaching the viewer.
    const int writes = viewer.writes.size();
    EXPECT_FALSE(model.setData(drawCell(box), 99));
    EXPECT_FALSE(model.setData(drawCell(box), QString::fromUtf8("full")));
    EXPECT_FALSE(model.setData(box, (int)eSceneDrawStateBBox));
    EXPECT_EQ(writes, viewer.writes.size());

    // Reset to Graph on a row clears it and everything below.
    EXPECT_TRUE(model.setData(drawCell(world), -1));
    for (const QModelIndex& row : { world, box, ball, cam }) {
        EXPECT_EQ(QString::fromUtf8("Full"), drawCell(row).data().toString());
        EXPECT_TRUE(row.data(SceneGraphModel::eDrawStateInheritedRole).toBool());
    }

    // While an edit holds the states the column shows nothing rather than wait; the change it
    // makes reads them again.
    viewer.states.setState("/World", eSceneDrawStateBBox);
    viewer.busy = true;
    model.refreshDrawStates();
    EXPECT_FALSE(drawCell(world).data().isValid());
    viewer.busy = false;
    model.refreshDrawStates();
    EXPECT_EQ(QString::fromUtf8("BBox"), drawCell(world).data().toString());

    // The column reads only the viewer.
    EXPECT_EQ(editCount, source->getEditCount());

    model.setDrawStateAccess(SceneGraphModel::DrawStatesReader(), SceneGraphModel::DrawStateWriter());
    EXPECT_FALSE(drawCell(world).data().isValid());
    EXPECT_FALSE(model.flags(drawCell(world)) & Qt::ItemIsEditable);
    EXPECT_TRUE(modelTestMessages.isEmpty()) << modelTestMessages.join(QLatin1Char('\n')).toStdString();
}
