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

#include "CacheStress.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <random>
#include <sstream>
#include <tuple>
#include <utility>

#include <gtest/gtest.h>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Project.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

#include "NativeParity.h"

NATRON_NAMESPACE_ENTER

namespace CacheStress {

namespace {

    const double kKeyTimeEpsilon = 1e-6;

    struct Runtime {
        AppInstancePtr app;
        StressGraph graph;
        std::vector<KnobIPtr> knobs;
        std::vector<bool> linked;
        // The expression primitive in force per (knob target, dimension); absent means none.
        std::map<std::pair<int, int>, Primitive> expressions;
        unsigned mipmap = 0;
    };

    double
    knobValueAt(const KnobIPtr& knob,
                int dim,
                double time)
    {
        if (Knob<double>* d = dynamic_cast<Knob<double>*>(knob.get())) {
            return d->getValueAtTime(time, dim);
        }
        if (Knob<int>* i = dynamic_cast<Knob<int>*>(knob.get())) {
            return i->getValueAtTime(time, dim);
        }
        if (Knob<bool>* b = dynamic_cast<Knob<bool>*>(knob.get())) {
            return b->getValueAtTime(time, dim) ? 1. : 0.;
        }

        return 0.;
    }

    bool
    knobSetValue(const KnobIPtr& knob,
                 int dim,
                 double v)
    {
        if (Knob<double>* d = dynamic_cast<Knob<double>*>(knob.get())) {
            d->setValue(v, ViewSpec::all(), dim, true);
        } else if (Knob<int>* i = dynamic_cast<Knob<int>*>(knob.get())) {
            i->setValue((int)std::lround(v), ViewSpec::all(), dim, true);
        } else if (Knob<bool>* b = dynamic_cast<Knob<bool>*>(knob.get())) {
            b->setValue(v != 0., ViewSpec::all(), dim, true);
        } else {
            return false;
        }

        return true;
    }

    bool
    knobSetValueAtTime(const KnobIPtr& knob,
                       int dim,
                       double time,
                       double v)
    {
        if (Knob<double>* d = dynamic_cast<Knob<double>*>(knob.get())) {
            d->setValueAtTime(time, v, ViewSpec::all(), dim);
        } else if (Knob<int>* i = dynamic_cast<Knob<int>*>(knob.get())) {
            i->setValueAtTime(time, (int)std::lround(v), ViewSpec::all(), dim);
        } else if (Knob<bool>* b = dynamic_cast<Knob<bool>*>(knob.get())) {
            b->setValueAtTime(time, v != 0., ViewSpec::all(), dim);
        } else {
            return false;
        }

        return true;
    }

    std::vector<double>
    keyTimes(const KnobIPtr& knob,
             int dim)
    {
        std::vector<double> times;
        const int n = knob->getKeyFramesCount(ViewIdx(0), dim);

        for (int i = 0; i < n; ++i) {
            double t = 0.;
            if (knob->getKeyFrameTime(ViewIdx(0), i, dim, &t)) {
                times.push_back(t);
            }
        }

        return times;
    }

    bool
    hasKeyAt(const KnobIPtr& knob,
             int dim,
             double time)
    {
        const std::vector<double> times = keyTimes(knob, dim);

        for (std::size_t i = 0; i < times.size(); ++i) {
            if (std::fabs(times[i] - time) < kKeyTimeEpsilon) {
                return true;
            }
        }

        return false;
    }

    bool
    knobIsAnimatedAnywhere(const KnobIPtr& knob)
    {
        for (int d = 0; d < knob->getDimension(); ++d) {
            if (knob->isAnimated(d, ViewIdx(0))) {
                return true;
            }
        }

        return false;
    }

    bool
    knobHasExpressionAnywhere(const KnobIPtr& knob)
    {
        for (int d = 0; d < knob->getDimension(); ++d) {
            if (!knob->getExpression(d).empty()) {
                return true;
            }
        }

        return false;
    }

    double
    currentTime(const Runtime& rt)
    {
        return rt.app->getTimeLine()->currentFrame();
    }

    int
    nodeIndex(const Runtime& rt,
              const NodePtr& node)
    {
        if (!node) {
            return -1;
        }
        for (std::size_t i = 0; i < rt.graph.nodes.size(); ++i) {
            if (rt.graph.nodes[i] == node) {
                return (int)i;
            }
        }

        return -1;
    }

    InspectorNode*
    viewerOf(const Runtime& rt)
    {
        if (rt.graph.viewer < 0) {
            return 0;
        }

        return dynamic_cast<InspectorNode*>(rt.graph.nodes[rt.graph.viewer].get());
    }

    std::string
    nodeName(const Runtime& rt,
             int index)
    {
        if ((index < 0) || (index >= (int)rt.graph.nodes.size()) || !rt.graph.nodes[index]) {
            return "(none)";
        }

        return rt.graph.nodes[index]->getScriptName();
    }

    std::string
    knobLabel(const Runtime& rt,
              int knob,
              int dim)
    {
        if ((knob < 0) || (knob >= (int)rt.graph.knobs.size())) {
            return "(no knob)";
        }
        std::ostringstream os;
        os << nodeName(rt, rt.graph.knobs[knob].node) << "." << rt.graph.knobs[knob].name << "[" << dim << "]";

        return os.str();
    }

    std::string
    expressionText(const Runtime& rt,
                   const Primitive& p)
    {
        if (p.exprSourceKnob < 0) {
            return p.text;
        }
        const KnobTarget& src = rt.graph.knobs[p.exprSourceKnob];
        std::ostringstream os;
        os << nodeName(rt, src.node) << "." << src.name << ".getValue(";
        if (p.exprSourceDim >= 0) {
            os << p.exprSourceDim;
        }
        os << ")";

        return os.str();
    }

