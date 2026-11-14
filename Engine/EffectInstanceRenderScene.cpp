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

#include "EffectInstance.h"
#include "EffectInstancePrivate.h"

#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Engine/Node.h"
#include "Engine/Nodes/Scene/SceneWriteBarrier.h"
#include "Engine/ParallelRenderArgs.h"

NATRON_NAMESPACE_ENTER

ScenePayload
EffectInstance::SceneRenderActionArgs::getInputScene(int inputNb) const
{
    std::map<int, ScenePayload>::const_iterator found = inputScenes.find(inputNb);

    return (found == inputScenes.end()) ? ScenePayload() : found->second;
}

const SceneEvaluationContext&
EffectInstance::SceneRenderActionArgs::getInputContext(int inputNb) const
{
    std::map<int, SceneEvaluationContext>::const_iterator found = inputContexts.find(inputNb);

    return (found == inputContexts.end()) ? context : found->second;
}

namespace {
// Pops the fallback frame args evaluateScene() pushes when it is called outside of a render set
// up by the scheduler, so a later direct call does not find this call's hash on the thread.
class ScopedSceneFallbackFrameArgs {
public:
    explicit ScopedSceneFallbackFrameArgs(const EffectInstance::EffectTLSDataPtr& tls)
        : _tls(tls)
    {
    }

    ~ScopedSceneFallbackFrameArgs()
    {
        if (_tls && !_tls->frameArgs.empty()) {
            _tls->frameArgs.pop_back();
        }
    }

private:
    EffectInstance::EffectTLSDataPtr _tls;
};

void
setSceneError(std::string* errorMessage,
              const std::string& message)
{
    if (errorMessage) {
        *errorMessage = message;
    }
}
} // anonymous namespace

bool
EffectInstance::producesSceneData() const
{
    NodePtr node = getNode();

    if (!node) {
        return false;
    }

    bool isAmbiguous = false;

    return node->getEffectiveOutputDataKind(&isAmbiguous) == eDataKindScene && !isAmbiguous;
}

