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

#include "PyRFV.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Knob.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/RFV.h"
#include "Engine/RenderContext.h"
#include "Engine/TimeLine.h"

NATRON_NAMESPACE_ENTER

namespace PyRFV {

const char* const kMainGlobalName = "NatronEngine_rfv";

namespace {

    // The knobs whose expressions this thread evaluates and the times and views they are
    // evaluated at, innermost last.
    struct ExpressionEntry {
        const KnobI* knob;
        ViewIdx view;
        bool hasTime;
        double time;
    };

    thread_local std::vector<ExpressionEntry> t_expressions;

    // The project variables this thread evaluates for rfv, innermost last, each with the time,
    // view and context it is evaluated for. A variable whose expression reads itself through rfv
    // gets None rather than recursing, and the rfv reads of a variable's expression keep the
    // render the outermost read came from: the project's knobs have no render of their own.
    struct VariableEvaluation {
        std::string name;
        double time = 0.;
        ViewIdx view = ViewIdx(0);
        RenderContextPtr context;
    };

    thread_local std::vector<VariableEvaluation> t_evaluatingVariables;

    class EvaluatingVariable {
    public:
        explicit EvaluatingVariable(const VariableEvaluation& evaluation)
        {
            t_evaluatingVariables.push_back(evaluation);
        }

        ~EvaluatingVariable()
        {
            t_evaluatingVariables.pop_back();
        }
    };

    bool
    isEvaluating(const std::string& name)
    {
        for (const VariableEvaluation& evaluation : t_evaluatingVariables) {
            if (evaluation.name == name) {
                return true;
            }
        }

        return false;
    }

    template <typename T>
    PyObject*
    toPythonList(const std::vector<T>& values,
                 PyObject* (*convert)(const T&))
    {
        PyObject* list = PyList_New((Py_ssize_t)values.size());
        if (!list) {
            return NULL;
        }
        for (std::size_t i = 0; i < values.size(); ++i) {
            PyObject* item = convert(values[i]);
            if (!item) {
                Py_DECREF(list);

                return NULL;
            }
            // Steals the reference, and cannot fail on an index of the list just made.
            PyList_SetItem(list, (Py_ssize_t)i, item);
        }

        return list;
    }

    PyObject*
    fromInt(const std::int64_t& value)
    {
        return PyLong_FromLongLong((long long)value);
    }

    PyObject*
    fromDouble(const double& value)
    {
        return PyFloat_FromDouble(value);
    }

    PyObject*
    fromString(const std::string& value)
    {
        return PyUnicode_FromStringAndSize(value.data(), (Py_ssize_t)value.size());
    }

    PyObject*
    toPython(const RenderContextValue& value)
    {
        if (const bool* b = std::get_if<bool>(&value)) {
            return PyBool_FromLong(*b ? 1 : 0);
        }
        if (const std::int64_t* i = std::get_if<std::int64_t>(&value)) {
            return fromInt(*i);
        }
        if (const double* d = std::get_if<double>(&value)) {
            return fromDouble(*d);
        }
        if (const std::string* s = std::get_if<std::string>(&value)) {
            return fromString(*s);
        }
        if (const std::vector<double>* ds = std::get_if<std::vector<double>>(&value)) {
            return toPythonList<double>(*ds, &fromDouble);
        }
        if (const std::vector<std::int64_t>* is = std::get_if<std::vector<std::int64_t>>(&value)) {
            return toPythonList<std::int64_t>(*is, &fromInt);
        }
        if (const std::vector<std::string>* ss = std::get_if<std::vector<std::string>>(&value)) {
            return toPythonList<std::string>(*ss, &fromString);
        }
        Py_RETURN_NONE;
    }

    // The UTF-8 text of a str, through the calls the limited API offers.
    bool
    utf8Of(PyObject* object,
           std::string* text)
    {
        if (!PyUnicode_Check(object)) {
            return false;
        }
        PyObject* bytes = PyUnicode_AsUTF8String(object);
        if (!bytes) {
            return false;
        }
        const char* data = PyBytes_AsString(bytes);
        if (data) {
            text->assign(data, (std::size_t)PyBytes_Size(bytes));
        }
        Py_DECREF(bytes);

        return data != NULL;
    }

    PyObject*
    rfvGetAttr(PyObject* self,
               PyObject* attribute)
    {
        std::string name;
        // Python's own protocol (__class__, __repr__, ...) is not a variable.
        if (!utf8Of(attribute, &name) || (name.compare(0, 2, "__") == 0)) {
            PyErr_Clear();
            return PyObject_GenericGetAttr(self, attribute);
        }

        return getVariable(name);
    }

    PyObject*
    rfvCall(PyObject* /*self*/,
            PyObject* args,
            PyObject* kwargs)
    {
        if (kwargs && (PyDict_Size(kwargs) > 0)) {
            PyErr_SetString(PyExc_TypeError, "rfv() takes no keyword arguments");

            return NULL;
        }
        const char* name = NULL;
        if (!PyArg_ParseTuple(args, "s:rfv", &name)) {
            return NULL;
        }

        return getVariable(name);
    }

    PyObject*
    rfvSubscript(PyObject* /*self*/,
                 PyObject* key)
    {
        std::string name;
        if (!utf8Of(key, &name)) {
            if (!PyErr_Occurred()) {
                PyErr_SetString(PyExc_TypeError, "rfv keys are variable names");
            }

            return NULL;
        }

        return getVariable(name);
    }

