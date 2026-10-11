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

#include "ColorKnobEditSession.h"

#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Image/ColorMath.h"
#include "Engine/ProjectColorManagement.h"
#include "Engine/Settings.h"

#include "Gui/Gui.h"
#include "Gui/GuiApplicationManager.h"
#include "Gui/KnobGui.h"
#include "Gui/KnobUndoCommand.h"

NATRON_NAMESPACE_ENTER

namespace {

// One counter for the whole process: every colour gesture (a wheel or slider drag in the
// adjustment panel, a swatch drag, a global-mode stroke) gets its own key, so consecutive
// gestures on the same knob never merge into one undo step.
quint64
allocateMergeKey()
{
    static quint64 counter = 0;

    ++counter;
    if (counter == 0) {
        // Wrapped past the top of the range: 0 is reserved to mean "no key".
        ++counter;
    }

    return counter;
}

} // namespace

ColorKnobEditSession::ColorKnobEditSession(const KnobGuiPtr& knobGui)
    : _knobGui(knobGui)
    , _nDims(0)
    , _start()
    , _current()
    , _committed()
    , _active(false)
    , _moved(false)
    , _label()
    , _mergeKey(0)
{
}

ColorKnobEditSession::~ColorKnobEditSession()
{
    abandon();
}

KnobColorValue
ColorKnobEditSession::readKnobValue(const KnobColorPtr& knob)
{
    KnobColorValue v;

    v.rgb[0] = v.rgb[1] = v.rgb[2] = 0.0;
    v.a = 1.0;
    v.hasAlpha = false;
    if (!knob) {
        return v;
    }

    const int nDims = knob->getDimension();

    v.rgb[0] = knob->getValue(0);
    if (nDims >= 3) {
        v.rgb[1] = knob->getValue(1);
        v.rgb[2] = knob->getValue(2);
    } else {
        v.rgb[1] = v.rgb[2] = v.rgb[0];
    }
    if (nDims >= 4) {
        v.a = knob->getValue(3);
        v.hasAlpha = true;
    }

    return v;
}

LumaBasis
ColorKnobEditSession::basisFor(const ProjectColorManagementPtr& colorManagement)
{
    if (!colorManagement) {
        return LumaBasis(ColorMath::acesCgLumaWeights(), ColorMath::acesCgToXYZ());
    }

    double w[3];
    double m[9];

    colorManagement->getWorkingLuma(w);
    if (colorManagement->getWorkingToXYZ(m)) {
        return LumaBasis(w, m);
    }

    return LumaBasis(w);
}

bool
ColorKnobEditSession::isActive() const
{
    return _active;
}

const QString&
ColorKnobEditSession::getLabel() const
{
    return _label;
}

KnobColorPtr
ColorKnobEditSession::getKnob() const
{
    KnobGuiPtr knobGui = _knobGui.lock();

    return knobGui ? std::dynamic_pointer_cast<KnobColor>(knobGui->getKnob()) : KnobColorPtr();
}

std::list<double>
ColorKnobEditSession::toKnobValues(const KnobColorValue& value) const
{
    std::list<double> values;

    values.push_back(value.rgb[0]);
    if (_nDims >= 3) {
        values.push_back(value.rgb[1]);
        values.push_back(value.rgb[2]);
    }
    if (_nDims >= 4) {
        values.push_back(value.hasAlpha ? value.a : _start.back());
    }

    return values;
}

QString
ColorKnobEditSession::undoText() const
{
    KnobColorPtr knob = getKnob();
    QString axis = _label;

    if (!axis.isEmpty()) {
        axis[0] = axis[0].toLower();
    }
    if (!knob) {
        return axis;
    }

    return QString::fromUtf8(knob->getLabel().c_str()) + QString::fromUtf8(": ") + axis;
}

void
ColorKnobEditSession::push(const std::list<double>& values)
{
    KnobGuiPtr knobGui = _knobGui.lock();

    if (!knobGui) {
        return;
    }

    KnobUndoCommand<double>* cmd = new KnobUndoCommand<double>(knobGui, _start, values, true, _mergeKey);

    cmd->setText(undoText());
    knobGui->pushUndoCommand(cmd);
    _committed = values;
}

void
ColorKnobEditSession::begin(const QString& label)
{
    abandon();

    KnobColorPtr knob = getKnob();

    if (!knob) {
        return;
    }
    _nDims = knob->getDimension();
    _start.clear();
    for (int i = 0; i < _nDims; ++i) {
        _start.push_back(knob->getValue(i));
    }
    _current = _start;
    _committed = _start;
    _active = true;
    _moved = false;
    _label = label;
    _mergeKey = allocateMergeKey();

    KnobGuiPtr knobGui = _knobGui.lock();
    Gui* gui = knobGui ? knobGui->getGui() : 0;

    if (gui) {
        gui->setDraftRenderEnabled(true);
    }
}

void
ColorKnobEditSession::move(const KnobColorValue& value,
                           const QString& label)
{
    if (!_active) {
        return;
    }
    _label = label;
    _current = toKnobValues(value);
    _moved = true;
    if (appPTR->getCurrentSettings()->getRenderOnEditingFinishedOnly()) {
        return;
    }
    if (_current != _committed) {
        push(_current);
    }
}

void
ColorKnobEditSession::end(const KnobColorValue& value,
                          const QString& label)
{
    if (!_active) {
        return;
    }
    _label = label;
    _current = toKnobValues(value);
    abandon();
}

void
ColorKnobEditSession::abandon()
{
    if (!_active) {
        return;
    }
    _active = false;

    KnobGuiPtr knobGui = _knobGui.lock();
    Gui* gui = knobGui ? knobGui->getGui() : 0;

    // Draft goes off before the final push so that the committed value renders at full quality.
    if (gui) {
        gui->setDraftRenderEnabled(false);
    }
    if (_current != _committed) {
        push(_current);
    } else if (_moved && gui && appPTR->getCurrentSettings()->isAutoProxyEnabled()) {
        gui->renderAllViewers(true);
    }
}

NATRON_NAMESPACE_EXIT
