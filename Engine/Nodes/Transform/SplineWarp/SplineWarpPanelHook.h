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

#ifndef Engine_Nodes_Transform_SplineWarp_SplineWarpPanelHook_h
#define Engine_Nodes_Transform_SplineWarp_SplineWarpPanelHook_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <memory>
#include <string>
#include <vector>

#include <QCoreApplication>

#include "Engine/EngineFwd.h"
#include "Engine/RotoCurvePanelHook.h"

NATRON_NAMESPACE_ENTER

class SplineWarpInteract;

/**
 * @brief What SplineWarp adds to its curve list: the Role, Linked To and Cookie Cutter columns, and the warp
 * actions in an item's context menu, disabled when they would refuse the selection.
 **/
class SplineWarpPanelHook
    : public RotoCurvePanelHook {
    Q_DECLARE_TR_FUNCTIONS(SplineWarpPanelHook)

public:
    enum ColumnEnum {
        eColumnRole = 0,
        eColumnLinkedTo,
        eColumnCookieCutter,
    };

    explicit SplineWarpPanelHook(const std::shared_ptr<SplineWarpInteract>& interact);

    virtual ~SplineWarpPanelHook();

    virtual std::vector<Column> getColumns() const OVERRIDE FINAL;
    virtual std::string getItemText(const RotoItemPtr& item, int column) const OVERRIDE FINAL;
    virtual std::vector<MenuAction> getItemMenuActions(const std::list<RotoItemPtr>& items) const OVERRIDE FINAL;
    virtual void onItemMenuActionTriggered(const std::string& actionId, const std::list<RotoItemPtr>& items) OVERRIDE FINAL;

private:
    std::weak_ptr<SplineWarpInteract> _interact;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Transform_SplineWarp_SplineWarpPanelHook_h
