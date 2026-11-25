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

#include "USDExpandHierarchy.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include <QtCore/QCoreApplication>
#include <QtCore/QMutexLocker>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QThread>
#include <QtCore/QTimer>

#include "Engine/ChoiceOption.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/PrimSelector.h"
#include "Engine/Nodes/Scene/SceneDrawStates.h"
#include "Engine/PythonUndoCommands.h"
#include "Serialization/NodeSerialization.h"

NATRON_NAMESPACE_ENTER

USDExpandHierarchy::USDExpandHierarchy(NodePtr node)
    : NativeEffectBase(node)
    , _page()
    , _ruleCount()
    , _rowsMutex()
    , _rows()
    , _panelRefreshPending(false)
{
}

USDExpandHierarchy::~USDExpandHierarchy()
{
}

const NodeSpec*
USDExpandHierarchy::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

// Every knob stays out of the node's hash: the rules never key a layer, and the node's output is
// its input's stack whatever they say.
void
USDExpandHierarchy::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    KnobIntPtr count = createKnob<KnobInt>(tr("Rule Count"));
    count->setName(kUSDExpandHierarchyParamRuleCount);
    count->setHintToolTip(tr("How many rules the node shows, at most %1. A later rule wins over an earlier one on the "
                             "prims both name. Rules past the count are kept but set nothing.")
                              .arg(kUSDExpandHierarchyMaxRules));
    count->setAnimationEnabled(false);
    count->setMinimum(1);
    count->setMaximum(kUSDExpandHierarchyMaxRules);
    count->setDisplayMinimum(1);
    count->setDisplayMaximum(16);
    count->setDefaultValue(1);
    count->setHashExempt(true);
    page->addKnob(count);
    _ruleCount = count;

    // The rows come last on the page, so the chunks created later go after them.
    _page = page;
    for (int i = 0; i < kUSDExpandHierarchyRuleChunk; ++i) {
        createRow(i);
    }
} // USDExpandHierarchy::initializeKnobs

void
USDExpandHierarchy::createRow(int index)
{
    KnobPagePtr page = _page.lock();
    if (!page) {
        return;
    }
    const std::string number = std::to_string(index);
    const QString suffix = QLatin1Char(' ') + QString::number(index);

    PrimSelector selector;
    selector.create(this,
                    page,
                    kUSDExpandHierarchyParamPattern + number,
                    tr("Pattern") + suffix,
                    tr("The prims this rule sets the draw of. A viewer matches it against the prims it has loaded, "
                       "and again after each load; a pattern matching nothing does nothing, as what it names may "
                       "be inside a payload not loaded yet. A blank pattern sets no rule."));
    KnobStringPtr pattern = selector.getKnob();
    pattern->setHashExempt(true);
    pattern->setSecretByDefault(index > 0);

    KnobChoicePtr draw = createKnob<KnobChoice>(tr("Draw") + suffix);
    draw->setName(kUSDExpandHierarchyParamDraw + number);
    draw->setHintToolTip(tr("How viewers draw the prims the pattern matches, unless a viewer sets their draw itself."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(sceneDrawStateName(eSceneDrawStateBBox), tr("BBox").toStdString(),
                                       tr("One box around everything below the prim. Payloads below stay unloaded.").toStdString()));
        options.push_back(ChoiceOption(sceneDrawStateName(eSceneDrawStateChildren), tr("Children").toStdString(),
                                       tr("Each child by its own draw; children nothing sets draw as boxes.").toStdString()));
        options.push_back(ChoiceOption(sceneDrawStateName(eSceneDrawStateFull), tr("Full").toStdString(),
                                       tr("Everything below the prim, as far as its payloads are loaded. Nothing is "
                                          "loaded.")
                                           .toStdString()));
        options.push_back(ChoiceOption(sceneDrawStateName(eSceneDrawStateLoadAll), tr("Full + Load").toStdString(),
                                       tr("Everything below the prim, and viewers load every payload below it.").toStdString()));
        options.push_back(ChoiceOption(sceneDrawStateName(eSceneDrawStateHidden), tr("Hidden").toStdString(),
                                       tr("Nothing, not even a box. Payloads below stay unloaded.").toStdString()));
        draw->populateChoices(options);
    }
    draw->setDefaultValue((int)eSceneDrawStateFull);
    draw->setAnimationEnabled(false);
    draw->setHashExempt(true);
    draw->setAddNewLine(false);
    draw->setSecretByDefault(index > 0);
    page->addKnob(draw);

    KnobIntPtr levels = createKnob<KnobInt>(tr("Levels") + suffix);
    levels->setName(kUSDExpandHierarchyParamLevels + number);
    levels->setHintToolTip(tr("How many levels below each match open before Draw applies: 0 sets Draw on the matches, "
                              "1 on their children, and so on, the levels above drawing by their children. An unloaded "
                              "payload on the way takes Draw where it is."));
    levels->setDefaultValue(0);
    levels->setMinimum(0);
    levels->setDisplayMinimum(0);
    levels->setDisplayMaximum(10);
    levels->setAnimationEnabled(false);
    levels->setHashExempt(true);
    levels->setSecretByDefault(index > 0);
    page->addKnob(levels);

    QMutexLocker locker(&_rowsMutex);
    _rows.push_back(Row { pattern, draw, levels });
} // USDExpandHierarchy::createRow