    PyObject*
    createRFVObject()
    {
        static PyType_Slot slots[] = {
            { Py_tp_new, (void*)&PyType_GenericNew },
            { Py_tp_getattro, (void*)&rfvGetAttr },
            { Py_tp_call, (void*)&rfvCall },
            { Py_mp_subscript, (void*)&rfvSubscript },
            { Py_tp_doc, (void*)"Reverse-Flow Variables: rfv.name, rfv('name') or rfv['name'] is the variable's value, None when unset." },
            { 0, NULL },
        };
        static PyType_Spec spec = {
            "NatronEngine.RFV",
            (int)sizeof(PyObject),
            0,
            Py_TPFLAGS_DEFAULT,
            slots,
        };
        PyObject* type = PyType_FromSpec(&spec);
        if (!type) {
            return NULL;
        }
        PyObject* object = PyObject_CallObject(type, NULL);
        Py_DECREF(type);

        return object;
    }
} // anonymous namespace

void
ensureDeclared()
{
    PyObject* mainModule = NATRON_PYTHON_NAMESPACE::getMainModule();
    if (!mainModule || PyObject_HasAttrString(mainModule, kMainGlobalName)) {
        return;
    }
    PyObject* object = createRFVObject();
    if (!object) {
        PyErr_Print();

        return;
    }
    PyObject_SetAttrString(mainModule, kMainGlobalName, object);
    Py_DECREF(object);
}

PyObject*
getVariable(const std::string& name)
{
    if (isEvaluating(name)) {
        Py_RETURN_NONE;
    }
    const ExpressionEntry* entry = t_expressions.empty() ? NULL : &t_expressions.back();
    const KnobI* knob = entry ? entry->knob : NULL;
    KnobHolder* holder = knob ? knob->getHolder() : NULL;
    const EffectInstance* effect = dynamic_cast<const EffectInstance*>(holder);
    const VariableEvaluation* outer = t_evaluatingVariables.empty() ? NULL : &t_evaluatingVariables.back();

    // In a render, or acting under a context for one, the context's variable wins. An expression
    // of a project variable read for a render reads under that render's context.
    RenderContextPtr context;
    const bool acting = ActionContextScope::current(&context);
    const ParallelRenderArgsPtr frameArgs = effect ? effect->getParallelRenderArgsTLS() : ParallelRenderArgsPtr();
    if (!acting) {
        if (frameArgs) {
            context = effect->getCurrentRenderContext();
        } else if (outer) {
            context = outer->context;
        }
    }
    // Variables are read at the time the expression is evaluated at, otherwise at the time the
    // render started at, or outside a render at the timeline's frame; at the view the render
    // started at, or outside a render the view the expression is evaluated at.
    AppInstancePtr app = holder ? holder->getApp() : appPTR->getTopLevelInstance();
    double time = 0.;
    ViewIdx view(0);
    if (frameArgs) {
        time = frameArgs->time;
        view = frameArgs->view;
    } else if (outer) {
        time = outer->time;
        view = outer->view;
    } else {
        TimeLinePtr timeline = app ? app->getTimeLine() : TimeLinePtr();
        time = timeline ? (double)timeline->currentFrame() : 0.;
        view = entry ? entry->view : ViewIdx(0);
    }
    if (entry && entry->hasTime) {
        time = entry->time;
    }

    if (context) {
        if (const RenderContextValue* value = context->find(RenderContextName::plain(name))) {
            const RenderContextBinding* binding = std::get_if<RenderContextBinding>(value);
            if (!binding) {
                return toPython(*value);
            }
            // The Set's variable at this time, its expression reading the context the Set
            // received. A knob deleted since, or off its node, is unset.
            const KnobIPtr boundKnob = binding->knob.lock();
            const KnobHolder* boundHolder = boundKnob ? boundKnob->getHolder() : NULL;
            const EffectInstance* boundEffect = dynamic_cast<const EffectInstance*>(boundHolder);
            if (!boundHolder || (boundEffect && !boundEffect->getNode())) {
                Py_RETURN_NONE;
            }
            RenderContextValue bound;
            bool found = false;
            try {
                ActionContextScope bindingContext(true, binding->captured);
                found = RFV::getVariableValue(boundKnob, time, view, &bound);
            } catch (const std::exception& e) {
                PyErr_SetString(PyExc_RuntimeError, e.what());

                return NULL;
            }
            if (!found) {
                Py_RETURN_NONE;
            }

            return toPython(bound);
        }
    }

    // Otherwise the project's variable is the base every context starts from.
    ProjectPtr project = app ? app->getProject() : ProjectPtr();
    KnobIPtr variable = RFV::findDeclaredVariable(project.get(), name);
    if (!variable) {
        Py_RETURN_NONE;
    }
    VariableEvaluation evaluation;
    evaluation.name = name;
    evaluation.context = context;
    evaluation.time = time;
    evaluation.view = view;
    RenderContextValue value;
    bool found = false;
    try {
        EvaluatingVariable evaluating(evaluation);
        found = RFV::getVariableValue(variable, evaluation.time, evaluation.view, &value);
    } catch (const std::exception& e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());

        return NULL;
    }
    if (!found) {
        Py_RETURN_NONE;
    }

    return toPython(value);
} // getVariable

RenderContextPtr
getProjectExpressionContext()
{
    RenderContextPtr context;
    if (ActionContextScope::current(&context)) {
        return context;
    }

    return t_evaluatingVariables.empty() ? RenderContextPtr() : t_evaluatingVariables.back().context;
}

ExpressionScope::ExpressionScope(const KnobI* knob,
                                 ViewIdx view)
{
    t_expressions.push_back(ExpressionEntry { knob, view, false, 0. });
}

ExpressionScope::ExpressionScope(const KnobI* knob,
                                 double time,
                                 ViewIdx view)
{
    t_expressions.push_back(ExpressionEntry { knob, view, true, time });
}

ExpressionScope::~ExpressionScope()
{
    t_expressions.pop_back();
}

} // namespace PyRFV

NATRON_NAMESPACE_EXIT