    std::string
    describePrim(const Runtime& rt,
                 const Primitive& p)
    {
        std::ostringstream os;

        switch (p.kind) {
        case ePrimSetInput:
            os << "setInput " << nodeName(rt, p.node) << ".input" << p.input << " <- " << nodeName(rt, p.source);
            break;
        case ePrimSwap:
            os << "swapInputs0And1 " << nodeName(rt, p.node);
            break;
        case ePrimSetValue:
            os << "setValue " << knobLabel(rt, p.knob, p.dim) << " = " << p.value;
            break;
        case ePrimSetValueAtTime:
            os << "setValueAtTime " << knobLabel(rt, p.knob, p.dim) << " t=" << p.time << " = " << p.value;
            break;
        case ePrimDeleteKey:
            os << "deleteKey " << knobLabel(rt, p.knob, p.dim) << " t=" << p.time;
            break;
        case ePrimRemoveAnimation:
            os << "removeAnimation " << knobLabel(rt, p.knob, p.dim);
            break;
        case ePrimSetExpression: {
            const std::string text = expressionText(rt, p);
            if (text.empty()) {
                os << "clearExpression " << knobLabel(rt, p.knob, p.dim);
            } else {
                os << "setExpression " << knobLabel(rt, p.knob, p.dim) << " = \"" << text << "\"";
            }
            break;
        }
        case ePrimLink:
        case ePrimUnlink: {
            const std::pair<int, int>& l = rt.graph.links[p.link];
            os << (p.kind == ePrimLink ? "link " : "unlink ") << nodeName(rt, rt.graph.knobs[l.first].node) << "."
               << rt.graph.knobs[l.first].name << " -> " << nodeName(rt, rt.graph.knobs[l.second].node) << "."
               << rt.graph.knobs[l.second].name;
            break;
        }
        case ePrimSetDisabled:
            os << (p.value != 0. ? "disable " : "enable ") << nodeName(rt, p.node);
            break;
        case ePrimFrameRange:
            os << "frameRange " << p.first << "-" << p.last;
            break;
        case ePrimSeek:
            os << "seek " << p.time;
            break;
        case ePrimViewerB:
            os << "viewer B = input" << p.input;
            break;
        case ePrimMipmap:
            os << "mipmap " << p.first;
            break;
        }

        return os.str();
    }

    KnobIntPtr
    frameRangeKnob(const Runtime& rt)
    {
        return std::dynamic_pointer_cast<KnobInt>(rt.app->getProject()->getKnobByName("frameRange"));
    }

    // Appends to inverse the primitives that restore what p is about to change; applied in order,
    // they undo p.
    void
    captureInverse(const Runtime& rt,
                   const Primitive& p,
                   std::vector<Primitive>* inverse)
    {
        switch (p.kind) {
        case ePrimSetInput: {
            Primitive q = p;
            q.source = nodeIndex(rt, rt.graph.nodes[p.node]->getInput(p.input));
            inverse->push_back(q);
            break;
        }
        case ePrimSwap:
            inverse->push_back(p);
            break;
        case ePrimSetValue: {
            Primitive q = p;
            q.value = knobValueAt(rt.knobs[p.knob], p.dim, currentTime(rt));
            inverse->push_back(q);
            break;
        }
        case ePrimSetValueAtTime:
        case ePrimDeleteKey: {
            const KnobIPtr& knob = rt.knobs[p.knob];
            Primitive q = p;
            if (hasKeyAt(knob, p.dim, p.time)) {
                q.kind = ePrimSetValueAtTime;
                q.value = knobValueAt(knob, p.dim, p.time);
            } else {
                q.kind = ePrimDeleteKey;
            }
            inverse->push_back(q);
            break;
        }
        case ePrimRemoveAnimation:
            break;
        case ePrimSetExpression: {
            std::map<std::pair<int, int>, Primitive>::const_iterator found = rt.expressions.find(std::make_pair(p.knob, p.dim));
            if (found != rt.expressions.end()) {
                inverse->push_back(found->second);
            } else {
                Primitive q = p;
                q.text.clear();
                q.exprSourceKnob = -1;
                inverse->push_back(q);
            }
            break;
        }
        case ePrimLink: {
            const int a = rt.graph.links[p.link].first;
            const KnobIPtr& knob = rt.knobs[a];
            Primitive unlink = p;
            unlink.kind = ePrimUnlink;
            inverse->push_back(unlink);
            for (int d = 0; d < knob->getDimension(); ++d) {
                Primitive remove;
                remove.kind = ePrimRemoveAnimation;
                remove.knob = a;
                remove.dim = d;
                inverse->push_back(remove);
                Primitive value;
                value.kind = ePrimSetValue;
                value.knob = a;
                value.dim = d;
                value.value = knobValueAt(knob, d, currentTime(rt));
                inverse->push_back(value);
            }
            break;
        }
        case ePrimUnlink: {
            Primitive q = p;
            q.kind = ePrimLink;
            inverse->push_back(q);
            break;
        }
        case ePrimSetDisabled: {
            Primitive q = p;
            KnobBoolPtr disabled = rt.graph.nodes[p.node]->getDisabledKnob();
            q.value = (disabled && disabled->getValue()) ? 1. : 0.;
            inverse->push_back(q);
            break;
        }
        case ePrimFrameRange: {
            Primitive q = p;
            KnobIntPtr range = frameRangeKnob(rt);
            if (range) {
                q.first = range->getValue(0);
                q.last = range->getValue(1);
                inverse->push_back(q);
            }
            break;
        }
        case ePrimSeek: {
            Primitive q = p;
            q.time = currentTime(rt);
            inverse->push_back(q);
            break;
        }
        case ePrimViewerB: {
            InspectorNode* viewer = viewerOf(rt);
            if (viewer) {
                int a = -1, b = -1;
                viewer->getActiveInputs(a, b);
                Primitive q = p;
                q.input = b;
                inverse->push_back(q);
            }
            break;
        }
        case ePrimMipmap: {
            Primitive q = p;
            q.first = (int)rt.mipmap;
            inverse->push_back(q);
            break;
        }
        } // switch
    } // captureInverse

