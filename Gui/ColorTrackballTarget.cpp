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

#include "ColorTrackballTarget.h"

#include <cstddef>
#include <list>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QCoreApplication>
#include <QEvent>
#include <QVariant>
#include <QWidget>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/Knob.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/ColorCorrect.h"
#include "Engine/Nodes/Color/Grade.h"

#include "Gui/DockablePanel.h"
#include "Gui/Gui.h"
#include "Gui/KnobGuiColor.h"
#include "Gui/KnobGuiContainerHelper.h"
#include "Gui/NodeGui.h"
#include "Gui/NodeSettingsPanel.h"

NATRON_NAMESPACE_ENTER

const char*
colorKnobTouchTagPropertyName()
{
    return "_natronColorKnobTouchTag";
}

namespace {
int
findKnobIndex(const std::vector<ColorKnobInfo>& knobs,
              const std::string& scriptName)
{
    for (std::size_t i = 0; i < knobs.size(); ++i) {
        if (knobs[i].scriptName == scriptName) {
            return (int)i;
        }
    }

    return -1;
}
} // namespace

int
pickTargetIndex(const std::string& pluginID,
                const std::vector<ColorKnobInfo>& knobs,
                int lastTouchedIndex)
{
    if ((lastTouchedIndex >= 0) && (lastTouchedIndex < (int)knobs.size())) {
        const ColorKnobInfo& touched = knobs[lastTouchedIndex];

        if (touched.visible && touched.enabled) {
            return lastTouchedIndex;
        }
    }

    if (pluginID == PLUGINID_NATRON_GRADE) {
        int idx = findKnobIndex(knobs, kGradeParamWhite);

        if ((idx >= 0) && knobs[idx].visible) {
            return idx;
        }
    } else if (pluginID == PLUGINID_NATRON_COLORCORRECT) {
        int idx = findKnobIndex(knobs, std::string(kColorCorrectGroupMaster) + kColorCorrectParamGain);

        if ((idx >= 0) && knobs[idx].visible) {
            return idx;
        }
    }

    for (std::size_t i = 0; i < knobs.size(); ++i) {
        if (knobs[i].visible) {
            return (int)i;
        }
    }

    return -1;
} // pickTargetIndex

std::vector<KnobGuiColor*>
orderedColorKnobs(DockablePanel* panel)
{
    std::vector<KnobGuiColor*> result;

    if (!panel) {
        return result;
    }

    const KnobsGuiMapping& mapping = panel->getKnobsMapping();

    for (KnobsGuiMapping::const_iterator it = mapping.begin(); it != mapping.end(); ++it) {
        KnobGuiColor* colorGui = dynamic_cast<KnobGuiColor*>(it->second.get());

        if (!colorGui || !colorGui->getAdjustPanel()) {
            // No adjustment panel means a simplified or viewer colour knob, which keeps the
            // old popup and is never a trackball target.
            continue;
        }
        result.push_back(colorGui);
    }

    return result;
}

NodeSettingsPanel*
topmostNodePanel(Gui* gui)
{
    if (!gui) {
        return 0;
    }

    const std::list<DockablePanel*>& panels = gui->getVisiblePanels();

    for (std::list<DockablePanel*>::const_iterator it = panels.begin(); it != panels.end(); ++it) {
        if ((*it)->isFloating()) {
            continue;
        }

        NodeSettingsPanel* nodePanel = dynamic_cast<NodeSettingsPanel*>(*it);

        if (nodePanel) {
            return nodePanel;
        }
    }

    return 0;
}

ColorKnobTouchTracker::ColorKnobTouchTracker(Gui* gui)
    : QObject(0)
    , _gui(gui)
    , _lastTouched()
{
    QCoreApplication* app = QCoreApplication::instance();

    if (app) {
        app->installEventFilter(this);
    }
}

ColorKnobTouchTracker::~ColorKnobTouchTracker()
{
    QCoreApplication* app = QCoreApplication::instance();

    if (app) {
        app->removeEventFilter(this);
    }
}

namespace {
std::map<Gui*, ColorKnobTouchTracker*>&
colorKnobTouchTrackers()
{
    static std::map<Gui*, ColorKnobTouchTracker*> trackers;

    return trackers;
}
} // namespace

