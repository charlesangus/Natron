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

#include "SceneSelectionTasks.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#ifdef emit
#define NATRON_SCENESELECTIONTASKS_CPP_RESTORE_EMIT
#undef emit
#endif
#include <pxr/base/tf/stringUtils.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/aov.h>
#include <pxr/imaging/hd/changeTracker.h>
#include <pxr/imaging/hd/dataSource.h>
#include <pxr/imaging/hd/dataSourceLocator.h>
#include <pxr/imaging/hd/legacyTaskFactory.h>
#include <pxr/imaging/hd/legacyTaskSchema.h>
#include <pxr/imaging/hd/mergingSceneIndex.h>
#include <pxr/imaging/hd/renderBufferSchema.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/retainedSceneIndex.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/sceneIndexPluginRegistry.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hdSt/tokens.h>
#include <pxr/imaging/hdx/renderTask.h>
#ifdef NATRON_SCENESELECTIONTASKS_CPP_RESTORE_EMIT
#undef NATRON_SCENESELECTIONTASKS_CPP_RESTORE_EMIT
#define emit
#endif

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {
// Storm's renderer display name, the one its scene-index plugins are registered under.
const char* const kStormDisplayName = "GL";

thread_local SceneSelectionTasks::Arming* tArmed = nullptr;

template <typename T>
HdDataSourceBaseHandle
retained(const T& value)
{
    return HdRetainedTypedSampledDataSource<T>::New(value);
}

// Held by the retained scene index and edited in place, so that a change is sent as a dirtied
// locator rather than as a new prim, which would make Hydra build the task again.
class TaskSource
    : public HdContainerDataSource {
public:
    HD_DECLARE_DATASOURCE(TaskSource);

    HdLegacyTaskFactorySharedPtr factory;
    HdxRenderTaskParams params;
    HdRprimCollection collection;
    TfTokenVector renderTags;

    TfTokenVector GetNames() override
    {
        static const TfTokenVector names = {
            HdLegacyTaskSchemaTokens->factory,
            HdLegacyTaskSchemaTokens->parameters,
            HdLegacyTaskSchemaTokens->collection,
            HdLegacyTaskSchemaTokens->renderTags,
        };

        return names;
    }

    HdDataSourceBaseHandle Get(const TfToken& name) override
    {
        if (name == HdLegacyTaskSchemaTokens->factory) {
            return retained(factory);
        }
        if (name == HdLegacyTaskSchemaTokens->parameters) {
            return retained(params);
        }
        if (name == HdLegacyTaskSchemaTokens->collection) {
            return retained(collection);
        }
        if (name == HdLegacyTaskSchemaTokens->renderTags) {
            return retained(renderTags);
        }

        return HdDataSourceBaseHandle();
    }

private:
    TaskSource() = default;
};

HD_DECLARE_DATASOURCE_HANDLES(TaskSource);

// HdEngine prepares every task before it executes any, so by Execute() all of them have said
// whether they have draw items. HdSt_RenderPass::HasDrawItems() only asks whether any rprim in the
// index has the collection's material tag and the task's render tags, whatever the root paths, so
// the selection's own draw items are gathered too.
class SelectionRenderTask
    : public HdxRenderTask {
public:
    SelectionRenderTask(HdSceneDelegate* delegate,
                        const SdfPath& id,
                        const std::shared_ptr<bool>& drawn)
        : HdxRenderTask(delegate, id)
        , _drawn(drawn)
    {
    }

    void Prepare(HdTaskContext* ctx,
                 HdRenderIndex* renderIndex) override
    {
        HdxRenderTask::Prepare(ctx, renderIndex);
        if (!*_drawn && _HasDrawItems() && !renderIndex->GetDrawItems(_collection, GetRenderTags()).empty()) {
            *_drawn = true;
        }
    }

    void Execute(HdTaskContext* ctx) override
    {
        if (*_drawn) {
            HdxRenderTask::Execute(ctx);
        }
    }

protected:
    void _Sync(HdSceneDelegate* delegate,
               HdTaskContext* ctx,
               HdDirtyBits* dirtyBits) override
    {
        if (*dirtyBits & HdChangeTracker::DirtyCollection) {
            const VtValue collection = delegate->Get(GetId(), HdTokens->collection);
            _collection = collection.IsHolding<HdRprimCollection>() ? collection.UncheckedGet<HdRprimCollection>() : HdRprimCollection();
        }
        HdxRenderTask::_Sync(delegate, ctx, dirtyBits);
    }

private:
    std::shared_ptr<bool> _drawn;
    HdRprimCollection _collection;
};

class SelectionRenderTaskFactory
    : public HdLegacyTaskFactory {
public:
    explicit SelectionRenderTaskFactory(const std::shared_ptr<bool>& drawn)
        : _drawn(drawn)
    {
    }

    HdTaskSharedPtr Create(HdSceneDelegate* delegate,
                           const SdfPath& id) override
    {
        return std::make_shared<SelectionRenderTask>(delegate, id, _drawn);
    }

private:
    std::shared_ptr<bool> _drawn;
};

class BufferSource
    : public HdContainerDataSource {
public:
    HD_DECLARE_DATASOURCE(BufferSource);

    GfVec3i dimensions;
    HdFormat format;
    bool multiSampled;
    std::uint32_t sampleCount;

    TfTokenVector GetNames() override
    {
        static const TfTokenVector names = {
            HdRenderBufferSchemaTokens->dimensions,
            HdRenderBufferSchemaTokens->format,
            HdRenderBufferSchemaTokens->multiSampled,
            HdStRenderBufferTokens->stormMsaaSampleCount,
        };

        return names;
    }

    HdDataSourceBaseHandle Get(const TfToken& name) override
    {
        if (name == HdRenderBufferSchemaTokens->dimensions) {
            return retained(dimensions);
        }
        if (name == HdRenderBufferSchemaTokens->format) {
            return retained(format);
        }
        if (name == HdRenderBufferSchemaTokens->multiSampled) {
            return retained(multiSampled);
        }
        if (name == HdStRenderBufferTokens->stormMsaaSampleCount) {
            return retained(sampleCount);
        }

        return HdDataSourceBaseHandle();
    }

private:
    BufferSource(const GfVec3i& dimensions,
                 HdFormat format,
                 bool multiSampled,
                 std::uint32_t sampleCount)
        : dimensions(dimensions)
        , format(format)
        , multiSampled(multiSampled)
        , sampleCount(sampleCount)
    {
    }
};

HD_DECLARE_DATASOURCE_HANDLES(BufferSource);

const TfTokenVector&
materialTags()
{
    static const TfTokenVector tags = {
        HdStMaterialTagTokens->defaultMaterialTag,
        HdStMaterialTagTokens->masked,
        HdStMaterialTagTokens->additive,
        HdStMaterialTagTokens->translucent,
    };

    return tags;
}

SdfPath
nextPrefix()
{
    static std::atomic<unsigned> count(0);

    return SdfPath(TfStringPrintf("/_NatronSelectionPass%u", ++count));
}
} // anonymous namespace

