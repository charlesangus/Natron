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

#ifndef Engine_Nodes_Scene_Utility_WavefrontCpu_h
#define Engine_Nodes_Scene_Utility_WavefrontCpu_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Engine/Nodes/Scene/UsdIncludes.h"

#include <memory>

#include "Global/Macros.h"

#include "Engine/Nodes/Scene/Utility/Tracer.h"
#include "Engine/Nodes/Scene/Utility/Wavefront.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Runs the wavefront's stages on the CPU, one tile per task on the global thread pool. Its tracer
 * outlives each render, so an edit only pays for what it changed.
 **/
class WavefrontCpu : public WavefrontExecutor {
public:
    WavefrontCpu();
    virtual ~WavefrontCpu();

    virtual bool render(const UtilityScene::Snapshot& snapshot,
                        const WavefrontFrame& frame,
                        WavefrontFilm& film,
                        const std::atomic<bool>& stop,
                        int threads) OVERRIDE;

private:
    std::unique_ptr<Tracer> _tracer;
    WavefrontShadeTables _shadeTables;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_Utility_WavefrontCpu_h
