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

#include "RFVSwitch.h"

#include <string>
#include <vector>

#include <QtCore/QString>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ContentHasher.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/RFV/RFVPatternMatch.h"
#include "Engine/Project.h"
#include "Engine/PyRFV.h"
#include "Engine/RFV.h"
#include "Engine/RenderContext.h"

NATRON_NAMESPACE_ENTER

namespace {

// The text of the pending Python error, which it clears.
std::string
takePythonError()
{
    PyObject* type = NULL;
    PyObject* value = NULL;
    PyObject* traceback = NULL;
    PyErr_Fetch(&type, &value, &traceback);
    std::string message;
    if (value) {
        PyObject* text = PyObject_Str(value);
        const char* utf8 = text ? PyUnicode_AsUTF8(text) : NULL;
        if (utf8) {
            message = utf8;
        }
        Py_XDECREF(text);
    }
    Py_XDECREF(type);
    Py_XDECREF(value);
    Py_XDECREF(traceback);
    PyErr_Clear();

    return message;
}
} // anonymous namespace

void
RFVSwitch::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    KnobStringPtr variable = createKnob<KnobString>(tr(kRFVSwitchParamVariableLabel));
    variable->setName(kRFVSwitchParamVariable);
    variable->setHintToolTip(tr(kRFVSwitchParamVariableHint));
    variable->setAnimationEnabled(false);
    page->addKnob(variable);
    _variable = variable;

    KnobStringPtr patterns = createKnob<KnobString>(tr(kRFVSwitchParamPatternsLabel));
    patterns->setName(kRFVSwitchParamPatterns);
    patterns->setHintToolTip(tr(kRFVSwitchParamPatternsHint));
    patterns->setAnimationEnabled(false);
    patterns->setAsMultiLine();
    page->addKnob(patterns);
    _patterns = patterns;

    KnobChoicePtr ifNoMatch = createKnob<KnobChoice>(tr(kRFVSwitchParamIfNoMatchLabel));
    ifNoMatch->setName(kRFVSwitchParamIfNoMatch);
    ifNoMatch->setHintToolTip(tr(kRFVSwitchParamIfNoMatchHint));
    ifNoMatch->setAnimationEnabled(false);
    std::vector<ChoiceOption> options;
    options.push_back(ChoiceOption(kRFVSwitchIfNoMatchError, "", tr(kRFVSwitchIfNoMatchErrorHint).toStdString()));
    options.push_back(ChoiceOption(kRFVSwitchIfNoMatchFirstInput, "", tr(kRFVSwitchIfNoMatchFirstInputHint).toStdString()));
    ifNoMatch->populateChoices(options);
    ifNoMatch->setDefaultValue(eRFVSwitchIfNoMatchError);
    page->addKnob(ifNoMatch);
    _ifNoMatch = ifNoMatch;
}

bool
RFVSwitch::knobChanged(KnobI* k,
                       ValueChangedReasonEnum /*reason*/,
                       ViewSpec /*view*/,
                       double /*time*/,
                       bool /*originatedFromMainThread*/)
{
    if (k == _patterns.lock().get()) {
        refreshInputsVisibility();

        return true;
    }
    if (k == _variable.lock().get()) {
        invalidateRenderContextReads();

        return true;
    }

    return false;
}

void
RFVSwitch::onInputChanged(int /*inputNo*/)
{
    refreshInputsVisibility();
}

void
RFVSwitch::onEffectCreated(bool /*mayCreateFileDialog*/,
                           const CreateNodeArgs& /*args*/)
{
    refreshInputsVisibility();
}

void
RFVSwitch::refreshInputsVisibility()
{
    KnobStringPtr patterns = _patterns.lock();
    NodePtr node = getNode();
    if (!patterns || !node) {
        return;
    }
    const int lines = (int)RFV::splitPatternLines(patterns->getValue()).size();
    const int inputs = node->getNInputs();
    for (int i = 0; i < inputs; ++i) {
        const bool visible = (i == 0) || (i < lines) || bool(node->getInput(i));
        if (node->isInputVisible(i) != visible) {
            node->setInputVisible(i, visible);
        }
    }
}

