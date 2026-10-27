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

#ifndef NATRON_ENGINE_PYRFV_H
#define NATRON_ENGINE_PYRFV_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The rfv Python object expressions read Reverse-Flow Variables through: rfv.<name>,
 * rfv('<name>') and rfv['<name>'] give the variable's value, and None when it is unset. The value
 * is read when the expression asks for it, on the thread evaluating the expression: the variable
 * of the render context the expression's node acts or renders under (see
 * EffectInstance::getCurrentRenderContext()) when that context sets it, otherwise the project's
 * variable. A context's binding (see RenderContextBinding) and the project's variable are
 * evaluated at the time the expression is evaluated at, and at the view the render started at,
 * or outside a render the view the expression is evaluated at. The project variable's own
 * expression reads rfv for that same render: at its time and view, under its context. Values are
 * Python bools, ints, floats, strings, or lists of them for a variable with several dimensions.
 **/
namespace PyRFV {

/**
 * @brief The __main__ global holding the rfv object; KnobHelper's expression scope binds the name
 * rfv to it.
 **/
extern const char* const kMainGlobalName;

/**
 * @brief Puts the rfv object in __main__ under kMainGlobalName unless it is there. The GIL must be
 * held.
 **/
void ensureDeclared();

/**
 * @brief The value of the variable as rfv gives it to the expression being evaluated on this
 * thread (see ExpressionScope): a new reference, Py_None when unset. The GIL must be held.
 **/
PyObject* getVariable(const std::string& name);

/**
 * @brief The context rfv reads under, on the calling thread, for an expression whose knob is not
 * on a node, such as a project variable's: that of the open ActionContextScope, else that of the
 * render a project variable is being read for through rfv, null outside both. A project-held
 * expression keeps its results under this context (see KnobHelper::getExpressionContextKey()).
 **/
RenderContextPtr getProjectExpressionContext();

/**
 * @brief Marks the knob whose expression the calling thread evaluates, and the time and view it
 * is evaluated at, for as long as the scope lives, so that rfv reads the context of the knob's
 * node and the variables of its project at that time. Without a time, rfv reads them at the
 * current time (see getVariable()). Scopes nest, the innermost winning.
 **/
class ExpressionScope {
public:
    explicit ExpressionScope(const KnobI* knob,
                             ViewIdx view = ViewIdx(0));
    ExpressionScope(const KnobI* knob,
                    double time,
                    ViewIdx view);
    ~ExpressionScope();

    ExpressionScope(const ExpressionScope&) = delete;
    ExpressionScope& operator=(const ExpressionScope&) = delete;
};

} // namespace PyRFV

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_PYRFV_H
