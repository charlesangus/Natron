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

#ifndef Gui_ColorTrackballGlobalMode_h
#define Gui_ColorTrackballGlobalMode_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>
#include <set>
#include <vector>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QKeySequence>
#include <QList>
#include <QMetaObject>
#include <QObject>
#include <QPoint>
#include <QPointF>
#include <QPointer>
#include <QString>
#include <QWidget>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/ColorWheelMath.h"
#include "Engine/Settings.h"

#include "Gui/ColorTrackballTarget.h"
#include "Gui/GuiFwd.h"

class QEvent;
class QKeyEvent;
class QMouseEvent;
class QWheelEvent;

NATRON_NAMESPACE_ENTER

class ColorTrackballHudBlock;
struct ColorTrackballHudState;
class ColorTrackballReveal;
class WindowHighlightFrame;

/**
 * @brief The bool dynamic property set on the global-mode target's row container while the
 * mode is engaged on it, for the application stylesheet to highlight the row by.
 **/
const char* colorTrackballTargetPropertyName();

/**
 * @brief What a key press does while the global trackball mode is engaged. Everything that is
 * not one of the mode's own keys is swallowed, so a stray key never reaches the widget that
 * happens to have focus (the node graph, a viewer) and acts on it behind the mode's back.
 **/
enum ColorGlobalModeKeyActionEnum {
    eColorGlobalModeKeySwallow,
    eColorGlobalModeKeySetMode,
    eColorGlobalModeKeyAccept,
    eColorGlobalModeKeyPreviousKnob,
    eColorGlobalModeKeyNextKnob,
    eColorGlobalModeKeyUndo,
    eColorGlobalModeKeyRedo
};

struct ColorGlobalModeKeyAction {
    ColorGlobalModeKeyActionEnum action = eColorGlobalModeKeySwallow;
    ColorGlobalModeEnum mode = eColorGlobalModeC;
};

/**
 * @brief Maps one key press to its global-mode action. Undo and redo are only honoured
 * between strokes: mid-stroke the knob holds a value the stroke has not committed yet, and
 * undoing under it would leave the stroke's next move pushing from a stale start.
 **/
ColorGlobalModeKeyAction classifyColorGlobalModeKey(int key, Qt::KeyboardModifiers modifiers, bool strokeActive);

/**
 * @brief The modifiers held once this key event has taken effect. Platforms disagree on
 * whether a modifier key's own press or release event already reports the new state, so the
 * key's own flag is forced on (press) or off (release).
 **/
Qt::KeyboardModifiers colorGlobalModeModifiersAfterKey(int key, bool press, Qt::KeyboardModifiers reported);

/**
 * @brief Whether a ShortcutOverride event for key/modifiers is claimed by the global trackball
 * mode's event filter rather than left for the application's own shortcut on the same key,
 * given the sequences passThrough currently lets through. True for every key while engaged,
 * including bare S, which is what keeps the Project Settings shortcut from firing while the
 * mode is on; false only for a sequence named in passThrough, such as the shortcut that both
 * engages and leaves the mode.
 **/
bool colorGlobalModeShortcutOverrideAccepted(int key, Qt::KeyboardModifiers modifiers, const QList<QKeySequence>& passThrough);

/**
 * @brief The next legal knob (visible and enabled) from current in direction (+1 = down the
 * panel, -1 = up), or -1 when there is none before the end of the list. There is no wrap.
 **/
int stepColorKnobIndex(const std::vector<ColorKnobInfo>& knobs, int current, int direction);

/**
 * @brief Adds one wheel event's angle delta to accumulator and returns how many whole notches
 * (120 units each) it now holds, signed, leaving the remainder in accumulator so a
 * high-resolution touchpad steps once per notch's worth of scrolling rather than per event.
 * A delta against the accumulated direction discards the remainder first.
 **/
int takeColorGlobalModeWheelNotches(int* accumulator, int angleDelta);

/**
 * @brief Whether global mode swaps the pointer for a crosshair from engage to exit.
 **/
bool colorGlobalModeFeedbackShowsCrosshair(Settings::ColorTrackballFeedbackEnum feedback);

/**
 * @brief Whether the mode graphic stands in for the pointer from engage to exit, rather than
 * coming up at the press point for the stroke's length.
 **/