bool
RFVSwitch::readVariable(const std::string& name,
                        double time,
                        ViewIdx view,
                        std::string* value,
                        std::string* error) const
{
    KnobStringPtr variable = _variable.lock();
    if (!variable || name.empty()) {
        return false;
    }
    PythonGILLocker pgl;
    // rfv resolves the variable for an expression of this node: under the context the node is
    // queried under, a Set node's binding evaluated at time, and the project's value otherwise.
    PyRFV::ExpressionScope scope(variable.get(), time, view);
    PyObject* object = PyRFV::getVariable(name);
    if (!object) {
        *error = takePythonError();

        return false;
    }
    bool found = false;
    if ((object != Py_None) && !PyList_Check(object) && !PyTuple_Check(object)) {
        PyObject* text = PyObject_Str(object);
        const char* utf8 = text ? PyUnicode_AsUTF8(text) : NULL;
        if (utf8) {
            *value = utf8;
            found = true;
        } else {
            *error = takePythonError();
        }
        Py_XDECREF(text);
    }
    Py_DECREF(object);

    return found;
}

Route
RFVSwitch::route(double time,
                 ViewIdx view)
{
    KnobStringPtr variable = _variable.lock();
    KnobStringPtr patterns = _patterns.lock();
    KnobChoicePtr ifNoMatch = _ifNoMatch.lock();
    if (!variable || !patterns || !ifNoMatch) {
        return Route::black();
    }
    const std::string name = variable->getValue();
    std::vector<std::string> lines = RFV::splitPatternLines(patterns->getValue());
    // A line past the last input chooses nothing.
    if ((int)lines.size() > getNInputs()) {
        lines.resize(getNInputs());
    }

    std::string value;
    std::string readError;
    std::string patternError;
    const bool isSet = readVariable(name, time, view, &value, &readError);
    // Matched even when unset, for the errors of the patterns.
    const int matched = RFV::firstMatch(value, lines, &patternError);
    const int chosen = isSet ? matched : -1;
    if (!patternError.empty() && (chosen >= 0)) {
        setPersistentMessage(eMessageTypeWarning, tr("Patterns %1").arg(QString::fromUtf8(patternError.c_str())).toStdString());
    }

    int input = chosen;
    if ((input < 0) && (ifNoMatch->getValue() == eRFVSwitchIfNoMatchFirstInput)) {
        input = 0;
    }
    if (input >= 0) {
        if (!getInput(input)) {
            return Route::black();
        }

        return Route::input(input, time);
    }

    const QString variableName = QString::fromUtf8(name.c_str());
    QString message;
    if (name.empty()) {
        message = tr("No variable is set to switch on.");
    } else if (!readError.empty()) {
        message = tr("The variable %1 could not be read: %2").arg(variableName, QString::fromUtf8(readError.c_str()));
    } else if (!isSet) {
        message = tr("The variable %1 is unset or a list on this path.").arg(variableName);
    } else {
        message = tr("The variable %1 = '%2' matches no pattern.").arg(variableName, QString::fromUtf8(value.c_str()));
    }
    if (!patternError.empty()) {
        message += QLatin1Char(' ') + tr("Patterns %1").arg(QString::fromUtf8(patternError.c_str()));
    }
    setPersistentMessage(eMessageTypeError, message.toStdString());

    return Route::none();
} // RFVSwitch::route

StatusEnum
RFVSwitch::render(const RenderActionArgs& /*args*/)
{
    // Reached only when route() found no input to pass through and posted why.
    return eStatusFailed;
}

RenderContextReadSet
RFVSwitch::getExtraContextReads() const
{
    RenderContextReadSet reads;
    KnobStringPtr variable = _variable.lock();
    const std::string name = variable ? variable->getValue() : std::string();
    if (name.empty()) {
        return reads;
    }
    reads.add(RenderContextName::plain(name));
    AppInstancePtr app = getApp();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();
    if (project) {
        RFV::appendProjectVariableReads(*project, &reads);
    }

    return reads;
}

void
RFVSwitch::appendNodeContentHash(ContentHasher& h) const
{
    AppInstancePtr app = getApp();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();
    const RenderContextReadSet reads = getExtraContextReads();
    if (!project || reads.empty()) {
        return;
    }
    const RFVDeclarationsConstPtr declared = RFV::getDeclarationSnapshot(project.get());
    if (!declared) {
        return;
    }
    for (const RFV::VariableDeclaration& declaration : *declared) {
        if (reads.readsAll() || reads.contains(RenderContextName::plain(declaration.name))) {
            h.appendString(declaration.name);
            h.appendU64(declaration.knob->getContentHash());
        }
    }
}

NATRON_NAMESPACE_EXIT
