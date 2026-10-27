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

#ifndef Engine_Nodes_RFV_RFVRead_h
#define Engine_Nodes_RFV_RFVRead_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/NativeEffectBase.h"

#define PLUGINID_NATRON_RFVREAD "RFVRead"
#define PLUGIN_MAJOR_NATRON_RFVREAD 1

#define kRFVReadParamVariables "variables"
#define kRFVReadParamVariablesLabel "Variables"
#define kRFVReadParamVariablesHint "Every Reverse-Flow Variable that reaches this node, with its value as rfv gives it here at the " \
                                   "timeline's frame: the project's variables, with the values of the Set nodes on the path from "   \
                                   "this node to the active viewer over them. A node on no viewer's path lists the project's."

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kRFVReadInputs[] = {
    InputSpec { .label = "Source", .kind = eDataKindPolymorphic },
};

/**
 * @brief Passes its input through unchanged, whatever its data kind, and lists in Variables the
 * Reverse-Flow Variables reaching it, resolved along the path to the active viewer without
 * rendering (see RFV::resolveContextAt()). It reads no variable, so it adds nothing to the context
 * key of any node.
 **/
class RFVRead
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new RFVRead(node);
    }

    explicit RFVRead(NodePtr node);

    virtual ~RFVRead();

    /**
     * @brief Lists in Variables the variables reaching this node at the timeline's frame from the
     * active viewer. Main thread.
     **/
    void refreshVariables();

    /**
     * @brief Lists in Variables the variables reaching this node at time from output (see
     * RFV::resolveContextBelow()), or the project's with no output. Main thread.
     **/
    void refreshVariablesBelow(const NodePtr& output,
                               double time);

    /**
     * @brief The text Variables shows.
     **/
    std::string getVariablesText() const WARN_UNUSED_RETURN;

    /**
     * @brief Has every RFVRead node of app refresh its Variables once control returns to the event
     * loop, each at most once however often this is called before then. Main thread.
     **/
    static void scheduleRefreshAll(const AppInstance* app);

private:
    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_RFVREAD,
        .label = "RFVRead",
        .description = "Lists every Reverse-Flow Variable that reaches this node, with its value at the timeline's "
                       "frame, along the path from this node to the active viewer: the project's variables, with "
                       "the values the Set nodes on that path set over them, the nearest winning. A node on no "
                       "viewer's path lists the project's. The node passes its input through unchanged.",
        .grouping = PLUGIN_GROUP_OTHER,
        .majorVersion = PLUGIN_MAJOR_NATRON_RFVREAD,
        .minorVersion = 0,
        .outputKind = eDataKindPolymorphic,
        .inputs = kRFVReadInputs,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual Route route(double time,
                        ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void onEffectCreated(bool mayCreateFileDialog,
                                 const CreateNodeArgs& args) OVERRIDE FINAL;

    virtual void refreshExtraStateAfterTimeChanged(bool isPlayback,
                                                   double time) OVERRIDE FINAL;

    virtual void onSettingsPanelOpened() OVERRIDE FINAL;

    void scheduleRefresh();

    KnobStringWPtr _variables;
    bool _refreshPending;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_RFV_RFVRead_h
