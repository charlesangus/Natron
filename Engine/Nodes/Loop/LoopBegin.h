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

#ifndef Engine_Nodes_Loop_LoopBegin_h
#define Engine_Nodes_Loop_LoopBegin_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/NativeEffectBase.h"

#define PLUGINID_NATRON_LOOPBEGIN "fr.natron.LoopBegin"
#define PLUGIN_MAJOR_NATRON_LOOPBEGIN 1

#define kLoopBeginParamName "name"

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kLoopBeginInputs[] = {
    InputSpec { .label = "Source", .kind = eDataKindImage },
};

/**
 * @brief Marks where a loop body starts. An identity on its input: the body is whatever lies
 * between it and the LoopEnd that pairs with it. Its name is what outer loops are seen as from
 * inside nested ones (`loops.<name>.*`), so it is unique within its group.
 **/
class LoopBegin
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new LoopBegin(node);
    }

    explicit LoopBegin(NodePtr node);

    static bool isLoopBegin(const NodePtr& node) WARN_UNUSED_RETURN;

    std::string getLoopName() const WARN_UNUSED_RETURN;

    /**
     * @brief Why the loop name cannot be used, or empty when it can: it must be an identifier,
     * and no other LoopBegin of the same group may have it.
     **/
    std::string getLoopNameError() const WARN_UNUSED_RETURN;

private:
    virtual LayerKnobSpec getLayerKnobSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return LayerKnobSpec();
    }

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_LOOPBEGIN,
        .label = "LoopBegin",
        .description = "Marks the start of a loop body. The nodes between a LoopBegin and the LoopEnd "
                       "below it are rendered once per iteration, each time with the loop variables "
                       "loop.iteration, loop.count, loop.t (0 to 1) and loop.frame set, which knob "
                       "drivers in the body read. A body node that reads no loop variable renders only "
                       "once. The image passes through LoopBegin unchanged. Name is how a nested loop "
                       "sees this one, as loops.<name>.*, and is unique within the group.",
        .grouping = PLUGIN_GROUP_LOOP,
        .majorVersion = PLUGIN_MAJOR_NATRON_LOOPBEGIN,
        .minorVersion = 0,
        .inputs = kLoopBeginInputs,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    virtual Route route(double time,
                        ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

    virtual void onEffectCreated(bool mayCreateFileDialog,
                                 const CreateNodeArgs& args) OVERRIDE FINAL;

    virtual void onKnobsLoaded() OVERRIDE FINAL;

    void refreshNameMessage();

    KnobStringWPtr _name;
    std::string _postedMessage;
};

/**
 * @brief The LoopBegins of node's group other than node itself.
 **/
std::list<NodePtr> otherLoopBeginsInGroup(const NodePtr& node) WARN_UNUSED_RETURN;

/**
 * @brief Shows message on node as a persistent message of type, unless it already shows it, or
 * clears what *posted says this caller showed when message is empty. *posted tracks it.
 **/
void refreshLoopMessage(const NodePtr& node,
                        MessageTypeEnum type,
                        const std::string& message,
                        std::string* posted);

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Loop_LoopBegin_h
