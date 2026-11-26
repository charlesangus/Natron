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

#ifndef Engine_Nodes_Network_NetworkNode_h
#define Engine_Nodes_Network_NetworkNode_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "Global/Enums.h"

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Network/PortDescriptor.h"
#include "Engine/UndoCommand.h"

#include "Serialization/SerializationFwd.h"

// The promoted ports, in input order, space separated.
#define kNetworkNodeParamPorts "ports"
// The output each input reads, as label=output entries; an input without one reads the first.
#define kNetworkNodeParamInputOutputs "inputOutputs"

NATRON_NAMESPACE_ENTER

enum class NetworkKnobKind {
    // A port only: the attribute has no value.
    None,
    Double,
    Int,
    Bool,
    Choice,
    Color,
    String,
    File,
    // One curve per dimension over [0, 1]: a ramp, or a lookup the target samples.
    Parametric,
    // A list of rows, one per line, the values of a row space separated.
    Table,
};

struct NetworkCurvePoint {
    double x = 0.;
    double y = 0.;
    KeyframeTypeEnum interpolation = eKeyframeTypeLinear;
};

struct NetworkChoice {
    std::string id;
    std::string label;
    // What the target writes for the choice.
    std::string value;
};

/**
 * @brief One attribute of a network node: a knob, a port, or both.
 **/
struct NetworkAttribute {
    std::string name;
    std::string label;
    std::string hint;
    std::string page;
    // The group within the page, empty for none.
    std::string group;
    NetworkKnobKind knob = NetworkKnobKind::None;
    int dimensions = 1;
    std::vector<double> defaults;
    // A Parametric knob's default control points, one list per dimension; an empty list is the
    // identity curve.
    std::vector<std::vector<NetworkCurvePoint>> curves;
    // The ramp this attribute holds one array of (its positions, values or interpolations). The
    // attributes of a ramp share one Parametric knob, named after the ramp.
    std::string ramp;
    // The default of a String, File or Table knob.
    std::string defaultText;
    std::vector<NetworkChoice> choices;
    int defaultChoice = 0;
    std::vector<std::string> dimensionNames;
    bool hasDisplayMinimum = false;
    double displayMinimum = 0.;
    bool hasDisplayMaximum = false;
    double displayMaximum = 1.;
    // The attribute can be promoted to a port.
    bool linkable = false;
    // The attribute is always a port and has no knob.
    bool portOnly = false;
    // promoted is false here: whether it is promoted is the node's state.
    PortDescriptor port;
};

/**
 * @brief What one network plug-in builds: identity, the targets it supports, its attributes
 * on their pages, its named outputs and the ports a new node starts with.
 **/
struct NetworkNodeLayout {
    std::string pluginId;
    std::string label;
    std::string description;
    // A menu path whose levels are separated by '/'.
    std::string grouping;
    std::vector<std::string> targets;
    std::vector<std::string> pages;
    std::vector<NetworkAttribute> attributes;
    std::vector<OutputPortDescriptor> outputs;
    std::vector<std::string> defaultPorts;
    // Lowercase words the Tab menu matches like the label, so that a type name finds the node.
    std::vector<std::string> searchAliases;
};

/**
 * @brief The one class every network plug-in builds: a node of a port network (shaders,
 * kernels, channels), reading its layout from its plug-in ID. Every attribute with a value is a
 * knob; the promoted ones are also inputs, in the order of the ports knob, and the output each
 * input reads is named on the downstream node. A network node joins network nodes only and is
 * never rendered: what reads the network walks it.
 **/
