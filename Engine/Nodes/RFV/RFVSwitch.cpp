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
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include <QtCore/QCoreApplication>
#include <QtCore/QMutexLocker>
#include <QtCore/QString>
#include <QtCore/QThread>
#include <QtCore/QTimer>

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
#include "Engine/UndoCommand.h"
#include "Engine/UndoRecorder.h"
#include "Serialization/NodeSerialization.h"

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

/**
 * @brief Undoes an Add After, a Remove or a Match Variable press: the patterns, the pattern count
 * and the inputs, as they were before and after it.
 **/
class RFVSwitchRowsCommand
    : public UndoCommand {
public:
    RFVSwitchRowsCommand(const NodePtr& node,
                         const RFVSwitch::RowsState& before,
                         const RFVSwitch::RowsState& after,
                         const std::string& text)
        : UndoCommand(text)
        , _node(node)
        , _before(before)
        , _after(after)
    {
    }

    virtual void redo() OVERRIDE FINAL
    {
        apply(_after);
    }

    virtual void undo() OVERRIDE FINAL
    {
        apply(_before);
    }

private:
    void apply(const RFVSwitch::RowsState& state)
    {
        NodePtr node = _node.lock();
        RFVSwitch* rfvSwitch = node ? dynamic_cast<RFVSwitch*>(node->getEffectInstance().get()) : NULL;
        if (rfvSwitch) {
            rfvSwitch->applyRowsState(state);
        }
    }

    NodeWPtr _node;
    RFVSwitch::RowsState _before;
    RFVSwitch::RowsState _after;
};

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

    KnobButtonPtr matchVariable = createKnob<KnobButton>(tr(kRFVSwitchParamMatchVariableLabel));
    matchVariable->setName(kRFVSwitchParamMatchVariable);
    matchVariable->setHintToolTip(tr(kRFVSwitchParamMatchVariableHint));
    matchVariable->setEvaluateOnChange(false);
    page->addKnob(matchVariable);
    _matchVariable = matchVariable;

    KnobIntPtr count = createKnob<KnobInt>(tr(kRFVSwitchParamPatternCountLabel));
    count->setName(kRFVSwitchParamPatternCount);
    count->setHintToolTip(tr(kRFVSwitchParamPatternCountHint));
    count->setAnimationEnabled(false);
    count->setMinimum(1);
    count->setMaximum(kRFVSwitchMaxRows);
    count->setDefaultValue(1);
    count->setSecretByDefault(true);
    page->addKnob(count);
    _patternCount = count;

    _page = page;
    for (int i = 0; i < kRFVSwitchRowChunk; ++i) {
        createRow(i, page, -1);
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

void
RFVSwitch::createRow(int index,
                     const KnobPagePtr& page,
                     int pageIndex)
{
    const std::string number = std::to_string(index);
    const QString suffix = QLatin1Char(' ') + QString::number(index);
    const auto place = [&page, &pageIndex](const KnobIPtr& knob) {
        if (pageIndex < 0) {
            page->addKnob(knob);
        } else {
            page->insertKnob(pageIndex++, knob);
        }
    };

    KnobStringPtr pattern = createKnob<KnobString>(tr(kRFVSwitchParamPatternLabel) + suffix);
    pattern->setName(kRFVSwitchParamPattern + number);
    pattern->setHintToolTip(tr(kRFVSwitchParamPatternHint));
    pattern->setAnimationEnabled(false);
    pattern->setAddNewLine(false);
    pattern->setSecretByDefault(index > 0);
    place(pattern);

    KnobButtonPtr addAfter = createKnob<KnobButton>(tr(kRFVSwitchParamAddAfterLabel) + suffix);
    addAfter->setName(kRFVSwitchParamAddAfter + number);
    addAfter->setHintToolTip(tr(kRFVSwitchParamAddAfterHint));
    addAfter->setIconLabel(NATRON_ICONS_PATH "lucide/plus.svg");
    addAfter->setEvaluateOnChange(false);
    addAfter->setAddNewLine(false);
    addAfter->setSecretByDefault(index > 0);
    place(addAfter);

    KnobButtonPtr remove = createKnob<KnobButton>(tr(kRFVSwitchParamRemoveLabel) + suffix);
    remove->setName(kRFVSwitchParamRemove + number);
    remove->setHintToolTip(tr(kRFVSwitchParamRemoveHint));
    remove->setIconLabel(NATRON_ICONS_PATH "lucide/minus.svg");
    remove->setEvaluateOnChange(false);
    remove->setSecretByDefault(index > 0);
    place(remove);

    QMutexLocker locker(&_rowsMutex);
    _patterns.push_back(pattern);
    _addAfter.push_back(addAfter);
    _remove.push_back(remove);
}

int
RFVSwitch::createdRows() const
{
    QMutexLocker locker(&_rowsMutex);

    return (int)_patterns.size();
}

KnobStringPtr
RFVSwitch::patternKnob(int row) const
{
    QMutexLocker locker(&_rowsMutex);

    return ((row >= 0) && (row < (int)_patterns.size())) ? _patterns[row].lock() : KnobStringPtr();
}

int
RFVSwitch::rowOfKnob(const std::string& name,
                     const char* prefix)
{
    const std::size_t prefixSize = std::char_traits<char>::length(prefix);
    if ((name.size() <= prefixSize) || (name.compare(0, prefixSize, prefix) != 0)) {
        return -1;
    }
    int row = 0;
    for (std::size_t i = prefixSize; i < name.size(); ++i) {
        const char c = name[i];
        if ((c < '0') || (c > '9') || (row >= kRFVSwitchMaxRows)) {
            return -1;
        }
        row = row * 10 + (c - '0');
    }

    return (row < kRFVSwitchMaxRows) ? row : -1;
}

int
RFVSwitch::getNInputs() const
{
    return _inputCount.load();
}

void
RFVSwitch::ensureRows(int rows,
                      bool loading)
{
    // Knobs and inputs are only created on the main thread.
    if (QThread::currentThread() != qApp->thread()) {
        return;
    }
    const int wanted = std::min(rows, (int)kRFVSwitchMaxRows);
    const int created = createdRows();
    KnobPagePtr page = _page.lock();
    if ((wanted <= created) || !page) {
        return;
    }
    const int target = std::min((int)kRFVSwitchMaxRows, (wanted + kRFVSwitchRowChunk - 1) / kRFVSwitchRowChunk * kRFVSwitchRowChunk);

    // If No Match always follows the last row.
    const KnobIPtr ifNoMatch = _ifNoMatch.lock();
    const std::vector<KnobIPtr> children = page->getChildren();
    int at = (int)children.size();
    for (int i = 0; i < (int)children.size(); ++i) {
        if (children[i] == ifNoMatch) {
            at = i;
            break;
        }
    }
    for (int i = created; i < target; ++i) {
        createRow(i, page, at);
        at += 3;
    }

    _inputCount.store(target);
    NodePtr node = getNode();
    if (node) {
        node->initializeInputs();
    }
    refreshRows();
    refreshInputsVisibility();
    if (loading) {
        return;
    }
    if (node && node->isNodeCreated()) {
        try {
            node->declarePythonFields();
        } catch (const std::logic_error&) {
            // The node is not in Python yet: creating it declares every knob.
        }
    }
    // Deferred, because the press of an Add After button lands here from inside its own widget,
    // which the rebuild destroys.
    if (!_panelRefreshPending) {
        _panelRefreshPending = true;
        QTimer::singleShot(0, this, [this]() {
            _panelRefreshPending = false;
            recreateKnobs(true);
        });
    }
} // RFVSwitch::ensureRows

void
RFVSwitch::onKnobsAboutToBeLoaded(const Serialization::NodeSerializationPtr& serialization)
{
    if (!serialization) {
        return;
    }
    int rows = 1;
    for (const Serialization::KnobSerialization& knob : serialization->knobs) {
        if (knob.name == kRFVSwitchParamPatternCount) {
            if (!knob.dimensions.empty() && knob.dimensions[0].value) {
                const Serialization::KnobValueSerialization& value = *knob.dimensions[0].value;
                if (const int* count = std::get_if<int>(&value)) {
                    rows = std::max(rows, *count);
                } else if (const double* countDouble = std::get_if<double>(&value)) {
                    rows = std::max(rows, (int)std::min(*countDouble, (double)kRFVSwitchMaxRows));
                }
            }
            continue;
        }
        for (const char* prefix : { kRFVSwitchParamPattern, kRFVSwitchParamAddAfter, kRFVSwitchParamRemove }) {
            rows = std::max(rows, rowOfKnob(knob.name, prefix) + 1);
        }
    }
    // Variadic inputs are labelled by their number alone.
    for (const auto& input : serialization->inputs) {
        rows = std::max(rows, rowOfKnob(input.first, "") + 1);
    }
    ensureRows(rows, true);
}

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
    // Recorded here because these edits come from no undo command of their own.
    const auto editRows = [this, k](const std::function<void()>& edit) {
        AppInstancePtr app = getApp();
        const UndoRecorderPtr recorder = app ? app->getRecordingUndoRecorder() : UndoRecorderPtr();
        RowsState before;
        if (recorder) {
            before = rowsState();
        }
        edit();
        if (recorder) {
            recordRowsEdit(recorder, before, k->getLabel());
        }
    };
    const std::string& name = k->getName();
    const int addAt = rowOfKnob(name, kRFVSwitchParamAddAfter);
    const int removeAt = rowOfKnob(name, kRFVSwitchParamRemove);
    KnobButtonPtr addAfter;
    KnobButtonPtr remove;
    {
        QMutexLocker locker(&_rowsMutex);
        if ((addAt >= 0) && (addAt < (int)_addAfter.size())) {
            addAfter = _addAfter[addAt].lock();
        }
        if ((removeAt >= 0) && (removeAt < (int)_remove.size())) {
            remove = _remove[removeAt].lock();
        }
    }
    if (addAfter && (k == addAfter.get())) {
        editRows([this, addAt]() { insertRowAfter(addAt); });

        return true;
    }
    if (remove && (k == remove.get())) {
        editRows([this, removeAt]() { removeRow(removeAt); });

        return true;
    }
    if (k == _matchVariable.lock().get()) {
        editRows([this]() { matchVariableOptions(); });

        return true;
    }
    if (k == _patternCount.lock().get()) {
        ensureRows(_patternCount.lock()->getValue(), false);
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

    return std::max(1, std::min(value, createdRows()));
}

std::vector<std::string>
RFVSwitch::patterns() const
{
    const int count = std::min(patternCount(), getNInputs());
    std::vector<KnobStringPtr> knobs;
    {
        QMutexLocker locker(&_rowsMutex);
        for (int i = 0; i < count && i < (int)_patterns.size(); ++i) {
            knobs.push_back(_patterns[i].lock());
        }
    }
    std::vector<std::string> values;
    values.reserve(knobs.size());
    for (const KnobStringPtr& pattern : knobs) {
        values.push_back(pattern ? pattern->getValue() : std::string());
    }

    return values;
}

std::shared_ptr<const RFV::CompiledPatterns>
RFVSwitch::compiledPatterns() const
{
    const std::vector<std::string> lines = patterns();
    QMutexLocker locker(&_compiledMutex);
    if (!_compiled || (_compiled->getLines() != lines)) {
        _compiled = std::make_shared<const RFV::CompiledPatterns>(lines);
    }

    return _compiled;
}

void
RFVSwitch::refreshRows()
{
    std::vector<KnobStringWPtr> patterns;
    std::vector<KnobButtonWPtr> addAfters;
    std::vector<KnobButtonWPtr> removes;
    {
        QMutexLocker locker(&_rowsMutex);
        patterns = _patterns;
        addAfters = _addAfter;
        removes = _remove;
    }
    const int count = patternCount();
    for (int i = 0; i < (int)patterns.size(); ++i) {
        const bool shown = i < count;
        KnobStringPtr pattern = patterns[i].lock();
        KnobButtonPtr addAfter = addAfters[i].lock();
        KnobButtonPtr remove = removes[i].lock();
        if (pattern && (pattern->getIsSecret() == shown)) {
            pattern->setSecret(!shown);
        }
        const bool addShown = shown && (count < kRFVSwitchMaxRows);
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
    if (!countKnob || !node || (row < 0) || (row >= count) || (count >= kRFVSwitchMaxRows)) {
        return;
    }
    ensureRows(count + 1, false);
    // A connected last input would fall off the end.
    if (node->getRealInput(node->getNInputs() - 1)) {
        ensureRows(createdRows() + 1, false);
    }
    if (count >= createdRows()) {
        return;
    }
    std::vector<std::string> values = patterns();
    values.resize(count);
    values.insert(values.begin() + row + 1, std::string());
    std::vector<NodePtr> inputs;
    for (int i = 0; i < node->getNInputs(); ++i) {
        inputs.push_back(node->getRealInput(i));
    }
    inputs.insert(inputs.begin() + row + 1, NodePtr());
    inputs.pop_back();

    beginChanges();
    for (int i = row + 1; i < (int)values.size(); ++i) {
        patternKnob(i)->setValue(values[i]);
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
        patternKnob(0)->setValue(std::string());

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
        patternKnob(i)->setValue(values[i]);
    }
    countKnob->setValue(count - 1);
    endChanges();
    reconnectInputs(inputs);
}

RFVSwitch::RowsState
RFVSwitch::rowsState() const
{
    RowsState state;
    KnobIntPtr countKnob = _patternCount.lock();
    state.count = countKnob ? countKnob->getValue() : 1;
    std::vector<KnobStringWPtr> patterns;
    {
        QMutexLocker locker(&_rowsMutex);
        patterns = _patterns;
    }
    for (const KnobStringWPtr& weak : patterns) {
        KnobStringPtr pattern = weak.lock();
        state.patterns.push_back(pattern ? pattern->getValue() : std::string());
    }
    NodePtr node = getNode();
    for (int i = 0; node && (i < node->getNInputs()); ++i) {
        state.inputs.push_back(node->getRealInput(i));
    }

    return state;
}

void
RFVSwitch::applyRowsState(const RowsState& state)
{
    KnobIntPtr countKnob = _patternCount.lock();
    NodePtr node = getNode();
    if (!countKnob || !node) {
        return;
    }
    ensureRows(std::max({ state.count, (int)state.patterns.size(), (int)state.inputs.size() }), false);
    // Rows and inputs created after state was taken are emptied.
    const int rows = createdRows();
    beginChanges();
    for (int i = 0; i < rows; ++i) {
        const std::string value = (i < (int)state.patterns.size()) ? state.patterns[i] : std::string();
        KnobStringPtr pattern = patternKnob(i);
        if (pattern && (pattern->getValue() != value)) {
            pattern->setValue(value);
        }
    }
    if (countKnob->getValue() != state.count) {
        countKnob->setValue(state.count);
    }
    endChanges();
    std::vector<NodePtr> inputs;
    for (int i = 0; i < node->getNInputs(); ++i) {
        inputs.push_back((i < (int)state.inputs.size()) ? state.inputs[i].lock() : NodePtr());
    }
    reconnectInputs(inputs);
    refreshRows();
}

void
RFVSwitch::recordRowsEdit(const UndoRecorderPtr& recorder,
                          const RowsState& before,
                          const std::string& text)
{
    NodePtr node = getNode();
    if (!recorder || !node) {
        return;
    }
    const RowsState after = rowsState();
    const auto sameInputs = [](const std::vector<NodeWPtr>& a,
                               const std::vector<NodeWPtr>& b) {
        const std::size_t n = std::max(a.size(), b.size());
        for (std::size_t i = 0; i < n; ++i) {
            const NodePtr left = (i < a.size()) ? a[i].lock() : NodePtr();
            const NodePtr right = (i < b.size()) ? b[i].lock() : NodePtr();
            if (left != right) {
                return false;
            }
        }

        return true;
    };
    const auto samePatterns = [](const std::vector<std::string>& a,
                                 const std::vector<std::string>& b) {
        const std::size_t n = std::max(a.size(), b.size());
        for (std::size_t i = 0; i < n; ++i) {
            if (((i < a.size()) ? a[i] : std::string()) != ((i < b.size()) ? b[i] : std::string())) {
                return false;
            }
        }

        return true;
    };
    if ((before.count == after.count) && samePatterns(before.patterns, after.patterns) && sameInputs(before.inputs, after.inputs)) {
        return;
    }
    UndoTouched touched;
    touched.nodes.push_back(node);
    for (const RowsState* state : { &before, &after }) {
        for (const NodeWPtr& weak : state->inputs) {
            NodePtr input = weak.lock();
            if (input && (std::find(touched.nodes.begin(), touched.nodes.end(), input) == touched.nodes.end())) {
                touched.nodes.push_back(input);
            }
        }
    }
    recorder->record(std::make_shared<RFVSwitchRowsCommand>(node, before, after, text), touched);
} // RFVSwitch::recordRowsEdit

void
RFVSwitch::matchVariableOptions()
{
    KnobStringPtr variable = _variable.lock();
    KnobIntPtr countKnob = _patternCount.lock();
    if (!variable || !countKnob) {
        return;
    }
    const std::string name = variable->getValue();
    const QString variableName = QString::fromUtf8(name.c_str());
    if (name.empty()) {
        postMatchNote(tr("Match Variable: no variable is named in Variable.").toStdString());

        return;
    }
    // The lookup choiceLabel() matches with, so the rows written are the labels it compares.
    AppInstancePtr app = getApp();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();
    KnobIPtr declared = project ? RFV::findDeclaredVariable(project.get(), name) : KnobIPtr();
    if (!declared) {
        postMatchNote(tr("Match Variable: the project declares no variable %1.").arg(variableName).toStdString());

        return;
    }
    KnobChoicePtr choice = std::dynamic_pointer_cast<KnobChoice>(declared);
    if (!choice) {
        postMatchNote(tr("Match Variable: the project variable %1 is not a Choice.").arg(variableName).toStdString());

        return;
    }

    const std::vector<ChoiceOption> entries = choice->getEntries_mt_safe();
    ensureRows(std::max(1, std::min((int)entries.size(), (int)kRFVSwitchMaxRows)), false);
    const int created = createdRows();
    const int rows = std::min((int)entries.size(), created);
    beginChanges();
    for (int i = 0; i < created; ++i) {
        std::string pattern;
        if (i < rows) {
            const ChoiceOption& entry = entries[i];
            pattern = RFV::patternForLabel(entry.label.empty() ? entry.id : entry.label);
        }
        KnobStringPtr knob = patternKnob(i);
        if (knob && (knob->getValue() != pattern)) {
            knob->setValue(pattern);
        }
    }
    countKnob->setValue(std::max(rows, 1));
    endChanges();
    refreshRows();
    refreshInputsVisibility();

    const int leftOut = (int)entries.size() - rows;
    if (leftOut > 0) {
        postMatchNote(tr("Match Variable: %1 has %2 options, more than the %3 rows a switch can have; the last %4 were left out.")
                          .arg(variableName)
                          .arg((int)entries.size())
                          .arg((int)kRFVSwitchMaxRows)
                          .arg(leftOut)
                          .toStdString());
    }
} // RFVSwitch::matchVariableOptions

void
RFVSwitch::postMatchNote(const std::string& note)
{
    NodePtr node = getNode();
    if (!node) {
        return;
    }
    QMutexLocker locker(&_messageMutex);
    _matchNote = note;
    QString shown;
    int shownType = 0;
    node->getPersistentMessage(&shown, &shownType, false);
    if ((shown.toStdString() != note) || (shownType != (int)eMessageTypeWarning)) {
        node->setPersistentMessage(eMessageTypeWarning, note);
    }
    _postedMessage = note;
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
    std::vector<std::string> labels;
    PyObject* object = PyRFV::getVariable(name, &labels);
    if (!object) {
        *error = takePythonError();

        return false;
    }
    bool found = false;
    if (object != Py_None) {
        const bool isIndex = !labels.empty() && PyLong_Check(object) && !PyBool_Check(object);
        long long index = isIndex ? PyLong_AsLongLong(object) : -1;
        if (isIndex && PyErr_Occurred()) {
            PyErr_Clear();
            index = -1;
        }
        if (isIndex && RFV::choiceLabel(RenderContextValue((std::int64_t)index), labels, value)) {
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

    std::string value;
    std::string readError;
    const bool isSet = readVariable(name, time, view, &value, &readError);
    // Matched even when unset, for the errors of the patterns.
    const std::shared_ptr<const RFV::CompiledPatterns> compiled = compiledPatterns();
    const int matched = compiled->firstMatch(value);
    const std::string& patternError = compiled->getError();
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
    std::string wanted = isError ? joined(_messageErrors) : joined(_messageWarnings);
    if (wanted.empty()) {
        wanted = _matchNote;
    }

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
    _matchNote.clear();
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
    // A Set's Choice is matched by the label of its option, so the context key keeps the labels.
    reads.add(RenderContextName::choiceLabels(name));
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
            for (const std::string& label : RFV::getChoiceLabels(declaration.knob)) {
                h.appendString(label);
            }
        }
    }
}

NATRON_NAMESPACE_EXIT
