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

#include "RFVRead.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <QtCore/QMetaObject>
#include <QtCore/QMutex>

#include "Engine/AppInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/RFV.h"
#include "Engine/RenderContext.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

namespace {

// Every RFVRead alive, so an edit anywhere in the graph reaches them without walking the project.
QMutex g_readersMutex;
std::vector<RFVRead*> g_readers;

std::string
formatFrame(double time)
{
    if (time == std::floor(time)) {
        return std::to_string((long long)time);
    }

    return RFV::formatValue(RenderContextValue(time));
}

// Updating the list must not look like an edit: no knobChanged, Python callback or render.
class PassiveKnobUpdate {
public:
    PassiveKnobUpdate(KnobHolder* holder,
                      const KnobIPtr& knob)
        : _holder(holder)
        , _knob(knob)
    {
        _knob->blockValueChanges();
        _holder->beginChanges();
    }

    ~PassiveKnobUpdate()
    {
        try {
            _holder->endChanges(true);
        } catch (...) {
        }
        _knob->unblockValueChanges();
    }

    PassiveKnobUpdate(const PassiveKnobUpdate&) = delete;
    PassiveKnobUpdate& operator=(const PassiveKnobUpdate&) = delete;

private:
    KnobHolder* _holder;
    KnobIPtr _knob;
};

} // anonymous namespace

RFVRead::RFVRead(NodePtr node)
    : NativeEffectBase(node)
    , _variables()
    , _refreshPending(false)
{
    QMutexLocker locker(&g_readersMutex);
    g_readers.push_back(this);
}

RFVRead::~RFVRead()
{
    QMutexLocker locker(&g_readersMutex);
    g_readers.erase(std::remove(g_readers.begin(), g_readers.end(), this), g_readers.end());
}

void
RFVRead::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    KnobStringPtr variables = createKnob<KnobString>(tr(kRFVReadParamVariablesLabel));
    variables->setName(kRFVReadParamVariables);
    variables->setHintToolTip(tr(kRFVReadParamVariablesHint));
    variables->setAnimationEnabled(false);
    variables->setIsPersistent(false);
    variables->setHashExempt(true);
    variables->setEvaluateOnChange(false);
    variables->setAsMultiLine();
    variables->setPreferredRows(12);
    // A disabled multi-line field is read-only but its text can still be selected and copied.
    variables->setAllDimensionsEnabled(false);
    page->addKnob(variables);
    _variables = variables;
}

Route
RFVRead::route(double time,
               ViewIdx /*view*/)
{
    return Route::input(0, time);
}

void
RFVRead::onEffectCreated(bool /*mayCreateFileDialog*/,
                         const CreateNodeArgs& /*args*/)
{
    scheduleRefresh();
}

void
RFVRead::refreshExtraStateAfterTimeChanged(bool isPlayback,
                                           double time)
{
    NativeEffectBase::refreshExtraStateAfterTimeChanged(isPlayback, time);
    AppInstancePtr app = getApp();
    refreshVariablesBelow(RFV::getActiveViewer(app), time);
}

void
RFVRead::onSettingsPanelOpened()
{
    refreshVariables();
}

std::string
RFVRead::getVariablesText() const
{
    KnobStringPtr variables = _variables.lock();

    return variables ? variables->getValue() : std::string();
}

void
RFVRead::refreshVariables()
{
    AppInstancePtr app = getApp();
    if (!app) {
        return;
    }
    refreshVariablesBelow(RFV::getActiveViewer(app), app->getTimeLine()->currentFrame());
}

void
RFVRead::refreshVariablesBelow(const NodePtr& output,
                               double time)
{
    KnobStringPtr variables = _variables.lock();
    NodePtr node = getNode();
    AppInstancePtr app = getApp();
    if (!variables || !node || !app || !node->isActivated() || app->getProject()->isProjectClosing()) {
        return;
    }

    const RFV::ResolvedContext resolved = RFV::resolveContextBelow(output, node, time, ViewIdx(0));
    std::string text = "from ";
    text += resolved.outputName.empty() ? std::string("the project") : resolved.outputName;
    text += ", frame " + formatFrame(time);
    for (const RenderContext::Entry& entry : resolved.context->entries()) {
        text += "\n";
        text += entry.first.ns.empty() ? entry.first.name : (entry.first.ns + "." + entry.first.name);
        text += " = " + RFV::formatValue(entry.second);
    }
    for (const std::pair<std::string, std::string>& error : resolved.errors) {
        text += "\n" + error.first + ": " + error.second;
    }

    if (text != variables->getValue()) {
        PassiveKnobUpdate passive(this, variables);
        variables->setValue(text);
    }
}

void
RFVRead::scheduleRefresh()
{
    if (_refreshPending) {
        return;
    }
    _refreshPending = true;
    // The edits that call this are mid-way: a connection or a variable is still being set up.
    QMetaObject::invokeMethod(this, [this]() {
        _refreshPending = false;
        refreshVariables(); }, Qt::QueuedConnection);
}

void
RFVRead::scheduleRefreshAll(const AppInstance* app)
{
    QMutexLocker locker(&g_readersMutex);
    for (RFVRead* reader : g_readers) {
        if (reader->getApp().get() == app) {
            reader->scheduleRefresh();
        }
    }
}

NATRON_NAMESPACE_EXIT
