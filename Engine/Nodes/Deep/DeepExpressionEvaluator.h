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

#ifndef Engine_Nodes_Deep_DeepExpressionEvaluator_h
#define Engine_Nodes_Deep_DeepExpressionEvaluator_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>
#include <vector>

#include "Engine/DeepPixelOps.h"
#include "Engine/EngineFwd.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief A per-sample float expression, compiled once and evaluated once per deep sample.
 *
 * Grammar, with C precedence and associativity, all values float with IEEE semantics (a
 * division by zero yields inf or NaN and passes through):
 *
 *   numeric literals                    1  0.5  .25  1e-3
 *   identifiers                         [A-Za-z_][A-Za-z0-9_.]*
 *   unary                               -  !
 *   binary                              +  -  *  /  %  ^        (% is fmod, ^ is power and
 *                                                               binds tighter than unary minus,
 *                                                               right-associative)
 *   comparison                          <  <=  >  >=  ==  !=    (yield 1 or 0)
 *   logical                             &&  ||                  (non-zero is true, yield 1 or 0)
 *   conditional                         c ? a : b
 *   parentheses, function calls         f(a, b, ...)
 *
 * Identifiers resolve, at compile time, first to the builtins
 *
 *   x  y            the pixel's coordinates
 *   sampleIndex     the sample's index within its pixel, from 0
 *   sampleCount     the number of samples in the pixel
 *   frame           the frame being rendered
 *   pi
 *   Z  ZBack        the sample's depths, ZBack reading as Z on a point sample
 *
 * and then to a channel of the view being evaluated, by the name given for it at compile
 * time; dotted names such as diffuse.R are allowed. Functions:
 *
 *   abs floor ceil round sqrt exp log sin cos tan     (x)
 *   pow min max step atan2                            (a, b)     step(edge, x) is x < edge ? 0 : 1
 *   clamp lerp smoothstep                             (a, b, c)  clamp(x, lo, hi), lerp(a, b, t),
 *                                                                smoothstep(e0, e1, x)
 *
 * Compilation produces a flat postfix program run on a fixed-size local stack; evaluate()
 * allocates nothing and touches no shared state, so one compiled expression can be evaluated
 * from any number of threads at once. Expressions needing more than kMaxStackDepth
 * intermediate values, or nesting deeper than that, are rejected at compile time.
 **/
class DeepExpressionEvaluator {
public:
    static constexpr int kMaxStackDepth = 64;

    enum OpEnum {
        eOpConstant,
        eOpChannel,
        eOpZ,
        eOpZBack,
        eOpX,
        eOpY,
        eOpSampleIndex,
        eOpSampleCount,
        eOpFrame,
        eOpNegate,
        eOpNot,
        eOpAdd,
        eOpSubtract,
        eOpMultiply,
        eOpDivide,
        eOpModulo,
        eOpPower,
        eOpLess,
        eOpLessEqual,
        eOpGreater,
        eOpGreaterEqual,
        eOpEqual,
        eOpNotEqual,
        eOpAnd,
        eOpOr,
        eOpSelect,
        eOpAbs,
        eOpFloor,
        eOpCeil,
        eOpRound,
        eOpSqrt,
        eOpExp,
        eOpLog,
        eOpSin,
        eOpCos,
        eOpTan,
        eOpMin,
        eOpMax,
        eOpStep,
        eOpAtan2,
        eOpClamp,
        eOpLerp,
        eOpSmoothstep
    };

    struct CompileError {
        int column;
        std::string message;

        CompileError()
            : column(0)
            , message()
        {
        }
    };

    DeepExpressionEvaluator();

    /**
     * @brief Compiles source against channelNames, which names the entries of the
     * DeepPixelView::channels evaluate() will be handed, in order. On failure returns false,
     * fills *error with a 1-based column and a message, and leaves this holding no program.
     **/
    bool compile(const std::string& source,
                 const std::vector<std::string>& channelNames,
                 CompileError* error) WARN_UNUSED_RETURN;

    bool isCompiled() const
    {
        return !_ops.empty();
    }

    // The number of stack slots evaluate() uses; at most kMaxStackDepth.
    int getStackDepth() const
    {
        return _stackDepth;
    }

    float evaluate(const DeepPixelView& pixel,
                   int sampleIndex,
                   int x,
                   int y,
                   float frame) const;

private:
    struct Op {
        OpEnum op;
        int index;
        float value;
    };

    class Parser;

    std::vector<Op> _ops;
    int _stackDepth;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Deep_DeepExpressionEvaluator_h
