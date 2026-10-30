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

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <QtCore/QMutexLocker>
#include <QtCore/QString>

#include "Engine/AbortableRenderInfo.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ContentHasher.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/RFV/RFVPatternMatch.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/PyRFV.h"
#include "Engine/RFV.h"
#include "Engine/RenderContext.h"

NATRON_NAMESPACE_ENTER

namespace {

// The UTF-8 text of a str, through the calls the limited API offers.
bool
utf8Of(PyObject* object,
       std::string* text)
{
    if (!object || !PyUnicode_Check(object)) {
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
        utf8Of(text, &message);
        Py_XDECREF(text);
    }
    Py_XDECREF(type);
    Py_XDECREF(value);
    Py_XDECREF(traceback);
    PyErr_Clear();

    return message;
}

// The text patterns match a value against. False when Python fails, its error pending.
bool
canonicalText(PyObject* object,
              std::string* text)
{
    if (PyBool_Check(object)) {
        *text = (object == Py_True) ? "True" : "False";

        return true;
    }
    if (PyFloat_Check(object)) {
        const double value = PyFloat_AsDouble(object);
        // A whole number reads as an int would, so 5.0 matches the pattern 5.
        if (std::isfinite(value) && (std::floor(value) == value) && (std::fabs(value) < 1e15)) {
            *text = std::to_string((long long)value);

            return true;
        }
        // repr() is the shortest text that reads back as the same double.
        PyObject* repr = PyObject_Repr(object);
        const bool done = utf8Of(repr, text);
        Py_XDECREF(repr);

        return done;
    }
    if (PyList_Check(object) || PyTuple_Check(object)) {
        // No brackets: a [ in a glob opens a character class.
        const Py_ssize_t size = PySequence_Size(object);
        if (size < 0) {
            return false;
        }
        text->clear();
        for (Py_ssize_t i = 0; i < size; ++i) {
            PyObject* item = PySequence_GetItem(object, i);
            std::string itemText;
            const bool done = item && canonicalText(item, &itemText);
            Py_XDECREF(item);
            if (!done) {
                return false;
            }
            if (i > 0) {
                text->append(", ");
            }
            text->append(itemText);
        }

        return true;
    }
    if (PyUnicode_Check(object)) {
        return utf8Of(object, text);
    }
    PyObject* str = PyObject_Str(object);
    const bool done = utf8Of(str, text);
    Py_XDECREF(str);

    return done;
}

std::string
joined(const std::set<std::string>& lines)
{
    std::string text;
    for (const std::string& line : lines) {
        if (!text.empty()) {
            text += '\n';
        }
        text += line;
    }

    return text;
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

    KnobIntPtr count = createKnob<KnobInt>(tr(kRFVSwitchParamPatternCountLabel));
    count->setName(kRFVSwitchParamPatternCount);
    count->setHintToolTip(tr(kRFVSwitchParamPatternCountHint));
    count->setAnimationEnabled(false);
    count->setMinimum(1);
    count->setMaximum(kRFVSwitchInputCount);
    count->setDefaultValue(1);
    count->setSecretByDefault(true);
    page->addKnob(count);
    _patternCount = count;

    for (int i = 0; i < kRFVSwitchInputCount; ++i) {
        const std::string number = std::to_string(i);
        const QString suffix = QLatin1Char(' ') + QString::number(i);

        KnobStringPtr pattern = createKnob<KnobString>(tr(kRFVSwitchParamPatternLabel) + suffix);
        pattern->setName(kRFVSwitchParamPattern + number);
        pattern->setHintToolTip(tr(kRFVSwitchParamPatternHint));
        pattern->setAnimationEnabled(false);
        pattern->setAddNewLine(false);
        pattern->setSecretByDefault(i > 0);
        page->addKnob(pattern);
        _patterns.push_back(pattern);

        KnobButtonPtr addAfter = createKnob<KnobButton>(tr(kRFVSwitchParamAddAfterLabel) + suffix);
        addAfter->setName(kRFVSwitchParamAddAfter + number);
        addAfter->setHintToolTip(tr(kRFVSwitchParamAddAfterHint));
        addAfter->setIconLabel(NATRON_ICONS_PATH "lucide/plus.svg");
        addAfter->setEvaluateOnChange(false);
        addAfter->setAddNewLine(false);
        addAfter->setSecretByDefault(i > 0);
        page->addKnob(addAfter);
        _addAfter.push_back(addAfter);

        KnobButtonPtr remove = createKnob<KnobButton>(tr(kRFVSwitchParamRemoveLabel) + suffix);
        remove->setName(kRFVSwitchParamRemove + number);
        remove->setHintToolTip(tr(kRFVSwitchParamRemoveHint));
        remove->setIconLabel(NATRON_ICONS_PATH "lucide/minus.svg");
        remove->setEvaluateOnChange(false);
        remove->setSecretByDefault(i > 0);
        page->addKnob(remove);
        _remove.push_back(remove);
    }

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
} // RFVSwitch::initializeKnobs

bool
RFVSwitch::knobChanged(KnobI* k,
                       ValueChangedReasonEnum /*reason*/,
                       ViewSpec /*view*/,
                       double /*time*/,
                       bool /*originatedFromMainThread*/)
{
    if (!k) {
        return false;
    }
    for (int i = 0; i < (int)_addAfter.size(); ++i) {
        if (k == _addAfter[i].lock().get()) {
            insertRowAfter(i);

            return true;
        }
        if (k == _remove[i].lock().get()) {
            removeRow(i);

            return true;
        }
    }
    if (k == _patternCount.lock().get()) {
        refreshRows();
        refreshInputsVisibility();
        clearPostedMessage();

        return true;
    }
    if (k == _variable.lock().get()) {
        invalidateRenderContextReads();
        clearPostedMessage();

        return true;
    }
    if ((k == _ifNoMatch.lock().get()) || (k->getName().compare(0, sizeof(kRFVSwitchParamPattern) - 1, kRFVSwitchParamPattern) == 0)) {
        clearPostedMessage();

        return true;
    }

    return false;
}

void
RFVSwitch::onInputChanged(int /*inputNo*/)
{
    refreshInputsVisibility();
    clearPostedMessage();
}

void
RFVSwitch::onEffectCreated(bool /*mayCreateFileDialog*/,
                           const CreateNodeArgs& /*args*/)
{
    refreshRows();
    refreshInputsVisibility();
}

int
RFVSwitch::patternCount() const
{
    KnobIntPtr count = _patternCount.lock();
    const int value = count ? count->getValue() : 1;

    return std::max(1, std::min(value, (int)_patterns.size()));
}

std::vector<std::string>
RFVSwitch::patterns() const
{
    std::vector<std::string> values;
    const int count = std::min(patternCount(), getNInputs());
    for (int i = 0; i < count; ++i) {
        KnobStringPtr pattern = _patterns[i].lock();
        values.push_back(pattern ? pattern->getValue() : std::string());
    }

    return values;
}

void
RFVSwitch::refreshRows()
{
    const int count = patternCount();
    for (int i = 0; i < (int)_patterns.size(); ++i) {
        const bool shown = i < count;
        KnobStringPtr pattern = _patterns[i].lock();
        KnobButtonPtr addAfter = _addAfter[i].lock();
        KnobButtonPtr remove = _remove[i].lock();
        if (pattern && (pattern->getIsSecret() == shown)) {
            pattern->setSecret(!shown);
        }
        const bool addShown = shown && (count < (int)_patterns.size());
        if (addAfter && (addAfter->getIsSecret() == addShown)) {
            addAfter->setSecret(!addShown);
        }
        if (remove && (remove->getIsSecret() == shown)) {
            remove->setSecret(!shown);
        }
    }
}

void
RFVSwitch::insertRowAfter(int row)
{
    KnobIntPtr countKnob = _patternCount.lock();
    NodePtr node = getNode();
    const int count = patternCount();
    if (!countKnob || !node || (row < 0) || (row >= count) || (count >= (int)_patterns.size())) {
        return;
    }
    std::vector<std::string> values = patterns();
    values.resize(count);
    values.insert(values.begin() + row + 1, std::string());
    std::vector<NodePtr> inputs;
    for (int i = 0; i < node->getNInputs(); ++i) {
        inputs.push_back(node->getRealInput(i));
    }
    // An input past the last falls off.
    inputs.insert(inputs.begin() + row + 1, NodePtr());
    inputs.pop_back();

    beginChanges();
    for (int i = row + 1; i < (int)values.size(); ++i) {
        _patterns[i].lock()->setValue(values[i]);
    }
    countKnob->setValue(count + 1);
    endChanges();
    reconnectInputs(inputs);
}

void
RFVSwitch::removeRow(int row)
{
    KnobIntPtr countKnob = _patternCount.lock();
    NodePtr node = getNode();
    const int count = patternCount();
    if (!countKnob || !node || (row < 0) || (row >= count)) {
        return;
    }
    if (count == 1) {
        _patterns[0].lock()->setValue(std::string());

        return;
    }
    std::vector<std::string> values = patterns();
    values.resize(count);
    values.erase(values.begin() + row);
    values.push_back(std::string());
    std::vector<NodePtr> inputs;
    for (int i = 0; i < node->getNInputs(); ++i) {
        inputs.push_back(node->getRealInput(i));
    }
    inputs.erase(inputs.begin() + row);
    inputs.push_back(NodePtr());

    beginChanges();
    for (int i = row; i < (int)values.size(); ++i) {
        _patterns[i].lock()->setValue(values[i]);
    }
    countKnob->setValue(count - 1);
    endChanges();
    reconnectInputs(inputs);
}

void
RFVSwitch::reconnectInputs(const std::vector<NodePtr>& inputs)
{
    NodePtr node = getNode();
    if (!node) {
        return;
    }
    for (int i = 0; i < (int)inputs.size() && i < node->getNInputs(); ++i) {
        const NodePtr current = node->getRealInput(i);
        if (current == inputs[i]) {
            continue;
        }
        if (!inputs[i]) {
            node->disconnectInput(i);
        } else if (current) {
            node->replaceInput(inputs[i], i);
        } else {
            node->connectInput(inputs[i], i);
        }
    }
    refreshInputsVisibility();
}

void
RFVSwitch::refreshInputsVisibility()
{
    NodePtr node = getNode();
    if (!node) {
        return;
    }
    const int rows = patternCount();
    const int inputs = node->getNInputs();
    for (int i = 0; i < inputs; ++i) {
        const bool visible = (i == 0) || (i < rows) || bool(node->getInput(i));
        if (node->isInputVisible(i) != visible) {
            node->setInputVisible(i, visible);
        }
    }
}

// A Choice variable is its option's label, the text its menu shows: rfv gives only the index.
bool
RFVSwitch::choiceLabel(const std::string& name,
                       PyObject* object,
                       std::string* label) const
{
    if (!PyLong_Check(object) || PyBool_Check(object)) {
        return false;
    }
    AppInstancePtr app = getApp();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();
    KnobChoicePtr choice = project ? std::dynamic_pointer_cast<KnobChoice>(RFV::findDeclaredVariable(project.get(), name)) : KnobChoicePtr();
    if (!choice) {
        return false;
    }
    const long index = PyLong_AsLong(object);
    if (PyErr_Occurred()) {
        PyErr_Clear();

        return false;
    }
    const std::vector<ChoiceOption> entries = choice->getEntries_mt_safe();
    if ((index < 0) || (index >= (long)entries.size())) {
        return false;
    }
    const ChoiceOption& entry = entries[index];
    *label = entry.label.empty() ? entry.id : entry.label;

    return true;
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
    // queried under, an RFVSet's binding evaluated at time, and the project's value otherwise.
    PyRFV::ExpressionScope scope(variable.get(), time, view);
    PyObject* object = PyRFV::getVariable(name);
    if (!object) {
        *error = takePythonError();

        return false;
    }
    bool found = false;
    if (object != Py_None) {
        if (choiceLabel(name, object, value)) {
            found = true;
        } else if (canonicalText(object, value)) {
            found = true;
        } else {
            *error = takePythonError();
        }
    }
    Py_DECREF(object);

    return found;
}

RFVSwitch::Outcome
RFVSwitch::evaluate(double time,
                    ViewIdx view) const
{
    Outcome outcome;
    KnobStringPtr variable = _variable.lock();
    KnobChoicePtr ifNoMatch = _ifNoMatch.lock();
    if (!variable || !ifNoMatch) {
        return outcome;
    }
    const std::string name = variable->getValue();
    const std::vector<std::string> lines = patterns();

    std::string value;
    std::string readError;
    std::string patternError;
    const bool isSet = readVariable(name, time, view, &value, &readError);
    // Matched even when unset, for the errors of the patterns.
    const int matched = RFV::firstMatch(value, lines, &patternError);
    const int chosen = isSet ? matched : -1;
    const QString patternMessage = patternError.empty() ? QString() : tr("Invalid pattern at %1").arg(QString::fromUtf8(patternError.c_str()));

    int input = chosen;
    if ((input < 0) && (ifNoMatch->getValue() == eRFVSwitchIfNoMatchFirstInput)) {
        input = 0;
    }
    if (input >= 0) {
        outcome.input = input;
        outcome.warning = patternMessage.toStdString();

        return outcome;
    }

    const QString variableName = QString::fromUtf8(name.c_str());
    QString message;
    if (name.empty()) {
        message = tr("No variable is set to switch on.");
    } else if (!readError.empty()) {
        message = tr("The variable %1 could not be read: %2").arg(variableName, QString::fromUtf8(readError.c_str()));
    } else if (!isSet) {
        message = tr("The variable %1 is unset on this path.").arg(variableName);
    } else {
        message = tr("The variable %1 = \"%2\" matches no pattern.").arg(variableName, QString::fromUtf8(value.c_str()));
    }
    if (!patternMessage.isEmpty()) {
        message += QLatin1Char(' ') + patternMessage;
    }
    outcome.error = message.toStdString();

    return outcome;
} // RFVSwitch::evaluate

void
RFVSwitch::reportOutcome(const Outcome& outcome)
{
    // Only a render shows what the node decided: a query outside one, such as a hash or a
    // metadata read, may run under a context no render reaches the node with.
    const ParallelRenderArgsPtr frameArgs = getParallelRenderArgsTLS();
    const AbortableRenderInfoPtr render = frameArgs ? frameArgs->abortInfo.lock() : AbortableRenderInfoPtr();
    NodePtr node = getNode();
    if (!render || !node) {
        return;
    }
    // A preview renders the node under its own context, which is not what the viewer shows.
    if (frameArgs->treeRoot && frameArgs->treeRoot->isRenderingPreview()) {
        return;
    }
    const U64 sequence = render->getRenderSequence();

    QMutexLocker locker(&_messageMutex);
    if (sequence < _messageSequence) {
        return;
    }
    if (sequence > _messageSequence) {
        _messageSequence = sequence;
        _messageErrors.clear();
        _messageWarnings.clear();
    }
    if (!outcome.error.empty()) {
        _messageErrors.insert(outcome.error);
    }
    if (!outcome.warning.empty()) {
        _messageWarnings.insert(outcome.warning);
    }
    const bool isError = !_messageErrors.empty();
    const std::string wanted = isError ? joined(_messageErrors) : joined(_messageWarnings);

    QString shown;
    int shownType = 0;
    node->getPersistentMessage(&shown, &shownType, false);
    const std::string shownText = shown.toStdString();
    if (wanted.empty()) {
        if (!_postedMessage.empty() && (shownText == _postedMessage)) {
            node->clearPersistentMessage(false);
        }
        _postedMessage.clear();

        return;
    }
    if (shownText == wanted) {
        _postedMessage = wanted;

        return;
    }
    // Another part of the node owns the message shown.
    if (!shownText.empty() && (shownText != _postedMessage)) {
        return;
    }
    const bool wasError = !_postedMessage.empty() && (shownType == (int)eMessageTypeError);
    node->setPersistentMessage(isError ? eMessageTypeError : eMessageTypeWarning, wanted);
    _postedMessage = wanted;
    if (isError && !wasError) {
        clearActionsCache();
    }
} // RFVSwitch::reportOutcome

void
RFVSwitch::clearPostedMessage()
{
    NodePtr node = getNode();
    if (!node) {
        return;
    }
    QMutexLocker locker(&_messageMutex);
    if (_postedMessage.empty()) {
        return;
    }
    QString shown;
    int shownType = 0;
    node->getPersistentMessage(&shown, &shownType, false);
    if (shown.toStdString() == _postedMessage) {
        node->clearPersistentMessage(false);
    }
    _postedMessage.clear();
    _messageErrors.clear();
    _messageWarnings.clear();
}

Route
RFVSwitch::route(double time,
                 ViewIdx view)
{
    const Outcome outcome = evaluate(time, view);
    reportOutcome(outcome);
    if (outcome.input < 0) {
        return Route::none();
    }
    if (!getInput(outcome.input)) {
        return Route::black();
    }

    return Route::input(outcome.input, time);
}

StatusEnum
RFVSwitch::render(const RenderActionArgs& args)
{
    // Reached only when route() found no input to pass through. The failure is reported again,
    // since route()'s answer may come from the cache.
    reportOutcome(evaluate(args.time, args.view));

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
