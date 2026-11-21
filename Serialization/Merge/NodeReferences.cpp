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

#include "Serialization/Merge/NodeReferences.h"

#include <algorithm>
#include <list>

#include "Serialization/KnobSerialization.h"
#include "Serialization/NodeSerialization.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

namespace {

    bool
    isIdentifierStart(char c)
    {
        return ((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) || (c == '_');
    }

    bool
    isIdentifierChar(char c)
    {
        return isIdentifierStart(c) || ((c >= '0') && (c <= '9'));
    }

    std::string
    childPath(const std::string& group,
              const std::string& name)
    {
        return group.empty() ? name : group + "." + name;
    }

    const std::string*
    renamed(const ScopedNodeRenames& renames,
            const std::string& group,
            const std::string& name)
    {
        ScopedNodeRenames::const_iterator scope = renames.find(group);
        if (scope == renames.end()) {
            return nullptr;
        }
        NodeRenames::const_iterator found = scope->second.find(name);

        return (found == scope->second.end()) ? nullptr : &found->second;
    }

    bool
    namesTopLevel(const std::string& token)
    {
        if (token.compare(0, 3, "app") != 0) {
            return false;
        }

        return std::all_of(token.begin() + 3, token.end(), [](char c) { return (c >= '0') && (c <= '9'); });
    }

    std::string
    renameInQualifiedName(const std::string& name,
                          const ScopedNodeRenames& renames)
    {
        std::string result;
        std::string group;
        std::size_t start = 0;
        for (;;) {
            const std::size_t dot = name.find('.', start);
            const std::string part = name.substr(start, (dot == std::string::npos) ? std::string::npos : dot - start);
            const std::string* found = renamed(renames, group, part);
            result += found ? *found : part;
            if (dot == std::string::npos) {
                return result;
            }
            result += '.';
            group = childPath(group, part);
            start = dot + 1;
        }
    }

} // namespace

std::string
renameNodesInExpression(std::string_view expression,
                        const ScopedNodeRenames& renames,
                        const std::string& group)
{
    if (renames.empty()) {
        return std::string(expression);
    }
    const std::size_t size = expression.size();
    std::string result;
    result.reserve(size);
    std::size_t i = 0;
    while (i < size) {
        const char c = expression[i];
        if ((c == '\'') || (c == '"')) {
            std::size_t end = i + 1;
            while ((end < size) && (expression[end] != c)) {
                end += (expression[end] == '\\') ? 2 : 1;
            }
            end = std::min(end + 1, size);
            result.append(expression.substr(i, end - i));
            i = end;
            continue;
        }
        if (!isIdentifierStart(c) || ((i > 0) && isIdentifierChar(expression[i - 1]))) {
            result += c;
            ++i;
            continue;
        }
        // After a '.' that ends no chain, as in f().Grade1, the name is an attribute of a value.
        bool references = (i == 0) || (expression[i - 1] != '.');
        std::string scope = group;
        bool first = true;
        for (;;) {
            std::size_t end = i + 1;
            while ((end < size) && isIdentifierChar(expression[end])) {
                ++end;
            }
            const std::string token(expression.substr(i, end - i));
            const bool followed = (end < size) && (expression[end] == '.');
            if (!followed || !references) {
                result += token;
                i = end;
                if (!followed) {
                    break;
                }
            } else {
                if (first && (token == "thisGroup")) {
                    result += token;
                } else if (first && namesTopLevel(token)) {
                    result += token;
                    scope.clear();
                } else {
                    const std::string* found = renamed(renames, scope, token);
                    result += found ? *found : token;
                    scope = childPath(scope, token);
                }
                i = end;
            }
            result += '.';
            ++i;
            first = false;
            if ((i >= size) || !isIdentifierStart(expression[i])) {
                break;
            }
        }
    }

    return result;
}

void
renameNodeReferences(std::list<KnobSerialization>* knobs,
                     const ScopedNodeRenames& renames,
                     const std::string& group)
{
    if (renames.empty()) {
        return;
    }
    for (KnobSerialization& knob : *knobs) {
        for (KnobDimensionSerialization& dim : knob.dimensions) {
            if (!dim.expression.empty()) {
                dim.expression = renameNodesInExpression(dim.expression, renames, group);
            }
            if (dim.master) {
                const std::string& full = dim.master->masterNodeNameFull;
                const std::size_t dot = full.rfind('.');
                const std::string masterGroup = full.empty() ? group : ((dot == std::string::npos) ? std::string() : full.substr(0, dot));
                if (const std::string* found = renamed(renames, masterGroup, dim.master->masterNodeName)) {
                    dim.master->masterNodeName = *found;
                }
                dim.master->masterNodeNameFull = renameInQualifiedName(full, renames);
            }
        }
    }
}

void
renameNodeReferencesRecursive(NodeSerialization* node,
                              const ScopedNodeRenames& renames,
                              const std::string& group)
{
    renameNodeReferences(&node->knobs, renames, group);
    const std::string inside = childPath(group, node->scriptName);
    for (const NodeSerializationPtr& child : node->children) {
        renameNodeReferencesRecursive(child.get(), renames, inside);
    }
}

} // namespace Serialization
NATRON_NAMESPACE_EXIT