int
USDExpandHierarchy::createdRows() const
{
    QMutexLocker locker(&_rowsMutex);

    return (int)_rows.size();
}

USDExpandHierarchy::Row
USDExpandHierarchy::rowAt(int index) const
{
    QMutexLocker locker(&_rowsMutex);

    return ((index >= 0) && (index < (int)_rows.size())) ? _rows[index] : Row();
}

int
USDExpandHierarchy::ruleCount() const
{
    KnobIntPtr count = _ruleCount.lock();
    const int value = count ? count->getValue() : 1;

    return std::max(1, std::min(value, createdRows()));
}

int
USDExpandHierarchy::rowOfKnob(const std::string& name,
                              const char* prefix)
{
    const std::size_t prefixSize = std::char_traits<char>::length(prefix);
    if ((name.size() <= prefixSize) || (name.compare(0, prefixSize, prefix) != 0)) {
        return -1;
    }
    int row = 0;
    for (std::size_t i = prefixSize; i < name.size(); ++i) {
        const char c = name[i];
        if ((c < '0') || (c > '9') || (row >= kUSDExpandHierarchyMaxRules)) {
            return -1;
        }
        row = row * 10 + (c - '0');
    }

    return (row < kUSDExpandHierarchyMaxRules) ? row : -1;
}

void
USDExpandHierarchy::ensureRows(int rows,
                               bool loading)
{
    // Knobs are only created on the main thread.
    if (!qApp || (QThread::currentThread() != qApp->thread())) {
        return;
    }
    const int wanted = std::min(rows, (int)kUSDExpandHierarchyMaxRules);
    const int created = createdRows();
    if (wanted <= created) {
        return;
    }
    const int target = std::min((int)kUSDExpandHierarchyMaxRules,
                                (wanted + kUSDExpandHierarchyRuleChunk - 1) / kUSDExpandHierarchyRuleChunk * kUSDExpandHierarchyRuleChunk);
    for (int i = created; i < target; ++i) {
        createRow(i);
    }
    refreshRows();
    if (loading) {
        return;
    }
    NodePtr node = getNode();
    if (node && node->isNodeCreated()) {
        try {
            node->declarePythonFields();
        } catch (const std::logic_error&) {
            // The node is not in Python yet: creating it declares every knob.
        }
    }
    // Deferred, because the edit that asked for the rows may come from a widget the rebuild destroys.
    if (!_panelRefreshPending.exchange(true)) {
        QTimer::singleShot(0, this, [this]() {
            _panelRefreshPending = false;
            recreateKnobs(true);
        });
    }
} // USDExpandHierarchy::ensureRows

