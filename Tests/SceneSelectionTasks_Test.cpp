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

#include <algorithm>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "Gui/SceneSelectionTasks.h"

#include <pxr/base/gf/vec4f.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/aov.h>
#include <pxr/imaging/hd/legacyTaskSchema.h>
#include <pxr/imaging/hd/renderBufferSchema.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/retainedSceneIndex.h>
#include <pxr/imaging/hd/sceneIndexObserver.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hdSt/tokens.h>

NATRON_NAMESPACE_USING

PXR_NAMESPACE_USING_DIRECTIVE

// The selection pass's prims, read back through the retained scene index the engine's render index
// would take them from, and the notices each setter sends to it.

namespace {
class Recorder
    : public HdSceneIndexObserver {
public:
    HdSceneIndexObserver::AddedPrimEntries added;
    HdSceneIndexObserver::DirtiedPrimEntries dirtied;
    std::size_t removed = 0;

    void PrimsAdded(const HdSceneIndexBase& /*sender*/,
                    const AddedPrimEntries& entries) override
    {
        added.insert(added.end(), entries.begin(), entries.end());
    }

    void PrimsRemoved(const HdSceneIndexBase& /*sender*/,
                      const RemovedPrimEntries& entries) override
    {
        removed += entries.size();
    }

    void PrimsDirtied(const HdSceneIndexBase& /*sender*/,
                      const DirtiedPrimEntries& entries) override
    {
        dirtied.insert(dirtied.end(), entries.begin(), entries.end());
    }

    void PrimsRenamed(const HdSceneIndexBase& /*sender*/,
                      const RenamedPrimEntries& /*entries*/) override
    {
    }

    void clear()
    {
        added.clear();
        dirtied.clear();
        removed = 0;
    }
};

HdLegacyTaskSchema
taskAt(const SceneSelectionTasks& tasks,
       const SdfPath& path)
{
    return HdLegacyTaskSchema::GetFromParent(tasks.getSceneIndex()->GetPrim(path).dataSource);
}

HdxRenderTaskParams
paramsAt(const SceneSelectionTasks& tasks,
         const SdfPath& path)
{
    const HdSampledDataSourceHandle source = taskAt(tasks, path).GetParameters();
    if (!source) {
        ADD_FAILURE() << path << " has no parameters";

        return HdxRenderTaskParams();
    }
    const VtValue value = source->GetValue(0.f);
    EXPECT_TRUE(value.IsHolding<HdxRenderTaskParams>()) << path;

    return value.IsHolding<HdxRenderTaskParams>() ? value.UncheckedGet<HdxRenderTaskParams>() : HdxRenderTaskParams();
}

HdRprimCollection
collectionAt(const SceneSelectionTasks& tasks,
             const SdfPath& path)
{
    const HdRprimCollectionDataSourceHandle source = taskAt(tasks, path).GetCollection();

    return source ? source->GetTypedValue(0.f) : HdRprimCollection();
}

HdRprimCollection
selectionOf(const SdfPathVector& roots,
            const TfToken& repr = HdReprTokens->smoothHull)
{
    HdRprimCollection collection(HdTokens->geometry, HdReprSelector(repr));

    collection.SetRootPaths(roots);

    return collection;
}

// Every dirtied entry is one of the tasks, each at most once, and names only locator.
void
expectOnlyLocator(const Recorder& recorder,
                  const SceneSelectionTasks& tasks,
                  const HdDataSourceLocator& locator,
                  std::size_t count)
{
    EXPECT_TRUE(recorder.added.empty());
    EXPECT_EQ(0u, recorder.removed);
    ASSERT_EQ(count, recorder.dirtied.size());
    for (const HdSceneIndexObserver::DirtiedPrimEntry& entry : recorder.dirtied) {
        EXPECT_NE(tasks.getTaskPaths().end(), std::find(tasks.getTaskPaths().begin(), tasks.getTaskPaths().end(), entry.primPath)) << entry.primPath;
        EXPECT_EQ(HdDataSourceLocatorSet { locator }, entry.dirtyLocators) << entry.primPath;
    }
}

class SceneSelectionTasksTest
    : public ::testing::Test {
protected:
    void SetUp() override
    {
        _tasks.reset(new SceneSelectionTasks(SdfPath("/_Selection")));
        _tasks->getSceneIndex()->AddObserver(HdSceneIndexObserverPtr(&_recorder));
    }

    void TearDown() override
    {
        _tasks->getSceneIndex()->RemoveObserver(HdSceneIndexObserverPtr(&_recorder));
        _tasks.reset();
    }

    Recorder _recorder;
    std::unique_ptr<SceneSelectionTasks> _tasks;
};

HdSceneIndexBaseRefPtr
sceneWithAMesh()
{
    HdRetainedSceneIndexRefPtr scene = HdRetainedSceneIndex::New();

    scene->AddPrims({ { SdfPath("/World/Card"), HdPrimTypeTokens->mesh, HdRetainedContainerDataSource::New() } });

    return scene;
}
} // namespace

