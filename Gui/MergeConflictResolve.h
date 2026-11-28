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

#ifndef Gui_MergeConflictResolve_h
#define Gui_MergeConflictResolve_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <functional>
#include <list>
#include <string>
#include <vector>

#include <QString>
#include <QUndoCommand>

#include "Engine/EngineFwd.h"
#include "Engine/ProjectMerge.h"
#include "Gui/GuiFwd.h"
#include "Serialization/Merge/MergeConflict.h"
#include "Serialization/ProjectSerialization.h"

NATRON_NAMESPACE_ENTER

struct TemplateEdit;

/**
 * @brief An undo step that also puts back the project's template imports and merge conflicts as
 * they were when it was made. Its children carry the graph edits of the step; the state after it
 * is the one captureAfter() records.
 **/
class MergeStateCommand
    : public QUndoCommand {
public:
    MergeStateCommand(const ProjectPtr& project,
                      const QString& text);

    void captureAfter();

    // Whether the step changes anything: a graph edit, an import or a conflict.
    bool hasEffect() const;

    virtual void undo() OVERRIDE FINAL;
    virtual void redo() OVERRIDE FINAL;

private:
    void restore(const std::list<Serialization::TemplateImportSerialization>& imports,
                 const std::list<Serialization::TemplateImportSerialization>& reference,
                 const std::vector<Serialization::MergeConflict>& conflicts);

    ProjectWPtr _project;
    std::list<Serialization::TemplateImportSerialization> _importsBefore;
    std::list<Serialization::TemplateImportSerialization> _importsAfter;
    std::vector<Serialization::MergeConflict> _conflictsBefore;
    std::vector<Serialization::MergeConflict> _conflictsAfter;
};

/**
 * @brief The template and merge actions of the GUI, each one undo step on the node graph's stack.
 **/
namespace MergeConflictResolve {
/**
 * @brief Imports the template file at path into the graph last worked in. Returns the new
 * import's key, or an empty string with error set when nothing was imported. A template without
 * nodes is still imported. When only part of it could be imported, the key is returned and error
 * lists the problems.
 **/
std::string importTemplate(Gui* gui,
                           const QString& path,
                           QString* error);

/**
 * @brief ProjectMerge::updateFromTemplate() on the open project as one undo step, which also puts
 * the import's base and the saved conflicts back. An empty path is the file the import records.
 * A partly applied update is still recorded so it can be undone. When the update raised
 * conflicts, the graph comparison is cleared and the first conflicted node framed.
 **/
ProjectMerge::UpdateResult updateFromTemplate(Gui* gui,
                                              const std::string& templateKey,
                                              const QString& path,
                                              bool allowMismatch = false);

/**
 * @brief Calls edit with a TemplateEdit whose applier records its graph edits, with the template
 * imports and merge conflicts, as one undo step titled text on graph, or on the graph last worked
 * in when graph is null. What edit applied before failing is still recorded. When the edit raised
 * conflicts, the graph comparison is cleared and the first conflicted node framed. Returns what
 * edit returned.
 **/
bool runTemplateEdit(Gui* gui,
                     NodeGraph* graph,
                     const QString& text,
                     const std::function<bool(TemplateEdit*)>& edit);

/**
 * @brief Gives each conflict at indices, in the project's list, the state side had, and removes
 * them from the list, as one undo step. Conflicts that cannot take a side are left; returns false
 * with error naming the first of them.
 **/
bool resolve(Gui* gui,
             const std::vector<std::size_t>& indices,
             ProjectMerge::ConflictSide side,
             QString* error);

/**
 * @brief Applies the handler option named option of the conflict at index and removes the
 * conflict from the list, as one undo step. Returns false with error set, leaving the conflict,
 * when the option cannot be applied.
 **/
bool resolveOption(Gui* gui,
                   std::size_t index,
                   const std::string& option,
                   QString* error);

/**
 * @brief Keeps what the merge chose for the conflicts at indices and removes them from the list,
 * as one undo step.
 **/
void accept(Gui* gui,
            const std::vector<std::size_t>& indices);

/**
 * @brief Where each of conflicts is in the project's list now, in their order, skipping those no
 * longer in it. A button or preview keeps the conflicts it was made for rather than their indices,
 * which shift when a conflict before them is resolved.
 **/
std::vector<std::size_t> indicesOf(Gui* gui,
                                   const std::vector<Serialization::MergeConflict>& conflicts);
} // namespace MergeConflictResolve

NATRON_NAMESPACE_EXIT

#endif // Gui_MergeConflictResolve_h
