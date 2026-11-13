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

#include "ValueKnobGlobalModeTarget.h"

#include <algorithm>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QCoreApplication>
#include <QEvent>
#include <QPainter>
#include <QWidget>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/EffectInstance.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"

#include "Gui/ColorTrackballHud.h"
#include "Gui/ColorTrackballReveal.h"
#include "Gui/ColorTrackballTarget.h"
#include "Gui/DockablePanel.h"
#include "Gui/GuiDefines.h"
#include "Gui/KnobGui.h"
#include "Gui/KnobGuiContainerI.h"
#include "Gui/KnobGuiValue.h"

NATRON_NAMESPACE_ENTER

namespace {

QString
targetText(const char* text)
{
    return QCoreApplication::translate("ValueKnobGlobalModeTarget", text);
}

QString
minusSign()
{
    return QString(QChar(0x2212));
}

QString
arrowSign()
{
    return QString(QChar(0x2192));
}

std::vector<double>
knobIncrements(const KnobIPtr& knob)
{
    std::vector<double> increments;

    if (KnobDoublePtr d = std::dynamic_pointer_cast<KnobDouble>(knob)) {
        increments = d->getIncrements();
    } else if (KnobIntPtr i = std::dynamic_pointer_cast<KnobInt>(knob)) {
        const std::vector<int>& ints = i->getIncrements();

        increments.assign(ints.begin(), ints.end());
    }
    if (knob) {
        increments.resize(knob->getDimension(), increments.empty() ? 1. : increments.back());
    }

    return increments;
}

NodePtr
nodeOf(const KnobIPtr& knob)
{
    EffectInstance* effect = knob ? dynamic_cast<EffectInstance*>(knob->getHolder()) : 0;

    return effect ? effect->getNode() : NodePtr();
}

QString
formatIncrement(double increment)
{
    return QString::number(increment, 'g', 4);
}

} // namespace

/**
 * @brief An accent outline drawn over one knob row while global mode edits it. A child of the
 * row that follows its size and never takes input.
 **/
class GlobalModeRowHighlight
    : public QWidget {
public:
    explicit GlobalModeRowHighlight(QWidget* row)
        : QWidget(row)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setAttribute(Qt::WA_NoSystemBackground, true);
        setFocusPolicy(Qt::NoFocus);
        setGeometry(row->rect());
        row->installEventFilter(this);
        raise();
        show();
    }

    virtual bool eventFilter(QObject* watched,
                             QEvent* event) OVERRIDE FINAL
    {
        if ((watched == parentWidget()) && ((event->type() == QEvent::Resize) || (event->type() == QEvent::LayoutRequest))) {
            setGeometry(parentWidget()->rect());
            raise();
        }

        return QWidget::eventFilter(watched, event);
    }

protected:
    virtual void paintEvent(QPaintEvent* /*e*/) OVERRIDE FINAL
    {
        QPainter p(this);
        QPen pen(colorTrackballHudAccent());

        pen.setWidthF(TO_DPIX(1.5));
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);

        const double inset = 0.5 * pen.widthF();

        p.drawRoundedRect(QRectF(rect()).adjusted(inset, inset, -inset, -inset), TO_DPIX(3), TO_DPIX(3));
    }
};

QString
globalModeValueTitle(const KnobIPtr& knob)
{
    if (!knob) {
        return QString();
    }

    NodePtr node = nodeOf(knob);
    const QString name = QString::fromUtf8(knob->getName().c_str());

    if (!node) {
        return name;
    }

    return QString::fromUtf8(node->getLabel().c_str()) + QString::fromUtf8(": ") + name;
}