    bool
    applyPrim(Runtime& rt,
              const Primitive& p,
              std::string* error)
    {
        switch (p.kind) {
        case ePrimSetInput: {
            const NodePtr& node = rt.graph.nodes[p.node];
            if (p.source < 0) {
                if (node->getInput(p.input)) {
                    if (node->disconnectInput(p.input) == -1) {
                        *error = "disconnectInput failed";

                        return false;
                    }
                }

                return true;
            }
            const NodePtr& source = rt.graph.nodes[p.source];
            if (node->getInput(p.input) == source) {
                return true;
            }
            const bool ok = node->getInput(p.input) ? node->replaceInput(source, p.input) : node->connectInput(source, p.input);
            if (!ok) {
                *error = "connecting the input failed";
            }

            return ok;
        }
        case ePrimSwap:
            rt.graph.nodes[p.node]->switchInput0And1();

            return true;
        case ePrimSetValue:
            if (!knobSetValue(rt.knobs[p.knob], p.dim, p.value)) {
                *error = "the knob takes no number";

                return false;
            }

            return true;
        case ePrimSetValueAtTime:
            if (!knobSetValueAtTime(rt.knobs[p.knob], p.dim, p.time, p.value)) {
                *error = "the knob takes no number";

                return false;
            }

            return true;
        case ePrimDeleteKey:
            if (hasKeyAt(rt.knobs[p.knob], p.dim, p.time)) {
                rt.knobs[p.knob]->deleteValueAtTime(eCurveChangeReasonInternal, p.time, ViewSpec::all(), p.dim, false);
            }

            return true;
        case ePrimRemoveAnimation:
            rt.knobs[p.knob]->removeAnimation(ViewSpec::all(), p.dim);

            return true;
        case ePrimSetExpression: {
            const std::string text = expressionText(rt, p);
            const KnobIPtr& knob = rt.knobs[p.knob];
            const std::pair<int, int> key(p.knob, p.dim);
            if (text.empty()) {
                knob->clearExpression(p.dim, true);
                rt.expressions.erase(key);

                return true;
            }
            try {
                knob->setExpression(p.dim, text, false, true);
            } catch (const std::exception& e) {
                *error = std::string("setExpression threw: ") + e.what();

                return false;
            }
            rt.expressions[key] = p;

            return true;
        }
        case ePrimLink: {
            const std::pair<int, int>& l = rt.graph.links[p.link];
            if (rt.linked[p.link]) {
                return true;
            }
            if (!rt.knobs[l.first]->linkTo(-1, rt.knobs[l.second], -1)) {
                *error = "linkTo failed";

                return false;
            }
            rt.linked[p.link] = true;

            return true;
        }
        case ePrimUnlink: {
            const std::pair<int, int>& l = rt.graph.links[p.link];
            if (!rt.linked[p.link]) {
                return true;
            }
            rt.knobs[l.first]->unlinkFromGroups();
            rt.linked[p.link] = false;

            return true;
        }
        case ePrimSetDisabled: {
            KnobBoolPtr disabled = rt.graph.nodes[p.node]->getDisabledKnob();
            if (!disabled) {
                *error = "the node has no disable knob";

                return false;
            }
            disabled->setValue(p.value != 0.);

            return true;
        }
        case ePrimFrameRange: {
            KnobIntPtr range = frameRangeKnob(rt);
            if (!range) {
                *error = "the project has no frameRange knob";

                return false;
            }
            range->setValue(p.first, ViewSpec::all(), 0);
            range->setValue(p.last, ViewSpec::all(), 1);

            return true;
        }
        case ePrimSeek:
            rt.app->getTimeLine()->seekFrame((SequenceTime)std::lround(p.time), false, NULL, eTimelineChangeReasonOtherSeek);

            return true;
        case ePrimViewerB: {
            InspectorNode* viewer = viewerOf(rt);
            if (!viewer) {
                *error = "the graph has no viewer";

                return false;
            }
            viewer->setInputB(p.input);

            return true;
        }
        case ePrimMipmap:
            rt.mipmap = (unsigned)p.first;

            return true;
        } // switch

        return false;
    } // applyPrim

    bool
    applyStep(Runtime& rt,
              const Step& step,
              std::vector<Primitive>* inverse,
              std::string* error)
    {
        std::vector<Primitive> undo;

        for (std::size_t i = 0; i < step.prims.size(); ++i) {
            std::vector<Primitive> inv;
            captureInverse(rt, step.prims[i], &inv);
            if (!applyPrim(rt, step.prims[i], error)) {
                *error = describePrim(rt, step.prims[i]) + ": " + *error;

                return false;
            }
            undo.insert(undo.begin(), inv.begin(), inv.end());
        }
        if (inverse) {
            *inverse = undo;
        }

        return true;
    }

    class Rng {
    public:
        explicit Rng(std::uint32_t seed)
            : _gen(seed)
        {
        }

        int pick(int n)
        {
            return (n <= 0) ? 0 : (int)(_gen() % (std::uint32_t)n);
        }

    private:
        std::mt19937 _gen;
    };

    bool
    sameCollection(const Runtime& rt,
                   int a,
                   int b)
    {
        return rt.graph.nodes[a]->getGroup() == rt.graph.nodes[b]->getGroup();
    }

    bool
    knobHasExpression(const Runtime& rt,
                      int knob,
                      int dim)
    {
        return rt.expressions.count(std::make_pair(knob, dim)) > 0;
    }

    // Whether some knob target that must stay unanimated is linked to knob.
    bool
    linkedToUnanimatable(const Runtime& rt,
                         int knob)
    {
        for (std::size_t l = 0; l < rt.graph.links.size(); ++l) {
            if (!rt.linked[l]) {
                continue;
            }
            const std::pair<int, int>& pair = rt.graph.links[l];
            if ((pair.first == knob) && !rt.graph.knobs[pair.second].animatable) {
                return true;
            }
            if ((pair.second == knob) && !rt.graph.knobs[pair.first].animatable) {
                return true;
            }
        }

        return false;
    }

    bool
    isLinked(const Runtime& rt,
             int knob)
    {
        for (std::size_t l = 0; l < rt.graph.links.size(); ++l) {
            if (rt.linked[l] && ((rt.graph.links[l].first == knob) || (rt.graph.links[l].second == knob))) {
                return true;
            }
        }

        return false;
    }

