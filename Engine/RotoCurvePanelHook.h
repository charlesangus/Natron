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

#ifndef Engine_RotoCurvePanelHook_h
#define Engine_RotoCurvePanelHook_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief What a curve host adds to the curve list of its settings panel (RotoPanel): columns after
 * the built-in ones, and actions at the end of an item's context menu. An effect returns one from
 * EffectInstance::getCurvePanelHook(); it stays owned by the effect. The panel re-reads an item's
 * columns when RotoCurveContext::itemPanelColumnsChanged() is emitted.
 **/
class RotoCurvePanelHook {
public:
    struct Column {
        std::string header;
        std::string toolTip;
    };

    struct MenuAction {
        std::string id;
        std::string label;
    };

    virtual ~RotoCurvePanelHook()
    {
    }

    // The extra columns, in order. Read once, when the panel is built.
    virtual std::vector<Column> getColumns() const = 0;

    // The text item shows in the extra column at index column of getColumns(). Called on the main thread.
    virtual std::string getItemText(const RotoItemPtr& item, int column) const = 0;

    // The actions appended to the context menu of the selected items.
    virtual std::vector<MenuAction> getItemMenuActions(const std::list<RotoItemPtr>& /*items*/) const
    {
        return std::vector<MenuAction>();
    }

    // The user picked the action with that id from the context menu of items.
    virtual void onItemMenuActionTriggered(const std::string& /*actionId*/,
                                           const std::list<RotoItemPtr>& /*items*/)
    {
    }
};

NATRON_NAMESPACE_EXIT

#endif // Engine_RotoCurvePanelHook_h
