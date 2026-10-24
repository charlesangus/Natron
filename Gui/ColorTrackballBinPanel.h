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

#ifndef Gui_ColorTrackballBinPanel_h
#define Gui_ColorTrackballBinPanel_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>
#include <vector>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QMetaObject>
#include <QPointer>
#include <QString>
#include <QWidget>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/ColorWheelMath.h"

#include "Gui/ColorAdjustPanel.h"
#include "Gui/GuiFwd.h"

class QEvent;
class QLabel;
class QToolButton;
class QVBoxLayout;

NATRON_NAMESPACE_ENTER

/**
 * @brief The bin-top panel's title for a knob: "node.knob", or the knob alone when it has no
 * node behind it.
 **/
QString colorTrackballBinPanelTitle(const QString& nodeLabel, const QString& knobName);

/**
 * @brief The full-width trackball at the top of the properties bin, used in the BinTop
 * placement: a title, a close button and one ColorAdjustPanel editing a single bound colour
 * knob through that knob's own edit session, so its gestures and undo steps are the in-knob
 * panel's. It is shown exactly while a knob is bound, and only one knob is bound at a time: the
 * bound knob's ▾ is checked and its channel lines unfold inline, every other knob's ▾ is not.
 *
 * The knob is held through a guard, never a bare pointer: the knob GUI dying, or its node
 * panel being closed or destroyed, unbinds and hides the panel. While it is shown, a change of
 * the topmost node in the bin rebinds it to that node's global-mode target (moving an engaged
 * global mode along), or hides it when that node has no colour knob to edit.
 *
 * The global-mode HUD sits between the title and the adjust panel while the BinTop placement
 * hosts it (see attachHud()).
 **/
class ColorTrackballBinPanel
    : public QWidget {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    /**
     * @brief The panel registered for gui. With a Gui, one is created on first use at the top
     * of its properties bin; with none, only a panel constructed for no Gui is found.
     **/
    static ColorTrackballBinPanel* instanceFor(Gui* gui);

    /**
     * @brief Registers itself as gui's panel, hidden and unbound.
     **/
    explicit ColorTrackballBinPanel(Gui* gui, QWidget* parent = nullptr);

    virtual ~ColorTrackballBinPanel() OVERRIDE;

    Gui* getGui() const
    {
        return _gui;
    }

    /**
     * @brief Binds knobGui and shows the panel, taking the binding off whichever knob held it
     * (its ▾ is unchecked) and checking knobGui's ▾. False, changing nothing, for a knob with no
     * adjustment panel (simplified and viewer knobs).
     **/
    bool bindTo(KnobGuiColor* knobGui);

    /**
     * @brief Lets go of the bound knob and hides. In the BinTop placement the knob's ▾ is
     * unchecked too, and global mode engaged on it is left; in InKnob the ▾ stays checked so
     * the knob shows its own panel instead.
     **/
    void unbind();

    KnobGuiColor* getBoundKnob() const;

    bool isBound() const
    {
        return getBoundKnob() != 0;
    }

    /**
     * @brief What the panel does when the topmost node in the bin changes and target is that
     * node's global-mode target (null when it has none): nothing while unbound, rebind to
     * target, or unbind and hide when there is none. An engaged global mode moves to target,
     * or is left with the panel when there is none.
     **/
    void followTopNode(KnobGuiColor* target);

    /**
     * @brief Inserts hud right below the title, as attachColorGlobalModeHud() does in a knob's
     * column.
     **/
    void attachHud(QWidget* hud);

    ColorAdjustPanel* getAdjustPanel() const
    {
        return _panel;
    }

    QString getTitle() const;

    QToolButton* getCloseButton() const
    {
        return _closeButton;
    }

    virtual bool eventFilter(QObject* watched, QEvent* event) OVERRIDE;

Q_SIGNALS:

    void boundKnobChanged();

private Q_SLOTS:

    void onCloseClicked();
    void onPanelValuesEdited(KnobColorValue value, ColorAdjustPanel::GesturePhaseEnum phase, QString undoLabel);

private:
    void releaseBinding();

    /**
     * @brief The unbind for a knob whose GUI, adjust panel or node panel is being destroyed:
     * a QWidget's destroyed() comes before its children go and before QPointers to it clear,
     * so the knob may still look alive while its widgets are half gone. Nothing of it is
     * touched; its edit session and global mode see the same destruction themselves.
     **/
    void dropDyingBinding();
    void refreshFromKnob();
    void refreshEditable();
    void refreshTitle();
    void watchTopNode();
    void queueTopNodeCheck();
    void checkTopNode();

    Gui* _gui;
    QWidget* _header;
    QLabel* _title;
    QToolButton* _closeButton;
    QVBoxLayout* _layout;
    ColorAdjustPanel* _panel;
    std::weak_ptr<KnobGuiColor> _knob;
    QPointer<KnobGuiColor> _knobObject;
    std::vector<QMetaObject::Connection> _knobConnections;
    QPointer<QObject> _topPanel;
    QPointer<QWidget> _watchedContainer;
    bool _topCheckQueued;
    bool _topCheckPending;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_ColorTrackballBinPanel_h