    // Builds one op of the given kind from the current state; false when none applies.
    bool
    generateOp(Runtime& rt,
               Rng& rng,
               OpKind kind,
               std::vector<std::vector<Primitive>>& undoStack,
               Step* step)
    {
        const StressGraph& g = rt.graph;
        step->kind = kind;
        step->prims.clear();

        switch (kind) {
        case eOpConnect:
        case eOpDisconnect:
        case eOpReplace: {
            std::vector<int> candidates;
            for (std::size_t i = 0; i < g.inputSlots.size(); ++i) {
                const InputSlot& s = g.inputSlots[i];
                const bool connected = bool(g.nodes[s.node]->getInput(s.input));
                if (((kind == eOpConnect) && !connected) || ((kind == eOpDisconnect) && connected && s.optional) || ((kind == eOpReplace) && connected)) {
                    candidates.push_back((int)i);
                }
            }
            if (candidates.empty()) {
                return false;
            }
            const InputSlot& s = g.inputSlots[candidates[rng.pick((int)candidates.size())]];
            if (kind == eOpDisconnect) {
                step->prims.push_back(setInputPrim(s.node, s.input, -1));

                return true;
            }
            const NodePtr current = g.nodes[s.node]->getInput(s.input);
            std::vector<int> sources;
            for (std::size_t i = 0; i < s.sources.size(); ++i) {
                const NodePtr& candidate = g.nodes[s.sources[i]];
                if ((candidate != current) && (g.nodes[s.node]->canConnectInput(candidate, s.input) == Node::eCanConnectInput_ok)) {
                    sources.push_back(s.sources[i]);
                }
            }
            if (sources.empty()) {
                return false;
            }
            step->prims.push_back(setInputPrim(s.node, s.input, sources[rng.pick((int)sources.size())]));

            return true;
        }
        case eOpSwap: {
            if (g.swappable.empty()) {
                return false;
            }
            Primitive p;
            p.kind = ePrimSwap;
            p.node = g.swappable[rng.pick((int)g.swappable.size())];
            step->prims.push_back(p);

            return true;
        }
        case eOpSetValue: {
            std::vector<int> targets;
            for (std::size_t i = 0; i < g.knobs.size(); ++i) {
                if (!g.knobs[i].values.empty() && !knobHasExpression(rt, (int)i, g.knobs[i].dim)) {
                    targets.push_back((int)i);
                }
            }
            if (targets.empty()) {
                return false;
            }
            const int t = targets[rng.pick((int)targets.size())];
            const KnobTarget& target = g.knobs[t];
            const double now = currentTime(rt);
            const double old = knobValueAt(rt.knobs[t], target.dim, now);
            double v = target.values[rng.pick((int)target.values.size())];
            if ((v == old) && (target.values.size() > 1)) {
                v = target.values[(rng.pick((int)target.values.size() - 1) + 1) % target.values.size()];
            }
            Primitive p = setValuePrim(t, target.dim, v);
            if (rt.knobs[t]->isAnimated(target.dim, ViewIdx(0))) {
                p.kind = ePrimSetValueAtTime;
                p.time = now;
            }
            step->prims.push_back(p);

            return true;
        }
        case eOpAddKey: {
            std::vector<int> targets;
            for (std::size_t i = 0; i < g.knobs.size(); ++i) {
                if (g.knobs[i].animatable && !g.knobs[i].values.empty() && !knobHasExpression(rt, (int)i, g.knobs[i].dim) && !linkedToUnanimatable(rt, (int)i)) {
                    targets.push_back((int)i);
                }
            }
            if (targets.empty()) {
                return false;
            }
            const int t = targets[rng.pick((int)targets.size())];
            Primitive p;
            p.kind = ePrimSetValueAtTime;
            p.knob = t;
            p.dim = g.knobs[t].dim;
            p.time = g.firstTime + rng.pick(g.lastTime - g.firstTime + 1);
            p.value = g.knobs[t].values[rng.pick((int)g.knobs[t].values.size())];
            step->prims.push_back(p);

            return true;
        }
        case eOpRemoveKey: {
            std::vector<std::pair<int, double>> keys;
            for (std::size_t i = 0; i < g.knobs.size(); ++i) {
                const std::vector<double> times = keyTimes(rt.knobs[i], g.knobs[i].dim);
                for (std::size_t k = 0; k < times.size(); ++k) {
                    keys.push_back(std::make_pair((int)i, times[k]));
                }
            }
            if (keys.empty()) {
                return false;
            }
            const std::pair<int, double>& key = keys[rng.pick((int)keys.size())];
            Primitive p;
            p.kind = ePrimDeleteKey;
            p.knob = key.first;
            p.dim = g.knobs[key.first].dim;
            p.time = key.second;
            step->prims.push_back(p);

            return true;
        }
        case eOpSetExpression:
        case eOpWholeValueExpression: {
            std::vector<std::pair<int, int>> pairs;
            for (std::size_t i = 0; i < g.exprTargets.size(); ++i) {
                const int t = g.exprTargets[i];
                if (knobHasExpression(rt, t, g.knobs[t].dim) || isLinked(rt, t)) {
                    continue;
                }
                const std::vector<int>& sources = (kind == eOpSetExpression) ? std::vector<int>() : g.wholeValueSources;
                if (kind == eOpSetExpression) {
                    for (std::size_t s = 0; s < g.knobs.size(); ++s) {
                        if (((int)s != t) && !g.knobs[s].animatable && (g.knobs[s].node != g.knobs[t].node) && sameCollection(rt, g.knobs[s].node, g.knobs[t].node) && !knobHasExpressionAnywhere(rt.knobs[s]) && !knobIsAnimatedAnywhere(rt.knobs[s])) {
                            pairs.push_back(std::make_pair(t, (int)s));
                        }
                    }
                } else {
                    for (std::size_t s = 0; s < sources.size(); ++s) {
                        const int src = sources[s];
                        if ((src != t) && (g.knobs[src].node != g.knobs[t].node) && sameCollection(rt, g.knobs[src].node, g.knobs[t].node) && !knobHasExpressionAnywhere(rt.knobs[src]) && !knobIsAnimatedAnywhere(rt.knobs[src])) {
                            pairs.push_back(std::make_pair(t, src));
                        }
                    }
                }
            }
            if (pairs.empty()) {
                return false;
            }
            const std::pair<int, int>& pick = pairs[rng.pick((int)pairs.size())];
            const int sourceDim = (kind == eOpSetExpression) ? g.knobs[pick.second].dim : -1;
            step->prims.push_back(dimensionExpressionPrim(pick.first, g.knobs[pick.first].dim, pick.second, sourceDim));

            return true;
        }
        case eOpFrameExpression: {
            std::vector<int> targets;
            for (std::size_t i = 0; i < g.exprTargets.size(); ++i) {
                const int t = g.exprTargets[i];
                if (!knobHasExpression(rt, t, g.knobs[t].dim) && !isLinked(rt, t) && !rt.knobs[t]->isAnimated(g.knobs[t].dim, ViewIdx(0))) {
                    targets.push_back(t);
                }
            }
            if (targets.empty()) {
                return false;
            }
            static const char* const kFrameExpressions[] = { "frame*0.1", "0.5+frame*0.05", "frame%2" };
            const int t = targets[rng.pick((int)targets.size())];
            step->prims.push_back(expressionPrim(t, g.knobs[t].dim, kFrameExpressions[rng.pick(3)]));

            return true;
        }
        case eOpClearExpression: {
            if (rt.expressions.empty()) {
                return false;
            }
            int n = rng.pick((int)rt.expressions.size());
            std::map<std::pair<int, int>, Primitive>::const_iterator it = rt.expressions.begin();
            std::advance(it, n);
            step->prims.push_back(expressionPrim(it->first.first, it->first.second, std::string()));

            return true;
        }
        case eOpLink: {
            std::vector<int> links;
            for (std::size_t l = 0; l < g.links.size(); ++l) {
                const int a = g.links[l].first;
                const int b = g.links[l].second;
                if (rt.linked[l] || knobIsAnimatedAnywhere(rt.knobs[a]) || knobHasExpressionAnywhere(rt.knobs[a]) || knobHasExpressionAnywhere(rt.knobs[b])) {
                    continue;
                }
                if (!g.knobs[a].animatable && knobIsAnimatedAnywhere(rt.knobs[b])) {
                    continue;
                }
                links.push_back((int)l);
            }
            if (links.empty()) {
                return false;
            }
            Primitive p;
            p.kind = ePrimLink;
            p.link = links[rng.pick((int)links.size())];
            step->prims.push_back(p);

            return true;
        }
        case eOpUnlink: {
            std::vector<int> links;
            for (std::size_t l = 0; l < g.links.size(); ++l) {
                if (rt.linked[l]) {
                    links.push_back((int)l);
                }
            }
            if (links.empty()) {
                return false;
            }
            Primitive p;
            p.kind = ePrimUnlink;
            p.link = links[rng.pick((int)links.size())];
            step->prims.push_back(p);

            return true;
        }
        case eOpToggleDisabled: {
            if (g.toggles.empty()) {
                return false;
            }
            Primitive p;
            p.kind = ePrimSetDisabled;
            p.node = g.toggles[rng.pick((int)g.toggles.size())];
            KnobBoolPtr disabled = g.nodes[p.node]->getDisabledKnob();
            if (!disabled) {
                return false;
            }
            p.value = disabled->getValue() ? 0. : 1.;
            step->prims.push_back(p);

            return true;
        }
        case eOpFrameRange: {
            Primitive p;
            p.kind = ePrimFrameRange;
            p.first = g.firstTime + rng.pick(2);
            p.last = g.lastTime - 1 + rng.pick(4);
            step->prims.push_back(p);

            return true;
        }
        case eOpTime: {
            const int span = g.lastTime - g.firstTime + 1;
            if (span < 2) {
                return false;
            }
            const int now = (int)std::lround(currentTime(rt));
            int t = g.firstTime + rng.pick(span);
            if (t == now) {
                t = g.firstTime + ((t - g.firstTime + 1 + rng.pick(span - 1)) % span);
            }
            Primitive p;
            p.kind = ePrimSeek;
            p.time = t;
            step->prims.push_back(p);

            return true;
        }
        case eOpRevisit: {
            if (undoStack.empty()) {
                return false;
            }
            const int n = 1 + rng.pick(std::min<int>(3, (int)undoStack.size()));
            for (int i = 0; i < n; ++i) {
                const std::vector<Primitive>& undo = undoStack.back();
                step->prims.insert(step->prims.end(), undo.begin(), undo.end());
                undoStack.pop_back();
            }

            return true;
        }
        case eOpViewerB: {
            InspectorNode* viewer = viewerOf(rt);
            if (!viewer || (g.viewerInputs.size() < 2)) {
                return false;
            }
            int a = -1, b = -1;
            viewer->getActiveInputs(a, b);
            std::vector<int> choices;
            for (std::size_t i = 0; i < g.viewerInputs.size(); ++i) {
                if (g.viewerInputs[i] != b) {
                    choices.push_back(g.viewerInputs[i]);
                }
            }
            if (choices.empty()) {
                return false;
            }
            step->prims.push_back(viewerBPrim(choices[rng.pick((int)choices.size())]));

            return true;
        }
        case eOpMipmap:
            step->prims.push_back(mipmapPrim(rt.mipmap == 0 ? 1 : 0));

            return true;
        case eOpCount:
            break;
        } // switch

        return false;
    } // generateOp

