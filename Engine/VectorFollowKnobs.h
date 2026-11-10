/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
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

#ifndef Engine_VectorFollowKnobs_h
#define Engine_VectorFollowKnobs_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/RotoCurvePanelHook.h"

NATRON_NAMESPACE_ENTER

class RotoCurveInteract;
class VectorFollowPoints;
class VectorFollowTask;
typedef std::shared_ptr<VectorFollowPoints> VectorFollowPointsPtr;
struct VectorFollowReport;

/**
 * @brief The Follow Vectors group of a curve host: its knobs, what its buttons do with the host's
 * selection, the context menu entry of its curve panel and the follow they run. Main thread.
 **/
class VectorFollowControls
    : public RotoCurvePanelHook {
public:
    VectorFollowControls(EffectInstance* host,
                         RotoCurveInteract* interact);

    virtual ~VectorFollowControls();

    void createKnobs(const KnobPagePtr& page);

    // True when k is one of this group's knobs.
    bool onKnobChanged(KnobI* k);

    // Nodes appear and lose their layers at any time and a choice knob cannot ask before it opens.
    void refreshSources();

    bool isFollowing() const;
    void abortFollow(bool wait);

    // The report of the last follow a button or the panel menu ran, until the next press; false
    // while it runs, after a refused press, and before any.
    bool getLastReport(VectorFollowReport* report) const;

    virtual std::vector<Column> getColumns() const OVERRIDE FINAL;
    virtual std::string getItemText(const RotoItemPtr& item, int column) const OVERRIDE FINAL;
    virtual std::vector<MenuAction> getItemMenuActions(const std::list<RotoItemPtr>& items) const OVERRIDE FINAL;
    virtual void onItemMenuActionTriggered(const std::string& actionId, const std::list<RotoItemPtr>& items) OVERRIDE FINAL;

    virtual VectorFollowControls* getVectorFollowControls() const OVERRIDE FINAL
    {
        return const_cast<VectorFollowControls*>(this);
    }

    // Points the host has selected beside its curves, which the buttons follow with them.
    void setSelectedPointsProvider(const std::function<std::list<VectorFollowPointsPtr>()>& provider);

private:
    struct Imp;
    std::unique_ptr<Imp> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_VectorFollowKnobs_h