class NetworkNode
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
        return new NetworkNode(node);
    }

    explicit NetworkNode(NodePtr node);

    NetworkNode(const NetworkNode&) = delete;
    NetworkNode& operator=(const NetworkNode&) = delete;

    virtual ~NetworkNode();

    /**
     * @brief Adds layout to the layouts plug-ins build from, replacing one with its plug-in ID.
     * A replaced layout stays alive, for the nodes built from it. Main thread.
     **/
    static const NetworkNodeLayout* addLayout(const NetworkNodeLayout& layout);

    // Fills in a layout, or sets the error and returns false.
    typedef std::function<bool(NetworkNodeLayout* layout, std::string* error)> LayoutBuilder;

    /**
     * @brief Registers how the layout of pluginId is built: on the first findLayout() of it, so that
     * a layout no node is built from costs nothing. A layout addLayout() added wins. Main thread.
     **/
    static void addLayoutBuilder(const std::string& pluginId, const LayoutBuilder& builder);

    /**
     * @brief The layout of pluginId, built on the first call when it has a builder. Null when
     * there is none, or its builder fails, which is reported once.
     **/
    static const NetworkNodeLayout* findLayout(const std::string& pluginId);

    /**
     * @brief Registers the plug-in of layout, which must have been added or have a builder; only
     * its plug-in ID, label, grouping and targets are read. It is for internal use only: hidden
     * from the nodes menu, created by ID or from the menu of a builder's graph. Returns null when
     * registration fails.
     **/
    static Plugin* registerPlugin(const NetworkNodeLayout& layout);

    // The plug-in ID and targets of every plug-in registerPlugin() registered, in its order.
    static std::vector<std::pair<std::string, std::vector<std::string>>> registeredPlugins();

    const NetworkNodeLayout& layout() const
    {
        return *_layout;
    }

    const NetworkAttribute* findAttribute(const std::string& name) const;

    /**
     * @brief The output named name, the first output for an empty name; null when there is none.
     **/
    const OutputPortDescriptor* findOutput(const std::string& name) const;

    const std::vector<OutputPortDescriptor>& getOutputPorts() const
    {
        return _layout->outputs;
    }

    /**
     * @brief The promoted ports, in input order. Any thread.
     **/
    std::vector<std::string> getPorts() const;

    std::optional<PortDescriptor> getInputPortDescriptor(int inputNb) const;

    /**
     * @brief The descriptor of every attribute that can be a port, in layout order, each with its
     * promoted state.
     **/
    std::vector<PortDescriptor> getPortDescriptors() const;

    /**
     * @brief Makes ports the node's inputs, in that order. Connections follow their port by
     * label; a removed port's connection is disconnected. Names that are not linkable attributes,
     * or repeat, are dropped, and port-only attributes missing from ports are added at the end.
     * Main thread.
     **/
    void setPorts(const std::vector<std::string>& ports);

    /**
     * @brief Adds attribute to the ports, after the others. False, changing nothing, when it is
     * not a linkable attribute. Main thread.
     **/
    bool promotePort(const std::string& attribute);

    /**
     * @brief Removes attribute from the ports, disconnecting it. False, changing nothing, for a
     * port-only attribute or one that is not an attribute. Main thread.
     **/
    bool demotePort(const std::string& attribute);

    /**
     * @brief The script name of the knob of attribute: the attribute's name, with every character
     * a script name cannot hold replaced by '_'. The port keeps the attribute's own name.
     **/
    static std::string knobNameOf(const std::string& attribute);

    /**
     * @brief The knob of attribute, null for a port-only attribute or one that is not an attribute.
     **/
    KnobIPtr getAttributeKnob(const std::string& attribute) const;

    static bool canPromote(const NetworkAttribute& attribute)
    {
        return attribute.linkable;
    }

    static bool canDemote(const NetworkAttribute& attribute)
    {
        return attribute.linkable && !attribute.portOnly;
    }

    /**
     * @brief The output of input's upstream node the input reads, empty for that node's first.
     * Removing the port drops its entry, and so does connecting the input to a node other than
     * the one it was last connected to, or to one without that output. A disconnected input keeps
     * its entry until it is connected again.
     **/
    std::string getOutputForInput(const std::string& inputLabel) const;

    void setOutputForInput(const std::string& inputLabel,
                           const std::string& outputName);

    /**
     * @brief Whether input inputLabel of downstream may read output outputName of upstream: the
     * nodes must share a target, the output must exist, the input must be a promoted port, and
     * canConnectPorts() must accept the two. *reason, if given, receives why a refused
     * connection is refused.
     **/
    static ConnectionCheckEnum checkConnection(const NetworkNode& upstream,
                                               const std::string& outputName,
                                               const NetworkNode& downstream,
                                               const std::string& inputLabel,
                                               std::string* reason = nullptr);

    /**
     * @brief checkConnection() for connecting upstream, or the node it passes on through Dots,
     * to input inputNb of downstream. It reads the output downstream names for that input when
     * upstream has it and is the node that input last had, if any; upstream's first otherwise.
     * eConnectionOk unless both nodes are network nodes.
     **/
    static ConnectionCheckEnum checkInputConnection(const NodePtr& upstream,
                                                    const Node& downstream,
                                                    int inputNb,
                                                    std::string* reason = nullptr);

    /**
     * @brief Whether connectPort(inputNb, upstream, outputName) would connect: checkConnection()
     * accepts it and it makes no cycle. *reason, if given, receives why not.
     **/
    bool canConnectPort(int inputNb,
                        const NodePtr& upstream,
                        const std::string& outputName,
                        std::string* reason = nullptr) const;

    /**
     * @brief Connects input inputNb to output outputName of upstream, replacing what the input
     * had, so that it reads that output from the start. A null upstream disconnects the input and
     * leaves it naming outputName. False, changing nothing, when canConnectPort() refuses it.
     * Main thread.
     **/
    bool connectPort(int inputNb,
                     const NodePtr& upstream,
                     const std::string& outputName,
                     std::string* reason = nullptr);

    /**
     * @brief Why a connection checkConnection() refused is refused, empty for eConnectionOk.
     **/
    static std::string connectionCheckReason(ConnectionCheckEnum check);

