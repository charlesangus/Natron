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

#ifndef Engine_Nodes_MissingPluginNode_h
#define Engine_Nodes_MissingPluginNode_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <string>
#include <vector>

#include <QMutex>
#include <QString>

#include "Engine/EffectInstance.h" // for PLUGINID_NATRON_MISSING_PLUGIN
#include "Engine/EngineFwd.h"
#include "Engine/Nodes/NativeEffectBase.h"

#include "Serialization/SerializationFwd.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Stands in for a node whose plug-in is not installed. It keeps the node's serialization
 * so a later save can write it back, takes one input per connection that serialization records
 * so the edges survive, shows an error naming the missing plug-in, and fails every render
 * instead of passing an input through.
 *
 * The inputs are taken from the serialization once it reaches the node, which is after the node's
 * first input initialization, so the node initializes its inputs again at that point.
 **/
class MissingPluginNode
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new MissingPluginNode(node);
    }

    explicit MissingPluginNode(NodePtr node)
        : NativeEffectBase(node)
    {
    }

    /**
     * @brief The serialization of the node this one replaces, null until the node was created
     * from one.
     **/
    Serialization::NodeSerializationPtr getStoredSerialization() const WARN_UNUSED_RETURN;

    /**
     * @brief Keeps the node graph state of the nodes the replaced node held, as read for it from a
     * project file in which the node was named nodeName, so a later save can write it back.
     **/
    void setStoredGuiChildren(const std::string& nodeName,
                              const std::list<Serialization::NodeGuiSerializationPtr>& children);

    /**
     * @brief The node graph state set by setStoredGuiChildren(), and in nodeName the fully
     * qualified name the node had then.
     **/
    std::list<Serialization::NodeGuiSerializationPtr> getStoredGuiChildren(std::string* nodeName) const WARN_UNUSED_RETURN;

    /**
     * @brief The last dot-separated part of a plug-in id with a trailing "Plugin" removed, so
     * net.sf.openfx.GradePlugin reads Grade.
     **/
    static std::string getShortPluginName(const std::string& pluginID) WARN_UNUSED_RETURN;

    /**
     * @brief A one-line identification of the missing plug-in such as "missing: Grade 2.0", empty
     * until the node was created from a serialization.
     **/
    QString getMissingSummary() const WARN_UNUSED_RETURN;

private:
    virtual bool getCanManageUserParameters() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return false;
    }

    virtual NativePluginDescription getNativePluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual void onKnobsAboutToBeLoaded(const Serialization::NodeSerializationPtr& serialization) OVERRIDE FINAL;

    virtual void onKnobsLoaded() OVERRIDE FINAL;

    virtual void onEffectCreated(bool mayCreateFileDialog,
                                 const CreateNodeArgs& args) OVERRIDE FINAL;

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    KnobStringWPtr _info;
    mutable QMutex _storedMutex;
    Serialization::NodeSerializationPtr _stored;
    std::vector<std::string> _inputLabels;
    std::string _guiChildrenNodeName;
    std::list<Serialization::NodeGuiSerializationPtr> _guiChildren;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_MissingPluginNode_h
