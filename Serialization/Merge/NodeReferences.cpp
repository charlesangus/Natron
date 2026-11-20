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
    renameInQualifiedName(const std::string& name,
                          const NodeRenames& renames)
    {
        std::string result;
        std::size_t start = 0;
        for (;;) {
            const std::size_t dot = name.find('.', start);
            const std::string part = name.substr(start, (dot == std::string::npos) ? std::string::npos : dot - start);
            NodeRenames::const_iterator found = renames.find(part);
            result += (found == renames.end()) ? part : found->second;
            if (dot == std::string::npos) {
                return result;
            }
            result += '.';
            start = dot + 1;
        }
    }

} // namespace

std::string
renameNodesInExpression(std::string_view expression,
                        const NodeRenames& renames)
{
    if (renames.empty()) {
        return std::string(expression);
    }
    std::string result;
    result.reserve(expression.size());
    std::size_t i = 0;
    while (i < expression.size()) {
        const char c = expression[i];
        if (!isIdentifierStart(c)) {
            result += c;
            ++i;
            continue;
        }
        std::size_t end = i + 1;
        while ((end < expression.size()) && isIdentifierChar(expression[end])) {
            ++end;
        }
        const std::string token(expression.substr(i, end - i));
        NodeRenames::const_iterator found = renames.find(token);
        const bool isReference = (end < expression.size()) && (expression[end] == '.');
        result += (found != renames.end() && isReference) ? found->second : token;
        i = end;
    }

    return result;
}

void
renameNodeReferences(std::list<KnobSerialization>* knobs,
                     const NodeRenames& renames)
{
    if (renames.empty()) {
        return;
    }
    for (KnobSerialization& knob : *knobs) {
        for (KnobDimensionSerialization& dim : knob.dimensions) {
            if (!dim.expression.empty()) {
                dim.expression = renameNodesInExpression(dim.expression, renames);
            }
            if (dim.master) {
                NodeRenames::const_iterator found = renames.find(dim.master->masterNodeName);
                if (found != renames.end()) {
                    dim.master->masterNodeName = found->second;
                }
                dim.master->masterNodeNameFull = renameInQualifiedName(dim.master->masterNodeNameFull, renames);
            }
        }
    }
}

void
renameNodeReferencesRecursive(NodeSerialization* node,
                              const NodeRenames& renames)
{
    renameNodeReferences(&node->knobs, renames);
    for (const NodeSerializationPtr& child : node->children) {
        renameNodeReferencesRecursive(child.get(), renames);
    }
}

} // namespace Serialization
NATRON_NAMESPACE_EXIT
