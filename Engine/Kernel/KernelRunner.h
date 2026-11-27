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

#ifndef Engine_Kernel_KernelRunner_h
#define Engine_Kernel_KernelRunner_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "Global/GlobalDefines.h"

#include "Engine/Kernel/KernelTypes.h"

NATRON_NAMESPACE_ENTER

/** @brief Lanes per batch: the spike found 64-256 fastest, and 1024 spills L1. */
inline constexpr int kKernelLaneCount = 256;

/**
 * @brief One lane's array value. Elements are flattened: an array of vectors holds three floats per
 * element. Only the store matching type is used.
 **/
struct KernelArrayValue {
    KernelBaseType type = KernelBaseType::eFloat;
    std::vector<float> floats;
    std::vector<std::int32_t> ints;
    std::vector<std::string> strings;

    std::size_t length() const;
    void clear();
};

/**
 * @brief The register file of one thread: every register holds kKernelLaneCount lanes.
 **/
class KernelRegisters {
public:
    explicit KernelRegisters(const KernelProgram& program);

    float* f(std::int32_t reg) { return _f.data() + std::size_t(reg) * kKernelLaneCount; }
    const float* f(std::int32_t reg) const { return _f.data() + std::size_t(reg) * kKernelLaneCount; }
    std::int32_t* i(std::int32_t reg) { return _i.data() + std::size_t(reg) * kKernelLaneCount; }
    const std::int32_t* i(std::int32_t reg) const { return _i.data() + std::size_t(reg) * kKernelLaneCount; }
    std::string* s(std::int32_t reg) { return _s.data() + std::size_t(reg) * kKernelLaneCount; }
    const std::string* s(std::int32_t reg) const { return _s.data() + std::size_t(reg) * kKernelLaneCount; }
    KernelArrayValue* a(std::int32_t reg) { return _a.data() + std::size_t(reg) * kKernelLaneCount; }
    const KernelArrayValue* a(std::int32_t reg) const { return _a.data() + std::size_t(reg) * kKernelLaneCount; }

private:
    std::vector<float> _f;
    std::vector<std::int32_t> _i;
    std::vector<std::string> _s;
    std::vector<KernelArrayValue> _a;
};

/**
 * @brief A call of a function the context provides. Lanes [0, laneCount) whose mask is non-zero
 * take part; results of the other lanes must be left alone.
 **/
struct KernelCall {
    const KernelProgram& program;
    std::uint32_t instruction;
    const KernelProgramFunction& function;
    KernelRegisters& registers;
    const std::int32_t* mask;
    int laneCount;
    std::span<const KernelOperand> results;
    std::span<const KernelOperand> arguments;
};

/** @brief Thrown by a context, or by the runner, for an error found while running. */
class KernelRunError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/**
 * @brief Moves values between a context's data and the registers on one thread. Work items are
 * the context's own unit (a page of elements for geometry); the runner splits each into batches
 * of at most kKernelLaneCount elements, numbered from 0 within the item.
 **/
class KernelRunWorker {
public:
    virtual ~KernelRunWorker() = default;

    /** @brief Returns how many elements the item holds. */
    virtual std::size_t beginItem(std::size_t item) = 0;

    /** @brief Fills the bound registers that need it for elements [first, first + laneCount). */
    virtual void gather(std::size_t first, int laneCount, KernelRegisters& registers) = 0;

    /** @brief Stores the written bindings of elements [first, first + laneCount). */
    virtual void scatter(std::size_t first, int laneCount, const KernelRegisters& registers) = 0;

    /** @brief Called once every batch of the item has run and scattered, not after an abort. */
    virtual void endItem(std::size_t item) = 0;

    virtual void call(const KernelCall& call) = 0;
};

/**
 * @brief What a run iterates over. Items are independent: each is handled whole by one thread, so
 * a context whose items write disjoint data gets the same result whatever the thread count.
 **/
class KernelRunContext {
public:
    virtual ~KernelRunContext() = default;

    virtual std::size_t itemCount() const = 0;

    /** @brief Called once per thread taking part, concurrently. */
    virtual std::unique_ptr<KernelRunWorker> createWorker() = 0;
};

enum class KernelRunStatus {
    eDone,
    eAborted,
    eFailed,
};

struct KernelRunOptions {
    // 0 uses the current thread budget, or the global pool's size when none is set.
    int threads = 0;
    // Polled before every batch, and periodically inside loops.
    std::function<bool()> aborted;
};

struct KernelRunResult {
    KernelRunStatus status = KernelRunStatus::eDone;
    std::string error;
    std::uint64_t batches = 0;
};

/**
 * @brief Runs program over every item of context on the global thread pool. Exceptions from the
 * context end the run with eFailed and their message.
 **/
KernelRunResult kernelRun(const KernelProgram& program, KernelRunContext& context, const KernelRunOptions& options);

/**
 * @brief The literal an operand holds when its register is loaded by the prologue, which is the case
 * for every argument of a parameter declared constant.
 **/
std::optional<std::int32_t> kernelConstantInt(const KernelProgram& program, KernelOperand operand);
std::optional<float> kernelConstantFloat(const KernelProgram& program, KernelOperand operand);
const std::string* kernelConstantString(const KernelProgram& program, KernelOperand operand);

NATRON_NAMESPACE_EXIT

#endif // Engine_Kernel_KernelRunner_h
