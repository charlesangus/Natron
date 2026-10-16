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

    Serialization::NodeSerializationPtr getStoredSerialization() const WARN_UNUSED_RETURN;

    void setStoredGuiChildren(const std::string& nodeName,
                              const std::list<Serialization::NodeGuiSerializationPtr>& children);

    std::list<Serialization::NodeGuiSerializationPtr> getStoredGuiChildren(std::string* nodeName) const WARN_UNUSED_RETURN;

    static std::string getShortPluginName(const std::string& pluginID) WARN_UNUSED_RETURN;

    QString getMissingSummary() const WARN_UNUSED_RETURN;

private:
    virtual bool getIsEditable() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return false;
    }

    virtual NativePluginDescription getNativePluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void initializeKnobs() OVERRIDE FINAL;

    // The inputs come from the serialization, which reaches the node only after its first input
    // initialization, so this initializes them again.
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