struct SceneSelectionTasks::Implementation {
    HdRetainedSceneIndexRefPtr sceneIndex;
    SdfPathVector taskPaths;
    std::vector<TaskSourceHandle> tasks;
    SdfPath colorPath;
    SdfPath depthPath;
    BufferSourceHandle color;
    BufferSourceHandle depth;
    std::shared_ptr<bool> drawn = std::make_shared<bool>(false);
    HdxRenderTaskParams givenParams;
    bool hasGivenParams = false;

    void dirtyTasks(const std::vector<std::size_t>& changed,
                    const HdDataSourceLocator& locator)
    {
        if (changed.empty()) {
            return;
        }
        HdSceneIndexObserver::DirtiedPrimEntries entries;
        const HdDataSourceLocatorSet locators { locator };
        for (std::size_t i : changed) {
            entries.push_back(HdSceneIndexObserver::DirtiedPrimEntry(taskPaths[i], locators));
        }
        sceneIndex->DirtyPrims(entries);
    }
};

SceneSelectionTasks::Arming::Arming()
    : _previous(tArmed)
{
    registerSceneIndex();
    tArmed = this;
}

SceneSelectionTasks::Arming::~Arming()
{
    tArmed = _previous;
}

std::shared_ptr<SceneSelectionTasks>
SceneSelectionTasks::Arming::take()
{
    std::shared_ptr<SceneSelectionTasks> tasks;

    tasks.swap(_tasks);

    return tasks;
}

