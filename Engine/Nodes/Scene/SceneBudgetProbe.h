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

#ifndef NATRON_ENGINE_SCENEBUDGETPROBE_H
#define NATRON_ENGINE_SCENEBUDGETPROBE_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <cstdint>
#include <string>

NATRON_NAMESPACE_ENTER

/**
 * @brief A bench-only Storm scene index that measures what the changes a viewport polygon budget
 * would make cost Storm, before that budget exists: holding a share of the gprims invisible, masking
 * a share of every instancer's instances, and omitting a share of the faces of a set of meshes
 * through their topology's invisibleFaces.
 *
 * It sits after every other filter of the Storm render index, so that prim types are final (an
 * implicit cube is a mesh there) and its notices reach the render index the way the budget's would.
 * It exists only when the environment sets NATRON_BUDGET_PROBE=1 before the first scene viewer
 * starts; otherwise registerForStorm() does nothing and no viewer pays for it.
 *
 * Kept free of OpenUSD's headers so the viewer can include it.
 */
namespace SceneBudgetProbe {

enum KindEnum {
    // Changes nothing: the frame that follows is a steady frame to compare the others with.
    eKindNone,
    // Holds invisible the gprims no instancer draws, a share of them chosen by a hash of the path.
    eKindGprims,
    // Masks off a share of every instancer's instances, chosen by a hash of the instance index.
    eKindInstances,
    // Lists a share of the faces of kFaceMeshes meshes as invisibleFaces.
    eKindFaces,
    // Undoes whatever the probe changed.
    eKindClear
};

constexpr std::size_t kFaceMeshes = 1000;

bool isEnabled();

void registerForStorm();

// False if name is not one of none, gprims, instances, faces or clear.
bool kindFromName(const std::string& name, KindEnum* kind);
const char* kindName(KindEnum kind);

struct Result {
    // Counts the probes serviced, so a caller can tell its own result from an earlier one.
    std::uint64_t serial = 0;
    KindEnum kind = eKindNone;
    double share = 0.;
    // Only chose what to change, and sent nothing.
    bool prepareOnly = false;
    // A probe scene index served the viewer's engine.
    bool found = false;
    // Gprims, instances or meshes the kind chose among, and the ones it changed.
    std::size_t candidates = 0;
    std::size_t selected = 0;
    // Faces listed invisible, for eKindFaces.
    std::size_t faces = 0;
    // Meshes among the selected ones that an instancer draws.
    std::size_t instancedMeshes = 0;
    std::size_t notices = 0;
    double prepareMs = 0.;
    // The render index taking in the notices.
    double sendMs = 0.;
};

/**
 * @brief Asks the next service() to probe kind on share (0 to 1) of what it applies to. With
 * prepareOnly it only chooses, and the next request of the same kind and share applies that choice
 * without paying for it again. A later request replaces one not serviced yet. Any thread.
 **/
void request(KindEnum kind, double share, bool prepareOnly);

/**
 * @brief Runs the pending request on the probe of the engine whose render instance is
 * renderInstanceId. Needs what SceneDrawFilter's changes need: the thread that renders with that
 * engine, between renders, with the viewer lock held.
 **/
void service(const std::string& renderInstanceId);

// The last serviced request's result. Any thread.
Result lastResult();
} // namespace SceneBudgetProbe

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_SCENEBUDGETPROBE_H