    struct Probe {
        bool rendered = false;
        bool ok = false;
        // The render succeeded and produced nothing (ok is true, plane is unset).
        bool empty = false;
        RenderedPlane plane;
        std::string error;
    };

    // Records a render's result in probe; an output that is legitimately empty is a result too.
    void
    setOutcome(Probe* probe,
               bool ok)
    {
        probe->rendered = true;
        probe->empty = !ok && (probe->error == kRenderProducedNoPlane);
        probe->ok = ok || probe->empty;
        if (probe->empty) {
            probe->error.clear();
        }
    }

    struct StepRecord {
        Step step;
        std::string log;
        double time = 0.;
        unsigned mipmap = 0;
        int probe = -1;
        Probe legacy;
        Probe taskGraph;
        Probe custom;
    };

    int
    resolveProbe(const Runtime& rt)
    {
        InspectorNode* viewer = viewerOf(rt);

        if (!viewer) {
            return rt.graph.probe;
        }
        int a = -1, b = -1;
        viewer->getActiveInputs(a, b);
        if (b < 0) {
            return -1;
        }

        return nodeIndex(rt, viewer->getInput(b));
    }

    RectI
    windowAt(const Runtime& rt)
    {
        return (rt.mipmap == 0) ? rt.graph.window : rt.graph.window.downscalePowerOfTwoSmallestEnclosing(rt.mipmap);
    }

    Probe
    renderOnce(const Runtime& rt,
               int probe,
               RenderSchedulerModeEnum mode,
               bool byPassCache)
    {
        Probe out;

        if (probe < 0) {
            return out;
        }
        setOutcome(&out, renderProbe(rt.graph.nodes[probe], currentTime(rt), rt.mipmap, windowAt(rt), mode, byPassCache, &out.plane, &out.error));

        return out;
    }

    std::string
    compareProbes(const Probe& reference,
                  const Probe& got,
                  const std::function<std::string(const RenderedPlane&, const RenderedPlane&)>& compare = std::function<std::string(const RenderedPlane&, const RenderedPlane&)>())
    {
        if (reference.rendered != got.rendered) {
            return reference.rendered ? "the cached pass rendered nothing" : "the cached pass rendered a probe the reference did not";
        }
        if (!reference.rendered) {
            return std::string();
        }
        if (!reference.ok) {
            return "the reference failed: " + reference.error;
        }
        if (!got.ok) {
            return "the cached render failed: " + got.error;
        }
        if (reference.empty != got.empty) {
            return reference.empty ? "the reference rendered nothing but the cached render produced a plane" : "the cached render produced nothing but the reference rendered a plane";
        }
        if (reference.empty) {
            return std::string();
        }

        return compare ? compare(reference.plane, got.plane) : compareExact(reference.plane, got.plane);
    }

    bool
    buildRuntime(const AppInstancePtr& app,
                 const GraphBuilder& builder,
                 Runtime* rt,
                 std::string* error)
    {
        app->getProject()->reset(false, true);
        clearEverything(app);
        rt->app = app;
        rt->graph = StressGraph();
        rt->expressions.clear();
        rt->mipmap = 0;
        if (!builder(app, &rt->graph, error)) {
            *error = "the graph builder failed: " + *error;

            return false;
        }
        rt->knobs.clear();
        for (std::size_t i = 0; i < rt->graph.knobs.size(); ++i) {
            const KnobTarget& t = rt->graph.knobs[i];
            if ((t.node < 0) || (t.node >= (int)rt->graph.nodes.size()) || !rt->graph.nodes[t.node]) {
                *error = "knob target " + t.name + " names no node";

                return false;
            }
            KnobIPtr knob = rt->graph.nodes[t.node]->getKnobByName(t.name);
            if (!knob || (t.dim >= knob->getDimension())) {
                *error = rt->graph.nodes[t.node]->getScriptName() + " has no knob " + t.name + " of that dimension";

                return false;
            }
            rt->knobs.push_back(knob);
        }
        rt->linked.assign(rt->graph.links.size(), false);
        if (rt->graph.window.isNull()) {
            *error = "the graph has an empty window";

            return false;
        }

        KnobIntPtr range = frameRangeKnob(*rt);
        if (range) {
            range->setValue(rt->graph.firstTime, ViewSpec::all(), 0);
            range->setValue(rt->graph.lastTime, ViewSpec::all(), 1);
        }
        app->getTimeLine()->seekFrame(rt->graph.firstTime, false, NULL, eTimelineChangeReasonOtherSeek);

        return true;
    } // buildRuntime

