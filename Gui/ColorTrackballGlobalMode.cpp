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

#include "ColorTrackballGlobalMode.h"

#include <cmath>
#include <cstddef>
#include <list>
#include <map>
#include <vector>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QApplication>
#include <QCoreApplication>
#include <QCursor>
#include <QEvent>
#include <QGuiApplication>
#include <QKeyCombination>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QRect>
#include <QScreen>
#include <QScrollArea>
#include <QStyle>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWindow>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/AppManager.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"

#include "Gui/ColorAdjustPanel.h"
#include "Gui/ColorKnobEditSession.h"
#include "Gui/ColorTrackballHud.h"
#include "Gui/ColorTrackballReveal.h"
#include "Gui/ColorWheelWidget.h"
#include "Gui/DockablePanel.h"
#include "Gui/FloatingWidget.h"
#include "Gui/Gui.h"
#include "Gui/KnobGuiColor.h"
#include "Gui/KnobGuiContainerHelper.h"
#include "Gui/NodeGui.h"
#include "Gui/NodeSettingsPanel.h"
#include "Gui/PropertiesBinWrapper.h"
#include "Gui/ViewerTab.h"

NATRON_NAMESPACE_ENTER

namespace {

const double kNeutralSaturation = 1e-9;
const int kWheelNotch = 120;
const int kEdgeWarpMargin = 8;
const int kMaxWarpSkips = 8;

double
wrapDegrees(double d)
{
    d = std::fmod(d, 360.0);
    if (d < 0.0) {
        d += 360.0;
    }

    return d >= 360.0 ? d - 360.0 : d;
}

Qt::KeyboardModifiers
withoutKeypad(Qt::KeyboardModifiers modifiers)
{
    return modifiers & ~Qt::KeypadModifier;
}

KnobColorValue
neutralValue()
{
    return neutralGainValue(false);
}

} // namespace

const char*
colorTrackballTargetPropertyName()
{
    return "_natronColorTrackballTarget";
}

ColorGlobalModeKeyAction
classifyColorGlobalModeKey(int key,
                           Qt::KeyboardModifiers modifiers,
                           bool strokeActive)
{
    ColorGlobalModeKeyAction result;
    Qt::KeyboardModifiers mods = withoutKeypad(modifiers);
    const bool ctrl = mods.testFlag(Qt::ControlModifier);
    const bool shift = mods.testFlag(Qt::ShiftModifier);
    const bool other = mods.testFlag(Qt::AltModifier) || mods.testFlag(Qt::MetaModifier);

    switch (key) {
    case Qt::Key_Escape:
    case Qt::Key_Return:
    case Qt::Key_Enter:
        result.action = eColorGlobalModeKeyAccept;

        return result;

    case Qt::Key_C:
    case Qt::Key_S:
    case Qt::Key_T:
    case Qt::Key_V:
        // Shift is allowed through: the user may already be holding it for the gear.
        if (!ctrl && !other) {
            result.action = eColorGlobalModeKeySetMode;
            result.mode = (key == Qt::Key_C) ? eColorGlobalModeC : (key == Qt::Key_S) ? eColorGlobalModeS
                : (key == Qt::Key_T)                                                  ? eColorGlobalModeT
                                                                                      : eColorGlobalModeV;
        }

        return result;

    case Qt::Key_Up:
    case Qt::Key_Down:
        if (ctrl && !other) {
            result.action = (key == Qt::Key_Up) ? eColorGlobalModeKeyPreviousKnob : eColorGlobalModeKeyNextKnob;
        }

        return result;

    case Qt::Key_Z:
        if (ctrl && !other && !strokeActive) {
            result.action = shift ? eColorGlobalModeKeyRedo : eColorGlobalModeKeyUndo;
        }

        return result;

    case Qt::Key_Y:
        if (ctrl && !shift && !other && !strokeActive) {
            result.action = eColorGlobalModeKeyRedo;
        }

        return result;

    default:
        return result;
    }
} // classifyColorGlobalModeKey

Qt::KeyboardModifiers
colorGlobalModeModifiersAfterKey(int key,
                                 bool press,
                                 Qt::KeyboardModifiers reported)
{
    Qt::KeyboardModifier flag;

    switch (key) {
    case Qt::Key_Control:
        flag = Qt::ControlModifier;
        break;
    case Qt::Key_Shift:
        flag = Qt::ShiftModifier;
        break;
    case Qt::Key_Alt:
        flag = Qt::AltModifier;
        break;
    case Qt::Key_Meta:
        flag = Qt::MetaModifier;
        break;
    default:
        return reported;
    }

    return press ? (reported | flag) : (reported & ~flag);
}

int
stepColorKnobIndex(const std::vector<ColorKnobInfo>& knobs,
                   int current,
                   int direction)
{
    if (direction == 0) {
        return -1;
    }

    const int step = (direction > 0) ? 1 : -1;

    for (int i = current + step; (i >= 0) && (i < (int)knobs.size()); i += step) {
        if (knobs[i].visible && knobs[i].enabled) {
            return i;
        }
    }

    return -1;
}

int
takeColorGlobalModeWheelNotches(int* accumulator,
                                int angleDelta)
{
    if (!accumulator) {
        return 0;
    }
    if (((*accumulator > 0) && (angleDelta < 0)) || ((*accumulator < 0) && (angleDelta > 0))) {
        *accumulator = 0;
    }
    *accumulator += angleDelta;

    const int notches = *accumulator / kWheelNotch;

    *accumulator -= notches * kWheelNotch;

    return notches;
}

bool
colorGlobalModeFeedbackShowsCrosshair(Settings::ColorTrackballFeedbackEnum feedback)
{
    return feedback == Settings::eColorTrackballFeedbackCrosshairReveal;
}

bool
colorGlobalModeFeedbackGraphicIsCursor(Settings::ColorTrackballFeedbackEnum feedback)
{
    return feedback == Settings::eColorTrackballFeedbackWidgetCursor;
}

