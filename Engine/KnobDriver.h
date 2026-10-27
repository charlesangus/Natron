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
 * variable, the dimension reads start + (end - start) * ease(t), t being the variable's value,
 * and its own value under any other. Linear easing extrapolates outside [0, 1]; smooth and curve
 * clamp t to it. A driver never changes once made: a knob is given a new one instead. MT-safe.
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
     * @brief curve is the easing curve over [0, 1], used by eEasingCurve only; the driver keeps
     * a copy of it.
     **/
    KnobDriver(const std::string& variable,
               double start,
               double end,
               EasingEnum easing,
               const Curve* curve = NULL);

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

    double getStart() const
    {
        return _start;
    }

    double getEnd() const
    {
        return _end;
    }

    EasingEnum getEasing() const
    {
        return _easing;
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
};

NATRON_NAMESPACE_EXIT

#endif // Engine_KnobDriver_h
