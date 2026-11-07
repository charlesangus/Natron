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

#include "UtilityRenderPass.h"

#include <algorithm>
#include <cstddef>
#include <vector>

#include <pxr/base/gf/rect2i.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/imaging/cameraUtil/framing.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/tokens.h>

#include "Engine/AppManager.h"
#include "Engine/Nodes/Scene/Utility/Tracer.h"
#include "Engine/Nodes/Scene/Utility/UtilityRenderBuffer.h"
#include "Engine/Nodes/Scene/Utility/UtilityRenderDelegate.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {
WavefrontRect
renderWindow(const HdRenderPassState& state)
{
    WavefrontRect window;
    const CameraUtilFraming& framing = state.GetFraming();

    if (framing.IsValid()) {
        window.x = framing.dataWindow.GetMinX();
        window.y = framing.dataWindow.GetMinY();
        window.width = framing.dataWindow.GetWidth();
        window.height = framing.dataWindow.GetHeight();
    } else {
        const GfVec4f& viewport = state.GetViewport();

        window.x = static_cast<int>(viewport[0]);
        window.y = static_cast<int>(viewport[1]);
        window.width = static_cast<int>(viewport[2]);
        window.height = static_cast<int>(viewport[3]);
    }

    return window;
}

bool
sameWindow(const WavefrontRect& a,
           const WavefrontRect& b)
{
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

// Writes tiles to the buffers bound under the AOV names the shading produces. Any other AOV keeps its
// clear value.
class AovFilm : public WavefrontFilm {
public:
    explicit AovFilm(const HdRenderPassAovBindingVector& bindings)
    {
        for (const HdRenderPassAovBinding& binding : bindings) {
            UtilityRenderBuffer* buffer = dynamic_cast<UtilityRenderBuffer*>(binding.renderBuffer);

            if (!buffer) {
                continue;
            }
            if (binding.aovName == HdAovTokens->color) {
                _color.push_back(buffer);
            } else if (binding.aovName == HdAovTokens->depth) {
                _depth.push_back(buffer);
            } else if (binding.aovName == HdAovTokens->primId) {
                _primId.push_back(buffer);
            } else if (binding.aovName == HdAovTokens->instanceId) {
                _instanceId.push_back(buffer);
            }
        }
    }

    virtual void writeTile(const WavefrontTile& tile) OVERRIDE
    {
        std::size_t index = 0;

        for (int row = 0; row < tile.rect.height; ++row) {
            const unsigned int y = static_cast<unsigned int>(tile.rect.y + row);

            for (int column = 0; column < tile.rect.width; ++column, ++index) {
                if (tile.hitGeometry[index] == Tracer::kNoHit) {
                    continue;
                }
                const unsigned int x = static_cast<unsigned int>(tile.rect.x + column);

                for (UtilityRenderBuffer* buffer : _color) {
                    buffer->setPixel(x, y, tile.color + index * 4, 4);
                }
                for (UtilityRenderBuffer* buffer : _depth) {
                    buffer->setPixel(x, y, tile.depth + index, 1);
                }
                for (UtilityRenderBuffer* buffer : _primId) {
                    buffer->setPixel(x, y, tile.primId[index]);
                }
                for (UtilityRenderBuffer* buffer : _instanceId) {
                    buffer->setPixel(x, y, tile.instanceId[index]);
                }
            }
        }
    }

private:
    std::vector<UtilityRenderBuffer*> _color;
    std::vector<UtilityRenderBuffer*> _depth;
    std::vector<UtilityRenderBuffer*> _primId;
    std::vector<UtilityRenderBuffer*> _instanceId;
};

void
setBuffersConverged(const HdRenderPassAovBindingVector& bindings,
                    bool converged)
{
    for (const HdRenderPassAovBinding& binding : bindings) {
        UtilityRenderBuffer* buffer = dynamic_cast<UtilityRenderBuffer*>(binding.renderBuffer);

        if (buffer) {
            buffer->setConverged(converged);
        }
    }
}

bool
buffersAreConverged(const HdRenderPassAovBindingVector& bindings)
{
    for (const HdRenderPassAovBinding& binding : bindings) {
        if (binding.renderBuffer && !binding.renderBuffer->IsConverged()) {
            return false;
        }
    }

    return true;
}
} // anon namespace

UtilityRenderPass::UtilityRenderPass(HdRenderIndex* index,
                                     const HdRprimCollection& collection,
                                     UtilityRenderParam* renderParam)
    : HdRenderPass(index, collection)
    , _renderParam(renderParam)
{
}

UtilityRenderPass::~UtilityRenderPass()
{
}

bool
UtilityRenderPass::IsConverged() const
{
    return _converged.load();
}

void
UtilityRenderPass::_Execute(const HdRenderPassStateSharedPtr& renderPassState,
                            const TfTokenVector& renderTags)
{
    const UtilityRenderParam::RenderScope renderScope(*_renderParam);
    const HdRenderPassAovBindingVector& bindings = renderPassState->GetAovBindings();

    if (_renderParam->isStopped()) {
        _converged.store(false);
        setBuffersConverged(bindings, false);
        _hasDrawn = false;

        return;
    }

    _renderParam->scene()->setRenderTags(renderTags);

    const UtilityScene::Snapshot snapshot = _renderParam->scene()->snapshot();
    WavefrontFrame frame;

    frame.window = renderWindow(*renderPassState);

    const bool hasCamera = renderPassState->GetCamera() != nullptr;

    if (hasCamera) {
        frame.worldToView = renderPassState->GetWorldToViewMatrix();
        frame.projection = renderPassState->GetProjectionMatrix();
    }

    if (_hasDrawn && buffersAreConverged(bindings) && snapshot.version == _drawnVersion && frame.worldToView == _drawnWorldToView
        && frame.projection == _drawnProjection && sameWindow(frame.window, _drawnWindow) && bindings == _drawnBindings) {
        _converged.store(true);

        return;
    }

    _converged.store(false);
    setBuffersConverged(bindings, false);
    _hasDrawn = false;
    for (const HdRenderPassAovBinding& binding : bindings) {
        UtilityRenderBuffer* buffer = dynamic_cast<UtilityRenderBuffer*>(binding.renderBuffer);

        if (buffer) {
            buffer->clear(binding.clearValue);
        }
    }

    bool finished = true;

    if (hasCamera && frame.window.width > 0 && frame.window.height > 0 && !snapshot.instances.empty() && wavefrontCameraIsUsable(frame)) {
        AovFilm film(bindings);
        const int threads = std::max(1, appPTR->getNCPUsAvailableForEffect());

        finished = _renderParam->executor()->render(snapshot, frame, film, _renderParam->stopFlag(), threads);
    }
    if (!finished) {
        return;
    }

    _drawnVersion = snapshot.version;
    _drawnWorldToView = frame.worldToView;
    _drawnProjection = frame.projection;
    _drawnWindow = frame.window;
    _drawnBindings = bindings;
    _hasDrawn = true;
    setBuffersConverged(bindings, true);
    _converged.store(true);
}

NATRON_NAMESPACE_EXIT
