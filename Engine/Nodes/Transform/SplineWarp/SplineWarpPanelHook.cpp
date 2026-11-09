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
#include "Engine/RotoItem.h"
#include "Engine/VectorFollowKnobs.h"

NATRON_NAMESPACE_ENTER

SplineWarpPanelHook::SplineWarpPanelHook(const std::shared_ptr<SplineWarpInteract>& interact)
    : _interact(interact)
    , _follow(nullptr)
{
}

SplineWarpPanelHook::~SplineWarpPanelHook()
{
}

std::vector<RotoCurvePanelHook::Column>
SplineWarpPanelHook::getColumns() const
{
    std::vector<Column> columns(4);

    columns[eColumnRole].header = tr("Role").toStdString();
    columns[eColumnRole].toolTip = tr("Source or Destination for a curve in a link, Boundary for a boundary. "
                                      "Curves with no role are ignored by the warp.")
                                       .toStdString();
    columns[eColumnLinkedTo].header = tr("Linked To").toStdString();
    columns[eColumnLinkedTo].toolTip = tr("The other curve of the link the curve is in.").toStdString();
    columns[eColumnCookieCutter].header = tr("Cookie Cutter").toStdString();
    columns[eColumnCookieCutter].toolTip = tr("Yes for a closed curve that makes the Output Mask, whatever its role.")
                                               .toStdString();
    columns[eColumnTransformLink].header = tr("Transform Link").toStdString();
    columns[eColumnTransformLink].toolTip = tr("The curve or layer whose transform the item takes in place of its own.").toStdString();

    return columns;
}

std::string
SplineWarpPanelHook::getItemText(const RotoItemPtr& item,
                                 int column) const
{
    std::shared_ptr<SplineWarpInteract> ui = _interact.lock();
    SplineWarpContextPtr context = ui ? ui->getWarpContext() : SplineWarpContextPtr();
    BezierPtr curve = std::dynamic_pointer_cast<Bezier>(item);

    if (context && (column == eColumnTransformLink)) {
        RotoItemPtr target = context->getTransformLink(item);

        return target ? target->getLabel() : std::string();
    }
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
            return tr("Boundary").toStdString();
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
    std::shared_ptr<SplineWarpInteract> ui = _interact.lock();

    // The viewer's right-click menu on a curve lists the same actions.
    std::vector<MenuAction> actions = ui ? ui->getCurveMenuActions(items) : std::vector<MenuAction>();
    if (_follow) {
        const std::vector<MenuAction> follow = _follow->getItemMenuActions(items);
        actions.insert(actions.end(), follow.begin(), follow.end());
    }

    return actions;
}

void
SplineWarpPanelHook::onItemMenuActionTriggered(const std::string& actionId,
                                               const std::list<RotoItemPtr>& items)
{
    if (_follow) {
        for (const MenuAction& action : _follow->getItemMenuActions(items)) {
            if (action.id == actionId) {
                _follow->onItemMenuActionTriggered(actionId, items);

                return;
            }
        }
    }
    std::shared_ptr<SplineWarpInteract> ui = _interact.lock();

    if (ui) {
        ui->runCurveMenuAction(actionId, items);
    }
}

VectorFollowControls*
SplineWarpPanelHook::getVectorFollowControls() const
{
    return _follow;
}

void
SplineWarpPanelHook::setVectorFollowControls(VectorFollowControls* follow)
{
    _follow = follow;
}

NATRON_NAMESPACE_EXIT