EffectInstance::RenderRoIRetCode
EffectInstance::evaluateScene(const EvaluateSceneArgs& args,
                              ScenePayload* outputScene,
                              std::string* errorMessage)
{
    assert(outputScene);
    if (!outputScene) {
        return eRenderRoIRetCodeFailed;
    }
    *outputScene = ScenePayload();

    // An evaluation reads layers and opens files from start to end: a layer reload waits for it,
    // and one waiting holds new evaluations off. Re-entrant, so only the outermost one counts.
    SceneWriteBarrier::ReadScope read;

    // Same guard as renderDeepRoI(): a render clone forwards to the main instance, which owns
    // the node's memoized layer and the TLS.
    if (_imp->mainInstance) {
        return _imp->mainInstance->evaluateScene(args, outputScene, errorMessage);
    }

    EffectTLSDataPtr tls = _imp->tlsData->getOrCreateTLSData();
    assert(tls);
    ParallelRenderArgsPtr frameArgs;
    std::unique_ptr<ScopedSceneFallbackFrameArgs> fallbackFrameArgs;
    if (tls->frameArgs.empty()) {
        // A direct call (a test, a script, the viewer asking for a stage): give the hash the
        // identity cache keys on and the abort flag somewhere to live, as renderDeepRoI() does.
        frameArgs = std::make_shared<ParallelRenderArgs>();
        {
            NodesWList outputs;
            getNode()->getOutputs_mt_safe(outputs);
            frameArgs->visitsCount = (int)outputs.size();
        }
        frameArgs->time = args.time;
        frameArgs->nodeHash = getHash();
        frameArgs->view = args.view;
        frameArgs->isSequentialRender = false;
        frameArgs->isRenderResponseToUserInteraction = true;
        frameArgs->abortInfo = args.abortInfo;
        tls->frameArgs.push_back(frameArgs);
        fallbackFrameArgs.reset(new ScopedSceneFallbackFrameArgs(tls));
    } else {
        frameArgs = tls->frameArgs.back();
    }

    if (aborted()) {
        return eRenderRoIRetCodeAborted;
    }

    const U64 nodeHash = frameArgs->nodeHash;

    ////////////////////////////////////////////////////////////////////////////////////////////
    ////////////////////////////// Check if effect is identity /////////////////////////////////

    {
        double identityTime = args.time;
        ViewIdx identityView(args.view);
        int identityInputNb = -1;
        bool identity;

        try {
            identity = isIdentity_public(true, nodeHash, args.time, RenderScale(), RectI(), args.view, &identityTime, &identityView, &identityInputNb);
        } catch (const std::exception& e) {
            setSceneError(errorMessage, getScriptName_mt_safe() + ": " + e.what());

            return eRenderRoIRetCodeFailed;
        } catch (...) {
            setSceneError(errorMessage, getScriptName_mt_safe() + ": could not tell whether the node passes its input through");

            return eRenderRoIRetCodeFailed;
        }

        if (identity) {
            if (identityInputNb == -1) {
                return eRenderRoIRetCodeOk;
            }
            if (identityInputNb == -2) {
                // An identity of itself at another time: guard against the degenerate answer
                // that would recurse forever.
                if (identityTime == args.time) {
                    setSceneError(errorMessage, getScriptName_mt_safe() + ": the node is an identity of itself");

                    return eRenderRoIRetCodeFailed;
                }

                return evaluateScene(EvaluateSceneArgs(identityTime, identityView, args.context, args.abortInfo), outputScene, errorMessage);
            }

            EffectInstancePtr identityInput = getIdentitySource(identityInputNb);
            if (!identityInput || !identityInput->producesSceneData()) {
                return eRenderRoIRetCodeOk;
            }

            return identityInput->evaluateScene(EvaluateSceneArgs(identityTime, identityView, args.context, args.abortInfo), outputScene, errorMessage);
        }
    }

    ////////////////////////////////////////////////////////////////////////////////////////////
    ////////////////////////////// Pull the scene inputs ///////////////////////////////////////

    SceneRenderActionArgs actionArgs;
    actionArgs.time = args.time;
    actionArgs.view = args.view;
    actionArgs.isRenderResponseToUserInteraction = frameArgs->isRenderResponseToUserInteraction;
    actionArgs.context = args.context;

    // The reach each input evaluated on a load set of its own gathered, on that input's paths.
    std::vector<std::pair<int, SceneLoadReachPtr>> inputReaches;
    const auto addInputReaches = [this, &args, &inputReaches]() {
        SceneEvaluationContext::Scope contextScope(args.context);

        for (const std::pair<int, SceneLoadReachPtr>& inputReach : inputReaches) {
            const std::vector<std::string> prefixes = inputReach.second->getPrefixes();
            if (prefixes.empty()) {
                continue;
            }
            std::vector<std::string> mapped;
            try {
                mapSceneInputReach(inputReach.first, prefixes, &mapped);
            } catch (...) {
                // Unmapped, a load anywhere has to re-evaluate the node.
                mapped.assign(1, std::string("/"));
            }
            args.context.reach->add(mapped);
        }
    };
    // Whatever way the evaluation ends, a load in what its inputs read has to re-evaluate it.
    struct InputReachesGuard {
        const std::function<void()> add;

        ~InputReachesGuard()
        {
            try {
                add();
            } catch (...) {
            }
        }
    } inputReachesGuard { addInputReaches };

    const int nInputs = getNInputs();
    for (int i = 0; i < nInputs; ++i) {
        EffectInstancePtr input = getInput(i);

        if (!input || !input->producesSceneData()) {
            continue;
        }

        NotifyInputNRenderingStarted_RAII inputNIsRendering_RAII(getNode().get(), i);

        SceneEvaluationContext inputContext;
        try {
            inputContext = getSceneInputContext(i, args.context, args.time, args.view);
        } catch (const std::exception& e) {
            setSceneError(errorMessage, getScriptName_mt_safe() + ": " + e.what());

            return eRenderRoIRetCodeFailed;
        } catch (...) {
            setSceneError(errorMessage, getScriptName_mt_safe() + ": could not tell which payloads input " + std::to_string(i) + " needs");

            return eRenderRoIRetCodeFailed;
        }

        ScenePayload inputScene;
        RenderRoIRetCode inputCode;
        if (inputContext.hasSameLoadSet(args.context) && (inputContext.reach == args.context.reach)) {
            inputCode = input->evaluateScene(args, &inputScene, errorMessage);
        } else {
            inputContext.id = getSceneInputContextId(args.context.id, i);
            inputContext.reach.reset();
            if (args.context.reach) {
                inputContext.reach = std::make_shared<SceneLoadReach>();
                inputReaches.push_back(std::make_pair(i, inputContext.reach));
            }
            actionArgs.inputContexts[i] = inputContext;
            inputCode = input->evaluateScene(EvaluateSceneArgs(args.time, args.view, inputContext, args.abortInfo), &inputScene, errorMessage);
        }
        if (inputCode != eRenderRoIRetCodeOk) {
            return inputCode;
        }
        actionArgs.inputScenes[i] = inputScene;
    }

    if (aborted()) {
        return eRenderRoIRetCodeAborted;
    }

    ////////////////////////////////////////////////////////////////////////////////////////////
    ////////////////////////////// Render //////////////////////////////////////////////////////

    ScenePayload renderedScene;
    std::string renderError;
    StatusEnum st;
    try {
        // What the node's memoized layer is kept for, without every node passing it on.
        SceneEvaluationContext::Scope contextScope(args.context);
        st = renderScene(actionArgs, &renderedScene, &renderError);
    } catch (const std::exception& e) {
        st = eStatusFailed;
        renderError = e.what();
    } catch (...) {
        st = eStatusFailed;
    }

    if (aborted()) {
        return eRenderRoIRetCodeAborted;
    }

    if (st != eStatusOK) {
        if (st == eStatusReplyDefault) {
            setSceneError(errorMessage, getScriptName_mt_safe() + " does not produce a scene");
        } else if (renderError.empty()) {
            setSceneError(errorMessage, getScriptName_mt_safe() + ": scene evaluation failed");
        } else {
            setSceneError(errorMessage, renderError);
        }

        return eRenderRoIRetCodeFailed;
    }

    *outputScene = renderedScene;

    return eRenderRoIRetCodeOk;
} // EffectInstance::evaluateScene