QString
globalModeValueRateLine(GlobalModeKindEnum kind,
                        const KnobIPtr& knob)
{
    const std::vector<double> increments = knobIncrements(knob);
    const QString increment = formatIncrement(increments.empty() ? 1. : increments.front());
    const QString drag = targetText("(drag x %1 X, y %1 Y)").arg(arrowSign());

    switch (kind) {
    case eGlobalModeKindTranslate:
        return targetText("Translate: 1 image px per px") + QString::fromUtf8("   ") + drag;
    case eGlobalModeKindRotate:
        return targetText("Rotate: 0.25° per px   (right/up CCW, left/down CW)");
    case eGlobalModeKindScale:
        return targetText("Scale: 0.5% per px, uniform   (Q/W: X or Y only)");
    case eGlobalModeKindSize:
        return targetText("Size: %1 per px, uniform   (Q/W: X or Y only)").arg(increment);
    case eGlobalModeKindSkew:
        return targetText("Skew: x drag %1 Skew X, y drag %1 Skew Y").arg(arrowSign());
    case eGlobalModeKindGenericSpatial2D: {
        const QString label = knob ? QString::fromUtf8(knob->getLabel().c_str()) : QString();

        return targetText("%1: 1 image px per px").arg(label) + QString::fromUtf8("   ") + drag;
    }
    case eGlobalModeKindGeneric:
        return targetText("Value: %1 per px   (right/up +, left/down %2)").arg(increment).arg(minusSign());
    case eGlobalModeKindColor:
    case eGlobalModeKindIneligible:
        break;
    }

    return QString();
}

ValueGlobalModeTargetBase::ValueGlobalModeTargetBase()
    : _rates()
    , _session()
    , _stroke()
    , _highlights()
    , _attached(false)
{
}

ValueGlobalModeTargetBase::~ValueGlobalModeTargetBase()
{
    for (std::size_t i = 0; i < _highlights.size(); ++i) {
        delete _highlights[i].data();
    }
}

DockablePanel*
ValueGlobalModeTargetBase::getPanel() const
{
    KnobGui* knobGui = getKnobGui();

    return knobGui ? dynamic_cast<DockablePanel*>(knobGui->getContainer()) : 0;
}

void
ValueGlobalModeTargetBase::refreshHighlight()
{
    for (std::size_t i = 0; i < _highlights.size(); ++i) {
        delete _highlights[i].data();
    }
    _highlights.clear();
    if (!_attached) {
        return;
    }

    const std::vector<KnobGuiPtr> knobGuis = getStrokeKnobGuis();

    for (std::size_t i = 0; i < knobGuis.size(); ++i) {
        KnobGuiValue* value = dynamic_cast<KnobGuiValue*>(knobGuis[i].get());
        QWidget* row = value ? value->getGlobalModeRowWidget() : 0;

        if (row) {
            _highlights.push_back(QPointer<GlobalModeRowHighlight>(new GlobalModeRowHighlight(row)));
        }
    }
}

void
ValueGlobalModeTargetBase::attach()
{
    _attached = true;
    refreshHighlight();

    KnobGui* knobGui = getKnobGui();

    if (!knobGui) {
        return;
    }
    touchGlobalModeKnob(knobGui);

    KnobGuiValue* value = dynamic_cast<KnobGuiValue*>(knobGui);

    revealKnobInPanel(getPanel(), knobGui->getKnob(), value ? value->getGlobalModeRowWidget() : 0);
}

void
ValueGlobalModeTargetBase::detach()
{
    _attached = false;
    refreshHighlight();
}

GlobalModeValueStrokeSpec
ValueGlobalModeTargetBase::fullSpec() const
{
    GlobalModeValueStrokeSpec spec = getStrokeSpec();
    const std::vector<KnobGuiPtr> knobGuis = getStrokeKnobGuis();

    spec.increments.clear();
    spec.isInt = false;
    for (std::size_t i = 0; i < knobGuis.size(); ++i) {
        KnobIPtr knob = knobGuis[i] ? knobGuis[i]->getKnob() : KnobIPtr();
        const std::vector<double> increments = knobIncrements(knob);

        spec.increments.insert(spec.increments.end(), increments.begin(), increments.end());
        if (std::dynamic_pointer_cast<KnobInt>(knob)) {
            spec.isInt = true;
        }
        if (const KnobValueRoleHolder* role = dynamic_cast<const KnobValueRoleHolder*>(knob.get())) {
            spec.clockwisePositive = spec.clockwisePositive || role->isValueRoleClockwise();
        }
    }

    return spec;
}