ColorKnobTouchTracker*
ColorKnobTouchTracker::instanceFor(Gui* gui)
{
    if (!gui) {
        return 0;
    }

    std::map<Gui*, ColorKnobTouchTracker*>& trackers = colorKnobTouchTrackers();
    std::map<Gui*, ColorKnobTouchTracker*>::iterator it = trackers.find(gui);

    if (it != trackers.end()) {
        return it->second;
    }

    ColorKnobTouchTracker* tracker = new ColorKnobTouchTracker(gui);

    trackers[gui] = tracker;
    // The map is keyed on the Gui's address, so the entry must go when that address can be
    // reused; connecting to its own destroyed() signal needs no change to Gui itself.
    QObject::connect(gui, &QObject::destroyed, [gui]() {
        std::map<Gui*, ColorKnobTouchTracker*>& trackers = colorKnobTouchTrackers();
        std::map<Gui*, ColorKnobTouchTracker*>::iterator it = trackers.find(gui);

        if (it != trackers.end()) {
            delete it->second;
            trackers.erase(it);
        }
    });

    return tracker;
}

void
ColorKnobTouchTracker::touch(KnobGuiColor* knobGui)
{
    if (!knobGui) {
        return;
    }

    KnobGuiContainerI* panel = knobGui->getContainer();

    if (!panel) {
        return;
    }

    _lastTouched[panel] = knobGui->shared_from_this();
}

KnobGui*
ColorKnobTouchTracker::lastTouched(KnobGuiContainerI* panel)
{
    std::map<KnobGuiContainerI*, KnobGuiWPtr>::iterator it = _lastTouched.find(panel);

    if (it == _lastTouched.end()) {
        return 0;
    }

    KnobGuiPtr locked = it->second.lock();

    if (!locked) {
        _lastTouched.erase(it);

        return 0;
    }

    return locked.get();
}

bool
ColorKnobTouchTracker::eventFilter(QObject* watched,
                                   QEvent* event)
{
    QEvent::Type t = event->type();

    if ((t == QEvent::MouseButtonPress) || (t == QEvent::Wheel) || (t == QEvent::FocusIn)) {
        KnobGuiColor* owner = 0;

        for (QWidget* w = qobject_cast<QWidget*>(watched); w && !owner; w = w->parentWidget()) {
            QVariant tag = w->property(colorKnobTouchTagPropertyName());

            if (tag.isValid()) {
                owner = static_cast<KnobGuiColor*>(tag.value<void*>());
            }
        }
        if (owner) {
            touch(owner);
        }
    }

    return QObject::eventFilter(watched, event);
}

KnobGuiColor*
resolveColorTrackballTarget(Gui* gui)
{
    NodeSettingsPanel* panel = topmostNodePanel(gui);

    if (!panel) {
        return 0;
    }

    std::vector<KnobGuiColor*> knobGuis = orderedColorKnobs(panel);

    if (knobGuis.empty()) {
        return 0;
    }

    std::vector<ColorKnobInfo> infos;

    infos.reserve(knobGuis.size());
    for (std::size_t i = 0; i < knobGuis.size(); ++i) {
        KnobIPtr knob = knobGuis[i]->getKnob();
        ColorKnobInfo info;

        info.scriptName = knob ? knob->getName() : std::string();
        info.enabled = knob && knob->isEnabled(0);
        info.visible = knobGuis[i] && !knobGuis[i]->isSecretRecursive();
        infos.push_back(info);
    }

    int lastTouchedIndex = -1;
    ColorKnobTouchTracker* tracker = ColorKnobTouchTracker::instanceFor(gui);

    if (tracker) {
        KnobGui* touched = tracker->lastTouched(panel);

        for (std::size_t i = 0; i < knobGuis.size(); ++i) {
            if (static_cast<KnobGui*>(knobGuis[i]) == touched) {
                lastTouchedIndex = (int)i;
                break;
            }
        }
    }

    std::string pluginID;
    NodeGuiPtr nodeGui = panel->getNode();
    NodePtr node = nodeGui ? nodeGui->getNode() : NodePtr();

    if (node) {
        pluginID = node->getPluginID();
    }

    int idx = pickTargetIndex(pluginID, infos, lastTouchedIndex);

    return (idx >= 0) ? knobGuis[idx] : 0;
} // resolveColorTrackballTarget

NATRON_NAMESPACE_EXIT
