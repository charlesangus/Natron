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

#ifndef Engine_Nodes_Scene_Utility_EmbreeTracer_h
#define Engine_Nodes_Scene_Utility_EmbreeTracer_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Engine/Nodes/Scene/UsdIncludes.h"

#include <memory>

#include "Global/Macros.h"

#include "Engine/Nodes/Scene/Utility/Tracer.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Traces with Embree on the CPU: one scene per geometry under one scene of instances.
 **/
class EmbreeTracer : public Tracer {
public:
    EmbreeTracer();
    virtual ~EmbreeTracer();

    virtual bool update(const UtilityScene::Snapshot& snapshot,
                        int threads,
                        const std::atomic<bool>* stop = nullptr) OVERRIDE;
    virtual void intersect(const RayBatch& rays,
                           const HitBatch& hits) const OVERRIDE;
    virtual void occluded(const RayBatch& rays,
                          std::uint8_t* blocked,
                          const OcclusionFilter* filter = nullptr) const OVERRIDE;
    virtual Stats stats() const OVERRIDE;
    virtual Outcome status() const OVERRIDE;

private:
    struct Implementation;
    std::unique_ptr<Implementation> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_Utility_EmbreeTracer_h