void
SceneSelectionTasks::registerSceneIndex()
{
    static std::once_flag once;

    std::call_once(once, []() {
        HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
            kStormDisplayName,
            [](const std::string& /*renderInstanceId*/, const HdSceneIndexBaseRefPtr& input, const HdContainerDataSourceHandle& /*args*/) -> HdSceneIndexBaseRefPtr {
                return SceneSelectionTasks::append(input);
            },
            nullptr,
            0,
            HdSceneIndexPluginRegistry::InsertionOrderAtStart);
    });
}

HdSceneIndexBaseRefPtr
SceneSelectionTasks::append(const HdSceneIndexBaseRefPtr& input)
{
    Arming* const armed = tArmed;

    if (!armed || !input) {
        return input;
    }
    const SdfPath prefix = nextPrefix();
    // Replaces what an earlier call made: only the scene index chain built last feeds the engine.
    armed->_tasks = std::make_shared<SceneSelectionTasks>(prefix);
    HdMergingSceneIndexRefPtr merged = HdMergingSceneIndex::New();
    merged->AddInputScene(input, SdfPath::AbsoluteRootPath());
    merged->AddInputScene(armed->_tasks->getSceneIndex(), prefix);

    return merged;
}

SceneSelectionTasks::SceneSelectionTasks(const SdfPath& prefix)
    : _imp(new Implementation)
{
    _imp->sceneIndex = HdRetainedSceneIndex::New();
    const SdfPath buffers = prefix.AppendChild(TfToken("AOVs"));
    _imp->colorPath = buffers.AppendChild(HdAovTokens->color);
    _imp->depthPath = buffers.AppendChild(HdAovTokens->depth);

    const HdLegacyTaskFactorySharedPtr factory = std::make_shared<SelectionRenderTaskFactory>(_imp->drawn);
    HdRetainedSceneIndex::AddedPrimEntries entries;
    for (const TfToken& tag : materialTags()) {
        TaskSourceHandle task = TaskSource::New();
        task->factory = factory;
        task->collection.SetMaterialTag(tag);
        const SdfPath path = prefix.AppendChild(TfToken("renderTask_" + tag.GetString()));
        entries.push_back({ path, HdPrimTypeTokens->task, HdRetainedContainerDataSource::New(HdLegacyTaskSchema::GetSchemaToken(), task) });
        _imp->taskPaths.push_back(path);
        _imp->tasks.push_back(task);
    }
    _imp->sceneIndex->AddPrims(entries);
}

SceneSelectionTasks::~SceneSelectionTasks()
{
}

HdSceneIndexBaseRefPtr
SceneSelectionTasks::getSceneIndex() const
{
    return _imp->sceneIndex;
}

const SdfPathVector&
SceneSelectionTasks::getTaskPaths() const
{
    return _imp->taskPaths;
}

const SdfPath&
SceneSelectionTasks::getColorBufferPath() const
{
    return _imp->colorPath;
}

const SdfPath&
SceneSelectionTasks::getDepthBufferPath() const
{
    return _imp->depthPath;
}

void
SceneSelectionTasks::setSelection(const HdRprimCollection& collection)
{
    std::vector<std::size_t> changed;

    for (std::size_t i = 0; i < _imp->tasks.size(); ++i) {
        HdRprimCollection tagged = collection;
        tagged.SetMaterialTag(_imp->tasks[i]->collection.GetMaterialTag());
        if (_imp->tasks[i]->collection == tagged) {
            continue;
        }
        _imp->tasks[i]->collection = tagged;
        changed.push_back(i);
    }
    _imp->dirtyTasks(changed, HdLegacyTaskSchema::GetCollectionLocator());
}

void
SceneSelectionTasks::setRenderTags(const TfTokenVector& renderTags)
{
    if (!_imp->tasks.empty() && (_imp->tasks.front()->renderTags == renderTags)) {
        return;
    }
    std::vector<std::size_t> changed;

    for (std::size_t i = 0; i < _imp->tasks.size(); ++i) {
        if (_imp->tasks[i]->renderTags == renderTags) {
            continue;
        }
        _imp->tasks[i]->renderTags = renderTags;
        changed.push_back(i);
    }
    _imp->dirtyTasks(changed, HdLegacyTaskSchema::GetRenderTagsLocator());
}

