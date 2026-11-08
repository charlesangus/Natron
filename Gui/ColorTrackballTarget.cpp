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
#include <memory>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QCoreApplication>
#include <QEvent>
#include <QPointer>
#include <QScrollArea>
#include <QVariant>
#include <QWidget>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/EffectInstance.h"
#include "Engine/GlobalModeKind.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/ColorCorrect.h"
#include "Engine/Nodes/Color/Grade.h"

#include "Gui/ColorTrackballGlobalMode.h"
#include "Gui/DockablePanel.h"
#include "Gui/Gui.h"
#include "Gui/KnobGuiColor.h"
#include "Gui/KnobGuiContainerHelper.h"
#include "Gui/KnobGuiValue.h"
#include "Gui/NodeGui.h"
#include "Gui/NodeSettingsPanel.h"
#include "Gui/TransformGroupGlobalModeTarget.h"
#include "Gui/ValueKnobGlobalModeTarget.h"

NATRON_NAMESPACE_ENTER

const char*
colorKnobTouchTagPropertyName()
{
    return "_natronColorKnobTouchTag";
}

const char*
globalModeKnobTouchTagPropertyName()
{
    return "_natronGlobalModeKnobTouchTag";
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

        if ((idx >= 0) && knobs[idx].visible && knobs[idx].enabled) {
            return idx;
        }
    } else if (pluginID == PLUGINID_NATRON_COLORCORRECT) {
        int idx = findKnobIndex(knobs, std::string(kColorCorrectGroupMaster) + kColorCorrectParamGain);

        if ((idx >= 0) && knobs[idx].visible && knobs[idx].enabled) {
            return idx;
        }
    }

    for (std::size_t i = 0; i < knobs.size(); ++i) {
        if (knobs[i].visible && knobs[i].enabled) {
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

namespace {
void
recordTouch(std::map<KnobGuiContainerI*, KnobGuiWPtr>* touched,
            KnobGuiContainerI* panel,
            const KnobGuiPtr& owner)
{
    // An entry whose knob is gone belongs to a closed panel, whose address a new one may reuse.
    for (std::map<KnobGuiContainerI*, KnobGuiWPtr>::iterator it = touched->begin(); it != touched->end();) {
        if (it->second.expired()) {
            it = touched->erase(it);
        } else {
            ++it;
        }
    }
    (*touched)[panel] = owner;
}

KnobGui*
recalledTouch(std::map<KnobGuiContainerI*, KnobGuiWPtr>* touched,
              KnobGuiContainerI* panel)
{
    std::map<KnobGuiContainerI*, KnobGuiWPtr>::iterator it = touched->find(panel);

    if (it == touched->end()) {
        return 0;
    }

    KnobGuiPtr locked = it->second.lock();

    if (!locked) {
        touched->erase(it);

        return 0;
    }

    return locked.get();
}
} // namespace

void
ColorKnobTouchTracker::touch(KnobGui* knobGui)
{
    if (!knobGui) {
        return;
    }

    KnobGuiContainerI* panel = knobGui->getContainer();
    // Empty while the knob's GUI is being torn down, which a late event can still land in.
    KnobGuiPtr owner = knobGui->weak_from_this().lock();

    if (!panel || !owner) {
        return;
    }
    recordTouch(&_lastTouchedAnyKind, panel, owner);
    if (dynamic_cast<KnobGuiColor*>(knobGui)) {
        recordTouch(&_lastTouched, panel, owner);
    }
}

KnobGui*
ColorKnobTouchTracker::lastTouched(KnobGuiContainerI* panel)
{
    return recalledTouch(&_lastTouched, panel);
}

KnobGui*
ColorKnobTouchTracker::lastTouchedAnyKind(KnobGuiContainerI* panel)
{
    return recalledTouch(&_lastTouchedAnyKind, panel);
}

bool
ColorKnobTouchTracker::eventFilter(QObject* watched,
                                   QEvent* event)
{
    QEvent::Type t = event->type();

    if ((t == QEvent::MouseButtonPress) || (t == QEvent::Wheel) || (t == QEvent::FocusIn)) {
        if (KnobGui* owner = globalModeKnobAt(qobject_cast<QWidget*>(watched))) {
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

void
revealKnobInPanel(DockablePanel* panel,
                  const KnobIPtr& knob,
                  QWidget* widget)
{
    if (!panel || !knob) {
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
    if (!widget) {
        return;
    }
    for (QWidget* w = widget->parentWidget(); w; w = w->parentWidget()) {
        if (QScrollArea* scrollArea = qobject_cast<QScrollArea*>(w)) {
            scrollArea->ensureWidgetVisible(widget);
            break;
        }
    }
} // revealKnobInPanel

bool
isGlobalModeKnob(KnobGui* knobGui)
{
    if (!knobGui || knobGui->isSecretRecursive()) {
        return false;
    }

    KnobIPtr knob = knobGui->getKnob();

    if (!knob || !knob->isEnabled(0)) {
        return false;
    }
    if (KnobGuiColor* color = dynamic_cast<KnobGuiColor*>(knobGui)) {
        return color->getAdjustPanel() != 0;
    }
    if (!dynamic_cast<KnobGuiValue*>(knobGui)) {
        return false;
    }

    const GlobalModeKindEnum kind = classifyGlobalModeKind(knob);

    return (kind != eGlobalModeKindIneligible) && (kind != eGlobalModeKindColor);
}

std::vector<KnobGui*>
restrictToPageOf(const std::vector<KnobGui*>& knobs,
                 KnobGui* reference)
{
    KnobIPtr referenceKnob = reference ? reference->getKnob() : KnobIPtr();
    KnobPagePtr referencePage = referenceKnob ? referenceKnob->getTopLevelPage() : KnobPagePtr();
    std::vector<KnobGui*> result;

    for (std::size_t i = 0; i < knobs.size(); ++i) {
        KnobIPtr knob = knobs[i]->getKnob();
        KnobPagePtr page = knob ? knob->getTopLevelPage() : KnobPagePtr();

        if (page == referencePage) {
            result.push_back(knobs[i]);
        }
    }

    return result;
}

std::vector<KnobGui*>
orderedGlobalModeKnobs(DockablePanel* panel,
                       KnobGui* samePageAs)
{
    std::vector<KnobGui*> result;

    if (!panel) {
        return result;
    }

    const KnobsGuiMapping& mapping = panel->getKnobsMapping();

    for (KnobsGuiMapping::const_iterator it = mapping.begin(); it != mapping.end(); ++it) {
        KnobGui* knobGui = it->second.get();

        if (isGlobalModeKnob(knobGui)) {
            result.push_back(knobGui);
        }
    }

    return samePageAs ? restrictToPageOf(result, samePageAs) : result;
}

std::shared_ptr<GlobalModeTarget>
makeGlobalModeTarget(KnobGui* knobGui)
{
    KnobGuiPtr owner = knobGui ? knobGui->weak_from_this().lock() : KnobGuiPtr();

    if (!owner) {
        return std::shared_ptr<GlobalModeTarget>();
    }
    if (KnobGuiColorPtr color = std::dynamic_pointer_cast<KnobGuiColor>(owner)) {
        if (!color->getAdjustPanel()) {
            return std::shared_ptr<GlobalModeTarget>();
        }

        return std::make_shared<ColorGlobalModeTarget>(color);
    }

    KnobIPtr knob = owner->getKnob();

    if (!knob || !dynamic_cast<KnobGuiValue*>(knobGui)) {
        return std::shared_ptr<GlobalModeTarget>();
    }

    const GlobalModeKindEnum kind = classifyGlobalModeKind(knob);

    if ((kind == eGlobalModeKindIneligible) || (kind == eGlobalModeKindColor)) {
        return std::shared_ptr<GlobalModeTarget>();
    }

    EffectInstance* effect = dynamic_cast<EffectInstance*>(knob->getHolder());
    NodePtr node = effect ? effect->getNode() : NodePtr();

    if (node) {
        const TransformInteractKnobs group = node->getTransformInteractKnobs(knob.get());
        GlobalModeGroupMemberEnum member = eGlobalModeGroupMemberTranslate;

        if (TransformGroupGlobalModeTarget::memberOf(group, knob.get(), &member)) {
            return std::make_shared<TransformGroupGlobalModeTarget>(knobGui->getContainer(), group, member);
        }
    }

    return std::make_shared<ValueKnobGlobalModeTarget>(owner);
} // makeGlobalModeTarget

KnobGui*
globalModeKnobAt(QWidget* widget)
{
    for (QWidget* w = widget; w; w = w->parentWidget()) {
        QVariant colorTag = w->property(colorKnobTouchTagPropertyName());

        if (colorTag.isValid()) {
            return colorTag.value<QPointer<KnobGuiColor>>().data();
        }

        QVariant valueTag = w->property(globalModeKnobTouchTagPropertyName());

        if (valueTag.isValid()) {
            return valueTag.value<QPointer<KnobGui>>().data();
        }
    }

    return 0;
}

void
touchGlobalModeKnob(KnobGui* knobGui)
{
    Gui* gui = knobGui ? knobGui->getGui() : 0;
    ColorKnobTouchTracker* tracker = gui ? ColorKnobTouchTracker::instanceFor(gui) : 0;

    if (tracker) {
        tracker->touch(knobGui);
    }
}

KnobGui*
resolveGlobalModeKnob(Gui* gui,
                      QWidget* hovered)
{
    KnobGui* underPointer = globalModeKnobAt(hovered);

    if (underPointer && dynamic_cast<NodeSettingsPanel*>(underPointer->getContainer()) && isGlobalModeKnob(underPointer)) {
        return underPointer;
    }

    NodeSettingsPanel* panel = topmostNodePanel(gui);

    if (!panel) {
        return 0;
    }

    ColorKnobTouchTracker* tracker = ColorKnobTouchTracker::instanceFor(gui);
    KnobGui* touched = tracker ? tracker->lastTouchedAnyKind(panel) : 0;

    if (isGlobalModeKnob(touched)) {
        return touched;
    }
    if (KnobGuiColor* color = resolveColorTrackballTarget(gui)) {
        return color;
    }

    const std::vector<KnobGui*> knobs = orderedGlobalModeKnobs(panel);

    return knobs.empty() ? 0 : knobs.front();
} // resolveGlobalModeKnob

NATRON_NAMESPACE_EXIT