bool colorGlobalModeFeedbackGraphicIsCursor(Settings::ColorTrackballFeedbackEnum feedback);

/**
 * @brief What global mode shows for a feedback at one moment. The crosshair stays pushed under
 * a blank cursor, which hides it while the mode graphic is the feedback; the graphic either
 * follows the pointer or stays where it was put.
 **/
struct ColorGlobalModeFeedbackState {
    bool crosshair = false;
    bool blankCursor = false;
    bool graphicShown = false;
    bool graphicFollowsPointer = false;
    /// The hint block has a window of its own by the pointer while engaged with no graphic up;
    /// with one up it sits under the graphic's disc.
    bool hudBlockFollowsPointer = false;
};

/**
 * @brief Moves the pointer to target, as a released stroke does to return it to where the
 * stroke began. False where the platform leaves it where it was (Wayland).
 **/
bool colorGlobalModeReturnPointer(const QPoint& target);

ColorGlobalModeFeedbackState colorGlobalModeFeedbackState(Settings::ColorTrackballFeedbackEnum feedback, bool engaged, bool strokeActive);

/**
 * @brief The feedback global mode works with. A change arriving mid-stroke is held until the
 * stroke ends, so one stroke never changes how it is shown halfway through.
 **/
class ColorGlobalModeFeedbackSwitch {
public:
    ColorGlobalModeFeedbackSwitch();

    /**
     * @brief Takes latest: at once outside a stroke, at strokeEnded() during one. True when the
     * value in effect changed.
     **/
    bool offer(Settings::ColorTrackballFeedbackEnum latest, bool strokeActive);

    /**
     * @brief Puts a held change into effect. True when the value in effect changed.
     **/
    bool strokeEnded();

    Settings::ColorTrackballFeedbackEnum getFeedback() const
    {
        return _feedback;
    }

private:
    Settings::ColorTrackballFeedbackEnum _feedback;
    bool _pending;
    Settings::ColorTrackballFeedbackEnum _pendingFeedback;
};

/**
 * @brief One entry on QApplication's override cursor stack, pushed the first time it is held
 * and popped the first time it is let go, so any number of exit paths asking to let go restore
 * the cursor exactly once. Let go on destruction.
 **/
class ColorGlobalModeOverrideCursor {
public:
    explicit ColorGlobalModeOverrideCursor(Qt::CursorShape shape);
    ~ColorGlobalModeOverrideCursor();

    void setHeld(bool held);

    bool isHeld() const
    {
        return _held;
    }

private:
    Qt::CursorShape _shape;
    bool _held;
};

/**
 * @brief Global mode's two override cursors, the crosshair and the blank one above it, kept in
 * that order on QApplication's stack whatever order they are asked for in. Both are let go on
 * destruction.
 **/
class ColorGlobalModeCursors {
public:
    ColorGlobalModeCursors();
    ~ColorGlobalModeCursors();

    void sync(bool crosshair, bool blank);

    bool isCrosshairHeld() const
    {
        return _crosshair.isHeld();
    }

    bool isBlankHeld() const
    {
        return _blank.isHeld();
    }

private:
    ColorGlobalModeOverrideCursor _crosshair;
    ColorGlobalModeOverrideCursor _blank;
};

/**
 * @brief One click-drag stroke's accumulated pointer travel, applied to the value the stroke
 * started from rather than step by step, so a long stroke made of many small moves lands
 * exactly where one big move would and a drag back returns to the start. Each move's pixels
 * are scaled by the gear held during that move before being added, so a modifier pressed or
 * released mid-stroke changes the speed from the next move without rescaling the travel so far.
 **/
class ColorGlobalModeStroke {
public:
    ColorGlobalModeStroke();

    /**
     * @brief Starts accumulating from start. rememberedHueDegrees is the hue saturation grows
     * along if start is neutral and has no hue of its own.
     **/
    void begin(const KnobColorValue& start, double rememberedHueDegrees);
    void addMove(double dx, double dy, double gearFactor);

    ChromaEditResult evaluate(ColorGlobalModeEnum mode, ColorKnobRoleEnum role, const LumaBasis& basis, const TrackballRates& rates) const;

