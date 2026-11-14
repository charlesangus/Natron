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

#ifndef NATRON_TESTS_SCENETESTAUTHORING_H
#define NATRON_TESTS_SCENETESTAUTHORING_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Engine/Nodes/Scene/UsdIncludes.h"

#include "Global/Macros.h"

#include <filesystem>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/SceneLoadSet.h"
#include "Engine/ScenePayload.h"

NATRON_NAMESPACE_ENTER

// /World/Block_<b>/B_<n>, each building a payload file holding /Geo, /car_0 and the mesh /Panel,
// the first one also the camera /Cam and /Reset, a transform that resets its stack, beside the mesh /World/Ground outside every payload. The
// files sit in a directory of their own so that no other test has their layers open.
class CarCity {
public:
    // authorExtentsHints puts an extentsHint of (0, 0, 0) to (2, 4, 6) on every building prim,
    // outside its payload, which its loaded content does not fill.
    CarCity(int blocks,
            int buildingsPerBlock,
            bool authorExtentsHints = false);

    ~CarCity();

    CarCity(const CarCity&) = delete;
    CarCity& operator=(const CarCity&) = delete;

    std::string rootPath() const;

    // A payload of the root layer alone.
    ScenePayload payload() const;

    int getNumBuildings() const;

    // How many building files are open as layers right now.
    int openedBuildings() const;

private:
    std::filesystem::path _dir;
    std::vector<std::string> _buildingPaths;
};

// The car of building index in block.
std::string carPath(int block,
                    int index);

// A load set with nothing loaded but the subtrees of paths.
SceneLoadSetPtr loadingOnly(const std::vector<std::string>& paths);

/**
 * @brief Evaluates node in a viewer context with loadSet and in the default one, opens both
 * results with the payloads of loadSet, and expects the same prims with the same properties,
 * leaving out what lies below a payload loadSet leaves unloaded.
 **/
void expectParity(const NodePtr& node,
                  const SceneLoadSetPtr& loadSet);

NATRON_NAMESPACE_EXIT

#endif // NATRON_TESTS_SCENETESTAUTHORING_H
