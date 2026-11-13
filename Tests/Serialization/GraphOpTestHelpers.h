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

#ifndef Tests_Serialization_GraphOpTestHelpers_h
#define Tests_Serialization_GraphOpTestHelpers_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cctype>
#include <cstdio>
#include <list>
#include <string>

#include <gtest/gtest.h>

#include "Serialization/KnobSerialization.h"
#include "Serialization/NodeSerialization.h"
#include "Serialization/Ops/SnapshotDiff.h"
#include "Serialization/ProjectSerialization.h"
#include "Serialization/Yaml/BlobSink.h"
#include "Serialization/Yaml/ProjectYaml.h"

namespace GraphOpTest {

// A well-formed node ID, distinct for each n.
inline std::string
nid(int n)
{
    char buffer[33];
    std::snprintf(buffer, sizeof(buffer), "%032x", n);

    return std::string(buffer);
}

// text with each @<number> replaced by nid(number), so fixtures stay readable.
inline std::string
withIds(const std::string& text)
{
    std::string result;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if ((text[i] == '@') && (i + 1 < text.size()) && std::isdigit((unsigned char)text[i + 1])) {
            std::size_t end = i + 1;
            while ((end < text.size()) && std::isdigit((unsigned char)text[end])) {
                ++end;
            }
            result += nid(std::stoi(text.substr(i + 1, end - i - 1)));
            i = end - 1;
        } else {
            result += text[i];
        }
    }

    return result;
}

// A project whose project.yaml body (knobs, formats, layers, nodes...) is body, IDs written @<n>.
inline NATRON_NAMESPACE::Serialization::ProjectSerialization
projectFromYaml(const std::string& body)
{
    NATRON_NAMESPACE::Serialization::NoBlobSource source;

    return NATRON_NAMESPACE::Serialization::decodeProjectYaml("format: natron-project\nformatVersion: 1\n" + withIds(body), source);
}

// Knobs in name order, the project's variables last and in their own order, which is meaningful.
inline void
sortKnobs(std::list<NATRON_NAMESPACE::Serialization::KnobSerialization>* knobs)
{
    knobs->sort([](const NATRON_NAMESPACE::Serialization::KnobSerialization& a, const NATRON_NAMESPACE::Serialization::KnobSerialization& b) {
        if (a.isUserKnob || b.isUserKnob) {
            return !a.isUserKnob && b.isUserKnob;
        }

        return a.name < b.name;
    });
}

inline void
sortNodeKnobs(std::list<NATRON_NAMESPACE::Serialization::NodeSerializationPtr>* nodes)
{
    for (NATRON_NAMESPACE::Serialization::NodeSerializationPtr& node : *nodes) {
        sortKnobs(&node->knobs);
        sortNodeKnobs(&node->children);
    }
}

// The project.yaml text of what graph operations carry (see editableState()).
inline std::string
editableText(const NATRON_NAMESPACE::Serialization::ProjectSerialization& project)
{
    NATRON_NAMESPACE::Serialization::InlineBlobSink sink;

    return NATRON_NAMESPACE::Serialization::encodeProjectYaml(NATRON_NAMESPACE::Serialization::editableState(project), sink);
}

// editableText() with knobs in name order: applyOps() adds a knob set for the first time after
// the others, where a live node keeps its plug-in's order.
inline std::string
canonicalText(const NATRON_NAMESPACE::Serialization::ProjectSerialization& project)
{
    NATRON_NAMESPACE::Serialization::ProjectSerialization state = NATRON_NAMESPACE::Serialization::editableState(project);
    sortKnobs(&state.projectKnobs);
    sortNodeKnobs(&state.nodes.nodes);
    NATRON_NAMESPACE::Serialization::InlineBlobSink sink;

    return NATRON_NAMESPACE::Serialization::encodeProjectYaml(state, sink);
}

#define EXPECT_SAME_PROJECT(expected, actual) EXPECT_EQ(GraphOpTest::canonicalText(expected), GraphOpTest::canonicalText(actual))

} // namespace GraphOpTest

#endif // Tests_Serialization_GraphOpTestHelpers_h