    /**
     * @brief The hue to remember once the stroke has reached value: value's own hue when it
     * has one, or, for an S stroke still on neutral, the start hue advanced by the stroke's hue
     * travel, so the next stroke grows saturation the way this one was heading.
     **/
    double rememberedHueAfter(ColorGlobalModeEnum mode, ColorKnobRoleEnum role, const LumaBasis& basis, const KnobColorValue& value, const TrackballRates& rates) const;

    const KnobColorValue& getStart() const
    {
        return _start;
    }

    double getTotalDx() const
    {
        return _dx;
    }

    double getTotalDy() const
    {
        return _dy;
    }

private:
    double hueTravelDegrees(ColorGlobalModeEnum mode, const TrackballRates& rates) const;

    KnobColorValue _start;
    double _startHue;
    double _dx;
    double _dy;
};

/**
 * @brief What is left of a right click after it has exited the mode: its release and the
 * context-menu request, which Qt raises on the press or the release depending on the platform
 * and which would otherwise open a menu under the pointer. Each is eaten once; a press or key
 * press that is not part of the click ends the wait.
 **/
class ColorGlobalModeRightClickTail {
public:
    ColorGlobalModeRightClickTail()
        : _release(false)
        , _menu(false)
    {
    }

    void arm()
    {
        _release = true;
        _menu = true;
    }

    bool isArmed() const
    {
        return _release || _menu;
    }

    /**
     * @brief True when the event belongs to the click and must be eaten.
     **/
    bool consume(QEvent::Type type,
                 Qt::MouseButton button);

private:
    bool _release;
    bool _menu;
};

/**
 * @brief The global trackball mode: once engaged on a colour knob, a click-drag anywhere in
 * the application's windows adjusts that knob (C = a trackball translate of hue and
 * saturation together, S = hue on x and saturation on y, T = temperature and tint, V = value),
 * each stroke being one undo step. Esc and Enter accept and exit, Ctrl+Up/Down and Ctrl+scroll
 * move to the previous or next colour knob on the same panel, and Ctrl+Z / Ctrl+Shift+Z /
 * Ctrl+Y undo and redo strokes between them on the knob's own panel. A modal dialog taking
 * over, or the application losing focus, ends any stroke; a modal dialog also leaves the mode.
 *
 * One controller exists per Gui, created when the Gui builds its actions and deleted with the
 * Gui. Its app-wide event filter is installed only while it is engaged.
 **/
