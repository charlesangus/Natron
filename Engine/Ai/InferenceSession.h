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

#ifndef Engine_Ai_InferenceSession_h
#define Engine_Ai_InferenceSession_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "Engine/Ai/Tensor.h"
#include "Global/Enums.h"

struct OrtSession;

NATRON_NAMESPACE_ENTER

class OrtLoader;

// Any inference failure. what() carries ONNX Runtime's own message when it produced one.
class AiError
    : public std::runtime_error {
public:
    explicit AiError(const std::string& message)
        : std::runtime_error(message)
    {
    }
};

// The device ran out of memory. The tiler retries with smaller tiles, then on the CPU.
class AiOutOfMemory
    : public AiError {
public:
    explicit AiOutOfMemory(const std::string& message)
        : AiError(message)
    {
    }
};

// The abort predicate fired. Whatever the run produced is incomplete and must be discarded.
class AiAborted
    : public AiError {
public:
    AiAborted()
        : AiError("Inference was aborted")
    {
    }
};

/**
 * @brief Polled from a thread of its own while a run is in progress, so it must not rely on
 * thread-local state such as the calling render's TLS: capture what it needs (for example the
 * render's AbortableRenderInfo) instead.
 **/
typedef std::function<bool()> AiAbortPredicate;

enum class AiGraphOptimization {
    eDisabled,
    eBasic,
    eExtended,
    eAll,
};

struct AiSessionOptions {
    // eAiDeviceAuto tries CUDA, MIGraphX and ROCm in turn, each only when the library offers it,
    // then the CPU.
    AiDeviceEnum device = eAiDeviceAuto;
    int gpuIndex = 0;
    // Intra-op threads for the CPU execution provider. 0 lets ONNX Runtime choose.
    int threads = 0;
    AiGraphOptimization optimization = AiGraphOptimization::eAll;

    // device, gpuIndex and threads from Preferences, with its NATRON_AI_DEVICE override applied.
    static AiSessionOptions fromPreferences(AiGraphOptimization optimization);

    bool operator<(const AiSessionOptions& other) const;
};

struct AiTensorInfo {
    std::string name;
    // False for float32, true for float16. Other element types are refused when the model opens.
    bool float16 = false;
    // -1 marks a dynamic dimension.
    std::vector<int64_t> shape;
};

/**
 * @brief One ONNX Runtime session on a model, on the execution provider the options and the
 * library allow. Sessions are shared through a process-wide cache, and run() may be called from
 * several threads at once.
 **/
class InferenceSession {
public:
    /**
     * @brief The cached session for modelPath, created on first use. The cache is keyed by the
     * model's path and modification time, the options and the loader, and keeps the most recently
     * used kCacheCapacity sessions. Throws AiError when ONNX Runtime is unavailable, or the model
     * cannot be opened even on the CPU. Concurrent first opens of one key create one session.
     **/
    static std::shared_ptr<InferenceSession> open(const std::string& modelPath,
                                                  const AiSessionOptions& options);

    // As above, with an explicit ONNX Runtime instead of OrtLoader::process().
    static std::shared_ptr<InferenceSession> open(const std::string& modelPath,
                                                  const AiSessionOptions& options,
                                                  const std::shared_ptr<const OrtLoader>& loader);

    static constexpr std::size_t kCacheCapacity = 8;

    static void clearCache();
    static std::size_t cacheSize();

    /**
     * @brief The execution providers to try for a requested device, in order, always ending with
     * the CPU. A device the library does not offer is dropped, and *missing (when given) receives
     * its ONNX Runtime name.
     **/
    static std::vector<AiDeviceEnum> providerOrder(AiDeviceEnum requested,
                                                   const std::vector<std::string>& available,
                                                   std::string* missing);

    // The ONNX Runtime name of a device's execution provider, for example CUDAExecutionProvider.
    static const char* providerName(AiDeviceEnum device);

    /**
     * @brief A test hook called at the start of every run with the shapes of the model's inputs, in
     * the model's order. A non-empty return value is treated as the message of a failed ONNX
     * Runtime run, so error classification and the tiler's recovery can be exercised without a
     * device that fails. An empty function removes the hook.
     **/
    typedef std::function<std::string(const InferenceSession& session, const std::vector<std::vector<int64_t>>& inputShapes)> FailureInjector;
    static void setFailureInjectorForTesting(FailureInjector injector);

    // Writes to the error log, or stderr when there is no application, under an "AI" heading.
    static void logWarning(const std::string& message);

    // Whether an ONNX Runtime failure message describes a device or host running out of memory.
    static bool isOutOfMemoryMessage(const std::string& message);

    ~InferenceSession();

    InferenceSession(const InferenceSession&) = delete;
    InferenceSession& operator=(const InferenceSession&) = delete;

    /**
     * @brief Runs the model on inputs named as the model names them, every model input given,
     * and returns every float32 or float16 output by name, shaped as the run produced it. Blocks
     * while the device already runs as many inferences as it allows. Throws AiAborted when abort
     * fires, before or during the run, AiOutOfMemory when memory runs out, and AiError otherwise.
     **/
    std::map<std::string, Tensor> run(const std::map<std::string, Tensor>& inputs,
                                      const AiAbortPredicate& abort = AiAbortPredicate()) const;

    // run() for a model with one input, returning its first output.
    Tensor run(const Tensor& input,
               const AiAbortPredicate& abort = AiAbortPredicate()) const;

    const std::string& modelPath() const
    {
        return _modelPath;
    }

    // The options the session was asked for.
    const AiSessionOptions& requestedOptions() const
    {
        return _options;
    }

    // The device the session actually runs on, after any fallback.
    AiDeviceEnum device() const
    {
        return _device;
    }

    bool isGpu() const
    {
        return _device != eAiDeviceCpu;
    }

    const std::shared_ptr<const OrtLoader>& loader() const
    {
        return _loader;
    }

    const std::vector<AiTensorInfo>& inputs() const
    {
        return _inputs;
    }

    const std::vector<AiTensorInfo>& outputs() const
    {
        return _outputs;
    }

private:
    InferenceSession() = default;

    static std::shared_ptr<InferenceSession> create(const std::string& modelPath,
                                                    const AiSessionOptions& options,
                                                    const std::shared_ptr<const OrtLoader>& loader);

    std::map<std::string, Tensor> runNamed(const std::map<std::string, const Tensor*>& inputs,
                                           const AiAbortPredicate& abort) const;

    std::shared_ptr<const OrtLoader> _loader;
    OrtSession* _session = nullptr;
    std::string _modelPath;
    AiSessionOptions _options;
    AiDeviceEnum _device = eAiDeviceCpu;
    std::vector<AiTensorInfo> _inputs;
    std::vector<AiTensorInfo> _outputs;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Ai_InferenceSession_h
