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

#include "KnobDriver.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <variant>

#include "Engine/ContentHasher.h"
#include "Engine/Curve.h"
#include "Engine/EffectInstance.h"
#include "Engine/Knob.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/RFV.h"
#include "Serialization/CurveSerialization.h"
#include "Serialization/KnobSerialization.h"

NATRON_NAMESPACE_ENTER

namespace {
std::shared_ptr<const Curve>
keyedCopy(const Curve* curve)
{
    return (curve && (curve->getKeyFramesCount() > 0)) ? std::make_shared<const Curve>(*curve) : std::shared_ptr<const Curve>();
}

void
appendCurveHash(ContentHasher& h,
                const std::shared_ptr<const Curve>& curve)
{
    h.appendBool(bool(curve));
    if (curve) {
        curve->appendContentHash(h);
    }
}

void
curveToSerialization(const std::shared_ptr<const Curve>& curve,
                     std::optional<Serialization::CurveSerialization>* serialization)
{
    serialization->reset();
    if (curve) {
        serialization->emplace();
        curve->toSerialization(&(**serialization));
    }
}
} // anonymous namespace

KnobDriver::KnobDriver(const std::string& variable,
                       double start,
                       double end,
                       EasingEnum easing,
                       const Curve* curve,
                       const Curve* startCurve,
                       const Curve* endCurve)
    : ContextReadSource()
    , _variable(variable.empty() ? std::string(kKnobDriverDefaultVariable) : variable)
    , _name(parseVariable(_variable))
    , _start(start)
    , _end(end)
    , _easing(easing)
    , _curve((curve && (easing == eEasingCurve)) ? std::make_shared<const Curve>(*curve) : std::shared_ptr<const Curve>())
    , _startCurve(keyedCopy(startCurve))
    , _endCurve(keyedCopy(endCurve))
{
}

KnobDriver::~KnobDriver()
{
}

RenderContextName
KnobDriver::parseVariable(const std::string& variable)
{
    const std::size_t dot = variable.rfind('.');

    if (dot == std::string::npos) {
        return RenderContextName::plain(variable);
    }

    return RenderContextName(variable.substr(0, dot), variable.substr(dot + 1));
}

const char*
KnobDriver::easingToString(EasingEnum easing)
{
    switch (easing) {
    case eEasingLinear:
        return "linear";
    case eEasingSmooth:
        return "smooth";
    case eEasingCurve:
        return "curve";
    }

    return "linear";
}

bool
KnobDriver::easingFromString(const std::string& name,
                             EasingEnum* easing)
{
    if (name.empty() || (name == "linear")) {
        *easing = eEasingLinear;
    } else if (name == "smooth") {
        *easing = eEasingSmooth;
    } else if (name == "curve") {
        *easing = eEasingCurve;
    } else {
        return false;
    }

    return true;
}

KnobDriverPtr
KnobDriver::fromSerialization(const Serialization::KnobDriverSerialization& serialization)
{
    EasingEnum easing;

    if (!easingFromString(serialization.easing, &easing)) {
        return KnobDriverPtr();
    }
    Curve curve;
    if (serialization.curve) {
        curve.fromSerialization(*serialization.curve);
    }
    Curve startCurve;
    if (serialization.startCurve) {
        startCurve.fromSerialization(*serialization.startCurve);
    }
    Curve endCurve;
    if (serialization.endCurve) {
        endCurve.fromSerialization(*serialization.endCurve);
    }

    return std::make_shared<const KnobDriver>(serialization.variable, serialization.start, serialization.end, easing, &curve, &startCurve, &endCurve);
}

void
KnobDriver::toSerialization(Serialization::KnobDriverSerialization* serialization) const
{
    serialization->variable = _variable;
    serialization->start = _start;
    serialization->end = _end;
    serialization->easing = easingToString(_easing);
    curveToSerialization(_curve, &serialization->curve);
    curveToSerialization(_startCurve, &serialization->startCurve);
    curveToSerialization(_endCurve, &serialization->endCurve);
}

double
KnobDriver::getStartAt(double time) const
{
    return _startCurve ? _startCurve->getValueAt(time, false) : _start;
}

double
KnobDriver::getEndAt(double time) const
{
    return _endCurve ? _endCurve->getValueAt(time, false) : _end;
}