TEST(SceneSelectionTasksPlugin, ReturnsItsInputWhenTheThreadIsNotArmed)
{
    const HdSceneIndexBaseRefPtr input = sceneWithAMesh();

    EXPECT_EQ(get_pointer(input), get_pointer(SceneSelectionTasks::append(input)));
}

TEST(SceneSelectionTasksPlugin, MergesTheTasksBesideTheSceneWhileArmed)
{
    const HdSceneIndexBaseRefPtr input = sceneWithAMesh();
    std::shared_ptr<SceneSelectionTasks> tasks;
    HdSceneIndexBaseRefPtr merged;
    {
        SceneSelectionTasks::Arming arming;
        merged = SceneSelectionTasks::append(input);
        tasks = arming.take();
        EXPECT_FALSE(arming.take());
    }
    ASSERT_TRUE(tasks);
    ASSERT_TRUE(merged);
    EXPECT_NE(get_pointer(input), get_pointer(merged));
    EXPECT_EQ(HdPrimTypeTokens->mesh, merged->GetPrim(SdfPath("/World/Card")).primType);
    ASSERT_EQ(4u, tasks->getTaskPaths().size());
    for (const SdfPath& path : tasks->getTaskPaths()) {
        EXPECT_EQ(HdPrimTypeTokens->task, merged->GetPrim(path).primType) << path;
        EXPECT_FALSE(path.HasPrefix(SdfPath("/World"))) << path;
    }

    tasks->setSize(GfVec3i(64, 32, 1), HdFormatFloat16Vec4, HdFormatFloat32);
    EXPECT_EQ(HdPrimTypeTokens->renderBuffer, merged->GetPrim(tasks->getColorBufferPath()).primType);
    EXPECT_EQ(HdPrimTypeTokens->renderBuffer, merged->GetPrim(tasks->getDepthBufferPath()).primType);

    EXPECT_EQ(get_pointer(input), get_pointer(SceneSelectionTasks::append(input)));
}

TEST(SceneSelectionTasksPlugin, EachEngineBuiltWhileArmedGetsTasksUnderItsOwnPrefix)
{
    const HdSceneIndexBaseRefPtr input = sceneWithAMesh();
    std::shared_ptr<SceneSelectionTasks> first;
    std::shared_ptr<SceneSelectionTasks> second;
    {
        SceneSelectionTasks::Arming arming;
        SceneSelectionTasks::append(input);
        first = arming.take();
    }
    {
        SceneSelectionTasks::Arming arming;
        SceneSelectionTasks::append(input);
        second = arming.take();
    }
    ASSERT_TRUE(first && second);
    EXPECT_NE(first->getTaskPaths().front(), second->getTaskPaths().front());
    EXPECT_NE(first->getColorBufferPath(), second->getColorBufferPath());
}

TEST_F(SceneSelectionTasksTest, OneTaskPerStormMaterialTagInDrawOrder)
{
    const TfToken tags[] = {
        HdStMaterialTagTokens->defaultMaterialTag,
        HdStMaterialTagTokens->masked,
        HdStMaterialTagTokens->additive,
        HdStMaterialTagTokens->translucent,
    };

    _tasks->setSelection(selectionOf({ SdfPath("/World/Card") }));
    ASSERT_EQ(4u, _tasks->getTaskPaths().size());
    for (std::size_t i = 0; i < 4; ++i) {
        const SdfPath& path = _tasks->getTaskPaths()[i];
        EXPECT_EQ(HdPrimTypeTokens->task, _tasks->getSceneIndex()->GetPrim(path).primType) << path;
        EXPECT_TRUE(taskAt(*_tasks, path).GetFactory()) << path;
        const HdRprimCollection collection = collectionAt(*_tasks, path);
        EXPECT_EQ(tags[i], collection.GetMaterialTag()) << path;
        EXPECT_EQ(SdfPathVector { SdfPath("/World/Card") }, collection.GetRootPaths()) << path;
        EXPECT_EQ(HdTokens->geometry, collection.GetName()) << path;
    }
}

