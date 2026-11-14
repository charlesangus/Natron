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
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
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

/**
 * The id evaluateScene() gives a scene input it maps onto another load set, by the id of the
 * context mapped from, the node (its main instance) and the input number. An entry lives as long
 * as both the context id it was mapped from and its node: it is erased when the id's owner
 * retires it (EffectInstance::retireSceneContextId()) or the node is destroyed, and the id it
 * handed out is retired with it. Never destroyed: a node may be destroyed during static teardown.
 **/
struct SceneInputContextIds {
    typedef std::tuple<U64, const EffectInstance*, int> Key;

    std::mutex lock;
    std::map<Key, U64> ids;
    std::map<const EffectInstance*, std::set<Key>> byNode;

    static SceneInputContextIds& instance()
    {
        static SceneInputContextIds* ids = new SceneInputContextIds;

        return *ids;
    }

    // Erases the entries mapped from the ids in retired, and from the ids those handed out. Call with lock held.
    void retire(std::vector<U64> retired)
    {
        while (!retired.empty()) {
            const U64 id = retired.back();
            retired.pop_back();
            std::map<Key, U64>::iterator it = ids.lower_bound(Key(id, nullptr, std::numeric_limits<int>::min()));
            while ((it != ids.end()) && (std::get<0>(it->first) == id)) {
                retired.push_back(it->second);
                std::map<const EffectInstance*, std::set<Key>>::iterator node = byNode.find(std::get<1>(it->first));
                if (node != byNode.end()) {
                    node->second.erase(it->first);
                    if (node->second.empty()) {
                        byNode.erase(node);
                    }
                }
                it = ids.erase(it);
            }
        }
    }
};
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

    // A scene worker's job outlives the destruction of the nodes it evaluates, which leaves their
    // effects without a node, or a node whose effect destroyNode() takes away; held, the node
    // stays valid until this evaluation ends.
    const NodePtr node = getNode();
    if (!node || node->isBeingDestroyed()) {
        return eRenderRoIRetCodeAborted;
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
            node->getOutputs_mt_safe(outputs);
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
                mapSceneInputReach(inputReach.first, args.time, args.view, prefixes, &mapped);
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

    // The inputs' draw rules in input order, kept from the action: what it sets on its output
    // are its own rules, and a payload it passes on from an input would otherwise bring that
    // input's rules twice.
    SceneDrawRules drawRules;
    const int nInputs = getNInputs();
    for (int i = 0; i < nInputs; ++i) {
        EffectInstancePtr input = getInput(i);

        if (!input || !input->producesSceneData()) {
            continue;
        }

        NotifyInputNRenderingStarted_RAII inputNIsRendering_RAII(node.get(), i);

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
            inputContext.id = getSceneInputContextId(args.context.id, i, true);
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
        appendSceneDrawRules(inputScene.getDrawRules(), &drawRules);
        actionArgs.inputScenes[i] = inputScene.getDrawRules().empty() ? inputScene : inputScene.withDrawRules(SceneDrawRules());
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

    if (drawRules.empty()) {
        *outputScene = renderedScene;
    } else {
        appendSceneDrawRules(renderedScene.getDrawRules(), &drawRules);
        *outputScene = renderedScene.withDrawRules(drawRules);
    }

    return eRenderRoIRetCodeOk;
} // EffectInstance::evaluateScene

U64
EffectInstance::getSceneInputContextId(U64 ctxId,
                                       int inputNb,
                                       bool registerMiss) const
{
    const EffectInstance* owner = _imp->mainInstance ? _imp->mainInstance : this;
    const SceneInputContextIds::Key key(ctxId, owner, inputNb);
    SceneInputContextIds& registry = SceneInputContextIds::instance();
    std::lock_guard<std::mutex> locker(registry.lock);

    std::map<SceneInputContextIds::Key, U64>::const_iterator found = registry.ids.find(key);
    if (found != registry.ids.end()) {
        return found->second;
    }
    const U64 id = SceneEvaluationContext::makeId();
    if (registerMiss) {
        registry.ids[key] = id;
        registry.byNode[owner].insert(key);
    }

    return id;
}

void
EffectInstance::retireSceneContextId(U64 id)
{
    SceneInputContextIds& registry = SceneInputContextIds::instance();
    std::lock_guard<std::mutex> locker(registry.lock);

    registry.retire(std::vector<U64>(1, id));
}

void
EffectInstance::retireSceneInputContextIds()
{
    if (_imp->mainInstance) {
        return;
    }
    SceneInputContextIds& registry = SceneInputContextIds::instance();
    std::lock_guard<std::mutex> locker(registry.lock);

    std::map<const EffectInstance*, std::set<SceneInputContextIds::Key>>::iterator node = registry.byNode.find(this);
    if (node == registry.byNode.end()) {
        return;
    }
    std::vector<U64> retired;
    for (const SceneInputContextIds::Key& key : node->second) {
        std::map<SceneInputContextIds::Key, U64>::iterator it = registry.ids.find(key);
        if (it != registry.ids.end()) {
            retired.push_back(it->second);
            registry.ids.erase(it);
        }
    }
    registry.byNode.erase(node);
    registry.retire(retired);
}

bool
EffectInstance::getSceneIdentity(double time,
                                 ViewIdx view,
                                 double* identityTime,
                                 ViewIdx* identityView,
                                 int* identityInputNb) const
{
    const EffectInstance* owner = _imp->mainInstance ? _imp->mainInstance : this;
    *identityTime = time;
    *identityView = view;
    *identityInputNb = -1;
    try {
        return const_cast<EffectInstance*>(owner)->isIdentity_public(true, owner->getHash(), time, RenderScale(), RectI(), view, identityTime, identityView, identityInputNb);
    } catch (...) {
        // evaluateScene() fails on it, which maps nothing either way.
        return false;
    }
}

SceneEvaluationContext
EffectInstance::getEvaluatedSceneInputContext(int inputNb,
                                              const SceneEvaluationContext& ctx,
                                              double time,
                                              ViewIdx view) const
{
    double identityTime = time;
    ViewIdx identityView = view;
    int identityInputNb = -1;
    if (getSceneIdentity(time, view, &identityTime, &identityView, &identityInputNb)) {
        // evaluateScene() hands ctx itself to what it passes through.
        if ((identityInputNb == -2) && (identityTime != time)) {
            return getEvaluatedSceneInputContext(inputNb, ctx, identityTime, identityView);
        }

        return ctx;
    }
    // evaluateScene() always has a reach to compare, so one stands in when ctx has none.
    SceneEvaluationContext asked = ctx;
    if (!asked.reach) {
        asked.reach = std::make_shared<SceneLoadReach>();
    }
    SceneEvaluationContext mapped = getSceneInputContext(inputNb, asked, time, view);
    if (mapped.hasSameLoadSet(asked) && (mapped.reach == asked.reach)) {
        return ctx;
    }
    // Only an evaluation registers an id: one not yet made is not one anything is stored under.
    mapped.id = getSceneInputContextId(ctx.id, inputNb, false);
    mapped.reach.reset();

    return mapped;
}

NATRON_NAMESPACE_EXIT