class ColorTrackballGlobalMode
    : public QObject {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    static ColorTrackballGlobalMode* instanceFor(Gui* gui);

    virtual ~ColorTrackballGlobalMode() OVERRIDE;

    Gui* getGui() const
    {
        return _gui;
    }

    /**
     * @brief Engages on target, or moves an engaged mode onto it. A fresh engage starts in
     * mode C whatever mode the previous invocation left; moving an already-engaged mode onto
     * another knob keeps the current mode. False, leaving the mode as it was, when target is
     * null or has no adjustment panel (simplified and viewer knobs).
     **/
    bool engage(const KnobGuiColorPtr& target);

    /**
     * @brief Accepts whatever the strokes did and leaves the mode. There is no cancel: every
     * stroke is already its own undo step.
     **/
    void exit();

    bool isEngaged() const
    {
        return _engaged;
    }

    KnobGuiColor* getTarget() const;

    /**
     * @brief The target's 1-based position n among its panel's legal colour knobs and their
     * count N, for an "n/N" readout; both 0 when not engaged.
     **/
    void getTargetPosition(int* n, int* count) const;

    /**
     * @brief The current mode. Reset to C by every fresh engage(), so it does not carry over
     * between invocations or nodes; only retargeting an engaged mode keeps it.
     **/
    ColorGlobalModeEnum getMode() const
    {
        return _mode;
    }

    void setMode(ColorGlobalModeEnum mode);

    /**
     * @brief The speed multiplier of the modifiers held at the latest key or pointer event.
     **/
    double getGear() const
    {
        return _gear;
    }

    bool isStrokeActive() const
    {
        return _strokeActive;
    }

    /**
     * @brief The value the user is looking at: the stroke's latest value mid-stroke, else the
     * target knob's current value.
     **/
    KnobColorValue getLiveValue() const;

    /**
     * @brief The widget that grabs the mouse for the length of a stroke, so the stroke keeps
     * receiving moves wherever the pointer goes. Defaults to the Gui's main window when unset
     * or hidden.
     **/
    void setGrabWidget(QWidget* widget);

    /**
     * @brief Key sequences let through to the application untouched while engaged, so that,
     * for instance, the shortcut that engages the mode can also leave it.
     **/
    void setPassThroughShortcuts(const QList<QKeySequence>& shortcuts);

    /**
     * @brief Moves to the previous (-1) or next (+1) legal colour knob on the target's panel,
     * ending any running stroke first. False, with knobListEndReached emitted, at either end.
     **/
    bool stepKnob(int direction);

    virtual bool eventFilter(QObject* watched, QEvent* event) OVERRIDE;

Q_SIGNALS:

    void engagedChanged(bool engaged);

    /**
     * @brief Anything a readout of the mode shows has changed: the target, mode, gear, stroke
     * state or the live value (including an undo or any other change to the knob).
     **/
    void stateChanged();

    /**
     * @brief A knob switch hit the top (-1) or bottom (+1) of the panel's colour knobs.
     **/
    void knobListEndReached(int direction);

private:
    explicit ColorTrackballGlobalMode(Gui* gui);

    void setTarget(const KnobGuiColorPtr& target);
    void clearTarget();
    void onTargetGone();
    DockablePanel* getTargetPanel() const;
    void revealTarget();
    QString strokeLabel() const;
    void setGear(double gearFactor);

    ColorTrackballHudBlock* ensureHudBlock();
    ColorTrackballReveal* ensureReveal();
    void syncReveal();
    void syncHudBlock(const ColorGlobalModeFeedbackState& state);
    void showChrome();
    void hideChrome();
    ColorTrackballHudState hudState() const;
    void updateHud();

    void beginStroke();
    void moveStroke(const QPointF& globalPos, Qt::KeyboardModifiers modifiers, bool spontaneous);
    void endStroke();
    void endStrokeWithoutRelease();
    bool warpNearScreenEdge(const QPointF& globalPos);
    void grabForStroke();
    void releaseStrokeGrab();

    bool handleMousePress(QObject* watched, QMouseEvent* e);
    bool handleMouseMove(QObject* watched, QMouseEvent* e);
    bool handleMouseRelease(QMouseEvent* e);
    bool handleWheel(QWheelEvent* e);
    bool leaveForModalWidget();
    bool isPassThrough(const QKeyEvent* e) const;
    bool handleShortcutOverride(QKeyEvent* e);
    bool handleKeyPress(QKeyEvent* e);
    bool handleKeyRelease(QKeyEvent* e);
    void undoOnTargetPanel(bool redo);
    bool offerFeedbackSetting();
    void applyFeedback();

    Gui* _gui;
    bool _engaged;
    std::weak_ptr<KnobGuiColor> _target;
    QPointer<KnobGuiColor> _targetObject;
    std::vector<QMetaObject::Connection> _targetConnections;
    ColorGlobalModeEnum _mode;
    double _gear;
    TrackballRates _rates;
    double _rememberedHue;

    bool _strokeActive;
    ColorGlobalModeStroke _stroke;
    KnobColorValue _strokeValue;
    ColorKnobRoleEnum _strokeRole;
    LumaBasis _strokeBasis;
    ColorGlobalModeEnum _strokeMode;

    Qt::MouseButtons _heldButtons;
    QPointF _lastPos;
    bool _warpPending;
    QPoint _warpTarget;
    int _warpSkips;
    QPointer<QWidget> _grabWidget;
    QPointer<QWidget> _grabbed;

    int _wheelAccumulator;
    std::set<int> _swallowedKeys;
    ColorGlobalModeRightClickTail _rightClickTail;
    QList<QKeySequence> _passThrough;

    ColorGlobalModeFeedbackSwitch _feedback;
    ColorGlobalModeCursors _cursors;
    QPoint _pointerPos;
    QPoint _strokeStartPos;

    QPointer<ColorTrackballHudBlock> _hudBlock;
    QPointer<ColorTrackballReveal> _reveal;
    bool _revealFollowing;
    const void* _revealKnob;
    ColorKnobRoleEnum _revealRole;
    std::vector<QPointer<WindowHighlightFrame>> _highlightFrames;
    QString _endOfListMessage;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_ColorTrackballGlobalMode_h
