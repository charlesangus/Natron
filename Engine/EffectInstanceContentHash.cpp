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

#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include "Engine/Curve.h"
#include "Engine/Hash64.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"

NATRON_NAMESPACE_ENTER

namespace {
// The length goes first so that "ab" then "c" and "a" then "bc" hash apart.
void
appendString(Hash64* hash,
             const std::string& text)
{
    hash->append<U64>((U64)text.size());
    for (std::size_t i = 0; i < text.size(); i += 8) {
        U64 word = 0;
        for (std::size_t b = 0; (b < 8) && (i + b < text.size()); ++b) {
            word |= (U64)(unsigned char)text[i + b] << (8 * b);
        }
        hash->append<U64>(word);
    }
}

void
appendCurve(Hash64* hash,
            const CurvePtr& curve)
{
    const KeyFrameSet keys = curve ? curve->getKeyFrames_mt_safe() : KeyFrameSet();

    hash->append<U64>((U64)keys.size());
    for (KeyFrameSet::const_iterator it = keys.begin(); it != keys.end(); ++it) {
        hash->append<double>(it->getTime());
        hash->append<double>(it->getValue());
        hash->append<double>(it->getLeftDerivative());
        hash->append<double>(it->getRightDerivative());
        hash->append<int>((int)it->getInterpolation());
    }
}

void
appendKnob(Hash64* hash,
           const KnobIPtr& knob,
           double time,
           ViewIdx view)
{
    KnobChoice* choice = dynamic_cast<KnobChoice*>(knob.get());
    Knob<int>* intKnob = dynamic_cast<Knob<int>*>(knob.get());
    Knob<bool>* boolKnob = dynamic_cast<Knob<bool>*>(knob.get());
    Knob<double>* doubleKnob = dynamic_cast<Knob<double>*>(knob.get());
    Knob<std::string>* stringKnob = dynamic_cast<Knob<std::string>*>(knob.get());
    // A menu that is rebuilt (layers, colourspaces) can move an entry, so the entry counts, not its index.
    const std::vector<ChoiceOption> entries = choice ? choice->getEntries_mt_safe() : std::vector<ChoiceOption>();
    const int nDims = knob->getDimension();

    appendString(hash, knob->getName());
    for (int d = 0; d < nDims; ++d) {
        appendString(hash, knob->getExpression(d));
        const bool animated = knob->isAnimated(d, view);
        hash->append<int>(animated ? 1 : 0);
        if (animated) {
            const CurvePtr curve = knob->getCurve(view, d);
            appendCurve(hash, curve);
            // The keyframes of a string hold indices into the knob's own list of strings.
            if (stringKnob && curve) {
                const KeyFrameSet keys = curve->getKeyFrames_mt_safe();
                for (KeyFrameSet::const_iterator it = keys.begin(); it != keys.end(); ++it) {
                    appendString(hash, stringKnob->getValueAtTime(it->getTime(), d, view));
                }
            }
        }
        // The value at the time covers what the curve does not: an expression, or a link to
        // another knob.
        if (choice) {
            const int index = choice->getValueAtTime(time, d, view);
            appendString(hash, ((index >= 0) && ((std::size_t)index < entries.size())) ? entries[(std::size_t)index].id : std::string());
        } else if (intKnob) {
            hash->append<int>(intKnob->getValueAtTime(time, d, view));
        } else if (boolKnob) {
            hash->append<int>(boolKnob->getValueAtTime(time, d, view) ? 1 : 0);
        } else if (doubleKnob) {
            hash->append<double>(doubleKnob->getValueAtTime(time, d, view));
        } else if (stringKnob) {
            appendString(hash, stringKnob->getValueAtTime(time, d, view));
        }
    }
    KnobParametric* parametric = dynamic_cast<KnobParametric*>(knob.get());
    if (parametric) {
        for (int d = 0; d < nDims; ++d) {
            appendCurve(hash, parametric->getParametricCurve(d));
        }
    }
}

U64
contentHashOf(const NodePtr& node,
              double time,
              ViewIdx view,
              std::map<const Node*, U64>* done)
{
    const std::map<const Node*, U64>::const_iterator found = done->find(node.get());

    if (found != done->end()) {
        return found->second;
    }
    // The graph has no cycles, but a node reached again while its own hash is being taken would
    // otherwise recurse forever.
    (*done)[node.get()] = 0;

    Hash64 hash;
    appendString(&hash, node->getPluginID());
    hash.append<int>(node->getMajorVersion());
    hash.append<int>(node->getMinorVersion());

    const EffectInstancePtr effect = node->getEffectInstance();
    if (effect) {
        const KnobsVec knobs = effect->getKnobs_mt_safe();
        for (KnobsVec::const_iterator it = knobs.begin(); it != knobs.end(); ++it) {
            if (*it && effect->isKnobInContentHash(**it)) {
                appendKnob(&hash, *it, time, view);
            }
        }
        effect->appendContentHash(time, view, &hash);
    }
    // Roto shapes and paint strokes are not knobs; the node's age, which the project saves, moves
    // with every edit of them.
    if (node->getRotoContext()) {
        hash.append<U64>(node->getKnobsAge());
    }

    // getInput() resolves groups and precomps to the nodes that render, so their insides count.
    const int nInputs = node->getNInputs();
    for (int i = 0; i < nInputs; ++i) {
        const NodePtr input = node->getInput(i);
        if (input) {
            hash.append<int>(i);
            hash.append<U64>(contentHashOf(input, time, view, done));
        }
    }
    hash.computeHash();
    (*done)[node.get()] = hash.value();

    return hash.value();
}
} // anonymous namespace

U64
EffectInstance::computeContentHash(double time,
                                   ViewIdx view) const
{
    const NodePtr node = getNode();

    if (!node) {
        return 0;
    }
    std::map<const Node*, U64> done;

    return contentHashOf(node, time, view, &done);
}

bool
EffectInstance::isKnobInContentHash(const KnobI& knob) const
{
    // A knob that is not saved is rebuilt from other state on load, and one that does not
    // trigger a render does not change the pixels; either would only make the hash differ
    // between sessions or after harmless edits.
    if (!knob.getEvaluateOnChange() || !knob.getIsPersistent()) {
        return false;
    }

    return !dynamic_cast<const KnobButton*>(&knob) && !dynamic_cast<const KnobPage*>(&knob)
        && !dynamic_cast<const KnobGroup*>(&knob) && !dynamic_cast<const KnobSeparator*>(&knob);
}

NATRON_NAMESPACE_EXIT