void
USDExpandHierarchy::refreshRows()
{
    std::vector<Row> rows;
    {
        QMutexLocker locker(&_rowsMutex);
        rows = _rows;
    }
    const int count = ruleCount();
    for (int i = 0; i < (int)rows.size(); ++i) {
        const bool shown = i < count;
        const KnobIPtr knobs[] = { rows[i].pattern.lock(), rows[i].draw.lock(), rows[i].levels.lock() };
        for (const KnobIPtr& knob : knobs) {
            if (knob && (knob->getIsSecret() == shown)) {
                knob->setSecret(!shown);
            }
        }
    }
}

void
USDExpandHierarchy::onKnobsAboutToBeLoaded(const Serialization::NodeSerializationPtr& serialization)
{
    if (!serialization) {
        return;
    }
    int rows = 1;
    for (const Serialization::KnobSerialization& knob : serialization->knobs) {
        if (knob.name == kUSDExpandHierarchyParamRuleCount) {
            if (!knob.dimensions.empty() && knob.dimensions[0].value) {
                const Serialization::KnobValueSerialization& value = *knob.dimensions[0].value;
                if (const int* count = std::get_if<int>(&value)) {
                    rows = std::max(rows, *count);
                } else if (const double* countDouble = std::get_if<double>(&value)) {
                    rows = std::max(rows, (int)std::min(*countDouble, (double)kUSDExpandHierarchyMaxRules));
                }
            }
            continue;
        }
        for (const char* prefix : { kUSDExpandHierarchyParamPattern, kUSDExpandHierarchyParamDraw, kUSDExpandHierarchyParamLevels }) {
            rows = std::max(rows, rowOfKnob(knob.name, prefix) + 1);
        }
    }
    ensureRows(rows, true);
}

void
USDExpandHierarchy::onKnobsLoaded()
{
    refreshRows();
}

void
USDExpandHierarchy::onEffectCreated(bool /*mayCreateFileDialog*/,
                                    const CreateNodeArgs& /*args*/)
{
    refreshRows();
}

bool
USDExpandHierarchy::knobChanged(KnobI* k,
                                ValueChangedReasonEnum /*reason*/,
                                ViewSpec /*view*/,
                                double /*time*/,
                                bool /*originatedFromMainThread*/)
{
    KnobIntPtr count = _ruleCount.lock();

    if (count && (k == count.get())) {
        ensureRows(count->getValue(), false);
        refreshRows();

        return true;
    }

    return false;
}

std::vector<USDExpandHierarchy::Rule>
USDExpandHierarchy::getRules() const
{
    const int count = ruleCount();
    std::vector<Rule> rules;

    rules.reserve(count);
    for (int i = 0; i < count; ++i) {
        const Row row = rowAt(i);
        KnobStringPtr pattern = row.pattern.lock();
        KnobChoicePtr draw = row.draw.lock();
        KnobIntPtr levels = row.levels.lock();
        Rule rule;
        rule.pattern = pattern ? pattern->getValue() : std::string();
        if (!draw || !sceneDrawStateFromName(draw->getActiveEntry().id, &rule.state)) {
            rule.state = eSceneDrawStateFull;
        }
        rule.levels = levels ? std::max(0, levels->getValue()) : 0;
        rules.push_back(rule);
    }

    return rules;
}

int
USDExpandHierarchy::firstFreeRow() const
{
    const std::vector<Rule> current = getRules();
    int at = (int)current.size();

    while ((at > 0) && PrimSelector::isBlank(current[at - 1].pattern)) {
        --at;
    }

    return at;
}

int
USDExpandHierarchy::getFreeRuleCount() const
{
    return (int)kUSDExpandHierarchyMaxRules - firstFreeRow();
}