ColorGlobalModeFeedbackState
colorGlobalModeFeedbackState(Settings::ColorTrackballFeedbackEnum feedback,
                             bool engaged,
                             bool strokeActive)
{
    ColorGlobalModeFeedbackState state;

    if (!engaged) {
        return state;
    }
    state.crosshair = colorGlobalModeFeedbackShowsCrosshair(feedback);
    if (colorGlobalModeFeedbackGraphicIsCursor(feedback)) {
        state.graphicShown = true;
        state.graphicFollowsPointer = !strokeActive;
    } else {
        state.graphicShown = strokeActive;
    }
    state.blankCursor = state.graphicShown;

    return state;
}

ColorGlobalModeFeedbackSwitch::ColorGlobalModeFeedbackSwitch()
    : _feedback(Settings::eColorTrackballFeedbackCrosshairReveal)
    , _pending(false)
    , _pendingFeedback(Settings::eColorTrackballFeedbackCrosshairReveal)
{
}

bool
ColorGlobalModeFeedbackSwitch::offer(Settings::ColorTrackballFeedbackEnum latest,
                                     bool strokeActive)
{
    _pending = true;
    _pendingFeedback = latest;
    if (strokeActive) {
        return false;
    }

    return strokeEnded();
}

bool
ColorGlobalModeFeedbackSwitch::strokeEnded()
{
    if (!_pending) {
        return false;
    }
    _pending = false;

    const bool changed = (_pendingFeedback != _feedback);

    _feedback = _pendingFeedback;

    return changed;
}

ColorGlobalModeOverrideCursor::ColorGlobalModeOverrideCursor(Qt::CursorShape shape)
    : _shape(shape)
    , _held(false)
{
}

ColorGlobalModeOverrideCursor::~ColorGlobalModeOverrideCursor()
{
    setHeld(false);
}

void
ColorGlobalModeOverrideCursor::setHeld(bool held)
{
    if (held == _held) {
        return;
    }
    // Without an application there is no stack left to restore.
    if (!qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
        _held = false;

        return;
    }
    _held = held;
    if (held) {
        QGuiApplication::setOverrideCursor(QCursor(_shape));
    } else {
        QGuiApplication::restoreOverrideCursor();
    }
}

ColorGlobalModeCursors::ColorGlobalModeCursors()
    : _crosshair(Qt::CrossCursor)
    , _blank(Qt::BlankCursor)
{
}

ColorGlobalModeCursors::~ColorGlobalModeCursors()
{
    sync(false, false);
}

void
ColorGlobalModeCursors::sync(bool crosshair,
                             bool blank)
{
    // Popping always takes the top entry, so the blank cursor comes off before the crosshair
    // under it changes, and goes back on afterwards.
    if ((crosshair != _crosshair.isHeld()) && _blank.isHeld()) {
        _blank.setHeld(false);
    }
    _crosshair.setHeld(crosshair);
    _blank.setHeld(blank);
}

ColorGlobalModeStroke::ColorGlobalModeStroke()
    : _start(neutralValue())
    , _startHue(0.0)
    , _dx(0.0)
    , _dy(0.0)
{
}

void
ColorGlobalModeStroke::begin(const KnobColorValue& start,
                             double rememberedHueDegrees)
{
    _start = start;
    _startHue = rememberedHueDegrees;
    _dx = 0.0;
    _dy = 0.0;
}

void
ColorGlobalModeStroke::addMove(double dx,
                               double dy,
                               double gearFactor)
{
    _dx += dx * gearFactor;
    _dy += dy * gearFactor;
}

double
ColorGlobalModeStroke::hueTravelDegrees(ColorGlobalModeEnum mode,
                                        const TrackballRates& rates) const
{
    return (mode == eColorGlobalModeS) ? _dx * rates.hueDegreesPerPixel : 0.0;
}

ChromaEditResult
ColorGlobalModeStroke::evaluate(ColorGlobalModeEnum mode,
                                ColorKnobRoleEnum role,
                                const LumaBasis& basis,
                                const TrackballRates& rates) const
{
    // Rotating a neutral start does nothing, so on its own the hue travel would be lost for
    // the whole stroke; advancing the remembered hue by it makes the saturation the same
    // stroke grows point the way the hue axis has been dragged.
    const double hue = wrapDegrees(_startHue + hueTravelDegrees(mode, rates));

    return globalModeEdit(mode, role, basis, _start, _dx, _dy, 1.0, rates, hue);
}

double
ColorGlobalModeStroke::rememberedHueAfter(ColorGlobalModeEnum mode,
                                          ColorKnobRoleEnum role,
                                          const LumaBasis& basis,
                                          const KnobColorValue& value,
                                          const TrackballRates& rates) const
{
    PlanePoint p = toPlane(role, basis, value);

    if (p.valid && (planeSaturation(p) > kNeutralSaturation)) {
        return planeHue(p);
    }

    return wrapDegrees(_startHue + hueTravelDegrees(mode, rates));
}

namespace {
std::map<Gui*, ColorTrackballGlobalMode*>&
globalModeControllers()
{
    static std::map<Gui*, ColorTrackballGlobalMode*> controllers;

    return controllers;
}
} // namespace

namespace {
// dynamic_cast rather than QObject::findChild: PropertiesBinWrapper has no Q_OBJECT of its own,
// so it has no distinct meta-object for findChild to match against.
bool
windowHoldsPropertiesBinOrViewer(QWidget* window)
{
    if (!window) {
        return false;
    }

    const QList<QWidget*> children = window->findChildren<QWidget*>();

    for (QList<QWidget*>::const_iterator it = children.begin(); it != children.end(); ++it) {
        if (dynamic_cast<PropertiesBinWrapper*>(*it) || dynamic_cast<ViewerTab*>(*it)) {
            return true;
        }
    }

    return false;
}

bool
isOnAnyScreen(const QPoint& p)
{
    return QGuiApplication::screenAt(p) != 0;
}
} // namespace