    std::string
    formatLog(const std::vector<StepRecord>& records,
              std::size_t upTo)
    {
        std::ostringstream os;

        for (std::size_t i = 0; (i < records.size()) && (i <= upTo); ++i) {
            os << "  step " << i << " [" << opName(records[i].step.kind) << "] t=" << records[i].time << " mip=" << records[i].mipmap
               << " probe#" << records[i].probe << ": " << records[i].log << "\n";
        }

        return os.str();
    }

    const char*
    envString(const char* name)
    {
        const char* v = std::getenv(name);

        return (v && *v) ? v : 0;
    }

} // namespace

OpMask
defaultOps()
{
    return allOps() & ~(opBit(eOpWholeValueExpression) | opBit(eOpViewerB) | opBit(eOpMipmap));
}

OpMask
allOps()
{
    return (OpMask(1) << unsigned(eOpCount)) - 1;
}

const char*
opName(OpKind kind)
{
    switch (kind) {
    case eOpConnect:
        return "connect";
    case eOpDisconnect:
        return "disconnect";
    case eOpReplace:
        return "replace";
    case eOpSwap:
        return "swap";
    case eOpSetValue:
        return "setValue";
    case eOpAddKey:
        return "addKey";
    case eOpRemoveKey:
        return "removeKey";
    case eOpSetExpression:
        return "setExpression";
    case eOpClearExpression:
        return "clearExpression";
    case eOpFrameExpression:
        return "frameExpression";
    case eOpWholeValueExpression:
        return "wholeValueExpression";
    case eOpLink:
        return "link";
    case eOpUnlink:
        return "unlink";
    case eOpToggleDisabled:
        return "toggleDisabled";
    case eOpFrameRange:
        return "frameRange";
    case eOpTime:
        return "time";
    case eOpRevisit:
        return "revisit";
    case eOpViewerB:
        return "viewerB";
    case eOpMipmap:
        return "mipmap";
    case eOpCount:
        break;
    }

    return "initial";
}

Primitive
setValuePrim(int knob,
             int dim,
             double value)
{
    Primitive p;

    p.kind = ePrimSetValue;
    p.knob = knob;
    p.dim = dim;
    p.value = value;

    return p;
}

Primitive
setInputPrim(int node,
             int input,
             int source)
{
    Primitive p;

    p.kind = ePrimSetInput;
    p.node = node;
    p.input = input;
    p.source = source;

    return p;
}

Primitive
viewerBPrim(int viewerInput)
{
    Primitive p;

    p.kind = ePrimViewerB;
    p.input = viewerInput;

    return p;
}

Primitive
mipmapPrim(int level)
{
    Primitive p;

    p.kind = ePrimMipmap;
    p.first = level;

    return p;
}

Primitive
expressionPrim(int knob,
               int dim,
               const std::string& text)
{
    Primitive p;

    p.kind = ePrimSetExpression;
    p.knob = knob;
    p.dim = dim;
    p.text = text;

    return p;
}

Primitive
dimensionExpressionPrim(int knob,
                        int dim,
                        int sourceKnob,
                        int sourceDim)
{
    Primitive p = expressionPrim(knob, dim, std::string());

    p.exprSourceKnob = sourceKnob;
    p.exprSourceDim = sourceDim;

    return p;
}

std::string
describe(const StressGraph& graph,
         const Primitive& prim)
{
    Runtime rt;

    rt.graph = graph;

    return describePrim(rt, prim);
}

NodePtr
createIn(const AppInstancePtr& app,
         const NodeCollectionPtr& parent,
         const std::string& pluginID,
         int major)
{
    if (!app || !parent) {
        return NodePtr();
    }
    CreateNodeArgs args(pluginID, parent);
    args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
    args.setProperty<int>(kCreateNodeArgsPropPluginVersion, major, 0);
    args.setProperty<int>(kCreateNodeArgsPropPluginVersion, -1, 1);

    return app->createNode(args);
}

NodePtr
createConstantIn(const AppInstancePtr& app,
                 const NodeCollectionPtr& parent,
                 const RectI& extent,
                 const std::vector<double>& color)
{
    NodePtr constant = createIn(app, parent, PLUGINID_NATRON_CONSTANT, PLUGIN_MAJOR_NATRON_CONSTANT);

    if (!constant) {
        return constant;
    }
    if (!setKnobValue(constant, kNativeGeneratorParamExtent, std::string(kNativeGeneratorExtentSize)) || !setKnobValues(constant, kNativeGeneratorParamBottomLeft, { (double)extent.x1, (double)extent.y1 }) || !setKnobValues(constant, kNativeGeneratorParamSize, { (double)extent.width(), (double)extent.height() }) || !setKnobValues(constant, kConstantParamColor, color)) {
        return NodePtr();
    }

    return constant;
}

NodePtr
createEmptyGroupIn(const AppInstancePtr& app,
                   const NodeCollectionPtr& parent)
{
    if (!app || !parent) {
        return NodePtr();
    }
    CreateNodeArgs args(PLUGINID_NATRON_GROUP, parent);
    args.setProperty<bool>(kCreateNodeArgsPropNodeGroupDisableCreateInitialNodes, true);
    args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);

    return app->createNode(args);
}

NodePtr
createViewerIn(const AppInstancePtr& app,
               const NodeCollectionPtr& parent)
{
    if (!app || !parent) {
        return NodePtr();
    }
    CreateNodeArgs args(PLUGINID_NATRON_VIEWER, parent);
    args.setProperty<bool>(kCreateNodeArgsPropAllowNonUserCreatablePlugins, true);
    args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);

    return app->createNode(args);
}

int
addNode(StressGraph* graph,
        const NodePtr& node)
{
    if (!node) {
        return -1;
    }
    graph->nodes.push_back(node);

    return (int)graph->nodes.size() - 1;
}

int
addKnob(StressGraph* graph,
        int node,
        const std::string& name,
        int dim,
        const std::vector<double>& values,
        bool animatable)
{
    KnobTarget t;

    t.node = node;
    t.name = name;
    t.dim = dim;
    t.values = values;
    t.animatable = animatable;
    graph->knobs.push_back(t);

    return (int)graph->knobs.size() - 1;
}

void
addSlot(StressGraph* graph,
        int node,
        int input,
        const std::vector<int>& sources,
        bool optional)
{
    InputSlot s;

    s.node = node;
    s.input = input;
    s.sources = sources;
    s.optional = optional;
    graph->inputSlots.push_back(s);
}

void
clearEverything(const AppInstancePtr& app)
{
    appPTR->clearAllCaches();
    if (!app) {
        return;
    }
    NodesList nodes;
    app->getProject()->getNodes_recursive(nodes, false);
    for (NodesList::iterator it = nodes.begin(); it != nodes.end(); ++it) {
        EffectInstancePtr effect = (*it)->getEffectInstance();
        if (effect) {
            effect->clearActionsCache();
        }
    }
}