bool
ValueGlobalModeTargetBase::beginStroke()
{
    const std::vector<KnobGuiPtr> knobGuis = getStrokeKnobGuis();

    if (knobGuis.empty() || !_session.begin(knobGuis, getStrokeLabel())) {
        return false;
    }
    touchGlobalModeKnob(knobGuis.front().get());
    _stroke.begin(_session.getStartValues());

    return true;
}

void
ValueGlobalModeTargetBase::moveStroke(double dx,
                                      double dy,
                                      double gearFactor)
{
    if (!_session.isActive()) {
        return;
    }
    _stroke.addMove(dx, dy, gearFactor);
    _session.move(_stroke.evaluate(fullSpec(), _rates));
}

void
ValueGlobalModeTargetBase::endStroke()
{
    _session.end(getStrokeLabel());
}

void
ValueGlobalModeTargetBase::abandonStroke()
{
    _session.abandon();
}

void
ValueGlobalModeTargetBase::restartStrokeTravel()
{
    if (_session.isActive()) {
        _stroke.begin(_session.getCurrentValues());
    }
}

void
ValueGlobalModeTargetBase::switchStrokeKnobs(const std::function<void()>& change)
{
    const bool wasStroking = _session.isActive();
    const QString label = getStrokeLabel();

    change();
    if (wasStroking) {
        const std::vector<KnobGuiPtr> knobGuis = getStrokeKnobGuis();

        // The physical stroke stays one undo step: the session keeps what the knobs switched
        // away from have been given, and travel carries on from the new knobs' latest values.
        if (!knobGuis.empty() && _session.switchKnobs(knobGuis)) {
            touchGlobalModeKnob(knobGuis.front().get());
            _stroke.begin(_session.getCurrentValues());
        } else {
            _session.end(label);
        }
    }
    refreshHighlight();
}

void
ValueGlobalModeTargetBase::syncReveal(ColorTrackballReveal* reveal,
                                      bool strokeActive)
{
    if (!reveal) {
        return;
    }

    const bool stroking = strokeActive && _session.isActive();

    reveal->setValueKind(getStrokeKind());
    reveal->setValueTravel(stroking ? _stroke.getTotalDx() : 0., stroking ? _stroke.getTotalDy() : 0.);
}

ValueKnobGlobalModeTarget::ValueKnobGlobalModeTarget(const KnobGuiPtr& knobGui)
    : ValueGlobalModeTargetBase()
    , _knobGui(knobGui)
    , _skewSibling()
    , _siblingFirst(false)
    , _kind(eGlobalModeKindIneligible)
    , _lock(kGlobalModeLockAll)
{
    KnobIPtr knob = knobGui ? knobGui->getKnob() : KnobIPtr();

    _kind = classifyGlobalModeKind(knob);
    if ((_kind != eGlobalModeKindSkew) || !knob || (knob->getDimension() != 1)) {
        return;
    }

    // skewX finds skewY and the other way round, so a lone skew knob still drives both axes.
    const std::string name = knob->getName();

    if (name.empty() || ((name.back() != 'X') && (name.back() != 'Y'))) {
        return;
    }

    std::string siblingName = name;

    siblingName.back() = (name.back() == 'X') ? 'Y' : 'X';

    KnobHolder* holder = knob->getHolder();
    KnobIPtr sibling = holder ? holder->getKnobByName(siblingName) : KnobIPtr();
    KnobGuiContainerI* container = knobGui->getContainer();
    KnobGuiPtr siblingGui = (sibling && container && (sibling->getDimension() == 1)) ? container->getKnobGui(sibling) : KnobGuiPtr();

    if (siblingGui && (classifyGlobalModeKind(sibling) == eGlobalModeKindSkew)) {
        _skewSibling = siblingGui;
        _siblingFirst = (name.back() == 'Y');
    }
}

ValueKnobGlobalModeTarget::~ValueKnobGlobalModeTarget()
{
}

KnobGui*
ValueKnobGlobalModeTarget::getKnobGui() const
{
    return _knobGui.lock().get();
}

