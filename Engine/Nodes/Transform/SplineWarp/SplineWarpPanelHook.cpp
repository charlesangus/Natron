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

#include "SplineWarpPanelHook.h"

#include "Engine/Bezier.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpContext.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpInteract.h"
#include "Engine/RotoDrawableItem.h"
#include "Engine/RotoItem.h"

NATRON_NAMESPACE_ENTER

namespace {
struct ActionInfo {
    SplineWarpActionEnum action;
    const char* id;
};

const ActionInfo kActions[] = {
    { eSplineWarpActionDuplicateAndLink, "duplicateAndLink" },
    { eSplineWarpActionLink, "link" },
    { eSplineWarpActionUnlink, "unlink" },
    { eSplineWarpActionSwap, "swap" },
    { eSplineWarpActionToggleHardBoundary, "toggleHardBoundary" },
    { eSplineWarpActionToggleSoftBoundary, "toggleSoftBoundary" },
    { eSplineWarpActionToggleCookieCutter, "toggleCookieCutter" },
};
} // anonymous namespace

SplineWarpPanelHook::SplineWarpPanelHook(const std::shared_ptr<SplineWarpInteract>& interact)
    : _interact(interact)
{
}

SplineWarpPanelHook::~SplineWarpPanelHook()
{
}

std::vector<RotoCurvePanelHook::Column>
SplineWarpPanelHook::getColumns() const
{
    std::vector<Column> columns(3);

    columns[eColumnRole].header = tr("Role").toStdString();
    columns[eColumnRole].toolTip = tr("Source or Destination for a curve in a link, Hard Boundary or Soft Boundary for a boundary. "
                                      "Curves with no role are ignored by the warp.")
                                       .toStdString();
    columns[eColumnLinkedTo].header = tr("Linked To").toStdString();
    columns[eColumnLinkedTo].toolTip = tr("The other curve of the link the curve is in.").toStdString();
    columns[eColumnCookieCutter].header = tr("Cookie Cutter").toStdString();
    columns[eColumnCookieCutter].toolTip = tr("Yes for a closed curve that cuts the Source to its shape (Cookie Cut Input) "
                                              "and makes the Output Mask, whatever its role.")
                                               .toStdString();

    return columns;
}

std::string
SplineWarpPanelHook::getItemText(const RotoItemPtr& item,
                                 int column) const
{
    std::shared_ptr<SplineWarpInteract> ui = _interact.lock();
    SplineWarpContextPtr context = ui ? ui->getWarpContext() : SplineWarpContextPtr();
    BezierPtr curve = std::dynamic_pointer_cast<Bezier>(item);

    if (!context || !curve) {
        return std::string();
    }
    SplineWarpLinkPtr link = context->getLink(curve);
    const bool isSource = link && (link->getSource() == curve);
    switch (column) {
    case eColumnRole:
        if (link) {
            return isSource ? tr("Source").toStdString() : tr("Destination").toStdString();
        }
        if (context->isBoundary(curve)) {
            return tr("Hard Boundary").toStdString();
        }
        if (context->isSoftBoundary(curve)) {
            return tr("Soft Boundary").toStdString();
        }
        break;
    case eColumnLinkedTo:
        if (link) {
            BezierPtr partner = isSource ? link->getDestination() : link->getSource();
            if (partner) {
                return partner->getLabel();
            }
        }
        break;
    case eColumnCookieCutter:
        if (context->isCookieCutter(curve)) {
            return tr("Yes").toStdString();
        }
        break;
    default:
        break;
    }

    return std::string();
}

std::vector<RotoCurvePanelHook::MenuAction>
SplineWarpPanelHook::getItemMenuActions(const std::list<RotoItemPtr>& items) const
{
    std::vector<MenuAction> actions;
    std::shared_ptr<SplineWarpInteract> ui = _interact.lock();

    if (!ui) {
        return actions;
    }
    const QString labels[] = {
        tr("Duplicate and Link"),
        tr("Link"),
        tr("Unlink"),
        tr("Swap Source/Destination"),
        tr("Toggle Hard Boundary"),
        tr("Toggle Soft Boundary"),
        tr("Toggle Cookie Cutter"),
    };
    for (std::size_t i = 0; i < sizeof(kActions) / sizeof(kActions[0]); ++i) {
        MenuAction action;
        action.id = kActions[i].id;
        action.label = labels[i].toStdString();
        action.enabled = ui->getRefusal(kActions[i].action, items).empty();
        actions.push_back(action);
    }

    return actions;
}

void
SplineWarpPanelHook::onItemMenuActionTriggered(const std::string& actionId,
                                               const std::list<RotoItemPtr>& items)
{
    std::shared_ptr<SplineWarpInteract> ui = _interact.lock();

    if (!ui) {
        return;
    }
    for (std::size_t i = 0; i < sizeof(kActions) / sizeof(kActions[0]); ++i) {
        if (actionId != kActions[i].id) {
            continue;
        }
        // The actions work on the viewer's selection, which the panel's items become, in their
        // order, so Link takes the first as the source.
        std::list<RotoDrawableItemPtr> drawables;
        for (std::list<RotoItemPtr>::const_iterator it = items.begin(); it != items.end(); ++it) {
            RotoDrawableItemPtr drawable = std::dynamic_pointer_cast<RotoDrawableItem>(*it);
            if (drawable) {
                drawables.push_back(drawable);
            }
        }
        ui->setSelection(drawables, std::list<std::pair<BezierCPPtr, BezierCPPtr>>());
        ui->runAction(kActions[i].action);

        return;
    }
}

NATRON_NAMESPACE_EXIT