// Created with no parent, as a window of its own that follows the cursor; deleted with the controller.
ColorTrackballHud*
ColorTrackballGlobalMode::ensureHud()
{
    if (!_hud) {
        _hud = new ColorTrackballHud();
        _hud->hide();
    }

    return _hud.data();
}

void
ColorTrackballGlobalMode::showChrome()
{
    // The HUD's own visibility follows applyFeedback() and exit(); only the window border is
    // done here.
    if (!_gui) {
        return;
    }
    _highlightFrames.push_back(QPointer<WindowHighlightFrame>(new WindowHighlightFrame(_gui)));

    const std::list<FloatingWidget*> floatingWindows = _gui->getFloatingWindows();

    for (std::list<FloatingWidget*>::const_iterator it = floatingWindows.begin(); it != floatingWindows.end(); ++it) {
        if (windowHoldsPropertiesBinOrViewer(*it)) {
            _highlightFrames.push_back(QPointer<WindowHighlightFrame>(new WindowHighlightFrame(*it)));
        }
    }
}

void
ColorTrackballGlobalMode::hideChrome()
{
    for (std::size_t i = 0; i < _highlightFrames.size(); ++i) {
        delete _highlightFrames[i].data();
    }
    _highlightFrames.clear();
}

void
ColorTrackballGlobalMode::refreshTargetPosition()
{
    getTargetPosition(&_targetPosition, &_targetCount);
}

// Everything the HUD shows beyond what the controller's own getters already carry: the node
// and knob labels, and the mode's own chroma readout, read off the live value through the
// target's own role and basis so the HUD never has to know either.
void
ColorTrackballGlobalMode::updateHud()
{
    if (!_hud || !_engaged) {
        return;
    }

    ColorTrackballHudState state;
    KnobGuiColor* target = getTarget();

    if (target) {
        DockablePanel* panel = dynamic_cast<DockablePanel*>(target->getContainer());
        NodeSettingsPanel* nodePanel = dynamic_cast<NodeSettingsPanel*>(panel);
        NodeGuiPtr nodeGui = nodePanel ? nodePanel->getNode() : NodeGuiPtr();
        NodePtr node = nodeGui ? nodeGui->getNode() : NodePtr();
        KnobIPtr knob = target->getKnob();

        state.nodeLabel = node ? QString::fromUtf8(node->getLabel().c_str()) : QString();
        state.knobLabel = knob ? QString::fromUtf8(knob->getLabel().c_str()) : QString();
        // Mid-stroke only the value moves; the knob list scan waits for the stroke to end.
        if (!_strokeActive) {
            refreshTargetPosition();
        }
        state.knobIndex = _targetPosition;
        state.knobCount = _targetCount;
        state.value = getLiveValue();

        const ColorKnobRoleEnum role = _strokeActive ? _strokeRole : target->getColorRole();
        const LumaBasis basis = _strokeActive ? _strokeBasis : target->getEditBasis();

        state.role = role;
        state.luma = luma(basis, state.value.rgb);
        switch (_mode) {
        case eColorGlobalModeC:
        case eColorGlobalModeS: {
            const PlanePoint p = toPlane(role, basis, state.value);

            state.chromaValid = p.valid;
            state.hueDegrees = planeHue(p);
            state.saturation = planeSaturation(p);
            break;
        }
        case eColorGlobalModeT: {
            const TempTintResult tt = toTempTint(role, basis, state.value);

            state.chromaValid = tt.valid;
            state.temp = tt.temp;
            state.tint = tt.tint;
            break;
        }
        case eColorGlobalModeV:
            break;
        }
    }

    state.mode = _mode;
    state.gear = _gear;
    state.endOfListMessage = _endOfListMessage;

    _hud->setState(state);
} // ColorTrackballGlobalMode::updateHud

ColorTrackballGlobalMode*
ColorTrackballGlobalMode::instanceFor(Gui* gui)
{
    if (!gui) {
        return 0;
    }

    std::map<Gui*, ColorTrackballGlobalMode*>& controllers = globalModeControllers();
    std::map<Gui*, ColorTrackballGlobalMode*>::iterator it = controllers.find(gui);

    if (it != controllers.end()) {
        return it->second;
    }

    ColorTrackballGlobalMode* controller = new ColorTrackballGlobalMode(gui);

    controllers[gui] = controller;
    // Keyed on the Gui's address, so the entry must go before that address can be reused.
    QObject::connect(gui, &QObject::destroyed, [gui]() {
        std::map<Gui*, ColorTrackballGlobalMode*>& controllers = globalModeControllers();
        std::map<Gui*, ColorTrackballGlobalMode*>::iterator it = controllers.find(gui);

        if (it != controllers.end()) {
            delete it->second;
            controllers.erase(it);
        }
    });

    return controller;
}

