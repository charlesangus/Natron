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

#ifndef Tests_Serialization_ProjectEquivalence_h
#define Tests_Serialization_ProjectEquivalence_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <map>
#include <string>

#include <gtest/gtest.h>

#include "Engine/EngineFwd.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief What a live project holds, read from the live objects rather than from a serialization,
 * so that two projects can be compared after one of them has been reset: one entry per fact,
 * keyed by where it lives (e.g. "node Group1.Blur2 knob size[0] curve"), with every double
 * written as its bit pattern.
 *
 * It covers the node tree (fully qualified names, plug-in ID and version), each node's inputs by
 * label, its position, size, user colour and overlay colour, every persistent knob of the nodes and of the project (raw values, curves with their
 * derivatives and interpolations, animated strings, parametric curves, choice options,
 * expressions and links), the project layer registry, the additional formats and the timeline.
 * Nodes that are not part of the project, such as the internal nodes of a RotoPaint, are left
 * out, and so are the project knobs that say where and when the file was saved rather than what
 * it holds.
 **/
struct ProjectSnapshot {
    std::map<std::string, std::string> entries;
};

ProjectSnapshot captureProjectSnapshot(const ProjectPtr& project);

/**
 * @brief Success when both snapshots hold the same entries; otherwise the message lists every
 * entry that is missing, extra or different, up to a limit.
 **/
::testing::AssertionResult projectsAreEquivalent(const ProjectSnapshot& expected,
                                                 const ProjectSnapshot& actual);

NATRON_NAMESPACE_EXIT

#endif // Tests_Serialization_ProjectEquivalence_h