KnobDriverPtr
KnobDriver::withRangeCurve(bool end,
                           const Curve& curve,
                           double time) const
{
    const bool keyed = curve.getKeyFramesCount() > 0;
    const double start = (!end && !keyed) ? getStartAt(time) : _start;
    const double endValue = (end && !keyed) ? getEndAt(time) : _end;
    const Curve* startCurve = end ? _startCurve.get() : &curve;
    const Curve* endCurve = end ? &curve : _endCurve.get();

    return std::make_shared<const KnobDriver>(_variable, start, endValue, _easing, _curve.get(), startCurve, endCurve);
}

void
KnobDriver::appendRangeKeyTimes(std::list<double>* times) const
{
    const Curve* curves[2] = { _startCurve.get(), _endCurve.get() };
    for (int i = 0; i < 2; ++i) {
        if (!curves[i]) {
            continue;
        }
        const KeyFrameSet keys = curves[i]->getKeyFrames_mt_safe();
        for (KeyFrameSet::const_iterator it = keys.begin(); it != keys.end(); ++it) {
            times->push_back(it->getTime());
        }
    }
}

double
KnobDriver::ease(double t) const
{
    switch (_easing) {
    case eEasingLinear:
        return t;
    case eEasingSmooth:
        t = std::max(0., std::min(1., t));

        return t * t * (3. - 2. * t);
    case eEasingCurve:
        t = std::max(0., std::min(1., t));

        // Without keys the curve is the identity rather than Curve's constant 0.
        if (!_curve || (_curve->getKeyFramesCount() == 0)) {
            return t;
        }

        return _curve->getValueAt(t, false);
    }

    return t;
}

std::string
KnobDriver::getProjectVariableName() const
{
    return _name.ns.empty() ? _name.name : std::string();
}

bool
KnobDriver::evaluate(const RenderContextPtr& context,
                     const Project* project,
                     double time,
                     ViewIdx view,
                     double* value) const
{
    const RenderContextValue* held = context ? context->find(_name) : NULL;
    RenderContextValue bound;

    if (!held) {
        // The project's variable is the base every context starts from.
        const KnobIPtr variable = (project && _name.ns.empty()) ? RFV::findDeclaredVariable(project, _name.name) : KnobIPtr();
        if (!variable) {
            return false;
        }
        try {
            ActionContextScope variableContext(bool(context), context);
            if (!RFV::getVariableValue(variable, time, view, &bound)) {
                return false;
            }
        } catch (const std::exception&) {
            return false;
        }
        held = &bound;
    } else if (const RenderContextBinding* binding = std::get_if<RenderContextBinding>(held)) {
        // A time-varying variable: its knob's value at the driven knob's time, under the context
        // it captured, as rfv reads it. A knob deleted since, or off its node, is unset.
        const KnobIPtr boundKnob = binding->knob.lock();
        const KnobHolder* boundHolder = boundKnob ? boundKnob->getHolder() : NULL;
        const EffectInstance* boundEffect = dynamic_cast<const EffectInstance*>(boundHolder);
        if (!boundHolder || (boundEffect && !boundEffect->getNode())) {
            return false;
        }
        try {
            ActionContextScope bindingContext(true, binding->captured);
            if (!RFV::getVariableValue(boundKnob, time, view, &bound)) {
                return false;
            }
        } catch (const std::exception&) {
            return false;
        }
        held = &bound;
    }
    double t;
    if (const double* d = std::get_if<double>(held)) {
        t = *d;
    } else if (const std::int64_t* i = std::get_if<std::int64_t>(held)) {
        t = (double)*i;
    } else if (const bool* b = std::get_if<bool>(held)) {
        t = *b ? 1. : 0.;
    } else {
        return false;
    }
    const double start = getStartAt(time);
    *value = start + (getEndAt(time) - start) * ease(t);

    return true;
} // KnobDriver::evaluate

void
KnobDriver::appendContentHash(ContentHasher& h) const
{
    h.appendString(_variable);
    h.appendDouble(_start);
    h.appendDouble(_end);
    h.appendI64((std::int64_t)_easing);
    appendCurveHash(h, _curve);
    appendCurveHash(h, _startCurve);
    appendCurveHash(h, _endCurve);
}

void
KnobDriver::appendContextReads(RenderContextReadSet* reads) const
{
    reads->add(_name);
}

NATRON_NAMESPACE_EXIT
