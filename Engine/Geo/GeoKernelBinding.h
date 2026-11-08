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

#ifndef Engine_Geo_GeoKernelBinding_h
#define Engine_Geo_GeoKernelBinding_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Global/GlobalDefines.h"

#include "Engine/GeoDetail.h"
#include "Engine/Kernel/KernelBinding.h"
#include "Engine/Kernel/KernelRunner.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Exposes geometry to the kernel language for a run over one element class.
 *
 * Builtins: @ptnum @primnum @vtxnum @elemnum @numpt @numprim @numvtx @numelem @Frame @Time.
 * Element numbers are dense; an element number that does not apply to the run class is -1.
 *
 * An @ attribute is looked up when the kernel runs: in the run class first, then, for a vertex
 * run, the vertex's point and primitive, then the detail. Missing attributes read as zero, and a
 * written attribute that does not exist is created on the run class. @opinputN_name reads the
 * element with the same number in input N.
 *
 * Functions: point prim vertex detail (any non-array result), npoints nprimitives nvertices,
 * nearpoint nearpoints, inpointgroup inprimgroup setpointgroup setprimgroup, addpoint addprim
 * addvertex removepoint removeprim setprimvertex. Their input and geometry handle arguments and
 * their attribute and group names must be literals; the only writable geometry handle is 0.
 **/
class GeoKernelBinding : public KernelBinding {
public:
    explicit GeoKernelBinding(GeoAttribClassEnum runOver);

    GeoAttribClassEnum runOver() const
    {
        return _runOver;
    }

    std::optional<KernelVariable> builtinVariable(std::string_view name) const override;
    KernelBindResult bindAttribute(const KernelAttributeRequest& request) const override;
    void functions(std::string_view name, std::vector<KernelFunctionSignature>& overloads) const override;

private:
    GeoAttribClassEnum _runOver;
};

struct GeoKernelRunOptions {
    double frame = 0.;
    double time = 0.;
    // 0 uses the current thread budget.
    int threads = 0;
    std::function<bool()> aborted;
};

struct GeoKernelRunResult {
    KernelRunStatus status = KernelRunStatus::eDone;
    std::string error;
    // The input 0 geometry unchanged unless status is eDone.
    GeoDetail detail;
    std::uint64_t batches = 0;
};

/**
 * @brief Runs a program compiled against GeoKernelBinding(runOver) over every live element of
 * that class in inputs[0], or once for the detail class. inputs[0] must not be null; other inputs
 * may be, and then read as empty geometry.
 **/
GeoKernelRunResult geoKernelRun(const KernelProgram& program,
                                GeoAttribClassEnum runOver,
                                std::span<const GeoDetail* const> inputs,
                                const GeoKernelRunOptions& options = GeoKernelRunOptions());

/** @brief Compiles source against GeoKernelBinding(runOver) and runs it; a compile error fails the run. */
GeoKernelRunResult geoKernelRun(std::string_view source,
                                GeoAttribClassEnum runOver,
                                std::span<const GeoDetail* const> inputs,
                                const GeoKernelRunOptions& options = GeoKernelRunOptions());

NATRON_NAMESPACE_EXIT

#endif // Engine_Geo_GeoKernelBinding_h