bool
renderProbe(const NodePtr& node,
            double time,
            unsigned mipmapLevel,
            const RectI& window,
            RenderSchedulerModeEnum mode,
            bool byPassCache,
            RenderedPlane* out,
            std::string* error)
{
    std::vector<RenderedPlane> planes;
    const std::list<ImageLayerDesc> rgba(1, ImageLayerDesc::getRGBAComponents());
    bool ok;

    if (mode == eRenderSchedulerModeLegacy) {
        ok = renderNodePlanesDirect(node, time, ViewIdx(0), mipmapLevel, window, rgba, &planes, error, byPassCache);
    } else {
        ok = renderNodePlanesInMode(node, time, ViewIdx(0), mipmapLevel, window, rgba, mode, byPassCache, &planes, error);
    }
    if (!ok) {
        return false;
    }
    if (planes.empty()) {
        if (error) {
            *error = "the render returned no plane";
        }

        return false;
    }
    // The task-graph path returns every plane of the root; the colour plane comes first unless
    // RGBA is named.
    for (std::size_t i = 0; i < planes.size(); ++i) {
        if (planes[i].layer == ImageLayerDesc::getRGBAComponents()) {
            *out = planes[i];

            return true;
        }
    }
    *out = planes.front();

    return true;
} // renderProbe

std::string
compareExact(const RenderedPlane& reference,
             const RenderedPlane& got)
{
    std::ostringstream os;

    if ((reference.window.x1 != got.window.x1) || (reference.window.y1 != got.window.y1) || (reference.window.x2 != got.window.x2) || (reference.window.y2 != got.window.y2)) {
        os << "windows differ";

        return os.str();
    }
    if (!(reference.layer == got.layer)) {
        os << "layers differ (" << reference.layer.getLayerID() << " vs " << got.layer.getLayerID() << ")";

        return os.str();
    }
    const std::size_t refComps = reference.channels.size();
    const std::size_t gotComps = got.channels.size();
    const std::size_t pixels = (std::size_t)reference.window.width() * (std::size_t)reference.window.height();
    if ((refComps == 0) || (refComps != gotComps) || (reference.pixels.size() != pixels * refComps) || (got.pixels.size() != pixels * gotComps)) {
        os << "plane sizes differ (" << refComps << " vs " << gotComps << " channels)";

        return os.str();
    }
    // Every channel of each plane must have its counterpart in the other, in whatever order.
    std::vector<std::pair<std::size_t, std::size_t>> common;
    for (std::size_t c = 0; c < refComps; ++c) {
        std::size_t match = gotComps;
        for (std::size_t d = 0; d < gotComps; ++d) {
            if (reference.channels[c] == got.channels[d]) {
                match = d;
                break;
            }
        }
        if (match == gotComps) {
            os << "channel " << reference.channels[c] << " is missing from the cached plane";

            return os.str();
        }
        common.push_back(std::make_pair(c, match));
    }
    std::size_t differing = 0;
    std::string first;
    for (std::size_t p = 0; p < pixels; ++p) {
        for (std::size_t k = 0; k < common.size(); ++k) {
            const float r = reference.pixels[p * refComps + common[k].first];
            const float g = got.pixels[p * gotComps + common[k].second];
            if (std::memcmp(&r, &g, sizeof(float)) != 0) {
                if (differing == 0) {
                    std::ostringstream f;
                    const int x = reference.window.x1 + (int)(p % (std::size_t)reference.window.width());
                    const int y = reference.window.y1 + (int)(p / (std::size_t)reference.window.width());
                    f << "pixel (" << x << "," << y << ") " << reference.channels[common[k].first] << ": reference " << r << ", cached " << g;
                    first = f.str();
                }
                ++differing;
            }
        }
    }
    if (differing == 0) {
        return std::string();
    }
    os << differing << " samples differ; first at " << first;

    return os.str();
} // compareExact

