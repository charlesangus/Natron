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

#ifndef Engine_ProjectTemplate_h
#define Engine_ProjectTemplate_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <set>
#include <string>
#include <vector>

#include <QString>

#include "Engine/EngineFwd.h"
#include "Engine/ProjectMerge.h"
#include "Serialization/Ops/GraphOp.h"
#include "Serialization/ProjectSerialization.h"
#include "Serialization/TemplateNodeSerialization.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief One template edit of the project (Define, Add to, Remove from, Detach and Publish
 * Template): the graph operations it made and how they went. The caller sets applier, so the
 * GUI can apply them as one undo step; the template entry and saved conflicts it changes are
 * set apart from them, as an update merge sets them.
 **/
struct TemplateEdit {
    // Applies the operations; ProjectMerge::applyAllOrNothing() when empty.
    ProjectMerge::OpApplier applier;

    // The template import the edit is about, the template node standing for it, and for a
    // publish the file written.
    std::string key;
    NodePtr templateNode;
    QString path;

    QString error;
    std::vector<Serialization::GraphOp> ops;
    std::vector<Serialization::OpResult> results;
};

namespace ProjectTemplateDetail {
/**
 * @brief name as a template key or file stem: letters, digits and underscores, others replaced by
 * underscores, "template" when nothing is left.
 **/
std::string keyStem(const QString& name);

/**
 * @brief stem, or stem with a higher trailing number, so that it is neither the key nor the name of
 * an import of project, nor the key of one of its template nodes, removed ones included.
 **/
std::string uniqueTemplateKey(const std::string& stem,
                              const Project& project);

/**
 * @brief name, or name with a higher trailing number, so that it is free in group and not in picked.
 **/
std::string uniqueScriptName(const std::string& name,
                             const NodeCollectionPtr& group,
                             const std::set<std::string>& picked);

// A new node or template ID: a UUID as 32 lowercase hex digits.
std::string newId();

/**
 * @brief A new template node holding state, positioned at x, y when hasPosition is set.
 **/
Serialization::NodeSerializationPtr makeTemplateNode(const Serialization::TemplateNodeState& state,
                                                     const std::string& scriptName,
                                                     bool hasPosition,
                                                     double x,
                                                     double y);

/**
 * @brief Where a template node goes above nodes: over their top-left corner. False when none of
 * them has a position.
 **/
bool positionAbove(const std::list<Serialization::NodeSerializationPtr>& nodes,
                   double* x,
                   double* y);

/**
 * @brief Applies edit->ops through edit->applier. False with edit->error set when one of them
 * did not apply.
 **/
bool applyOps(const ProjectPtr& project,
              TemplateEdit* edit);

/**
 * @brief The name the Python API gives an update status: "updated", "upToDate",
 * "templateMismatch", "lowMatch" or "error".
 **/
QString updateStatusName(ProjectMerge::UpdateStatus status);
} // namespace ProjectTemplateDetail

NATRON_NAMESPACE_EXIT

#endif // Engine_ProjectTemplate_h