ColorTrackballGlobalMode::ColorTrackballGlobalMode(Gui* gui)
    : QObject(0)
    , _gui(gui)
    , _engaged(false)
    , _target()
    , _targetObject()
    , _targetConnections()
    , _mode(eColorGlobalModeC)
    , _gear(1.0)
    , _rates()
    , _rememberedHue(0.0)
    , _strokeActive(false)
    , _stroke()
    , _strokeValue(neutralValue())
    , _strokeRole(eColorKnobRoleGain)
    , _strokeBasis(ColorKnobEditSession::basisFor(ProjectColorManagementPtr()))
    , _strokeMode(eColorGlobalModeC)
    , _heldButtons(Qt::NoButton)
    , _lastPos()
    , _warpPending(false)
    , _warpTarget()
    , _warpSkips(0)
    , _grabWidget()
    , _grabbed()
    , _wheelAccumulator(0)
    , _swallowedKeys()
    , _passThrough()
    , _feedback()
    , _cursors()
    , _pointerPos()
    , _hud()
    , _reveal()
    , _revealFollowing(false)
    , _revealKnob(0)
    , _revealRole(eColorKnobRoleGain)
    , _highlightFrames()
    , _endOfListMessage()
    , _targetPosition(0)
    , _targetCount(0)
{
    // Driven by the signals rather than called from each place that emits them, so every
    // emitter keeps the HUD and the highlight frames current without knowing about them.
    QObject::connect(this, &ColorTrackballGlobalMode::engagedChanged, this, [this](bool engaged) {
        if (engaged) {
            showChrome();
        } else {
            hideChrome();
        }
    });
    QObject::connect(this, &ColorTrackballGlobalMode::stateChanged, this, [this]() {
        updateHud();
        syncReveal();
    });
    SettingsPtr settings = appPTR ? appPTR->getCurrentSettings() : SettingsPtr();

    if (settings) {
        QObject::connect(settings.get(), &Settings::settingChanged, this, [this](KnobI*) {
            if (offerFeedbackSetting()) {
                applyFeedback();
            }
        });
    }
    QObject::connect(this, &ColorTrackballGlobalMode::knobListEndReached, this, [this](int direction) {
        _endOfListMessage = (direction < 0) ? tr("First colour knob.") : tr("Last colour knob.");
        updateHud();
    });
}

ColorTrackballGlobalMode::~ColorTrackballGlobalMode()
{
    // The Gui is going away, so its main window may already be half torn down: only the
    // filter is removed, nothing is pushed or repainted.
    QCoreApplication* app = QCoreApplication::instance();

    if (_engaged && app) {
        app->removeEventFilter(this);
    }
    for (std::size_t i = 0; i < _targetConnections.size(); ++i) {
        QObject::disconnect(_targetConnections[i]);
    }
    hideChrome();
    delete _hud.data();
    delete _reveal.data();
}

KnobGuiColor*
ColorTrackballGlobalMode::getTarget() const
{
    KnobGuiColorPtr target = _target.lock();

    return (target && _targetObject) ? target.get() : 0;
}

DockablePanel*
ColorTrackballGlobalMode::getTargetPanel() const
{
    KnobGuiColor* target = getTarget();

    return target ? dynamic_cast<DockablePanel*>(target->getContainer()) : 0;
}

QString
ColorTrackballGlobalMode::strokeLabel() const
{
    return tr("Global %1").arg(colorGlobalModeKeyName(_strokeActive ? _strokeMode : _mode));
}

void
ColorTrackballGlobalMode::setGrabWidget(QWidget* widget)
{
    _grabWidget = widget;
}

void
ColorTrackballGlobalMode::setPassThroughShortcuts(const QList<QKeySequence>& shortcuts)
{
    _passThrough = shortcuts;
}

KnobColorValue
ColorTrackballGlobalMode::getLiveValue() const
{
    if (_strokeActive) {
        return _strokeValue;
    }

    KnobGuiColor* target = getTarget();

    return target ? target->getCurrentColorValue() : neutralValue();
}

void
ColorTrackballGlobalMode::getTargetPosition(int* n,
                                            int* count) const
{
    if (n) {
        *n = 0;
    }
    if (count) {
        *count = 0;
    }

    KnobGuiColor* target = getTarget();
    DockablePanel* panel = getTargetPanel();

    if (!_engaged || !target || !panel) {
        return;
    }

    std::vector<KnobGuiColor*> knobs = orderedColorKnobs(panel);
    int legal = 0;
    int position = 0;

    for (std::size_t i = 0; i < knobs.size(); ++i) {
        KnobIPtr knob = knobs[i]->getKnob();

        if (knobs[i] == target) {
            position = legal + 1;
        } else if (!knob || !knob->isEnabled(0) || knobs[i]->isSecretRecursive()) {
            continue;
        }
        ++legal;
    }
    if (n) {
        *n = position;
    }
    if (count) {
        *count = legal;
    }
}

void
ColorTrackballGlobalMode::setGear(double gearFactor)
{
    if (gearFactor == _gear) {
        return;
    }
    _gear = gearFactor;
    Q_EMIT stateChanged();
}

void
ColorTrackballGlobalMode::setMode(ColorGlobalModeEnum mode)
{
    if (mode == _mode) {
        return;
    }
    if (_strokeActive) {
        // The travel so far is folded into a new start, so the new mode accumulates from
        // where the old one left the value instead of reinterpreting the old mode's pixels.
        _rememberedHue = _stroke.rememberedHueAfter(_strokeMode, _strokeRole, _strokeBasis, _strokeValue, _rates);
        _stroke.begin(_strokeValue, _rememberedHue);
        _strokeMode = mode;
    }
    _mode = mode;
    Q_EMIT stateChanged();
}

void
ColorTrackballGlobalMode::clearTarget()
{
    for (std::size_t i = 0; i < _targetConnections.size(); ++i) {
        QObject::disconnect(_targetConnections[i]);
    }
    _targetConnections.clear();

    KnobGuiColor* target = getTarget();

    if (target) {
        target->setGlobalModeHighlighted(false);
    }
    _target.reset();
    _targetObject = 0;
}

void
ColorTrackballGlobalMode::onTargetGone()
{
    // The target's KnobGui or panel is being destroyed: its edit session has already ended
    // the stroke or is about to, so nothing is pushed from here.
    for (std::size_t i = 0; i < _targetConnections.size(); ++i) {
        QObject::disconnect(_targetConnections[i]);
    }
    _targetConnections.clear();
    _target.reset();
    _targetObject = 0;
    _strokeActive = false;
    exit();
}