std::vector<KnobGui*>
ValueKnobGlobalModeTarget::getKnobGuis() const
{
    const std::vector<KnobGuiPtr> knobGuis = getStrokeKnobGuis();
    std::vector<KnobGui*> result;

    for (std::size_t i = 0; i < knobGuis.size(); ++i) {
        result.push_back(knobGuis[i].get());
    }

    return result;
}

std::vector<KnobGuiPtr>
ValueKnobGlobalModeTarget::getStrokeKnobGuis() const
{
    std::vector<KnobGuiPtr> result;
    KnobGuiPtr knobGui = _knobGui.lock();
    KnobGuiPtr sibling = _skewSibling.lock();

    if (!knobGui) {
        return result;
    }
    if (sibling && _siblingFirst) {
        result.push_back(sibling);
    }
    result.push_back(knobGui);
    if (sibling && !_siblingFirst) {
        result.push_back(sibling);
    }

    return result;
}

int
ValueKnobGlobalModeTarget::slotCount() const
{
    const std::vector<KnobGuiPtr> knobGuis = getStrokeKnobGuis();
    int count = 0;

    for (std::size_t i = 0; i < knobGuis.size(); ++i) {
        KnobIPtr knob = knobGuis[i]->getKnob();

        count += knob ? knob->getDimension() : 0;
    }

    return count;
}

GlobalModeValueStrokeSpec
ValueKnobGlobalModeTarget::getStrokeSpec() const
{
    GlobalModeValueStrokeSpec spec;

    spec.kind = _kind;
    spec.lockDimension = _lock;

    return spec;
}

QString
ValueKnobGlobalModeTarget::getStrokeLabel() const
{
    return targetText("Global edit");
}

bool
ValueKnobGlobalModeTarget::handleKey(int key,
                                     Qt::KeyboardModifiers modifiers,
                                     bool /*strokeActive*/)
{
    if (modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) {
        return false;
    }

    GlobalModeValueKeyAction action = classifyGlobalModeValueKey(key, false, slotCount());
    int lock = _lock;

    switch (action.action) {
    case eGlobalModeValueKeyLockAll:
        lock = kGlobalModeLockAll;
        break;
    case eGlobalModeValueKeyLockDimension:
        lock = action.dimension;
        break;
    case eGlobalModeValueKeyNone:
    case eGlobalModeValueKeySwitchMember:
        return false;
    }
    if (lock == _lock) {
        return false;
    }
    _lock = lock;
    restartStrokeTravel();

    return true;
}

void
ValueKnobGlobalModeTarget::fillHudState(ColorTrackballHudState* state) const
{
    KnobGuiPtr knobGui = _knobGui.lock();
    KnobIPtr knob = knobGui ? knobGui->getKnob() : KnobIPtr();
    const int slots = slotCount();
    const bool skewPair = (_kind == eGlobalModeKindSkew) && (slots == 2);
    const char* keys[4] = { "Q", "W", "E", "R" };
    const char* axes[4] = { "X", "Y", "Z", "W" };
    std::vector<ColorTrackballHudChip> row;

    state->knobLabel = globalModeValueTitle(knob);
    for (int i = 0; i < 4; ++i) {
        ColorTrackballHudChip chip;

        chip.key = QString::fromUtf8(keys[i]);
        if (skewPair) {
            if (i >= 2) {
                continue;
            }
            chip.caption = targetText("Skew %1").arg(QString::fromUtf8(axes[i]));
        } else {
            chip.caption = QString::fromUtf8(axes[i]);
        }
        chip.dimmed = (i >= slots);
        chip.lit = (_lock == i);
        row.push_back(chip);
    }

    ColorTrackballHudChip all;

    all.key = QString::fromUtf8("A");
    all.caption = targetText("All");
    all.lit = (_lock == kGlobalModeLockAll);
    row.push_back(all);
    state->chipRows.push_back(row);
    state->rateLine = globalModeValueRateLine(_kind, knob);
}

NATRON_NAMESPACE_EXIT
