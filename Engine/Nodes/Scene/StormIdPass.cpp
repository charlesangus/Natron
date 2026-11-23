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

#include "StormIdPass.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#ifdef emit
#define NATRON_STORMIDPASS_CPP_RESTORE_EMIT
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
#include <pxr/imaging/hdx/taskControllerSceneIndex.h>
#ifdef NATRON_STORMIDPASS_CPP_RESTORE_EMIT
#undef NATRON_STORMIDPASS_CPP_RESTORE_EMIT
#define emit
#endif

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {
// Storm's renderer display name, the one its scene-index plugins are registered under.
const char* const kStormDisplayName = "GL";

thread_local StormIdTasks::Arming* tArmed = nullptr;

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

// The probe task is executed alone, before the private depth buffer exists, so that an opaque
// scene never has that buffer made; it never draws, so its bindings are the engine's own.
class IdPassRenderTask
    : public HdxRenderTask {
public:
    IdPassRenderTask(HdSceneDelegate* delegate,
                     const SdfPath& id,
                     const std::shared_ptr<bool>& drawn,
                     bool probe)
        : HdxRenderTask(delegate, id)
        , _drawn(drawn)
        , _probe(probe)
    {
    }

    void Prepare(HdTaskContext* ctx,
                 HdRenderIndex* renderIndex) override
    {
        HdxRenderTask::Prepare(ctx, renderIndex);
        // HdSt_RenderPass::HasDrawItems() only asks whether any rprim in the index has the
        // collection's material tag and the task's render tags, whatever the root paths.
        if (_probe && !*_drawn && _HasDrawItems() && !renderIndex->GetDrawItems(_collection, GetRenderTags()).empty()) {
            *_drawn = true;
        }
    }

    void Execute(HdTaskContext* ctx) override
    {
        if (!_probe && *_drawn) {
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
    const bool _probe;
    HdRprimCollection _collection;
};

class IdPassRenderTaskFactory
    : public HdLegacyTaskFactory {
public:
    IdPassRenderTaskFactory(const std::shared_ptr<bool>& drawn,
                            bool probe)
        : _drawn(drawn)
        , _probe(probe)
    {
    }

    HdTaskSharedPtr Create(HdSceneDelegate* delegate,
                           const SdfPath& id) override
    {
        return std::make_shared<IdPassRenderTask>(delegate, id, _drawn, _probe);
    }

private:
    std::shared_ptr<bool> _drawn;
    const bool _probe;
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

// The material tags whose prims write the engine's depth in its opaque passes.
const TfTokenVector&
depthMaterialTags()
{
    static const TfTokenVector tags = {
        HdStMaterialTagTokens->defaultMaterialTag,
        HdStMaterialTagTokens->masked,
    };

    return tags;
}

std::atomic<std::uint64_t> gDepthBuffersAdded(0);

SdfPath
nextPrefix()
{
    static std::atomic<unsigned> count(0);

    return SdfPath(TfStringPrintf("/_NatronIdPass%u", ++count));
}

// The parameters of the engine's render task for the default material tag, which carry the camera,
// framing, shading and multisampling it last rendered with.
bool
beautyParams(const HdSceneIndexBaseRefPtr& controller,
             const SdfPathVector& taskPaths,
             VtValue* params)
{
    for (const SdfPath& path : taskPaths) {
        const HdLegacyTaskSchema task = HdLegacyTaskSchema::GetFromParent(controller->GetPrim(path).dataSource);
        const HdRprimCollectionDataSourceHandle collection = task.GetCollection();
        if (!collection || (collection->GetTypedValue(0.f).GetMaterialTag() != HdStMaterialTagTokens->defaultMaterialTag)) {
            continue;
        }
        const HdSampledDataSourceHandle parameters = task.GetParameters();
        if (!parameters) {
            continue;
        }
        *params = parameters->GetValue(0.f);
        if (params->IsHolding<HdxRenderTaskParams>()) {
            return true;
        }
    }

    return false;
}

struct BufferDescription {
    GfVec3i dimensions;
    HdFormat format = HdFormatInvalid;
    bool multiSampled = false;
    // HdStRenderBuffer's own, for a buffer that does not say.
    std::uint32_t sampleCount = 4;

    bool matches(const BufferDescription& other) const
    {
        return (dimensions == other.dimensions) && (multiSampled == other.multiSampled) && (sampleCount == other.sampleCount);
    }
};

bool
bufferDescription(const HdSceneIndexBaseRefPtr& controller,
                  const SdfPath& path,
                  BufferDescription* description)
{
    const HdRenderBufferSchema buffer = HdRenderBufferSchema::GetFromParent(controller->GetPrim(path).dataSource);
    const HdVec3iDataSourceHandle dimensionsSource = buffer.GetDimensions();
    const HdFormatDataSourceHandle formatSource = buffer.GetFormat();

    if (!dimensionsSource || !formatSource) {
        return false;
    }
    description->dimensions = dimensionsSource->GetTypedValue(0.f);
    description->format = formatSource->GetTypedValue(0.f);
    if (const HdBoolDataSourceHandle multiSampled = buffer.GetMultiSampled()) {
        description->multiSampled = multiSampled->GetTypedValue(0.f);
    }
    if (const HdContainerDataSourceHandle container = buffer.GetContainer()) {
        if (const HdTypedSampledDataSource<std::uint32_t>::Handle count = HdTypedSampledDataSource<std::uint32_t>::Cast(container->Get(HdStRenderBufferTokens->stormMsaaSampleCount))) {
            description->sampleCount = count->GetTypedValue(0.f);
        }
    }

    return (description->format != HdFormatInvalid) && (description->dimensions[0] > 0) && (description->dimensions[1] > 0);
}

HdxRenderTaskParams
unblended(const HdxRenderTaskParams& params)
{
    HdxRenderTaskParams own = params;

    own.blendEnable = false;
    own.blendColorOp = HdBlendOpAdd;
    own.blendColorSrcFactor = HdBlendFactorOne;
    own.blendColorDstFactor = HdBlendFactorZero;
    own.blendAlphaOp = HdBlendOpAdd;
    own.blendAlphaSrcFactor = HdBlendFactorOne;
    own.blendAlphaDstFactor = HdBlendFactorZero;
    own.depthMaskEnable = true;
    own.aovInputBindings.clear();
    own.aovBindings.clear();

    return own;
}

HdRenderPassAovBinding
binding(const TfToken& aov,
        const SdfPath& buffer)
{
    HdRenderPassAovBinding b;

    b.aovName = aov;
    b.renderBufferId = buffer;

    return b;
}
} // anonymous namespace

struct StormIdTasks::Implementation {
    HdRetainedSceneIndexRefPtr sceneIndex;
    SdfPathVector taskPaths;
    // The depth tasks, then the id task, as taskPaths holds them.
    std::vector<TaskSourceHandle> tasks;
    std::vector<TfToken> materialTags;
    SdfPath probePath;
    TaskSourceHandle probe;
    SdfPath depthPath;
    BufferSourceHandle depth;
    std::shared_ptr<bool> drawn = std::make_shared<bool>(false);
    HdxRenderTaskParams givenParams;
    SdfPath givenPrimId;
    SdfPath givenInstanceId;
    bool hasGivenParams = false;

    // The index tasks.size() stands for the probe.
    void dirtyTasks(const std::vector<std::size_t>& changed,
                    const HdDataSourceLocator& locator)
    {
        if (changed.empty()) {
            return;
        }
        HdSceneIndexObserver::DirtiedPrimEntries entries;
        const HdDataSourceLocatorSet locators { locator };
        for (std::size_t i : changed) {
            entries.push_back(HdSceneIndexObserver::DirtiedPrimEntry((i < taskPaths.size()) ? taskPaths[i] : probePath, locators));
        }
        sceneIndex->DirtyPrims(entries);
    }
};

StormIdTasks::Arming::Arming()
    : _previous(tArmed)
{
    registerSceneIndex();
    tArmed = this;
}

StormIdTasks::Arming::~Arming()
{
    tArmed = _previous;
}

std::shared_ptr<StormIdTasks>
StormIdTasks::Arming::take()
{
    std::shared_ptr<StormIdTasks> tasks;

    tasks.swap(_tasks);

    return tasks;
}

void
StormIdTasks::registerSceneIndex()
{
    static std::once_flag once;

    std::call_once(once, []() {
        HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
            kStormDisplayName,
            [](const std::string& /*renderInstanceId*/, const HdSceneIndexBaseRefPtr& input, const HdContainerDataSourceHandle& /*args*/) -> HdSceneIndexBaseRefPtr {
                return StormIdTasks::append(input);
            },
            nullptr,
            0,
            HdSceneIndexPluginRegistry::InsertionOrderAtStart);
    });
}

HdSceneIndexBaseRefPtr
StormIdTasks::append(const HdSceneIndexBaseRefPtr& input)
{
    Arming* const armed = tArmed;

    if (!armed || !input) {
        return input;
    }
    const SdfPath prefix = nextPrefix();
    // Replaces what an earlier call made: only the scene index chain built last feeds the engine.
    armed->_tasks = std::make_shared<StormIdTasks>(prefix);
    HdMergingSceneIndexRefPtr merged = HdMergingSceneIndex::New();
    merged->AddInputScene(input, SdfPath::AbsoluteRootPath());
    merged->AddInputScene(armed->_tasks->getSceneIndex(), prefix);

    return merged;
}

StormIdTasks::StormIdTasks(const SdfPath& prefix)
    : _imp(new Implementation)
{
    _imp->sceneIndex = HdRetainedSceneIndex::New();
    _imp->depthPath = prefix.AppendChild(TfToken("AOVs")).AppendChild(HdAovTokens->depth);

    std::vector<std::pair<TfToken, HdLegacyTaskFactorySharedPtr>> passes;
    const HdLegacyTaskFactorySharedPtr drawFactory = std::make_shared<IdPassRenderTaskFactory>(_imp->drawn, false);
    for (const TfToken& tag : depthMaterialTags()) {
        passes.push_back(std::make_pair(tag, drawFactory));
    }
    passes.push_back(std::make_pair(HdStMaterialTagTokens->translucent, drawFactory));

    HdRetainedSceneIndex::AddedPrimEntries entries;
    for (const std::pair<TfToken, HdLegacyTaskFactorySharedPtr>& pass : passes) {
        TaskSourceHandle task = TaskSource::New();
        task->factory = pass.second;
        task->collection.SetMaterialTag(pass.first);
        const bool ids = (pass.first == HdStMaterialTagTokens->translucent);
        const SdfPath path = prefix.AppendChild(TfToken((ids ? "idTask_" : "depthTask_") + pass.first.GetString()));
        entries.push_back({ path, HdPrimTypeTokens->task, HdRetainedContainerDataSource::New(HdLegacyTaskSchema::GetSchemaToken(), task) });
        _imp->taskPaths.push_back(path);
        _imp->tasks.push_back(task);
        _imp->materialTags.push_back(pass.first);
    }
    _imp->probe = TaskSource::New();
    _imp->probe->factory = std::make_shared<IdPassRenderTaskFactory>(_imp->drawn, true);
    _imp->probe->collection.SetMaterialTag(HdStMaterialTagTokens->translucent);
    _imp->probePath = prefix.AppendChild(TfToken("probeTask_" + HdStMaterialTagTokens->translucent.GetString()));
    entries.push_back({ _imp->probePath, HdPrimTypeTokens->task, HdRetainedContainerDataSource::New(HdLegacyTaskSchema::GetSchemaToken(), _imp->probe) });
    _imp->sceneIndex->AddPrims(entries);
}

StormIdTasks::~StormIdTasks()
{
}

HdSceneIndexBaseRefPtr
StormIdTasks::getSceneIndex() const
{
    return _imp->sceneIndex;
}

const SdfPathVector&
StormIdTasks::getTaskPaths() const
{
    return _imp->taskPaths;
}

const SdfPath&
StormIdTasks::getProbeTaskPath() const
{
    return _imp->probePath;
}

const SdfPath&
StormIdTasks::getDepthBufferPath() const
{
    return _imp->depthPath;
}

void
StormIdTasks::setCollection(const HdRprimCollection& collection)
{
    std::vector<std::size_t> changed;

    for (std::size_t i = 0; i < _imp->tasks.size(); ++i) {
        HdRprimCollection tagged = collection;
        tagged.SetMaterialTag(_imp->materialTags[i]);
        if (_imp->tasks[i]->collection == tagged) {
            continue;
        }
        _imp->tasks[i]->collection = tagged;
        changed.push_back(i);
    }
    HdRprimCollection probed = collection;
    probed.SetMaterialTag(HdStMaterialTagTokens->translucent);
    if (!(_imp->probe->collection == probed)) {
        _imp->probe->collection = probed;
        changed.push_back(_imp->tasks.size());
    }
    _imp->dirtyTasks(changed, HdLegacyTaskSchema::GetCollectionLocator());
}

void
StormIdTasks::setRenderTags(const TfTokenVector& renderTags)
{
    std::vector<std::size_t> changed;

    for (std::size_t i = 0; i < _imp->tasks.size(); ++i) {
        if (_imp->tasks[i]->renderTags == renderTags) {
            continue;
        }
        _imp->tasks[i]->renderTags = renderTags;
        changed.push_back(i);
    }
    if (_imp->probe->renderTags != renderTags) {
        _imp->probe->renderTags = renderTags;
        changed.push_back(_imp->tasks.size());
    }
    _imp->dirtyTasks(changed, HdLegacyTaskSchema::GetRenderTagsLocator());
}

void
StormIdTasks::setRenderParams(const HdxRenderTaskParams& params,
                              const SdfPath& primIdBuffer,
                              const SdfPath& instanceIdBuffer)
{
    if (_imp->hasGivenParams && (_imp->givenParams == params) && (_imp->givenPrimId == primIdBuffer) && (_imp->givenInstanceId == instanceIdBuffer)) {
        return;
    }
    _imp->givenParams = params;
    _imp->givenPrimId = primIdBuffer;
    _imp->givenInstanceId = instanceIdBuffer;
    _imp->hasGivenParams = true;
    std::vector<std::size_t> changed;

    for (std::size_t i = 0; i < _imp->tasks.size(); ++i) {
        HdxRenderTaskParams own = unblended(params);
        HdRenderPassAovBinding depth = binding(HdAovTokens->depth, _imp->depthPath);
        if (_imp->materialTags[i] == HdStMaterialTagTokens->translucent) {
            // A translucent fragment of any alpha leaves its ids, as Storm's pick takes them.
            own.enableAlphaToCoverage = false;
            own.aovBindings.push_back(depth);
            own.aovBindings.push_back(binding(HdAovTokens->primId, primIdBuffer));
            own.aovBindings.push_back(binding(HdAovTokens->instanceId, instanceIdBuffer));
        } else {
            if (i == 0) {
                depth.clearValue = VtValue(1.f);
            }
            own.aovBindings.push_back(depth);
        }
        if (_imp->tasks[i]->params == own) {
            continue;
        }
        _imp->tasks[i]->params = own;
        changed.push_back(i);
    }
    if (!(_imp->probe->params == params)) {
        _imp->probe->params = params;
        changed.push_back(_imp->tasks.size());
    }
    _imp->dirtyTasks(changed, HdLegacyTaskSchema::GetParametersLocator());
}

void
StormIdTasks::setSize(const GfVec3i& dimensions,
                      HdFormat depthFormat,
                      bool multiSampled,
                      std::uint32_t sampleCount)
{
    if (!_imp->depth) {
        _imp->depth = BufferSource::New(dimensions, depthFormat, multiSampled, sampleCount);
        _imp->sceneIndex->AddPrims({
            { _imp->depthPath, HdPrimTypeTokens->renderBuffer, HdRetainedContainerDataSource::New(HdRenderBufferSchema::GetSchemaToken(), _imp->depth) },
        });
        ++gDepthBuffersAdded;
        // HdxRenderSetupTask keeps the buffer it resolved a binding to until its parameters are
        // synced again, and a buffer added after a release is a new one.
        std::vector<std::size_t> all;
        for (std::size_t i = 0; i < _imp->tasks.size(); ++i) {
            all.push_back(i);
        }
        _imp->dirtyTasks(all, HdLegacyTaskSchema::GetParametersLocator());

        return;
    }
    const BufferSourceHandle& source = _imp->depth;
    HdDataSourceLocatorSet locators;
    if (source->dimensions != dimensions) {
        source->dimensions = dimensions;
        locators.insert(HdRenderBufferSchema::GetDimensionsLocator());
    }
    if (source->format != depthFormat) {
        source->format = depthFormat;
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
        _imp->sceneIndex->DirtyPrims({ HdSceneIndexObserver::DirtiedPrimEntry(_imp->depthPath, locators) });
    }
}

void
StormIdTasks::releaseDepthBuffer()
{
    if (!_imp->depth) {
        return;
    }
    _imp->sceneIndex->RemovePrims({ HdSceneIndexObserver::RemovedPrimEntry(_imp->depthPath) });
    _imp->depth.reset();
}

std::size_t
StormIdTasks::getDepthBufferBytes() const
{
    const BufferSourceHandle& source = _imp->depth;

    if (!source) {
        return 0;
    }
    std::size_t texels = 1;
    for (int i = 0; i < 3; ++i) {
        texels *= (std::size_t)std::max(1, source->dimensions[i]);
    }
    // HdStRenderBuffer keeps a single-sampled texture to resolve into beside the multisampled one.
    const std::size_t copies = source->multiSampled ? ((std::size_t)source->sampleCount + 1) : 1;

    return texels * HdDataSizeOfFormat(source->format) * copies;
}

std::uint64_t
StormIdTasks::getDepthBufferAddCount()
{
    return gDepthBuffersAdded.load();
}

void
StormIdTasks::beginPass()
{
    *_imp->drawn = false;
}

bool
StormIdTasks::hasDrawn() const
{
    return *_imp->drawn;
}

StormIdEngine::StormIdEngine(const Parameters& parameters)
    : StormIdEngine(StormIdTasks::Arming(), parameters)
{
}

// The Arming outlives the base's construction, during which the renderer's scene index chain is
// built and the plugin runs.
StormIdEngine::StormIdEngine(StormIdTasks::Arming&& arming,
                             const Parameters& parameters)
    : UsdImagingGLEngine(parameters)
    , _tasks(arming.take())
{
    if (_taskControllerSceneIndex) {
        _armedController = HdSceneIndexBasePtr(get_pointer(_taskControllerSceneIndex));
    }
}

StormIdEngine::~StormIdEngine()
{
}

bool
StormIdEngine::hasIdPass() const
{
    return bool(_tasks) && bool(_renderer) && bool(_taskControllerSceneIndex) && bool(_armedController) && (get_pointer(_armedController) == static_cast<HdSceneIndexBase*>(get_pointer(_taskControllerSceneIndex)));
}

bool
StormIdEngine::renderTranslucentIds(const UsdImagingGLRenderParams& params)
{
    if (!hasIdPass()) {
        return false;
    }
    const HdSceneIndexBaseRefPtr controller = _taskControllerSceneIndex;
    VtValue base;
    if (!beautyParams(controller, _taskControllerSceneIndex->GetRenderingTaskPaths(), &base)) {
        return false;
    }
    const SdfPath depthPath = _taskControllerSceneIndex->GetRenderBufferPath(HdAovTokens->depth);
    const SdfPath primIdPath = _taskControllerSceneIndex->GetRenderBufferPath(HdAovTokens->primId);
    const SdfPath instanceIdPath = _taskControllerSceneIndex->GetRenderBufferPath(HdAovTokens->instanceId);
    BufferDescription depth;
    BufferDescription primId;
    BufferDescription instanceId;
    if (!bufferDescription(controller, depthPath, &depth) || !bufferDescription(controller, primIdPath, &primId) || !bufferDescription(controller, instanceIdPath, &instanceId) || !depth.matches(primId) || !depth.matches(instanceId)) {
        return false;
    }

    _tasks->setCollection(_renderCollection);
    TfTokenVector renderTags;
    _ComputeRenderTags(params, &renderTags);
    _tasks->setRenderTags(renderTags);
    _tasks->setRenderParams(base.UncheckedGet<HdxRenderTaskParams>(), primIdPath, instanceIdPath);
    _tasks->beginPass();
    _Execute(params, SdfPathVector(1, _tasks->getProbeTaskPath()));
    if (!_tasks->hasDrawn()) {
        _tasks->releaseDepthBuffer();

        return false;
    }
    _tasks->setSize(depth.dimensions, depth.format, depth.multiSampled, depth.sampleCount);
    _Execute(params, _tasks->getTaskPaths());

    return true;
}

std::size_t
StormIdEngine::getIdPassBytes() const
{
    return _tasks ? _tasks->getDepthBufferBytes() : 0;
}

NATRON_NAMESPACE_EXIT