void
ColorTrackballGlobalMode::setTarget(const KnobGuiColorPtr& target)
{
    // The outgoing knob is left exactly as the switch leaves it, open and unfolded, with
    // nothing restored.
    clearTarget();
    // A successful switch reaches a real knob, so whatever end-of-list message the previous
    // step may have left standing no longer describes the HUD's new target.
    _endOfListMessage.clear();
    _target = target;
    _targetObject = target.get();

    _targetConnections.push_back(QObject::connect(target.get(), &QObject::destroyed, this, [this]() {
        onTargetGone();
    }));

    DockablePanel* panel = dynamic_cast<DockablePanel*>(target->getContainer());

    if (panel) {
        _targetConnections.push_back(QObject::connect(panel, &QObject::destroyed, this, [this]() {
            onTargetGone();
        }));
        _targetConnections.push_back(QObject::connect(panel, &DockablePanel::closeChanged, this, [this](bool closed) {
            if (closed) {
                exit();
            }
        }));
    }

    KnobIPtr knob = target->getKnob();
    KnobSignalSlotHandlerPtr handler = knob ? knob->getSignalSlotHandler() : KnobSignalSlotHandlerPtr();

    if (handler) {
        // A stroke's own pushes are reported by moveStroke() already; this covers every other
        // change to the knob, such as an undo or an edit from elsewhere.
        _targetConnections.push_back(QObject::connect(handler.get(), &KnobSignalSlotHandler::valueChanged, this, [this](ViewSpec, int, int) {
            if (!_strokeActive) {
                Q_EMIT stateChanged();
            }
        }));
    }

    target->setGlobalModeHighlighted(true);
    // Opens and unfolds the knob; switching to another one, or leaving the mode, never folds
    // or closes it back up, so working across several knobs in one session leaves every one of
    // them open afterwards instead of needing to be reopened.
    target->setAdjustPanelExpanded(true);
    target->touchColorKnob();
    revealTarget();
}

void
ColorTrackballGlobalMode::revealTarget()
{
    KnobGuiColor* target = getTarget();
    DockablePanel* panel = getTargetPanel();

    if (!target || !panel) {
        return;
    }

    KnobIPtr knob = target->getKnob();

    if (!knob) {
        return;
    }

    KnobPagePtr page = knob->getTopLevelPage();
    KnobPageGuiPtr currentPage = panel->getCurrentPage();

    if (page && (!currentPage || (currentPage->pageKnob.lock() != page))) {
        panel->setPageActiveIndex(page);
    }
    for (KnobIPtr parent = knob->getParentKnob(); parent; parent = parent->getParentKnob()) {
        KnobGroupPtr group = std::dynamic_pointer_cast<KnobGroup>(parent);

        if (!group || group->isTab() || group->getValue()) {
            continue;
        }
        // Not UserEdited: that reason is the one change KnobGui does not mirror back into the
        // group's own widget, which is what actually lays the children out.
        group->setValue(true, ViewSpec::all(), 0, eValueChangedReasonNatronGuiEdited, 0);
    }

    // The whole column, not just the row: with the panel open and the HUD attached, the column
    // has grown well past the row alone, and every part of it should land in view together.
    QWidget* column = target->getGlobalModeColumnWidget();

    if (!column) {
        return;
    }
    for (QWidget* w = column->parentWidget(); w; w = w->parentWidget()) {
        if (QScrollArea* scrollArea = qobject_cast<QScrollArea*>(w)) {
            scrollArea->ensureWidgetVisible(column);
            break;
        }
    }
}

bool
ColorTrackballGlobalMode::engage(const KnobGuiColorPtr& target)
{
    if (!target || !target->getAdjustPanel()) {
        return false;
    }
    if (_engaged && (getTarget() == target.get())) {
        return true;
    }

    endStroke();
    if (!_engaged) {
        offerFeedbackSetting();
    }
    setTarget(target);
    if (!_engaged) {
        setMode(eColorGlobalModeC);
        _engaged = true;
        _pointerPos = QCursor::pos();
        applyFeedback();
        _heldButtons = Qt::NoButton;
        _wheelAccumulator = 0;
        _swallowedKeys.clear();
        _warpPending = false;
        QCoreApplication* app = QCoreApplication::instance();

        if (app) {
            app->installEventFilter(this);
        }
        Q_EMIT engagedChanged(true);
    }
    Q_EMIT stateChanged();

    return true;
}

void
ColorTrackballGlobalMode::exit()
{
    if (!_engaged) {
        return;
    }
    endStroke();
    releaseStrokeGrab();
    _heldButtons = Qt::NoButton;
    _engaged = false;

    // The touched knob is left open and unfolded, exactly as the user left it; only the HUD
    // goes away.
    if (_hud) {
        _hud->hide();
    }

    QCoreApplication* app = QCoreApplication::instance();

    if (app) {
        app->removeEventFilter(this);
    }
    clearTarget();
    applyFeedback();
    Q_EMIT engagedChanged(false);
    Q_EMIT stateChanged();
}

bool
ColorTrackballGlobalMode::stepKnob(int direction)
{
    KnobGuiColor* target = getTarget();
    DockablePanel* panel = getTargetPanel();

    if (!_engaged || !target || !panel) {
        return false;
    }
    endStroke();

    std::vector<KnobGuiColor*> knobs = orderedColorKnobs(panel);
    std::vector<ColorKnobInfo> infos;
    int current = -1;

    infos.reserve(knobs.size());
    for (std::size_t i = 0; i < knobs.size(); ++i) {
        KnobIPtr knob = knobs[i]->getKnob();
        ColorKnobInfo info;

        info.scriptName = knob ? knob->getName() : std::string();
        info.enabled = knob && knob->isEnabled(0);
        info.visible = !knobs[i]->isSecretRecursive();
        infos.push_back(info);
        if (knobs[i] == target) {
            current = (int)i;
        }
    }

    int next = stepColorKnobIndex(infos, current, direction);

    if (next < 0) {
        Q_EMIT knobListEndReached(direction > 0 ? 1 : -1);

        return false;
    }

    KnobGuiColorPtr nextTarget = std::dynamic_pointer_cast<KnobGuiColor>(knobs[next]->shared_from_this());

    if (!nextTarget) {
        return false;
    }
    setTarget(nextTarget);
    Q_EMIT stateChanged();

    return true;
}