// Whatever blending and depth state the engine's own task has, the pass draws opaque with a LESS
// test into its own two buffers, and keeps the engine's camera, framing and shading.
TEST_F(SceneSelectionTasksTest, TasksDrawOpaqueIntoTheirOwnBuffersWithTheEnginesCamera)
{
    HdxRenderTaskParams engine;
    engine.camera = SdfPath("/_Engine/freeCamera");
    engine.framing = CameraUtilFraming(GfRect2i(GfVec2i(0, 0), 64, 32));
    engine.enableLighting = true;
    engine.alphaThreshold = 0.25f;
    engine.cullStyle = HdCullStyleNothing;
    engine.blendEnable = true;
    engine.blendColorDstFactor = HdBlendFactorOneMinusSrcAlpha;
    engine.depthFunc = HdCmpFuncLEqual;
    engine.depthMaskEnable = false;
    engine.enableAlphaToCoverage = true;
    engine.useAovMultiSample = true;
    HdRenderPassAovBinding engineColor;
    engineColor.aovName = HdAovTokens->color;
    engineColor.renderBufferId = SdfPath("/_Engine/AOVs/color");
    engine.aovBindings.push_back(engineColor);
    engine.aovInputBindings.push_back(engineColor);

    _tasks->setRenderParams(engine);

    for (std::size_t i = 0; i < _tasks->getTaskPaths().size(); ++i) {
        const SdfPath& path = _tasks->getTaskPaths()[i];
        const HdxRenderTaskParams params = paramsAt(*_tasks, path);
        EXPECT_EQ(engine.camera, params.camera) << path;
        EXPECT_EQ(engine.framing, params.framing) << path;
        EXPECT_TRUE(params.enableLighting) << path;
        EXPECT_EQ(0.25f, params.alphaThreshold) << path;
        EXPECT_EQ(HdCullStyleNothing, params.cullStyle) << path;
        EXPECT_FALSE(params.blendEnable) << path;
        EXPECT_EQ(HdBlendFactorZero, params.blendColorDstFactor) << path;
        EXPECT_EQ(HdCmpFuncLess, params.depthFunc) << path;
        EXPECT_TRUE(params.depthMaskEnable) << path;
        EXPECT_FALSE(params.enableAlphaToCoverage) << path;
        EXPECT_FALSE(params.useAovMultiSample) << path;
        EXPECT_TRUE(params.aovInputBindings.empty()) << path;
        ASSERT_EQ(2u, params.aovBindings.size()) << path;
        EXPECT_EQ(HdAovTokens->color, params.aovBindings[0].aovName);
        EXPECT_EQ(_tasks->getColorBufferPath(), params.aovBindings[0].renderBufferId);
        EXPECT_EQ(HdAovTokens->depth, params.aovBindings[1].aovName);
        EXPECT_EQ(_tasks->getDepthBufferPath(), params.aovBindings[1].renderBufferId);
        if (i == 0) {
            EXPECT_EQ(VtValue(GfVec4f(0.f, 0.f, 0.f, 0.f)), params.aovBindings[0].clearValue);
            EXPECT_EQ(VtValue(1.f), params.aovBindings[1].clearValue);
        } else {
            EXPECT_TRUE(params.aovBindings[0].clearValue.IsEmpty()) << path << " clears what the tasks before it drew";
            EXPECT_TRUE(params.aovBindings[1].clearValue.IsEmpty()) << path << " clears what the tasks before it drew";
        }
    }
}

TEST_F(SceneSelectionTasksTest, TheBuffersTakeTheGivenSizeAndFormats)
{
    _tasks->setSize(GfVec3i(64, 32, 1), HdFormatFloat16Vec4, HdFormatFloat32);

    EXPECT_EQ(2u, _recorder.added.size());
    const std::pair<SdfPath, HdFormat> buffers[] = {
        { _tasks->getColorBufferPath(), HdFormatFloat16Vec4 },
        { _tasks->getDepthBufferPath(), HdFormatFloat32 },
    };
    for (const std::pair<SdfPath, HdFormat>& buffer : buffers) {
        const HdSceneIndexPrim prim = _tasks->getSceneIndex()->GetPrim(buffer.first);
        EXPECT_EQ(HdPrimTypeTokens->renderBuffer, prim.primType) << buffer.first;
        const HdRenderBufferSchema schema = HdRenderBufferSchema::GetFromParent(prim.dataSource);
        ASSERT_TRUE(schema.GetDimensions() && schema.GetFormat()) << buffer.first;
        EXPECT_EQ(GfVec3i(64, 32, 1), schema.GetDimensions()->GetTypedValue(0.f)) << buffer.first;
        EXPECT_EQ(buffer.second, schema.GetFormat()->GetTypedValue(0.f)) << buffer.first;
        ASSERT_TRUE(schema.GetMultiSampled()) << buffer.first;
        EXPECT_FALSE(schema.GetMultiSampled()->GetTypedValue(0.f)) << buffer.first;
    }
}

