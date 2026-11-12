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

#include "SceneMenuRefresh.h"

#include <string>
#include <utility>

#include <QCoreApplication>
#include <QMetaObject>
#include <QPointer>
#include <QThreadPool>

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/ScenePayload.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

struct SceneMenuRefresh::State {
    // Moves with every request, so a Read that finishes behind a newer request is told apart.
    U64 generation = 0;
    U64 applyCount = 0;
    bool running = false;
    Prepare pending;
    QPointer<QObject> pendingContext;
};

namespace {
// Behind every render: a menu waits rather than delay a frame.
const int kReadPriority = -1;

void start(const std::shared_ptr<SceneMenuRefresh::State>& state, QObject* context, const SceneMenuRefresh::Prepare& prepare);

void
finish(const std::shared_ptr<SceneMenuRefresh::State>& state,
       U64 generation,
       const SceneMenuRefresh::Apply& apply,
       const QPointer<QObject>& receiver)
{
    state->running = false;
    if ((generation == state->generation) && receiver && apply) {
        apply();
        ++state->applyCount;
    }
    if (!state->pending) {
        return;
    }
    SceneMenuRefresh::Prepare next;
    std::swap(next, state->pending);
    const QPointer<QObject> nextContext = state->pendingContext;
    state->pendingContext.clear();
    if (nextContext) {
        start(state, nextContext.data(), next);
    }
}

void
start(const std::shared_ptr<SceneMenuRefresh::State>& state,
      QObject* context,
      const SceneMenuRefresh::Prepare& prepare)
{
    const U64 generation = ++state->generation;
    SceneMenuRefresh::Read read = prepare ? prepare() : SceneMenuRefresh::Read();
    if (!read) {
        return;
    }
    if (!QCoreApplication::instance()) {
        const SceneMenuRefresh::Apply apply = read();
        if (apply) {
            apply();
            ++state->applyCount;
        }

        return;
    }
    state->running = true;
    QPointer<QObject> receiver(context);
    QThreadPool::globalInstance()->start([state, generation, read = std::move(read), receiver = std::move(receiver)]() mutable {
        SceneMenuRefresh::Apply apply;
        try {
            apply = read();
        } catch (...) {
            apply = SceneMenuRefresh::Apply();
        }
        // What the Read holds goes here rather than wait for the main thread to run the Apply.
        read = SceneMenuRefresh::Read();
        QMetaObject::invokeMethod(QCoreApplication::instance(), [state, generation, apply = std::move(apply), receiver = std::move(receiver)]() { finish(state, generation, apply, receiver); }, Qt::QueuedConnection);
    },
                                         kReadPriority);
}
} // anonymous namespace

SceneMenuRefresh::SceneMenuRefresh()
    : _state(std::make_shared<State>())
{
}

SceneMenuRefresh::~SceneMenuRefresh()
{
    _state->pending = Prepare();
    _state->pendingContext.clear();
}

void
SceneMenuRefresh::request(QObject* context,
                          const Prepare& prepare)
{
    if (_state->running) {
        ++_state->generation;
        _state->pending = prepare;
        _state->pendingContext = context;

        return;
    }
    start(_state, context, prepare);
}

bool
SceneMenuRefresh::isBusy() const
{
    return _state->running;
}

U64
SceneMenuRefresh::getApplyCount() const
{
    return _state->applyCount;
}

ScenePayload
SceneMenuRefresh::evaluateInput(const EffectInstancePtr& input,
                                double time)
{
    if (!input) {
        return ScenePayload();
    }
    const NodePtr node = input->getNode();
    if (!node || !node->isActivated()) {
        return ScenePayload();
    }
    const AppInstancePtr app = input->getApp();
    if (!app || !app->getProject() || app->getProject()->isProjectClosing()) {
        return ScenePayload();
    }
    ScenePayload scene;
    std::string error;
    if (input->evaluateScene(EffectInstance::EvaluateSceneArgs(time, ViewIdx(0)), &scene, &error) != EffectInstance::eRenderRoIRetCodeOk) {
        return ScenePayload();
    }

    return scene;
}

NATRON_NAMESPACE_EXIT