void
ColorTrackballGlobalMode::grabForStroke()
{
    QWidget* grabber = _grabWidget.data();

    if (!grabber || !grabber->isVisible()) {
        grabber = _gui;
    }
    if (!grabber || !grabber->isVisible()) {
        return;
    }
    grabber->grabMouse();
    _grabbed = grabber;
}

void
ColorTrackballGlobalMode::releaseStrokeGrab()
{
    QPointer<QWidget> grabbed = _grabbed;

    // Cleared first, so the UngrabMouse this release may send is not read as a lost grab.
    _grabbed = 0;
    if (grabbed && (QWidget::mouseGrabber() == grabbed.data())) {
        grabbed->releaseMouse();
    }
}

void
ColorTrackballGlobalMode::beginStroke()
{
    KnobGuiColor* target = getTarget();

    if (!target || !target->isColorEditable()) {
        return;
    }
    target->touchColorKnob();
    _strokeRole = target->getColorRole();
    _strokeBasis = target->getEditBasis();
    _strokeMode = _mode;
    _strokeValue = target->getCurrentColorValue();
    _stroke.begin(_strokeValue, _rememberedHue);
    _strokeActive = true;
    target->getEditSession()->begin(strokeLabel());
    Q_EMIT stateChanged();
}

bool
ColorTrackballGlobalMode::warpNearScreenEdge(const QPointF& globalPos)
{
    QPoint p = globalPos.toPoint();
    QScreen* screen = QGuiApplication::screenAt(p);

    if (!screen) {
        return false;
    }

    // Only an edge with no other screen beyond it stops the pointer; across a border between
    // two monitors it carries on by itself.
    const int m = kEdgeWarpMargin;

    if (isOnAnyScreen(p + QPoint(-m, 0)) && isOnAnyScreen(p + QPoint(m, 0)) && isOnAnyScreen(p + QPoint(0, -m)) && isOnAnyScreen(p + QPoint(0, m))) {
        return false;
    }

    QPoint centre = screen->geometry().center();

    QCursor::setPos(screen, centre);
    // Where the platform refuses to move the pointer (Wayland), pos() still reports where it
    // was, and waiting for a move that lands on the centre would stall the stroke.
    if ((QCursor::pos(screen) - centre).manhattanLength() > 1) {
        return false;
    }
    _warpPending = true;
    _warpTarget = centre;
    _warpSkips = 0;
    _lastPos = QPointF(centre);

    return true;
}

void
ColorTrackballGlobalMode::moveStroke(const QPointF& globalPos,
                                     Qt::KeyboardModifiers modifiers,
                                     bool spontaneous)
{
    if (spontaneous && _warpPending) {
        // Moves queued before the warp still carry pre-warp positions; measured against the
        // centre they would read as a jump back across the screen.
        if ((globalPos.toPoint() - _warpTarget).manhattanLength() > 2) {
            if (++_warpSkips < kMaxWarpSkips) {
                return;
            }
        }
        _warpPending = false;
        _lastPos = globalPos;

        return;
    }

    const double dx = globalPos.x() - _lastPos.x();
    const double dy = globalPos.y() - _lastPos.y();

    _lastPos = globalPos;
    setGear(gear(modifiers.testFlag(Qt::ControlModifier), modifiers.testFlag(Qt::ShiftModifier)));
    if (!_strokeActive || ((dx == 0.0) && (dy == 0.0))) {
        return;
    }

    KnobGuiColor* target = getTarget();

    if (!target) {
        return;
    }
    _stroke.addMove(dx, dy, _gear);

    ChromaEditResult r = _stroke.evaluate(_strokeMode, _strokeRole, _strokeBasis, _rates);

    if (r.valid) {
        _strokeValue = r.value;
    }
    target->getEditSession()->move(_strokeValue, strokeLabel());
    if (spontaneous) {
        warpNearScreenEdge(globalPos);
    }
    Q_EMIT stateChanged();
}

void
ColorTrackballGlobalMode::endStroke()
{
    if (!_strokeActive) {
        return;
    }
    _strokeActive = false;
    _warpPending = false;
    _rememberedHue = _stroke.rememberedHueAfter(_strokeMode, _strokeRole, _strokeBasis, _strokeValue, _rates);

    KnobGuiColor* target = getTarget();

    if (target) {
        target->getEditSession()->end(_strokeValue, tr("Global %1").arg(colorGlobalModeKeyName(_strokeMode)));
    }
    if (_feedback.strokeEnded()) {
        applyFeedback();
    }
    Q_EMIT stateChanged();
}

// For a stroke whose release will never arrive: the grab was taken away, the application lost
// focus, or a modal dialog took over. The stroke so far is kept as its own undo step.
void
ColorTrackballGlobalMode::endStrokeWithoutRelease()
{
    if (_heldButtons == Qt::NoButton) {
        return;
    }
    _heldButtons = Qt::NoButton;
    endStroke();
    releaseStrokeGrab();
}

// Offers the feedback preference to the switch. True when the feedback in effect changed.
bool
ColorTrackballGlobalMode::offerFeedbackSetting()
{
    SettingsPtr settings = appPTR ? appPTR->getCurrentSettings() : SettingsPtr();

    if (!settings) {
        return false;
    }

    return _feedback.offer(settings->getColorTrackballFeedback(), _strokeActive);
}