std::vector<USDExpandHierarchy::Rule>
USDExpandHierarchy::getRulesForStates(const std::vector<std::pair<std::string, SceneDrawStateEnum>>& states)
{
    std::vector<Rule> rules;
    // In path order a path's descendants follow it, so, walking back, a path that is an ancestor
    // of any path of the open rule is an ancestor of the last one added.
    std::string last;
    for (std::vector<std::pair<std::string, SceneDrawStateEnum>>::const_reverse_iterator it = states.rbegin(); it != states.rend(); ++it) {
        const std::string& path = it->first;
        const bool ancestor = !last.empty() && ((path == "/") || ((last.size() > path.size()) && (last.compare(0, path.size(), path) == 0) && (last[path.size()] == '/')));
        if (rules.empty() || (rules.back().state != it->second) || ancestor) {
            Rule rule;
            rule.pattern = path;
            rule.state = it->second;
            rules.push_back(rule);
        } else {
            rules.back().pattern += ' ';
            rules.back().pattern += path;
        }
        last = path;
    }

    return rules;
}

void
USDExpandHierarchy::appendRules(const std::vector<Rule>& rules)
{
    KnobIntPtr count = _ruleCount.lock();
    if (!count || rules.empty()) {
        return;
    }
    const int at = firstFreeRow();
    const int needed = std::min((int)kUSDExpandHierarchyMaxRules, at + (int)rules.size());
    ensureRows(needed, false);
    const int rows = std::min(needed, createdRows());

    recordKnobEdit(count);
    for (int i = at; i < rows; ++i) {
        const Row row = rowAt(i);
        recordKnobEdit(row.pattern.lock());
        recordKnobEdit(row.draw.lock());
        recordKnobEdit(row.levels.lock());
    }
    beginChanges();
    for (int i = at; i < rows; ++i) {
        const Rule& rule = rules[i - at];
        const Row row = rowAt(i);
        KnobStringPtr pattern = row.pattern.lock();
        KnobChoicePtr draw = row.draw.lock();
        KnobIntPtr levels = row.levels.lock();
        if (pattern) {
            pattern->setValue(rule.pattern);
        }
        if (draw) {
            draw->setValueFromID(sceneDrawStateName(rule.state), 0);
        }
        if (levels) {
            levels->setValue(std::max(0, rule.levels));
        }
    }
    if (count->getValue() < rows) {
        count->setValue(rows);
    }
    endChanges();
    refreshRows();
} // USDExpandHierarchy::appendRules

SceneDrawRules
USDExpandHierarchy::getDrawRules() const
{
    const std::vector<Rule> rules = getRules();
    SceneDrawRules drawRules;
    drawRules.reserve(rules.size());
    const std::string origin = getScriptName_mt_safe();

    // Every row in order, a repeat too: of two rows deciding a prim the later one wins.
    for (const Rule& rule : rules) {
        if (PrimSelector::isBlank(rule.pattern)) {
            continue;
        }
        SceneDrawRule drawRule;
        drawRule.pattern = rule.pattern;
        drawRule.state = rule.state;
        drawRule.levels = rule.levels;
        drawRule.origin = origin;
        drawRules.push_back(drawRule);
    }

    return drawRules;
}

StatusEnum
USDExpandHierarchy::renderScene(const SceneRenderActionArgs& args,
                                ScenePayload* outputScene,
                                std::string* /*errorMessage*/)
{
    const SceneDrawRules rules = getDrawRules();

    // A pattern that does not parse sets no rule, and the others still apply.
    QStringList bad;
    for (const SceneDrawRule& rule : rules) {
        std::string error;
        if (!PrimSelector::validate(rule.pattern, &error)) {
            bad.push_back(QString::fromStdString(rule.pattern) + QString::fromUtf8(": ") + QString::fromStdString(error));
        }
    }
    if (bad.isEmpty()) {
        clearPersistentMessage(false);
    } else {
        setPersistentMessage(eMessageTypeWarning, tr("These patterns set no rule: %1").arg(bad.join(QString::fromUtf8("; "))).toStdString());
    }

    *outputScene = args.getInputScene(0).withDrawRules(rules);

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
