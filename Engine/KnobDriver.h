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

#ifndef Engine_KnobDriver_h
#define Engine_KnobDriver_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <memory>
#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/RenderContext.h"
#include "Engine/ViewIdx.h"
#include "Serialization/SerializationFwd.h"

#define kKnobDriverDefaultVariable "loop.t"

NATRON_NAMESPACE_ENTER

class ContentHasher;

/**
 * @brief Sets a knob dimension from a render context variable: under a context holding the
 * variable, the dimension reads start + (end - start) * ease(t), t being the variable's value
 * and start and end their values at the render time, and its own value under any other. Linear
 * easing extrapolates outside [0, 1]; smooth and curve clamp t to it. Start and end are each a
 * constant or, when keyed, a curve over time. A driver never changes once made: a knob is given
 * a new one instead. MT-safe.
 **/
class KnobDriver
    : public ContextReadSource {
public:
    enum EasingEnum {
        eEasingLinear = 0,
        eEasingSmooth,
        eEasingCurve,
    };

    /**
     * @brief curve is the easing curve over [0, 1], used by eEasingCurve only. startCurve and
     * endCurve animate start and end over time; one without keys leaves the constant in use.
     * The driver keeps copies of the curves.
     **/
    KnobDriver(const std::string& variable,
               double start,
               double end,
               EasingEnum easing,
               const Curve* curve = NULL,
               const Curve* startCurve = NULL,
               const Curve* endCurve = NULL);

    virtual ~KnobDriver();

    /**
     * @brief The context name a variable stands for: the part after the last dot is the name,
     * the part before it the namespace ("loop.t", "loops.outer.t", or a plain "gain").
     **/
    static RenderContextName parseVariable(const std::string& variable);

    static const char* easingToString(EasingEnum easing);

    /**
     * @brief False for an unknown name; an empty one is linear.
     **/
    static bool easingFromString(const std::string& name, EasingEnum* easing);

    static KnobDriverPtr fromSerialization(const Serialization::KnobDriverSerialization& serialization);
    void toSerialization(Serialization::KnobDriverSerialization* serialization) const;

    const std::string& getVariable() const
    {
        return _variable;
    }

    /**
     * @brief The constant start, which the start curve overrides while it has keys.
     **/
    double getStart() const
    {
        return _start;
    }

    double getEnd() const
    {
        return _end;
    }

    /**
     * @brief The start curve, null unless start is keyed.
     **/
    const Curve* getStartCurve() const
    {
        return _startCurve.get();
    }

    const Curve* getEndCurve() const
    {
        return _endCurve.get();
    }

    double getStartAt(double time) const;
    double getEndAt(double time) const;

    /**
     * @brief A copy with the start (or end) curve replaced. When the curve has no keys the
     * constant becomes this driver's value at time, as removing a knob's last key keeps its value.
     */
    KnobDriverPtr withRangeCurve(bool end, const Curve& curve, double time) const;

    /**
     * @brief Appends the times of the start and end keys.
     */
    void appendRangeKeyTimes(std::list<double>* times) const;

    /**
     * @brief Whether start or end is keyed, so the driven value changes with the time.
     **/
    bool isAnimated() const
    {
        return bool(_startCurve) || bool(_endCurve);
    }

    EasingEnum getEasing() const
    {
        return _easing;
    }

    /**
     * @brief The easing curve, null unless the easing is eEasingCurve and one was given.
     **/
    const Curve* getCurve() const
    {
        return _curve.get();
    }

    /**
     * @brief The eased fraction of the way from start to end at t.
     **/
    double ease(double t) const;

    /**
     * @brief The driven value under context, and true, if context holds the variable as a
     * number or a bool, or as a binding whose knob gives one at time and view. A plain name the
     * context lacks is read, as rfv reads it, from the variable of that name project declares,
     * under context; project may be null.
     **/
    bool evaluate(const RenderContextPtr& context, const Project* project, double time, ViewIdx view, double* value) const;

    /**
     * @brief The project variable a plain name could fall back to; empty for a namespaced one.
     **/
    std::string getProjectVariableName() const;

    void appendContentHash(ContentHasher& h) const;

    virtual void appendContextReads(RenderContextReadSet* reads) const OVERRIDE FINAL;

private:
    const std::string _variable;
    const RenderContextName _name;
    const double _start;
    const double _end;
    const EasingEnum _easing;
    const std::shared_ptr<const Curve> _curve;
    const std::shared_ptr<const Curve> _startCurve;
    const std::shared_ptr<const Curve> _endCurve;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_KnobDriver_h