void
SceneSelectionTasks::setRenderParams(const HdxRenderTaskParams& params)
{
    if (_imp->hasGivenParams && (_imp->givenParams == params)) {
        return;
    }
    _imp->givenParams = params;
    _imp->hasGivenParams = true;
    std::vector<std::size_t> changed;

    for (std::size_t i = 0; i < _imp->tasks.size(); ++i) {
        HdxRenderTaskParams own = params;
        own.blendEnable = false;
        own.blendColorOp = HdBlendOpAdd;
        own.blendColorSrcFactor = HdBlendFactorOne;
        own.blendColorDstFactor = HdBlendFactorZero;
        own.blendAlphaOp = HdBlendOpAdd;
        own.blendAlphaSrcFactor = HdBlendFactorOne;
        own.blendAlphaDstFactor = HdBlendFactorZero;
        own.depthFunc = HdCmpFuncLess;
        own.depthMaskEnable = true;
        own.enableAlphaToCoverage = false;
        own.aovInputBindings.clear();
        own.aovBindings.clear();
        const bool clears = (i == 0);
        HdRenderPassAovBinding color;
        color.aovName = HdAovTokens->color;
        color.renderBufferId = _imp->colorPath;
        if (clears) {
            color.clearValue = VtValue(GfVec4f(0.f, 0.f, 0.f, 0.f));
        }
        HdRenderPassAovBinding depth;
        depth.aovName = HdAovTokens->depth;
        depth.renderBufferId = _imp->depthPath;
        if (clears) {
            depth.clearValue = VtValue(1.f);
        }
        own.aovBindings.push_back(color);
        own.aovBindings.push_back(depth);
        if (_imp->tasks[i]->params == own) {
            continue;
        }
        _imp->tasks[i]->params = own;
        changed.push_back(i);
    }
    _imp->dirtyTasks(changed, HdLegacyTaskSchema::GetParametersLocator());
}

void
SceneSelectionTasks::setSize(const GfVec3i& dimensions,
                             HdFormat colorFormat,
                             HdFormat depthFormat,
                             bool multiSampled,
                             std::uint32_t sampleCount)
{
    if (!_imp->color) {
        _imp->color = BufferSource::New(dimensions, colorFormat, multiSampled, sampleCount);
        _imp->depth = BufferSource::New(dimensions, depthFormat, multiSampled, sampleCount);
        _imp->sceneIndex->AddPrims({
            { _imp->colorPath, HdPrimTypeTokens->renderBuffer, HdRetainedContainerDataSource::New(HdRenderBufferSchema::GetSchemaToken(), _imp->color) },
            { _imp->depthPath, HdPrimTypeTokens->renderBuffer, HdRetainedContainerDataSource::New(HdRenderBufferSchema::GetSchemaToken(), _imp->depth) },
        });

        return;
    }
    HdSceneIndexObserver::DirtiedPrimEntries entries;
    const auto update = [&entries, &dimensions, multiSampled, sampleCount](const SdfPath& path,
                                                                           const BufferSourceHandle& source,
                                                                           HdFormat format) {
        HdDataSourceLocatorSet locators;
        if (source->dimensions != dimensions) {
            source->dimensions = dimensions;
            locators.insert(HdRenderBufferSchema::GetDimensionsLocator());
        }
        if (source->format != format) {
            source->format = format;
            locators.insert(HdRenderBufferSchema::GetFormatLocator());
        }
        if (source->multiSampled != multiSampled) {
            source->multiSampled = multiSampled;
            locators.insert(HdRenderBufferSchema::GetMultiSampledLocator());
        }
        if (source->sampleCount != sampleCount) {
            source->sampleCount = sampleCount;
            locators.insert(HdRenderBufferSchema::GetDefaultLocator().Append(HdStRenderBufferTokens->stormMsaaSampleCount));
        }
        if (!locators.IsEmpty()) {
            entries.push_back(HdSceneIndexObserver::DirtiedPrimEntry(path, locators));
        }
    };
    update(_imp->colorPath, _imp->color, colorFormat);
    update(_imp->depthPath, _imp->depth, depthFormat);
    if (!entries.empty()) {
        _imp->sceneIndex->DirtyPrims(entries);
    }
}

void
SceneSelectionTasks::beginPass()
{
    *_imp->drawn = false;
}

bool
SceneSelectionTasks::hasDrawn() const
{
    return *_imp->drawn;
}

NATRON_NAMESPACE_EXIT
