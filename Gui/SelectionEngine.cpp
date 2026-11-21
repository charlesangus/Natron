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

#include "SelectionEngine.h"

#include <algorithm>
#include <cstdint>

#ifdef emit
#define NATRON_SELECTIONENGINE_CPP_RESTORE_EMIT
#undef emit
#endif
#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/aov.h>
#include <pxr/imaging/hd/legacyRenderControlInterface.h>
#include <pxr/imaging/hd/legacyTaskSchema.h>
#include <pxr/imaging/hd/renderBuffer.h>
#include <pxr/imaging/hd/renderBufferSchema.h>
#include <pxr/imaging/hd/renderer.h>
#include <pxr/imaging/hdSt/tokens.h>
#include <pxr/imaging/hdx/taskControllerSceneIndex.h>
#ifdef NATRON_SELECTIONENGINE_CPP_RESTORE_EMIT
#undef NATRON_SELECTIONENGINE_CPP_RESTORE_EMIT
#define emit
#endif

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {
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
} // anonymous namespace

SelectionEngine::SelectionEngine(const Parameters& parameters)
    : SelectionEngine(SceneSelectionTasks::Arming(), parameters)
{
}

// The Arming outlives the base's construction, during which the renderer's scene index chain is
// built and the plugin runs.
SelectionEngine::SelectionEngine(SceneSelectionTasks::Arming&& arming,
                                 const Parameters& parameters)
    : UsdImagingGLEngine(parameters)
    , _tasks(arming.take())
{
    if (_taskControllerSceneIndex) {
        _armedController = HdSceneIndexBasePtr(get_pointer(_taskControllerSceneIndex));
    }
}

SelectionEngine::~SelectionEngine()
{
}

bool
SelectionEngine::hasSelectionPass() const
{
    return bool(_tasks) && bool(_renderer) && bool(_taskControllerSceneIndex) && bool(_armedController) && (get_pointer(_armedController) == static_cast<HdSceneIndexBase*>(get_pointer(_taskControllerSceneIndex)));
}

void
SelectionEngine::setSelection(const SdfPathVector& selection)
{
    _roots.clear();
    _roots.reserve(selection.size());
    for (const SdfPath& path : selection) {
        if (path.IsAbsolutePath() && path.IsPrimPath()) {
            _roots.push_back(path.ReplacePrefix(SdfPath::AbsoluteRootPath(), _sceneDelegateId));
        }
    }
    std::sort(_roots.begin(), _roots.end());
    _roots.erase(std::unique(_roots.begin(), _roots.end()), _roots.end());
    _selectionChanged = true;
}

bool
SelectionEngine::renderSelection(const UsdImagingGLRenderParams& params)
{
    if (!hasSelectionPass() || _roots.empty()) {
        return false;
    }
    const HdSceneIndexBaseRefPtr controller = _taskControllerSceneIndex;
    VtValue base;
    if (!beautyParams(controller, _taskControllerSceneIndex->GetRenderingTaskPaths(), &base)) {
        return false;
    }
    BufferDescription color;
    BufferDescription depth;
    if (!bufferDescription(controller, _taskControllerSceneIndex->GetRenderBufferPath(HdAovTokens->color), &color) || !bufferDescription(controller, _taskControllerSceneIndex->GetRenderBufferPath(HdAovTokens->depth), &depth) || !color.matches(depth)) {
        return false;
    }

    if (_selectionChanged || (_renderCollection != _selectedFrom)) {
        HdRprimCollection collection = _renderCollection;
        collection.SetRootPaths(_roots);
        _tasks->setSelection(collection);
        _selectedFrom = _renderCollection;
        _selectionChanged = false;
    }
    TfTokenVector renderTags;
    _ComputeRenderTags(params, &renderTags);

    _tasks->setSize(color.dimensions, color.format, depth.format, color.multiSampled, color.sampleCount);
    _tasks->setRenderTags(renderTags);
    _tasks->setRenderParams(base.UncheckedGet<HdxRenderTaskParams>());
    _tasks->beginPass();
    _Execute(params, _tasks->getTaskPaths());

    return _tasks->hasDrawn();
}

HgiTextureHandle
SelectionEngine::bufferTexture(const SdfPath& path) const
{
    if (!hasSelectionPass()) {
        return HgiTextureHandle();
    }
    HdLegacyRenderControlInterface* const control = _renderer->GetLegacyRenderControl();
    HdRenderBuffer* const buffer = control ? control->GetRenderBuffer(path) : nullptr;
    if (!buffer) {
        return HgiTextureHandle();
    }
    const VtValue resource = buffer->GetResource(false);

    return resource.IsHolding<HgiTextureHandle>() ? resource.UncheckedGet<HgiTextureHandle>() : HgiTextureHandle();
}

HgiTextureHandle
SelectionEngine::colorTexture() const
{
    return _tasks ? bufferTexture(_tasks->getColorBufferPath()) : HgiTextureHandle();
}

HgiTextureHandle
SelectionEngine::depthTexture() const
{
    return _tasks ? bufferTexture(_tasks->getDepthBufferPath()) : HgiTextureHandle();
}

NATRON_NAMESPACE_EXIT