private:
    struct SpecStorage {
        std::vector<std::string> labels;
        std::vector<PortDescriptor> ports;
    };

    // The inputs follow the ports, which change at run time, so the spec has none: the input
    // readers below read the published ports instead, which a reader on another thread can hold.
    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return &_spec;
    }

    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual std::string getInputLabel(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual bool isInputOptional(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual bool isInputMask(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual DataKindEnum getInputDataKind(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    std::shared_ptr<const SpecStorage> currentSpec() const
    {
        return _current.load(std::memory_order_acquire);
    }

    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual void onKnobsAboutToBeLoaded(const Serialization::NodeSerializationPtr& serialization) OVERRIDE FINAL;

    virtual void onKnobsLoaded() OVERRIDE FINAL;

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void onInputChanged(int inputNb) OVERRIDE FINAL;

    virtual void onInputConnectionChanged(int inputNb) OVERRIDE FINAL;

    std::vector<std::string> sanitizePorts(const std::vector<std::string>& ports) const;

    void publishSpec(const std::vector<std::string>& ports);

    void applyPorts(const std::vector<std::string>& ports);

    void refreshKnobsEnabled();

    static void enableUnlinkedDimensions(KnobI* knob);

    // Writes text to the ports knob without calling knobChanged().
    void writePortsKnob(const std::string& text);

    std::map<std::string, std::string> getInputOutputs() const;

    void setInputOutputs(const std::map<std::string, std::string>& outputs);

    void pruneInputOutputs();

    const NetworkNodeLayout* _layout;
    NodeSpec _spec;
    std::atomic<std::shared_ptr<const SpecStorage>> _current;
    // applyPorts() disconnects ports that move and reconnects them at their new index; their
    // output names and knob states must survive that.
    bool _applyingPorts;
    // The node each port was last connected to, not saved.
    std::map<std::string, NodeWPtr> _portUpstreams;
    // Attributes of one ramp share a knob.
    std::map<std::string, KnobIWPtr> _attributeKnobs;
    // The knobs disabled because a port of theirs is connected; only these are enabled again, so
    // a knob disabled for any other reason stays disabled.
    std::set<KnobI*> _disabledKnobs;
    KnobStringWPtr _portsKnob;
    KnobStringWPtr _inputOutputsKnob;
};

/**
 * @brief Replaces the ports of a network node, undoably: undo restores the ports, their
 * connections and the outputs they read.
 **/
class NetworkEditPortsCommand
    : public UndoCommand {
public:
    NetworkEditPortsCommand(const NodePtr& node,
                            const std::vector<std::string>& ports,
                            const std::string& text = "Edit Ports");

    virtual void redo() OVERRIDE FINAL;

    virtual void undo() OVERRIDE FINAL;

private:
    NodeWPtr _node;
    std::vector<std::string> _before;
    std::vector<std::string> _after;
    std::map<std::string, NodeWPtr> _connections;
    std::map<std::string, std::string> _outputs;
};

/**
 * @brief The NetworkEditPortsCommand that promotes (or demotes) attribute of node. False when node
 * is not a network node or attribute cannot be promoted (or demoted); command is then null, and
 * also when attribute already is (or is not) a port.
 **/
bool makeNetworkPortCommand(const NodePtr& node,
                            const std::string& attribute,
                            bool promote,
                            UndoCommandPtr* command);

/**
 * @brief Promotes attribute of node, a network node, through a NetworkEditPortsCommand pushed on
 * node. True when attribute is then a port; false, pushing nothing, when node is not a network
 * node or attribute is not linkable. Main thread.
 **/
bool promoteNetworkPort(const NodePtr& node,
                        const std::string& attribute);

/**
 * @brief Demotes attribute of node, a network node, through a NetworkEditPortsCommand pushed on
 * node, disconnecting it. True when attribute is then not a port; false, pushing nothing, when
 * node is not a network node or attribute is port-only or not an attribute. Main thread.
 **/
bool demoteNetworkPort(const NodePtr& node,
                       const std::string& attribute);

std::string networkJoinPorts(const std::vector<std::string>& ports);

std::vector<std::string> networkSplitPorts(const std::string& text);

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Network_NetworkNode_h
