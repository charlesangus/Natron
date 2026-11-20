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

#ifndef Engine_Nodes_Test_PortTestNode_h
#define Engine_Nodes_Test_PortTestNode_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/UndoCommand.h"

#include "Serialization/SerializationFwd.h"

#define kPortTestPluginIDPrefix "test.natron.built-in.PortNode."
#define kPortTestAllPortsSuffix ".AllPorts"
#define kPortTestParamPorts "ports"
#define kPortTestParamInputOutputs "inputOutputs"

NATRON_NAMESPACE_ENTER

struct PortTestAttribute {
    std::string name;
    std::string type;
    bool bindable = false;
};

struct PortTestOutput {
    std::string name;
    std::string type;
};

/**
 * @brief What one port-node kind declares: its attributes (each a knob, and a port when promoted),
 * its outputs, the targets it supports and the attributes promoted by default. Generic: a shader,
 * a kernel node and a channel node all fit it.
 **/
struct PortTestLayout {
    std::string pluginId;
    std::string label;
    std::string description;
    std::vector<PortTestAttribute> attributes;
    std::vector<PortTestOutput> outputs;
    std::vector<std::string> targets;
    std::vector<std::string> defaultPorts;
};

/**
 * @brief A throwaway generic port node: one class for every kind, its NodeSpec built at run time
 * from the kind's layout, its inputs the promoted attributes, which can change while the node is
 * live and connected. Registered as one plug-in per kind, every one built by this class. The app
 * registers it only when NATRON_GUI_TEST_NODES=1, the gtests always.
 **/
class PortTestNode
    : public NativeEffectBase {
public:
    enum ConnectionCheckEnum {
        eConnectionOk = 0,
        eConnectionNoCommonTarget,
        eConnectionNoSuchOutput,
        eConnectionNoSuchInput,
        eConnectionTypeMismatch,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new PortTestNode(node);
    }

    explicit PortTestNode(NodePtr node);

    PortTestNode(const PortTestNode&) = delete;
    PortTestNode& operator=(const PortTestNode&) = delete;

    virtual ~PortTestNode();

    static const std::vector<PortTestLayout>& layouts();

    static const PortTestLayout* findLayout(const std::string& pluginId);

    // Registers one plug-in per layout and returns their IDs.
    static std::vector<std::string> registerPlugins();

    const PortTestLayout& layout() const
    {
        return *_layout;
    }

    std::vector<std::string> getPorts() const;

    /**
     * @brief Makes ports the node's inputs, in that order. Connections follow their port by
     * label; a removed port's connection is disconnected. Names that are not attributes, or
     * repeat, are dropped. Main thread only.
     **/
    void setPorts(const std::vector<std::string>& ports);

    // The output of input's upstream node the input reads, empty for that node's first.
    std::string getOutputForInput(const std::string& inputLabel) const;

    void setOutputForInput(const std::string& inputLabel,
                           const std::string& outputName);

    /**
     * @brief Whether input inputLabel of downstream may read output outputName of upstream:
     * the nodes must share a target, then the output's type must convert to the port's.
     **/
    static ConnectionCheckEnum checkConnection(const PortTestNode& upstream,
                                               const std::string& outputName,
                                               const PortTestNode& downstream,
                                               const std::string& inputLabel);

    static bool portTypesConvert(const std::string& from,
                                 const std::string& to);

    const PortTestAttribute* findAttribute(const std::string& name) const;

    const PortTestOutput* findOutput(const std::string& name) const;

private:
    struct SpecStorage {
        std::deque<std::string> labels;
        std::vector<InputSpec> inputs;
        NodeSpec spec;
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return _spec.load(std::memory_order_acquire);
    }

    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual void onKnobsAboutToBeLoaded(const Serialization::NodeSerializationPtr& serialization) OVERRIDE FINAL;

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    std::vector<std::string> sanitizePorts(const std::vector<std::string>& ports) const;

    void publishSpec(const std::vector<std::string>& ports);

    void applyPorts(const std::vector<std::string>& ports);

    std::map<std::string, std::string> getInputOutputs() const;

    const PortTestLayout* _layout;
    // Render threads read the spec while the main thread replaces it, so a replaced spec is
    // retired rather than freed and every pointer handed out stays valid for the node's life.
    std::vector<std::unique_ptr<SpecStorage>> _specs;
    std::atomic<const NodeSpec*> _spec;
    std::vector<std::string> _appliedPorts;
    KnobStringWPtr _portsKnob;
    KnobStringWPtr _inputOutputsKnob;
};

/**
 * @brief An undoable port edit: setPorts() plus, on undo, the connections the edit removed.
 **/
class PortTestEditPortsCommand
    : public UndoCommand {
public:
    PortTestEditPortsCommand(const NodePtr& node,
                             const std::vector<std::string>& ports);

    virtual void redo() OVERRIDE FINAL;

    virtual void undo() OVERRIDE FINAL;

private:
    NodeWPtr _node;
    std::vector<std::string> _before;
    std::vector<std::string> _after;
    std::map<std::string, NodeWPtr> _connections;
};

std::string portTestJoinPorts(const std::vector<std::string>& ports);

std::vector<std::string> portTestSplitPorts(const std::string& text);

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Test_PortTestNode_h