TEST_F(SceneSelectionTasksTest, ASelectionChangeDirtiesOnlyTheCollections)
{
    _tasks->setSelection(selectionOf({ SdfPath("/World/Card") }));
    expectOnlyLocator(_recorder, *_tasks, HdLegacyTaskSchema::GetCollectionLocator(), 4);

    _recorder.clear();
    _tasks->setSelection(selectionOf({ SdfPath("/World/Card") }));
    EXPECT_TRUE(_recorder.dirtied.empty()) << "the same selection again";

    _tasks->setSelection(selectionOf({ SdfPath("/World/Backdrop"), SdfPath("/World/Card") }));
    expectOnlyLocator(_recorder, *_tasks, HdLegacyTaskSchema::GetCollectionLocator(), 4);
    EXPECT_EQ((SdfPathVector { SdfPath("/World/Backdrop"), SdfPath("/World/Card") }), collectionAt(*_tasks, _tasks->getTaskPaths()[3]).GetRootPaths());

    // Wireframe changes the engine collection's repr, and the pass follows it.
    _recorder.clear();
    _tasks->setSelection(selectionOf({ SdfPath("/World/Backdrop"), SdfPath("/World/Card") }, HdReprTokens->wire));
    expectOnlyLocator(_recorder, *_tasks, HdLegacyTaskSchema::GetCollectionLocator(), 4);
    EXPECT_EQ(HdReprTokens->wire, collectionAt(*_tasks, _tasks->getTaskPaths()[0]).GetReprSelector()[0]);
    EXPECT_EQ(HdStMaterialTagTokens->masked, collectionAt(*_tasks, _tasks->getTaskPaths()[1]).GetMaterialTag());
}

TEST_F(SceneSelectionTasksTest, ARenderTagChangeDirtiesOnlyTheRenderTags)
{
    const TfTokenVector geometry = { HdRenderTagTokens->geometry };
    const TfTokenVector withProxy = { HdRenderTagTokens->geometry, HdRenderTagTokens->proxy };

    _tasks->setRenderTags(geometry);
    expectOnlyLocator(_recorder, *_tasks, HdLegacyTaskSchema::GetRenderTagsLocator(), 4);

    _recorder.clear();
    _tasks->setRenderTags(geometry);
    EXPECT_TRUE(_recorder.dirtied.empty()) << "the same render tags again";

    _tasks->setRenderTags(withProxy);
    expectOnlyLocator(_recorder, *_tasks, HdLegacyTaskSchema::GetRenderTagsLocator(), 4);
    const HdTokenVectorDataSourceHandle tags = taskAt(*_tasks, _tasks->getTaskPaths()[2]).GetRenderTags();
    ASSERT_TRUE(tags);
    EXPECT_EQ(withProxy, tags->GetTypedValue(0.f));
}

TEST_F(SceneSelectionTasksTest, ACameraChangeDirtiesOnlyTheParameters)
{
    HdxRenderTaskParams engine;
    engine.camera = SdfPath("/_Engine/freeCamera");

    _tasks->setRenderParams(engine);
    expectOnlyLocator(_recorder, *_tasks, HdLegacyTaskSchema::GetParametersLocator(), 4);

    _recorder.clear();
    engine.blendEnable = true;
    engine.depthMaskEnable = false;
    _tasks->setRenderParams(engine);
    EXPECT_TRUE(_recorder.dirtied.empty()) << "only state the pass overrides changed";

    engine.camera = SdfPath("/World/Cam");
    _tasks->setRenderParams(engine);
    expectOnlyLocator(_recorder, *_tasks, HdLegacyTaskSchema::GetParametersLocator(), 4);
}

TEST_F(SceneSelectionTasksTest, AResizeDirtiesOnlyWhatChanged)
{
    _tasks->setSize(GfVec3i(64, 32, 1), HdFormatFloat16Vec4, HdFormatFloat32);
    _recorder.clear();

    _tasks->setSize(GfVec3i(64, 32, 1), HdFormatFloat16Vec4, HdFormatFloat32);
    EXPECT_TRUE(_recorder.added.empty());
    EXPECT_TRUE(_recorder.dirtied.empty()) << "the same size again";

    _tasks->setSize(GfVec3i(128, 32, 1), HdFormatFloat16Vec4, HdFormatFloat32);
    EXPECT_TRUE(_recorder.added.empty());
    ASSERT_EQ(2u, _recorder.dirtied.size());
    for (const HdSceneIndexObserver::DirtiedPrimEntry& entry : _recorder.dirtied) {
        EXPECT_EQ(HdDataSourceLocatorSet { HdRenderBufferSchema::GetDimensionsLocator() }, entry.dirtyLocators) << entry.primPath;
    }

    _recorder.clear();
    _tasks->setSize(GfVec3i(128, 32, 1), HdFormatFloat32Vec4, HdFormatFloat32);
    ASSERT_EQ(1u, _recorder.dirtied.size());
    EXPECT_EQ(_tasks->getColorBufferPath(), _recorder.dirtied[0].primPath);
    EXPECT_EQ(HdDataSourceLocatorSet { HdRenderBufferSchema::GetFormatLocator() }, _recorder.dirtied[0].dirtyLocators);
}