U64
EffectInstance::getSceneInputContextId(U64 ctxId,
                                       int inputNb) const
{
    static const std::size_t kSceneInputContextIdsMax = 256;
    const EffectInstance* owner = _imp->mainInstance ? _imp->mainInstance : this;
    EffectInstance::Implementation& imp = *owner->_imp;
    const EffectInstance::Implementation::SceneInputContextKey key(ctxId, inputNb);
    QMutexLocker locker(&imp.sceneInputContextIdsMutex);

    std::map<EffectInstance::Implementation::SceneInputContextKey, EffectInstance::Implementation::SceneInputContextIdsEntry>::iterator found = imp.sceneInputContextIds.find(key);
    if (found != imp.sceneInputContextIds.end()) {
        imp.sceneInputContextIdUses.splice(imp.sceneInputContextIdUses.begin(), imp.sceneInputContextIdUses, found->second.use);

        return found->second.id;
    }
    if (imp.sceneInputContextIds.size() >= kSceneInputContextIdsMax) {
        imp.sceneInputContextIds.erase(imp.sceneInputContextIdUses.back());
        imp.sceneInputContextIdUses.pop_back();
    }
    imp.sceneInputContextIdUses.push_front(key);
    const U64 id = SceneEvaluationContext::makeId();
    imp.sceneInputContextIds[key] = EffectInstance::Implementation::SceneInputContextIdsEntry { id, imp.sceneInputContextIdUses.begin() };

    return id;
}

SceneEvaluationContext
EffectInstance::getEvaluatedSceneInputContext(int inputNb,
                                              const SceneEvaluationContext& ctx,
                                              double time,
                                              ViewIdx view) const
{
    // evaluateScene() always has a reach to compare, so one stands in when ctx has none.
    SceneEvaluationContext asked = ctx;
    if (!asked.reach) {
        asked.reach = std::make_shared<SceneLoadReach>();
    }
    SceneEvaluationContext mapped = getSceneInputContext(inputNb, asked, time, view);
    if (mapped.hasSameLoadSet(asked) && (mapped.reach == asked.reach)) {
        return ctx;
    }
    mapped.id = getSceneInputContextId(ctx.id, inputNb);
    mapped.reach.reset();

    return mapped;
}

NATRON_NAMESPACE_EXIT
