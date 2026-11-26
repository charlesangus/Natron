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

#ifndef Engine_Nodes_Network_NetworkBuilder_h
#define Engine_Nodes_Network_NetworkBuilder_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>
#include <vector>

#include "Engine/NodeGroup.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief A group whose sub-graph is a network: the nodes inside are network nodes wired by their
 * ports, and the builder turns them into one result for its targets. Users dive into it like any
 * group; it starts empty.
 **/
class NetworkBuilder
    : public NodeGroup {
public:
    NetworkBuilder(const NodePtr& node,
                   const std::vector<std::string>& targets);

    virtual ~NetworkBuilder();

    // The targets the network is built for, in the order their stripes are drawn.
    const std::vector<std::string>& getTargets() const
    {
        return _targets;
    }

    bool supportsTarget(const std::string& target) const;

    /**
     * @brief Whether pluginId is a network node type (see NetworkNode::registerPlugin()) with one of
     * this builder's targets: the types a user creates inside its graph, though they are for
     * internal use only.
     **/
    bool hostsPlugin(const std::string& pluginId) const;

    virtual bool isSubGraphUserVisible() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

private:
    const std::vector<std::string> _targets;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Network_NetworkBuilder_h
