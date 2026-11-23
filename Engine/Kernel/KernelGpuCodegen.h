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

#ifndef Engine_Kernel_KernelGpuCodegen_h
#define Engine_Kernel_KernelGpuCodegen_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Global/GlobalDefines.h"

#include "Engine/Kernel/KernelTypes.h"

NATRON_NAMESPACE_ENTER

/** @brief How a value a context provides reaches a GPU run. */
enum class KernelGpuForm : std::uint8_t {
    // A value per element, gathered before the run and scattered after it.
    ePerElement,
    // One value for the whole run, which the host evaluates.
    eUniform,
    eUnsupported,
};

/**
 * @brief What a context (geometry, and later others) can hand to a GPU run. The generator asks it
 * about every binding and every context function the program uses, so it never sees the
 * context's data types.
 **/
class KernelGpuTarget {
public:
    virtual ~KernelGpuTarget() = default;

    /** @brief why receives a short reason when the result is eUnsupported. */
    virtual KernelGpuForm bindingForm(const KernelProgramBinding& binding, std::string* why) const = 0;

    /**
     * @brief eUniform when every call of function gives the same results throughout a run, so the
     * host can evaluate it once; eUnsupported otherwise. ePerElement is not accepted.
     **/
    virtual KernelGpuForm functionForm(const KernelProgramFunction& function) const = 0;
};

/**
 * @brief One word per element: column c of element e is word c * stride + e of the data buffer.
 * Floats are stored as their bits.
 **/
struct KernelGpuColumn {
    KernelBank bank = KernelBank::eFloat;
    std::int32_t reg = 0;
    // Loaded into the register before the body runs; otherwise the register starts at zero.
    bool load = false;
    // Stored back after the body ran.
    bool store = false;
};

/**
 * @brief One word of the uniform buffer: a register of a uniform binding, or a result of a call
 * the host evaluates, whichever of binding and instruction is set.
 **/
struct KernelGpuUniform {
    KernelOperand operand;
    std::int32_t binding = -1;
    std::int32_t instruction = -1;
    std::uint32_t result = 0;
};

/**
 * @brief A program lowered to a GLSL compute shader, entry point "main", one thread per element.
 *
 * Descriptor set 0: binding 0 "data" (the columns, read and written), binding 1 "uni" (the
 * uniforms, read only), binding 2 "status" (one word, set to non-zero when an element ran past
 * the jump budget, after which the outputs must be discarded). A binding the program does not use
 * may be absent from the compiled module. Push constants: first, count, stride and jump budget,
 * four uints; thread t runs element first + t when that is below count.
 **/
struct KernelGpuPlan {
    // Empty when the program can run on the GPU.
    std::string cpuOnlyReason;
    // The builtin, function or variable that keeps the program on the CPU, when one does.
    std::string cpuOnlyName;
    std::string source;
    std::vector<KernelGpuColumn> columns;
    std::vector<KernelGpuUniform> uniforms;

    bool runsOnGpu() const
    {
        return cpuOnlyReason.empty();
    }
};

inline constexpr std::uint32_t kKernelGpuGroupSize = 64;
inline constexpr std::uint32_t kKernelGpuPushBytes = 16;

/** @brief The most backward jumps a run may let one element take before giving it up as runaway. */
inline constexpr std::uint32_t kKernelGpuJumpBudget = 1u << 24;

KernelGpuPlan kernelGpuPlan(const KernelProgram& program, const KernelGpuTarget& target);

NATRON_NAMESPACE_EXIT

#endif // Engine_Kernel_KernelGpuCodegen_h