// Brings the cursor and, while engaged, the HUD in line with the feedback in effect; every
// path in and out of the mode ends here, which is what keeps the override cursor stack
// balanced. exit() has already put the HUD away by the time it gets here.
void
ColorTrackballGlobalMode::applyFeedback()
{
    syncReveal();
    if (!_engaged) {
        return;
    }

    ColorTrackballHud* hud = ensureHud();

    updateHud();
    hud->followCursorTo(QCursor::pos());
    hud->show();
}

// Created with no parent, as a window of its own; deleted with the controller.
ColorTrackballReveal*
ColorTrackballGlobalMode::ensureReveal()
{
    if (!_reveal) {
        _reveal = new ColorTrackballReveal();
        _reveal->hide();
    }

    return _reveal.data();
}

// Brings the override cursors and the mode graphic in line with the feedback, the engaged and
// stroke state, the mode and the live value. Runs on every stateChanged and from
// applyFeedback(), so the stroke's start and end and every exit path pass through it.
void
ColorTrackballGlobalMode::syncReveal()
{
    const ColorGlobalModeFeedbackState state = colorGlobalModeFeedbackState(_feedback.getFeedback(), _engaged, _strokeActive);
    KnobGuiColor* target = getTarget();

    _cursors.sync(state.crosshair, state.blankCursor);
    if (!state.graphicShown || !target) {
        if (_reveal) {
            _reveal->hide();
        }
        _revealFollowing = false;
        if (!_engaged) {
            // The next engage re-reads the knob, in case its colour management changed.
            _revealKnob = 0;
        }

        return;
    }

    ColorTrackballReveal* reveal = ensureReveal();
    const ColorKnobRoleEnum role = target->getColorRole();

    if ((_revealKnob != target) || (_revealRole != role)) {
        ColorAdjustPanel* panel = target->getAdjustPanel();
        ColorWheelWidget* wheel = panel ? panel->getWheel() : 0;

        reveal->setKnob(role, target->getEditBasis(), wheel ? wheel->getDisplayConverter() : ColorTrackballReveal::DisplayConverter(), _rates.wheelRim);
        _revealKnob = target;
        _revealRole = role;
    }
    reveal->setMode(_strokeActive ? _strokeMode : _mode);
    if (_strokeActive) {
        reveal->setStroke(_stroke.getStart(), _strokeValue, _stroke.getTotalDx(), _stroke.getTotalDy(), _rates);
    } else {
        const KnobColorValue value = target->getCurrentColorValue();

        reveal->setStroke(value, value, 0.0, 0.0, _rates);
    }

    // Placed on the press point when a stroke brings it up, and on the pointer whenever it
    // starts following; frozen in between.
    const bool wasShown = reveal->isVisible();

    if (!wasShown || (state.graphicFollowsPointer && !_revealFollowing)) {
        reveal->centreOn(_pointerPos);
    }
    _revealFollowing = state.graphicFollowsPointer;
    if (!wasShown) {
        reveal->show();
        reveal->raise();
    }
}

bool
ColorTrackballGlobalMode::leaveForModalWidget()
{
    if (!QApplication::activeModalWidget()) {
        return false;
    }
    endStrokeWithoutRelease();
    exit();

    return true;
}

namespace {
QPointF
eventGlobalPosition(QObject* watched,
                    QMouseEvent* e)
{
    // A synthetic event built with only a local position gets QCursor::pos() as its global
    // one, which never moves; mapping the local position is right for both kinds.
    if (watched && watched->isWidgetType()) {
        return static_cast<QWidget*>(watched)->mapToGlobal(e->position());
    }

    return e->globalPosition();
}
} // namespace

bool
ColorTrackballGlobalMode::handleMousePress(QObject* watched,
                                           QMouseEvent* e)
{
    const bool first = (_heldButtons == Qt::NoButton);

    _heldButtons |= e->button();
    if (!first) {
        return true;
    }
    _lastPos = eventGlobalPosition(watched, e);
    _pointerPos = _lastPos.toPoint();
    _warpPending = false;
    setGear(gear(e->modifiers().testFlag(Qt::ControlModifier), e->modifiers().testFlag(Qt::ShiftModifier)));
    grabForStroke();
    beginStroke();

    return true;
}

bool
ColorTrackballGlobalMode::handleMouseMove(QObject* watched,
                                          QMouseEvent* e)
{
    _pointerPos = eventGlobalPosition(watched, e).toPoint();
    if (_hud) {
        _hud->followCursorTo(_pointerPos);
    }
    if (_reveal && _revealFollowing && _reveal->isVisible()) {
        _reveal->centreOn(_pointerPos);
    }
    if (_heldButtons == Qt::NoButton) {
        // Buttons held since before the mode was engaged belong to a drag some widget started;
        // it must still see the rest of it, or it is left stuck mid-drag.
        return e->buttons() == Qt::NoButton;
    }
    if (e->spontaneous() && (e->buttons() == Qt::NoButton)) {
        // The release went somewhere this filter never saw it.
        endStrokeWithoutRelease();

        return true;
    }
    moveStroke(eventGlobalPosition(watched, e), e->modifiers(), e->spontaneous());

    return true;
}

bool
ColorTrackballGlobalMode::handleMouseRelease(QMouseEvent* e)
{
    if (!(_heldButtons & e->button())) {
        return false;
    }
    _heldButtons &= ~e->button();
    if (_heldButtons == Qt::NoButton) {
        endStroke();
        releaseStrokeGrab();
    }

    return true;
}

bool
ColorTrackballGlobalMode::handleWheel(QWheelEvent* e)
{
    if (!withoutKeypad(e->modifiers()).testFlag(Qt::ControlModifier)) {
        return true;
    }

    QPoint delta = e->angleDelta();
    int notches = takeColorGlobalModeWheelNotches(&_wheelAccumulator, (delta.y() != 0) ? delta.y() : delta.x());

    // Scrolling up moves up the panel, to the previous knob.
    while (notches != 0) {
        if (!stepKnob(notches > 0 ? -1 : 1)) {
            _wheelAccumulator = 0;
            break;
        }
        notches += (notches > 0) ? -1 : 1;
    }

    return true;
}