Result
run(const AppInstancePtr& app,
    const GraphBuilder& builder,
    std::uint32_t seed,
    const Options& options)
{
    Result result;
    result.seed = seed;

    std::vector<OpKind> kinds;
    for (int k = 0; k < (int)eOpCount; ++k) {
        if (options.ops & opBit((OpKind)k)) {
            kinds.push_back((OpKind)k);
        }
    }

    // Pass A: apply and record the ops, rendering every step from empty caches.
    std::vector<StepRecord> records;
    {
        Runtime rt;
        std::string error;
        if (!buildRuntime(app, builder, &rt, &error)) {
            result.ok = false;
            result.report = error;

            return result;
        }
        Rng rng(seed);
        std::vector<std::vector<Primitive>> undoStack;
        // The first record renders the graph as built, so the first edit already meets filled caches.
        const int total = 1 + (int)options.scripted.size() + (options.seeded ? options.steps : 0);
        for (int i = 0; i < total; ++i) {
            StepRecord record;
            if (i == 0) {
                record.step = Step();
                record.step.kind = eOpCount;
            } else if (i <= (int)options.scripted.size()) {
                record.step = options.scripted[i - 1];
            } else {
                bool generated = false;
                for (int attempt = 0; !kinds.empty() && (attempt < 64) && !generated; ++attempt) {
                    generated = generateOp(rt, rng, kinds[rng.pick((int)kinds.size())], undoStack, &record.step);
                }
                if (!generated) {
                    record.step = Step();
                }
            }
            std::ostringstream log;
            for (std::size_t p = 0; p < record.step.prims.size(); ++p) {
                log << (p ? "; " : "") << describePrim(rt, record.step.prims[p]);
            }
            record.log = record.step.prims.empty() ? std::string(i == 0 ? "(initial state)" : "(no op applies)") : log.str();
            std::vector<Primitive> inverse;
            if (!applyStep(rt, record.step, &inverse, &error)) {
                result.ok = false;
                result.failedStep = i;
                std::ostringstream os;
                os << "NATRON_CACHE_STRESS_SEED=" << seed << ": step " << i << " could not be applied in the reference pass: " << error << "\n"
                   << formatLog(records, records.size());
                result.report = os.str();

                return result;
            }
            if ((record.step.kind != eOpRevisit) && (record.step.kind != eOpCount) && !record.step.prims.empty()) {
                undoStack.push_back(inverse);
            }
            record.time = currentTime(rt);
            record.mipmap = rt.mipmap;
            record.probe = resolveProbe(rt);
            if (options.legacy) {
                clearEverything(app);
                record.legacy = renderOnce(rt, record.probe, eRenderSchedulerModeLegacy, true);
            }
            if (options.taskGraph) {
                clearEverything(app);
                record.taskGraph = renderOnce(rt, record.probe, eRenderSchedulerModeTaskGraph, true);
            }
            if (options.cachedRender && options.cachedRenderReference && (record.probe >= 0)) {
                clearEverything(app);
                setOutcome(&record.custom, options.cachedRender(rt.graph.nodes[record.probe], record.time, record.mipmap, windowAt(rt), &record.custom.plane, &record.custom.error));
            }
            records.push_back(record);
            // A step whose reference fails proves nothing about the cache, even if the cached
            // render fails the same way.
            const Probe* failed = 0;
            const char* failedMode = "";
            if (record.legacy.rendered && !record.legacy.ok) {
                failed = &record.legacy;
                failedMode = "legacy";
            } else if (record.taskGraph.rendered && !record.taskGraph.ok) {
                failed = &record.taskGraph;
                failedMode = "task graph";
            } else if (record.custom.rendered && !record.custom.ok) {
                failed = &record.custom;
                failedMode = options.cachedRenderName.c_str();
            }
            if (failed) {
                std::ostringstream os;
                os << "NATRON_CACHE_STRESS_SEED=" << seed << ": step " << i << " (" << failedMode << "): the cache-off reference render failed: "
                   << failed->error << "\n"
                   << "op log:\n"
                   << formatLog(records, i);
                result.ok = false;
                result.failedStep = i;
                result.report = os.str();

                return result;
            }
        }
    }

    // Pass B: the same ops on a fresh graph, every render through caches nothing clears.
    Runtime rt;
    std::string error;
    if (!buildRuntime(app, builder, &rt, &error)) {
        result.ok = false;
        result.report = error;

        return result;
    }
    clearEverything(app);
    // The viewer's cache would look its frame up by the input it shows and the viewer hash; one
    // key answering two different references is a stale hit waiting to happen.
    std::map<std::tuple<int, U64, double, unsigned>, std::size_t> viewerKeys;
    for (std::size_t i = 0; i < records.size(); ++i) {
        const StepRecord& record = records[i];
        if (!applyStep(rt, record.step, 0, &error)) {
            std::ostringstream os;
            os << "NATRON_CACHE_STRESS_SEED=" << seed << ": step " << i << " applied in the reference pass but not in the cached pass: " << error << "\n"
               << formatLog(records, i);
            result.ok = false;
            result.failedStep = (int)i;
            result.report = os.str();

            return result;
        }
        const int probe = resolveProbe(rt);
        std::string mismatch;
        std::string mode;
        if (probe != record.probe) {
            mismatch = "the passes probe different nodes";
        }
        // Alternate which mode renders first, so each also runs against the other's entries.
        const bool legacyFirst = (i % 2) == 0;
        if (mismatch.empty() && options.cachedRender) {
            Probe got;
            if (probe >= 0) {
                setOutcome(&got, options.cachedRender(rt.graph.nodes[probe], currentTime(rt), rt.mipmap, windowAt(rt), &got.plane, &got.error));
            }
            if (options.cachedRenderReference) {
                mismatch = compareProbes(record.custom, got);
                if (!mismatch.empty()) {
                    mismatch = "against its own cache-off render: " + mismatch;
                }
            }
            if (mismatch.empty()) {
                mismatch = compareProbes(record.legacy, got, options.compare);
            }
            mode = options.cachedRenderName;
        }
        for (int pass = 0; (pass < 2) && mismatch.empty() && !options.cachedRender; ++pass) {
            const bool doLegacy = (pass == 0) == legacyFirst;
            if (doLegacy && options.legacy) {
                const Probe got = renderOnce(rt, probe, eRenderSchedulerModeLegacy, false);
                mismatch = compareProbes(record.legacy, got);
                mode = "legacy";
            } else if (!doLegacy && options.taskGraph) {
                const Probe got = renderOnce(rt, probe, eRenderSchedulerModeTaskGraph, false);
                mismatch = compareProbes(record.taskGraph, got);
                mode = "task graph";
            }
        }
        if (mismatch.empty() && (rt.graph.viewer >= 0) && (probe >= 0)) {
            const Probe& ref = options.legacy ? record.legacy : record.taskGraph;
            const std::tuple<int, U64, double, unsigned> key(probe, rt.graph.nodes[rt.graph.viewer]->getHashValue(), record.time, record.mipmap);
            std::map<std::tuple<int, U64, double, unsigned>, std::size_t>::const_iterator seen = viewerKeys.find(key);
            if (seen != viewerKeys.end()) {
                const Probe& earlier = options.legacy ? records[seen->second].legacy : records[seen->second].taskGraph;
                const std::string diff = compareProbes(earlier, ref);
                if (!diff.empty()) {
                    std::ostringstream os;
                    os << "the viewer hash and shown input equal those of step " << seen->second << " but the image differs (" << diff << ")";
                    mismatch = os.str();
                    mode = "viewer key";
                }
            } else {
                viewerKeys[key] = i;
            }
        }
        if (!mismatch.empty()) {
            std::ostringstream os;
            os << "cache stress mismatch: NATRON_CACHE_STRESS_SEED=" << seed << " step " << i << " (" << mode << "): " << mismatch << "\n"
               << "op log:\n"
               << formatLog(records, i);
            result.ok = false;
            result.failedStep = (int)i;
            result.report = os.str();

            return result;
        }
    }

    return result;
} // run

int
stepBudget(int defaultSteps)
{
    const char* v = envString("NATRON_CACHE_STRESS_STEPS");

    if (!v) {
        return defaultSteps;
    }
    const int n = std::atoi(v);

    return (n > 0) ? n : defaultSteps;
}

std::vector<std::uint32_t>
seedsToRun(const std::vector<std::uint32_t>& defaults)
{
    const char* single = envString("NATRON_CACHE_STRESS_SEED");

    if (single) {
        return std::vector<std::uint32_t>(1, (std::uint32_t)std::strtoul(single, 0, 10));
    }
    std::vector<std::uint32_t> seeds = defaults;
    const char* extra = envString("NATRON_CACHE_STRESS_SEEDS");
    const int n = extra ? std::atoi(extra) : 0;
    const std::uint32_t base = defaults.empty() ? 1u : defaults.front();
    for (int i = 0; i < n; ++i) {
        seeds.push_back(base ^ (0x9e3779b9u * (std::uint32_t)(i + 1)));
    }

    return seeds;
}

int
expectClean(const AppInstancePtr& app,
            const GraphBuilder& builder,
            const std::vector<std::uint32_t>& seeds,
            Options options)
{
    options.steps = stepBudget(options.steps);
    const std::vector<std::uint32_t> toRun = options.seeded ? seedsToRun(seeds) : std::vector<std::uint32_t>(1, seeds.empty() ? 0u : seeds.front());
    int failures = 0;

    for (std::size_t i = 0; i < toRun.size(); ++i) {
        std::printf("[ STRESS   ] seed %u, %d scripted + %d seeded steps\n", toRun[i], (int)options.scripted.size(), options.seeded ? options.steps : 0);
        std::fflush(stdout);
        const Result r = run(app, builder, toRun[i], options);
        if (!r.ok) {
            ++failures;
            ADD_FAILURE() << r.report;
        }
    }

    return failures;
}

} // namespace CacheStress

NATRON_NAMESPACE_EXIT
