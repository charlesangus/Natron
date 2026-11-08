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

#ifndef Gui_ColorTrackballTarget_h
#define Gui_ColorTrackballTarget_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QObject>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/EngineFwd.h"

#include "Gui/GuiFwd.h"

class QEvent;
class QWidget;

NATRON_NAMESPACE_ENTER

class GlobalModeTarget;

/**
 * @brief One colour knob candidate for global-mode targeting: its script name (to match the
 * Grade/ColorCorrect fallback knobs by name) and whether it is currently a legal target
 * (not secret, recursively, and not disabled on its first dimension).
 **/
struct ColorKnobInfo {
    std::string scriptName;
    bool enabled;
    bool visible;
};

/**
 * @brief The dynamic property name KnobGuiColor sets, with a QPointer to itself as the value,
 * on its own per-knob container and adjustment panel widgets. ColorKnobTouchTracker's app-wide filter
 * walks a touched widget's parent chain looking for this property to find which colour knob,
 * if any, the touch belongs to.
 **/
const char* colorKnobTouchTagPropertyName();

/**
 * @brief The same for every KnobGuiValue's row, with a QPointer<KnobGui> as the value. A
 * colour knob carries both; its colour tag wins.
 **/
const char* globalModeKnobTouchTagPropertyName();

/**
 * @brief The fallback chain from the Design notes: the last-touched knob if it is still a
 * legal target, else the node's canonical gain knob by plugin ID and script name if it is
 * legal, else the first legal colour knob, else none (-1). Legal means visible and enabled. Pure and Qt-free so it can be exercised without a
 * Gui, a node or any widget.
 **/
int pickTargetIndex(const std::string& pluginID,
                    const std::vector<ColorKnobInfo>& knobs,
                    int lastTouchedIndex);

/**
 * @brief The panel's colour knobs in declaration order, limited to the ones that can host the
 * trackball (a below-row adjustment panel): simplified and viewer colour knobs keep the old
 * popup and are never a target.
 **/
std::vector<KnobGuiColor*> orderedColorKnobs(DockablePanel* panel);

/**
 * @brief The first docked node panel in Gui::getVisiblePanels(), which keeps the front entry as
 * the one last brought to the top of the properties bin.
 **/
NodeSettingsPanel* topmostNodePanel(Gui* gui);

/**
 * @brief Tracks, per properties-bin panel, which colour knob was last touched there: a wheel or
 * swatch gesture's Start, or a press, scroll or focus landing anywhere inside a tagged
 * container. One tracker is lazily created per Gui and outlives individual node panels, so
 * switching back to a node recalls what was last touched on it.
 **/
class ColorKnobTouchTracker
    : public QObject {
public:
    /**
     * @brief The tracker for this Gui, created on first use and kept alive for the Gui's
     * lifetime without the Gui itself needing to hold a pointer to it.
     **/
    static ColorKnobTouchTracker* instanceFor(Gui* gui);

    Gui* getGui() const
    {
        return _gui;
    }

    /**
     * @brief Records knobGui as the panel's last-touched knob of any kind, and as its
     * last-touched colour knob when it is one.
     **/
    void touch(KnobGui* knobGui);

    /**
     * @brief The last-touched colour knob recorded for this panel, or null. A recorded knob
     * that no longer exists is dropped rather than returned.
     **/
    KnobGui* lastTouched(KnobGuiContainerI* panel);

    /**
     * @brief As lastTouched(), for a knob of any kind.
     **/
    KnobGui* lastTouchedAnyKind(KnobGuiContainerI* panel);

    virtual bool eventFilter(QObject* watched, QEvent* event) OVERRIDE;

    virtual ~ColorKnobTouchTracker();

private:
    explicit ColorKnobTouchTracker(Gui* gui);

    Gui* _gui;
    std::map<KnobGuiContainerI*, KnobGuiWPtr> _lastTouched;
    std::map<KnobGuiContainerI*, KnobGuiWPtr> _lastTouchedAnyKind;
};

/**
 * @brief The current global-mode target on this Gui: the topmost node panel's last-touched
 * colour knob, falling back through pickTargetIndex, or null when the topmost node has no
 * usable colour knob.
 **/
KnobGuiColor* resolveColorTrackballTarget(Gui* gui);

/**
 * @brief Pages the panel to knob, opens any closed group holding it, and scrolls widget into
 * view.
 **/
void revealKnobInPanel(DockablePanel* panel, const KnobIPtr& knob, QWidget* widget);

/**
 * @brief Whether knobGui can take global mode: a colour knob with an adjustment panel, or a
 * numeric value knob of an eligible kind, in either case visible and enabled.
 **/
bool isGlobalModeKnob(KnobGui* knobGui);

/**
 * @brief Every knob in the panel that can take global mode, colour or numeric, in declaration
 * order, which is the order Ctrl+Up and Ctrl+Down step through. When samePageAs is given, only the
 * knobs on its settings page are returned, so stepping stays on the page of the engaged knob.
 **/
std::vector<KnobGui*> orderedGlobalModeKnobs(DockablePanel* panel, KnobGui* samePageAs = 0);

/**
 * @brief The entries of knobs that sit on the same top-level settings page as reference, in
 * order. Knobs with no page match only a reference with no page.
 **/
std::vector<KnobGui*> restrictToPageOf(const std::vector<KnobGui*>& knobs, KnobGui* reference);

/**
 * @brief The target that drives knobGui: the colour target for a colour knob, the transform
 * group with knobGui's member active for one of its node's transform interact knobs, or a lone
 * value target. Null for anything else.
 **/
std::shared_ptr<GlobalModeTarget> makeGlobalModeTarget(KnobGui* knobGui);

/**
 * @brief The global-mode knob whose row holds widget, found through the touch tags, or null.
 **/
KnobGui* globalModeKnobAt(QWidget* widget);

/**
 * @brief Records knobGui as last touched on its Gui's tracker.
 **/
void touchGlobalModeKnob(KnobGui* knobGui);

/**
 * @brief The knob Ctrl+E engages: the eligible knob under the pointer when it sits in a node
 * panel, else the topmost node panel's last-touched eligible knob of any kind, else the colour
 * fallback of resolveColorTrackballTarget, else the panel's first eligible knob.
 **/
KnobGui* resolveGlobalModeKnob(Gui* gui, QWidget* hovered);

NATRON_NAMESPACE_EXIT

#endif // Gui_ColorTrackballTarget_h
