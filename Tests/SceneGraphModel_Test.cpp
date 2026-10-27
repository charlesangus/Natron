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

#include <cstdio>
#include <memory>
#include <mutex>
#include <string>

#include <gtest/gtest.h>

#include "Gui/SceneGraphModel.h"

#include <QAbstractItemModelTester>
#include <QElapsedTimer>
#include <QMimeData>
#include <QPersistentModelIndex>
#include <QStringList>

#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneStage.h"

NATRON_NAMESPACE_USING

namespace {

// A stage the test syncs itself, read under its own lock as the viewer's is.
class TestStageSource
    : public SceneGraphStageSource {
public:
    void readStage(const std::function<void(const SceneStage* stage)>& read) const override
    {
        std::lock_guard<std::mutex> lock(mutex);

        read(stage.isValid() && !stage.getPayload().isEmpty() ? &stage : nullptr);
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

    mutable std::mutex mutex;
    SceneStage stage;
};

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
    EXPECT_LT(elapsed, 50) << "populating the root of " << kPrims << " prims took " << elapsed << " ms";

    // A prim far down the list is found by reading the rows up to it.
    const QModelIndex last = model.indexOfPath(QString::fromUtf8("/P099999"), true);
    ASSERT_TRUE(last.isValid());
    EXPECT_EQ(kPrims - 1, last.row());
}
