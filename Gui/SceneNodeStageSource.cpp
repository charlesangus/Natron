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

#include "SceneNodeStageSource.h"

#include <string>

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/SceneStage.h"
#include "Engine/Nodes/Scene/SceneWriteBarrier.h"
#include "Engine/TLSHolder.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

SceneNodeStageSource::SceneNodeStageSource(const NodePtr& node,
                                           QObject* context,
                                           const SyncedFunc& synced)
    : _node(node)
    , _context(context)
    , _synced(synced)
{
    _worker = std::thread([this]() { run(); });
}

SceneNodeStageSource::~SceneNodeStageSource()
{
    stop();
    SceneWriteBarrier::ReadScope readScope;
    std::lock_guard<std::mutex> lock(_stageMutex);
    _stage.reset();
}

void
SceneNodeStageSource::stop()
{
    {
        std::lock_guard<std::mutex> lock(_queueMutex);
        _quit = true;
    }
    _queueChanged.notify_all();
    if (_worker.joinable()) {
        _worker.join();
    }
}

void
SceneNodeStageSource::readStage(const std::function<void(const SceneStage* stage)>& read) const
{
    SceneWriteBarrier::ReadScope readScope;
    std::lock_guard<std::mutex> lock(_stageMutex);

    read((_shown && _stage && _stage->isValid()) ? _stage.get() : nullptr);
}

U64
SceneNodeStageSource::getEditCount() const
{
    return _editCount;
}

NodePtr
SceneNodeStageSource::getNode() const
{
    return _node.lock();
}

void
SceneNodeStageSource::request(double time)
{
    {
        std::lock_guard<std::mutex> lock(_queueMutex);
        _pending = true;
        _pendingTime = time;
    }
    _queueChanged.notify_one();
}

void
SceneNodeStageSource::run()
{
    for (;;) {
        double time = 0.;
        {
            std::unique_lock<std::mutex> lock(_queueMutex);
            _queueChanged.wait(lock, [this]() { return _quit || _pending; });
            if (_quit) {
                return;
            }
            _pending = false;
            time = _pendingTime;
        }
        NodePtr node = _node.lock();
        EffectInstancePtr effect = node ? node->getEffectInstance() : EffectInstancePtr();
        ScenePayload payload;
        std::string error;
        bool ok = false;
        if (effect && node->isActivated() && effect->producesSceneData()) {
            ok = (effect->evaluateScene(EffectInstance::EvaluateSceneArgs(time, ViewIdx(0)), &payload, &error) == EffectInstance::eRenderRoIRetCodeOk);
        } else {
            error = "The node puts out no scene.";
        }
        node.reset();
        effect.reset();
        appPTR->getAppTLS()->cleanupTLSForThread();
        {
            SceneWriteBarrier::ReadScope readScope;
            std::lock_guard<std::mutex> lock(_stageMutex);
            if (!_stage) {
                _stage.reset(new SceneStage);
            }
            _stage->sync(ok ? payload : ScenePayload());
            _shown = ok && !payload.isEmpty();
            _editCount = _stage->getEditCount();
        }
        std::weak_ptr<SceneNodeStageSource> self = weak_from_this();
        const QString qerror = QString::fromStdString(error);
        QMetaObject::invokeMethod(_context, [self, ok, qerror]() {
            std::shared_ptr<SceneNodeStageSource> source = self.lock();
            if (source && source->_synced) {
                source->_synced(ok, qerror);
            } }, Qt::QueuedConnection);
    }
}

NATRON_NAMESPACE_EXIT
