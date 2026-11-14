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

#ifndef Engine_Nodes_Scene_Utility_UtilitySubdiv_h
#define Engine_Nodes_Scene_Utility_UtilitySubdiv_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Engine/Nodes/Scene/UsdIncludes.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec3i.h>
#include <pxr/base/vt/array.h>
#include <pxr/imaging/pxOsd/meshTopology.h>

#include "Global/Macros.h"

#define kUtilitySettingSubdivLevel "subdivLevel"
#define kUtilitySettingMaxSubdivTriangles "maxSubdivTriangles"
#define kUtilityPrimvarSubdivLevel "natron:subdivLevel"

NATRON_NAMESPACE_ENTER

/**
 * @brief A mesh's topology uniformly refined by OpenSubdiv to one level, with the stencil tables that
 * carry coarse values to the refined ones. Immutable once made, and shared through a process-wide
 * cache keyed by topology, level and face-varying channels, so instances of one cage and its later
 * point edits or motion samples never refine again: they only re-apply the stencils. MT-safe.
 **/
class UtilitySubdivRefinement {
public:
    static const int kDefaultLevel = 2;
    static const int kMaxLevel = 6;
    static const std::size_t kDefaultMaxTriangles = 50000000;

    struct Stats {
        // Refinements begun, whether finished, failed or stopped; a caller joining one does not count.
        std::size_t refinementsStarted = 0;
        std::size_t refinerBuilds = 0;
        std::size_t evaluations = 0;
        std::size_t cachedEntries = 0;
        std::size_t cachedBytes = 0;
    };

    // Polled while refining or evaluating, from any thread: true asks the work to end. Must be MT-safe.
    typedef std::function<bool()> StopCheck;

    enum class Outcome {
        Done,
        Failed,
        Stopped,
    };

    struct Impl;

    explicit UtilitySubdivRefinement(std::unique_ptr<Impl> impl);
    ~UtilitySubdivRefinement();

    /**
     * @brief The refinement of topology to level (at least 1), from the cache or made now. Each entry of
     * faceVaryingIndices is one face-varying channel's value index per face-vertex. Null, with error set,
     * when the scheme is none or OpenSubdiv cannot refine the topology.
     **/
    static std::shared_ptr<const UtilitySubdivRefinement> get(const PXR_NS::PxOsdMeshTopology& topology,
                                                              int level,
                                                              const std::vector<PXR_NS::VtIntArray>& faceVaryingIndices,
                                                              std::string* error);
    /**
     * @brief As above, ending soon after stopped returns true: the caller gets Stopped back within a
     * poll interval however long the refinement would take, and nothing is cached for it. A long
     * refinement is shared by every caller asking for it while it runs and halts once none waits, so
     * a caller asking again before then resumes it.
     **/
    static std::shared_ptr<const UtilitySubdivRefinement> get(const PXR_NS::PxOsdMeshTopology& topology,
                                                              int level,
                                                              const std::vector<PXR_NS::VtIntArray>& faceVaryingIndices,
                                                              const StopCheck& stopped,
                                                              Outcome* outcome,
                                                              std::string* error);

    // Triangles uniform refinement to level gives, holes excluded, without refining.
    static std::size_t estimateTriangles(const PXR_NS::PxOsdMeshTopology& topology,
                                         int level);
    // The highest level up to level whose triangles fit within maxTriangles, at least 0.
    static int fitLevel(const PXR_NS::PxOsdMeshTopology& topology,
                        int level,
                        std::size_t maxTriangles);

    static Stats stats();
    static void setCacheBudget(std::size_t bytes);
    static void clearCache();

    int level() const;
    std::size_t coarsePointCount() const;
    std::size_t refinedPointCount() const;
    // Into the refined points, wound as HdMeshUtil winds the cage's.
    const PXR_NS::VtVec3iArray& triangles() const;
    // The cage face each triangle comes from.
    const PXR_NS::VtIntArray& coarseFaces() const;
    int faceVaryingChannelCount() const;
    std::size_t refinedFaceVaryingCount(int channel) const;
    // Per triangle, the refined face-varying value of each corner in channel.
    const PXR_NS::VtVec3iArray& faceVaryingTriangles(int channel) const;
    std::size_t memoryBytes() const;

    // The refined points at the refinement's level.
    void refine(const PXR_NS::VtVec3fArray& coarse,
                PXR_NS::VtVec3fArray* refined) const;
    /**
     * @brief The refined points carried to the limit surface, with unit limit normals there. Normals
     * stay empty for the bilinear scheme, whose surface is faceted.
     **/
    void evaluate(const PXR_NS::VtVec3fArray& coarse,
                  PXR_NS::VtVec3fArray* points,
                  PXR_NS::VtVec3fArray* normals) const;
    // As above, unless stopped returns true first: then false, with points and normals left alone.
    bool evaluate(const PXR_NS::VtVec3fArray& coarse,
                  PXR_NS::VtVec3fArray* points,
                  PXR_NS::VtVec3fArray* normals,
                  const StopCheck& stopped) const;

    /**
     * @brief Carries count coarse values of width floats each to the refined ones in dst, which must hold
     * as many as the refinement gives. Vertex values follow the surface's rules, varying ones interpolate
     * linearly. Returns false, leaving dst alone, when count is below the coarse count.
     **/
    bool interpolateVertex(const float* src,
                           std::size_t count,
                           int width,
                           float* dst) const;
    bool interpolateVarying(const float* src,
                            std::size_t count,
                            int width,
                            float* dst) const;
    bool interpolateFaceVarying(int channel,
                                const float* src,
                                std::size_t count,
                                int width,
                                float* dst) const;

    // Empty when coarse holds fewer values than the cage has points.
    template <class T>
    PXR_NS::VtArray<T> interpolate(const PXR_NS::VtArray<T>& coarse,
                                   bool varying) const
    {
        PXR_NS::VtArray<T> refined(refinedPointCount());
        const float* src = reinterpret_cast<const float*>(coarse.cdata());
        float* dst = reinterpret_cast<float*>(refined.data());
        const int width = static_cast<int>(sizeof(T) / sizeof(float));
        const bool done = varying ? interpolateVarying(src, coarse.size(), width, dst) : interpolateVertex(src, coarse.size(), width, dst);

        return done ? refined : PXR_NS::VtArray<T>();
    }

    // Empty when coarse holds fewer values than the channel indexes.
    template <class T>
    PXR_NS::VtArray<T> interpolateFaceVarying(int channel,
                                              const PXR_NS::VtArray<T>& coarse) const
    {
        PXR_NS::VtArray<T> refined(refinedFaceVaryingCount(channel));
        const bool done = interpolateFaceVarying(channel, reinterpret_cast<const float*>(coarse.cdata()), coarse.size(), static_cast<int>(sizeof(T) / sizeof(float)), reinterpret_cast<float*>(refined.data()));

        return done ? refined : PXR_NS::VtArray<T>();
    }

private:
    std::unique_ptr<Impl> _imp;
};

typedef std::shared_ptr<const UtilitySubdivRefinement> UtilitySubdivRefinementPtr;

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_Utility_UtilitySubdiv_h