bool
colorGlobalModeShortcutOverrideAccepted(int key,
                                        Qt::KeyboardModifiers modifiers,
                                        const QList<QKeySequence>& passThrough)
{
    QKeyCombination combination(withoutKeypad(modifiers), Qt::Key(key));

    for (QList<QKeySequence>::const_iterator it = passThrough.begin(); it != passThrough.end(); ++it) {
        if ((it->count() == 1) && ((*it)[0] == combination)) {
            return false;
        }
    }

    return true;
}

bool
ColorTrackballGlobalMode::isPassThrough(const QKeyEvent* e) const
{
    return !colorGlobalModeShortcutOverrideAccepted(e->key(), e->modifiers(), _passThrough);
}

bool
ColorTrackballGlobalMode::handleShortcutOverride(QKeyEvent* e)
{
    if (isPassThrough(e)) {
        return false;
    }
    // Accepting the override is what keeps an application shortcut on the same key (Undo in
    // the Edit menu, a node graph action) from firing; the key press that follows is then
    // delivered as a plain key event for handleKeyPress to act on.
    e->accept();

    return true;
}

void
ColorTrackballGlobalMode::undoOnTargetPanel(bool redo)
{
    // Straight to the target's own panel stack: the Edit menu's Undo follows whichever stack
    // was last made active, which hovering the node graph or a viewer quietly changes.
    DockablePanel* panel = getTargetPanel();

    if (!panel) {
        return;
    }
    if (redo) {
        panel->onRedoPressed();
    } else {
        panel->onUndoClicked();
    }
    Q_EMIT stateChanged();
}

bool
ColorTrackballGlobalMode::handleKeyPress(QKeyEvent* e)
{
    if (isPassThrough(e)) {
        return false;
    }

    const int key = e->key();
    Qt::KeyboardModifiers mods = colorGlobalModeModifiersAfterKey(key, true, e->modifiers());

    setGear(gear(mods.testFlag(Qt::ControlModifier), mods.testFlag(Qt::ShiftModifier)));
    _swallowedKeys.insert(key);

    ColorGlobalModeKeyAction action = classifyColorGlobalModeKey(key, mods, _strokeActive);

    switch (action.action) {
    case eColorGlobalModeKeySwallow:
        break;
    case eColorGlobalModeKeySetMode:
        setMode(action.mode);
        break;
    case eColorGlobalModeKeyAccept:
        if (!e->isAutoRepeat()) {
            exit();
        }
        break;
    case eColorGlobalModeKeyPreviousKnob:
        stepKnob(-1);
        break;
    case eColorGlobalModeKeyNextKnob:
        stepKnob(1);
        break;
    case eColorGlobalModeKeyUndo:
        undoOnTargetPanel(false);
        break;
    case eColorGlobalModeKeyRedo:
        undoOnTargetPanel(true);
        break;
    }

    return true;
}

bool
ColorTrackballGlobalMode::handleKeyRelease(QKeyEvent* e)
{
    const int key = e->key();
    Qt::KeyboardModifiers mods = colorGlobalModeModifiersAfterKey(key, false, e->modifiers());

    setGear(gear(mods.testFlag(Qt::ControlModifier), mods.testFlag(Qt::ShiftModifier)));
    if (e->isAutoRepeat()) {
        return _swallowedKeys.count(key) > 0;
    }

    // A key pressed before the mode was engaged (the engaging shortcut's own keys) still gets
    // its release, so no widget is left believing it is held.
    return _swallowedKeys.erase(key) > 0;
}

bool
ColorTrackballGlobalMode::eventFilter(QObject* watched,
                                      QEvent* event)
{
    if (!_engaged) {
        return QObject::eventFilter(watched, event);
    }

    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
    case QEvent::KeyPress:
    case QEvent::WindowActivate:
        // Every press would otherwise be swallowed, leaving a modal dialog impossible to use.
        if (leaveForModalWidget()) {
            return QObject::eventFilter(watched, event);
        }
        break;
    default:
        break;
    }

    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
        return handleMousePress(watched, static_cast<QMouseEvent*>(event));

    case QEvent::UngrabMouse:
        if (watched == _grabbed.data()) {
            endStrokeWithoutRelease();
        }
        break;

    case QEvent::ApplicationDeactivate:
        endStrokeWithoutRelease();
        break;

    case QEvent::ApplicationStateChange:
        if (static_cast<QApplicationStateChangeEvent*>(event)->applicationState() != Qt::ApplicationActive) {
            endStrokeWithoutRelease();
        }
        break;

    case QEvent::MouseMove:
        return handleMouseMove(watched, static_cast<QMouseEvent*>(event));

    case QEvent::MouseButtonRelease:
        return handleMouseRelease(static_cast<QMouseEvent*>(event));

    case QEvent::Wheel:
        return handleWheel(static_cast<QWheelEvent*>(event));

    case QEvent::ContextMenu:
        return true;

    case QEvent::TabletPress:
    case QEvent::TabletMove:
    case QEvent::TabletRelease:
        // Left unaccepted, Qt replays a tablet event as a mouse event, which the stroke code
        // then handles like any other pointer.
        event->ignore();

        return true;

    case QEvent::ShortcutOverride:
        return handleShortcutOverride(static_cast<QKeyEvent*>(event));

    case QEvent::KeyPress:
        return handleKeyPress(static_cast<QKeyEvent*>(event));

    case QEvent::KeyRelease:
        return handleKeyRelease(static_cast<QKeyEvent*>(event));

    default:
        break;
    }

    return QObject::eventFilter(watched, event);
} // ColorTrackballGlobalMode::eventFilter

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_ColorTrackballGlobalMode.cpp"
