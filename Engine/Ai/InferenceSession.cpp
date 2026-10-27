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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "InferenceSession.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <iostream>
#include <mutex>
#include <set>
#include <system_error>
#include <thread>
#include <tuple>

#include <QDateTime>
#include <QString>

#include <onnxruntime_c_api.h>

#include "Engine/Ai/OrtLoader.h"
#include "Engine/AppManager.h"
#include "Engine/Settings.h"

NATRON_NAMESPACE_ENTER

namespace {

const std::chrono::milliseconds kAbortPollInterval(20);

std::string
statusMessage(const OrtApi* api,
              OrtStatus* status)
{
    std::string message = api->GetErrorMessage(status);
    api->ReleaseStatus(status);

    return message;
}

[[noreturn]] void
throwFailure(const std::string& context,
             const std::string& message)
{
    const std::string what = context + ": " + message;

    if (InferenceSession::isOutOfMemoryMessage(message)) {
        throw AiOutOfMemory(what);
    }
    throw AiError(what);
}

void
check(const OrtApi* api,
      OrtStatus* status,
      const std::string& context)
{
    if (status) {
        throwFailure(context, statusMessage(api, status));
    }
}

void
warnOnce(const std::string& message)
{
    static std::mutex mutex;
    static std::set<std::string> warned;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!warned.insert(message).second) {
            return;
        }
    }
    InferenceSession::logWarning(message);
}

GraphOptimizationLevel
ortOptimizationLevel(AiGraphOptimization level)
{
    switch (level) {
    case AiGraphOptimization::eDisabled:
        return ORT_DISABLE_ALL;
    case AiGraphOptimization::eBasic:
        return ORT_ENABLE_BASIC;
    case AiGraphOptimization::eExtended:
        return ORT_ENABLE_EXTENDED;
    case AiGraphOptimization::eAll:
        break;
    }

    return ORT_ENABLE_ALL;
}

// Creates a session on one execution provider. Null, with ONNX Runtime's message in *error, when
// the provider cannot be appended or the session cannot be created on it.
OrtSession*
createOn(const OrtLoader& loader,
         const std::string& modelPath,
         const AiSessionOptions& options,
         AiDeviceEnum device,
         std::string* error)
{
    const OrtApi* api = loader.api();
    OrtSessionOptions* sessionOptions = nullptr;
    OrtStatus* status = api->CreateSessionOptions(&sessionOptions);

    if (status) {
        *error = statusMessage(api, status);

        return nullptr;
    }
    std::unique_ptr<OrtSessionOptions, std::function<void(OrtSessionOptions*)>> guard(sessionOptions, [api](OrtSessionOptions* p) {
        api->ReleaseSessionOptions(p);
    });

    if (options.threads > 0) {
        status = api->SetIntraOpNumThreads(sessionOptions, options.threads);
    }
    if (!status) {
        status = api->SetSessionGraphOptimizationLevel(sessionOptions, ortOptimizationLevel(options.optimization));
    }
    if (!status) {
        switch (device) {
        case eAiDeviceCuda: {
            OrtCUDAProviderOptions cuda;
            cuda.device_id = options.gpuIndex;
            status = api->SessionOptionsAppendExecutionProvider_CUDA(sessionOptions, &cuda);
            break;
        }
        case eAiDeviceRocm: {
            OrtROCMProviderOptions rocm;
            rocm.device_id = options.gpuIndex;
            status = api->SessionOptionsAppendExecutionProvider_ROCM(sessionOptions, &rocm);
            break;
        }
        case eAiDeviceMigraphx: {
            OrtMIGraphXProviderOptions migraphx {};
            migraphx.device_id = options.gpuIndex;
            migraphx.migraphx_mem_limit = SIZE_MAX;
            status = api->SessionOptionsAppendExecutionProvider_MIGraphX(sessionOptions, &migraphx);
            break;
        }
        case eAiDeviceOpenvino: {
            const std::string deviceType = options.gpuIndex > 0 ? "GPU." + std::to_string(options.gpuIndex) : std::string("GPU");
            const char* keys[] = { "device_type" };
            const char* values[] = { deviceType.c_str() };
            status = api->SessionOptionsAppendExecutionProvider(sessionOptions, "OpenVINO", keys, values, 1);
            break;
        }
        case eAiDeviceAuto:
        case eAiDeviceCpu:
            break;
        }
    }
    OrtSession* session = nullptr;
    if (!status) {
        status = api->CreateSession(loader.env(), modelPath.c_str(), sessionOptions, &session);
    }
    if (status) {
        *error = statusMessage(api, status);

        return nullptr;
    }

    return session;
}

// The model's inputs or outputs. Non-tensor and non-float entries are refused for inputs and
// skipped for outputs, since a Tensor can neither feed nor hold them.
std::vector<AiTensorInfo>
readTensorInfos(const OrtApi* api,
                OrtSession* session,
                bool inputs,
                const std::string& context)
{
    OrtAllocator* allocator = nullptr;

    check(api, api->GetAllocatorWithDefaultOptions(&allocator), context);

    std::size_t count = 0;
    check(api, inputs ? api->SessionGetInputCount(session, &count) : api->SessionGetOutputCount(session, &count), context);

    std::vector<AiTensorInfo> infos;
    for (std::size_t i = 0; i < count; ++i) {
        AiTensorInfo info;
        char* name = nullptr;
        check(api, inputs ? api->SessionGetInputName(session, i, allocator, &name) : api->SessionGetOutputName(session, i, allocator, &name), context);
        info.name = name;
        OrtStatus* freed = api->AllocatorFree(allocator, name);
        if (freed) {
            api->ReleaseStatus(freed);
        }

        OrtTypeInfo* typeInfo = nullptr;
        check(api, inputs ? api->SessionGetInputTypeInfo(session, i, &typeInfo) : api->SessionGetOutputTypeInfo(session, i, &typeInfo), context);
        std::unique_ptr<OrtTypeInfo, std::function<void(OrtTypeInfo*)>> guard(typeInfo, [api](OrtTypeInfo* p) {
            api->ReleaseTypeInfo(p);
        });

        ONNXType onnxType = ONNX_TYPE_UNKNOWN;
        check(api, api->GetOnnxTypeFromTypeInfo(typeInfo, &onnxType), context);
        ONNXTensorElementDataType elementType = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
        const OrtTensorTypeAndShapeInfo* tensorInfo = nullptr;
        if (onnxType == ONNX_TYPE_TENSOR) {
            check(api, api->CastTypeInfoToTensorInfo(typeInfo, &tensorInfo), context);
            check(api, api->GetTensorElementType(tensorInfo, &elementType), context);
        }
        if ((elementType != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) && (elementType != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16)) {
            if (inputs) {
                throw AiError(context + ": input '" + info.name + "' is not a float32 or float16 tensor");
            }
            continue;
        }
        info.float16 = (elementType == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16);

        std::size_t rank = 0;
        check(api, api->GetDimensionsCount(tensorInfo, &rank), context);
        info.shape.resize(rank);
        if (rank) {
            check(api, api->GetDimensions(tensorInfo, info.shape.data(), rank), context);
        }
        for (int64_t& d : info.shape) {
            if (d <= 0) {
                d = -1;
            }
        }
        infos.push_back(info);
    }

    return infos;
}

/**
 * @brief Caps how many runs one device does at once, so parallel renders queue instead of
 * exhausting its memory. Waiting is abortable.
 **/
class DeviceGate {
public:
    explicit DeviceGate(int limit)
        : _limit(std::max(1, limit))
    {
    }

    void acquire(const AiAbortPredicate& abort)
    {
        std::unique_lock<std::mutex> lock(_mutex);

        while (_used >= _limit) {
            if (abort) {
                _cond.wait_for(lock, kAbortPollInterval);
                if ((_used >= _limit) && abort()) {
                    throw AiAborted();
                }
            } else {
                _cond.wait(lock);
            }
        }
        ++_used;
    }

    void release()
    {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            --_used;
        }
        _cond.notify_one();
    }

private:
    std::mutex _mutex;
    std::condition_variable _cond;
    const int _limit;
    int _used = 0;
};

DeviceGate&
deviceGate(AiDeviceEnum device,
           int gpuIndex,
           int threads)
{
    static std::mutex mutex;
    static std::map<std::string, std::unique_ptr<DeviceGate>>* gates = new std::map<std::string, std::unique_ptr<DeviceGate>>();
    const bool cpu = (device == eAiDeviceCpu);
    const std::string key = cpu ? std::string("cpu") : std::string(InferenceSession::providerName(device)) + ":" + std::to_string(gpuIndex);
    std::lock_guard<std::mutex> lock(mutex);
    std::unique_ptr<DeviceGate>& gate = (*gates)[key];

    if (!gate) {
        // A GPU runs one inference at a time. CPU runs share the cores: with a thread count set,
        // as many run at once as fit in the machine; left to ONNX Runtime, each takes them all.
        int limit = 1;
        if (cpu && (threads > 0)) {
            limit = std::max<int>(1, (int)std::thread::hardware_concurrency() / threads);
        }
        gate.reset(new DeviceGate(limit));
    }

    return *gate;
}

class GateLock {
public:
    GateLock(DeviceGate& gate,
             const AiAbortPredicate& abort)
        : _gate(gate)
    {
        _gate.acquire(abort);
    }

    ~GateLock()
    {
        _gate.release();
    }

private:
    DeviceGate& _gate;
};

class ValueList {
public:
    explicit ValueList(const OrtApi* api)
        : _api(api)
    {
    }

    ~ValueList()
    {
        for (OrtValue* v : values) {
            if (v) {
                _api->ReleaseValue(v);
            }
        }
    }

    std::vector<OrtValue*> values;

private:
    const OrtApi* _api;
};

std::mutex&
injectorMutex()
{
    static std::mutex mutex;

    return mutex;
}

InferenceSession::FailureInjector&
injector()
{
    static InferenceSession::FailureInjector* hook = new InferenceSession::FailureInjector();

    return *hook;
}

struct CacheKey {
    std::string path;
    int64_t mtime;
    AiSessionOptions options;
    const OrtLoader* loader;

    bool operator<(const CacheKey& other) const
    {
        if (path != other.path) {
            return path < other.path;
        }
        if (mtime != other.mtime) {
            return mtime < other.mtime;
        }
        if (options < other.options) {
            return true;
        }
        if (other.options < options) {
            return false;
        }

        return std::less<const OrtLoader*>()(loader, other.loader);
    }
};

struct CacheEntry {
    std::shared_future<std::shared_ptr<InferenceSession>> session;
    uint64_t lastUse = 0;
};

struct Cache {
    std::mutex mutex;
    std::map<CacheKey, CacheEntry> entries;
    uint64_t clock = 0;
};

Cache&
cache()
{
    // Never destroyed, like the loader: sessions must not be released after ONNX Runtime's own
    // static state is gone at exit.
    static Cache* c = new Cache();

    return *c;
}

} // namespace

AiSessionOptions
AiSessionOptions::fromPreferences(AiGraphOptimization optimization)
{
    AiSessionOptions options;

    options.optimization = optimization;
    SettingsPtr settings = appPTR ? appPTR->getCurrentSettings() : SettingsPtr();
    if (settings) {
        options.device = settings->getAiDevice();
        options.gpuIndex = settings->getAiGpuIndex();
        options.threads = settings->getAiThreads();
    } else {
        options.device = Settings::resolveAiDevice(eAiDeviceAuto, std::getenv("NATRON_AI_DEVICE"));
    }

    return options;
}

bool
AiSessionOptions::operator<(const AiSessionOptions& other) const
{
    return std::make_tuple((int)device, gpuIndex, threads, (int)optimization) < std::make_tuple((int)other.device, other.gpuIndex, other.threads, (int)other.optimization);
}

const char*
InferenceSession::providerName(AiDeviceEnum device)
{
    switch (device) {
    case eAiDeviceCuda:
        return "CUDAExecutionProvider";
    case eAiDeviceMigraphx:
        return "MIGraphXExecutionProvider";
    case eAiDeviceRocm:
        return "ROCMExecutionProvider";
    case eAiDeviceOpenvino:
        return "OpenVINOExecutionProvider";
    case eAiDeviceAuto:
    case eAiDeviceCpu:
        break;
    }

    return "CPUExecutionProvider";
}

std::vector<AiDeviceEnum>
InferenceSession::providerOrder(AiDeviceEnum requested,
                                const std::vector<std::string>& available,
                                std::string* missing)
{
    auto offered = [&available](AiDeviceEnum device) {
        return std::find(available.begin(), available.end(), providerName(device)) != available.end();
    };
    std::vector<AiDeviceEnum> order;

    if (requested == eAiDeviceAuto) {
        for (AiDeviceEnum device : { eAiDeviceCuda, eAiDeviceMigraphx, eAiDeviceRocm }) {
            if (offered(device)) {
                order.push_back(device);
            }
        }
    } else if (requested != eAiDeviceCpu) {
        if (offered(requested)) {
            order.push_back(requested);
        } else if (missing) {
            *missing = providerName(requested);
        }
    }
    order.push_back(eAiDeviceCpu);

    return order;
}

void
InferenceSession::logWarning(const std::string& message)
{
    if (appPTR) {
        appPTR->writeToErrorLog_mt_safe(QString::fromUtf8("AI"), QDateTime::currentDateTime(), QString::fromStdString(message));
    } else {
        std::cerr << "AI: " << message << std::endl;
    }
}

bool
InferenceSession::isOutOfMemoryMessage(const std::string& message)
{
    std::string lower(message);

    for (char& c : lower) {
        c = (char)std::tolower((unsigned char)c);
    }
    // CUDA and HIP runtime errors, cuDNN/cuBLAS/MIOpen allocation statuses, ONNX Runtime's arena
    // and std::bad_alloc caught inside the library.
    static const char* const markers[] = {
        "out of memory",
        "outofmemory",
        "out_of_memory",
        "memoryallocation",
        "alloc_failed",
        "failed to allocate memory",
        "bad_alloc",
    };
    for (const char* marker : markers) {
        if (lower.find(marker) != std::string::npos) {
            return true;
        }
    }

    return false;
}

void
InferenceSession::setFailureInjectorForTesting(FailureInjector hook)
{
    std::lock_guard<std::mutex> lock(injectorMutex());
    injector() = std::move(hook);
}

std::shared_ptr<InferenceSession>
InferenceSession::open(const std::string& modelPath,
                       const AiSessionOptions& options)
{
    return open(modelPath, options, OrtLoader::process());
}

std::shared_ptr<InferenceSession>
InferenceSession::open(const std::string& modelPath,
                       const AiSessionOptions& options,
                       const std::shared_ptr<const OrtLoader>& loader)
{
    if (!loader || !loader->available()) {
        throw AiError(loader ? loader->unavailableReason() : std::string("ONNX Runtime is not loaded"));
    }
    std::error_code ec;
    const std::filesystem::file_time_type mtime = std::filesystem::last_write_time(modelPath, ec);
    if (ec) {
        throw AiError(modelPath + ": " + ec.message());
    }
    const CacheKey key { modelPath, (int64_t)mtime.time_since_epoch().count(), options, loader.get() };
    Cache& c = cache();
    std::promise<std::shared_ptr<InferenceSession>> promise;
    {
        std::unique_lock<std::mutex> lock(c.mutex);
        auto found = c.entries.find(key);
        if (found != c.entries.end()) {
            found->second.lastUse = ++c.clock;
            std::shared_future<std::shared_ptr<InferenceSession>> pending = found->second.session;
            lock.unlock();

            // Another thread may still be creating it: wait for that rather than create a second.
            return pending.get();
        }
        CacheEntry& entry = c.entries[key];
        entry.session = promise.get_future().share();
        entry.lastUse = ++c.clock;
    }

    std::shared_ptr<InferenceSession> session;
    try {
        session = create(modelPath, options, loader);
    } catch (...) {
        promise.set_exception(std::current_exception());
        std::lock_guard<std::mutex> lock(c.mutex);
        c.entries.erase(key);
        throw;
    }
    promise.set_value(session);

    std::lock_guard<std::mutex> lock(c.mutex);
    while (c.entries.size() > kCacheCapacity) {
        auto oldest = c.entries.end();
        for (auto it = c.entries.begin(); it != c.entries.end(); ++it) {
            const bool ready = it->second.session.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
            if (ready && ((oldest == c.entries.end()) || (it->second.lastUse < oldest->second.lastUse))) {
                oldest = it;
            }
        }
        if (oldest == c.entries.end()) {
            break;
        }
        c.entries.erase(oldest);
    }

    return session;
}

void
InferenceSession::clearCache()
{
    Cache& c = cache();
    std::lock_guard<std::mutex> lock(c.mutex);

    for (auto it = c.entries.begin(); it != c.entries.end();) {
        if (it->second.session.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            it = c.entries.erase(it);
        } else {
            ++it;
        }
    }
}

std::size_t
InferenceSession::cacheSize()
{
    Cache& c = cache();
    std::lock_guard<std::mutex> lock(c.mutex);

    return c.entries.size();
}

std::shared_ptr<InferenceSession>
InferenceSession::create(const std::string& modelPath,
                         const AiSessionOptions& options,
                         const std::shared_ptr<const OrtLoader>& loader)
{
    std::string missing;
    const std::vector<AiDeviceEnum> order = providerOrder(options.device, loader->providers(), &missing);

    if (!missing.empty()) {
        warnOnce("The ONNX Runtime at " + loader->libraryPath() + " has no " + missing + "; AI inference runs on the CPU instead");
    }

    std::vector<std::string> failures;
    for (AiDeviceEnum device : order) {
        std::string error;
        OrtSession* raw = createOn(*loader, modelPath, options, device, &error);
        if (!raw) {
            if (device == eAiDeviceCpu) {
                throwFailure(modelPath, error);
            }
            failures.push_back(std::string(providerName(device)) + ": " + error);
            continue;
        }

        std::shared_ptr<InferenceSession> session(new InferenceSession);
        session->_loader = loader;
        session->_session = raw;
        session->_modelPath = modelPath;
        session->_options = options;
        session->_device = device;
        session->_inputs = readTensorInfos(loader->api(), raw, true, modelPath);
        session->_outputs = readTensorInfos(loader->api(), raw, false, modelPath);
        if (session->_outputs.empty()) {
            throw AiError(modelPath + ": the model has no float32 or float16 output");
        }
        for (const std::string& failure : failures) {
            logWarning(modelPath + " could not be opened on " + failure + "; using " + providerName(device) + " instead");
        }

        return session;
    }

    throw AiError(modelPath + ": no execution provider could open the model");
}

InferenceSession::~InferenceSession()
{
    if (_session) {
        _loader->api()->ReleaseSession(_session);
    }
}

std::map<std::string, Tensor>
InferenceSession::run(const std::map<std::string, Tensor>& inputs,
                      const AiAbortPredicate& abort) const
{
    std::map<std::string, const Tensor*> named;

    for (const auto& given : inputs) {
        if (std::none_of(_inputs.begin(), _inputs.end(), [&given](const AiTensorInfo& info) { return info.name == given.first; })) {
            throw AiError(_modelPath + ": the model has no input '" + given.first + "'");
        }
        named[given.first] = &given.second;
    }

    return runNamed(named, abort);
}

std::map<std::string, Tensor>
InferenceSession::runNamed(const std::map<std::string, const Tensor*>& inputs,
                           const AiAbortPredicate& abort) const
{
    if (abort && abort()) {
        throw AiAborted();
    }
    const OrtApi* api = _loader->api();

    OrtMemoryInfo* memoryInfo = nullptr;
    check(api, api->CreateCpuMemoryInfo(OrtDeviceAllocator, OrtMemTypeDefault, &memoryInfo), _modelPath);
    std::unique_ptr<OrtMemoryInfo, std::function<void(OrtMemoryInfo*)>> memoryGuard(memoryInfo, [api](OrtMemoryInfo* p) {
        api->ReleaseMemoryInfo(p);
    });

    std::vector<const char*> inputNames;
    ValueList inputValues(api);
    std::vector<std::vector<uint16_t>> halfInputs;
    halfInputs.reserve(_inputs.size());
    for (const AiTensorInfo& info : _inputs) {
        auto given = inputs.find(info.name);
        if (given == inputs.end()) {
            throw AiError(_modelPath + ": input '" + info.name + "' was not given");
        }
        const Tensor& tensor = *given->second;
        bool shapeMatches = (tensor.rank() == info.shape.size());
        for (std::size_t d = 0; shapeMatches && d < info.shape.size(); ++d) {
            shapeMatches = (info.shape[d] < 0) || (info.shape[d] == tensor.dim(d));
        }
        if (!shapeMatches || tensor.empty()) {
            throw AiError(_modelPath + ": input '" + info.name + "' is " + Tensor::shapeString(tensor.shape()) + " but the model takes " + Tensor::shapeString(info.shape));
        }

        OrtValue* value = nullptr;
        if (info.float16) {
            halfInputs.emplace_back(tensor.size());
            std::vector<uint16_t>& half = halfInputs.back();
            const float* src = tensor.data();
            for (std::size_t i = 0; i < half.size(); ++i) {
                half[i] = floatToHalf(src[i]);
            }
            check(api, api->CreateTensorWithDataAsOrtValue(memoryInfo, half.data(), half.size() * sizeof(uint16_t), tensor.shape().data(), tensor.rank(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16, &value), _modelPath);
        } else {
            // ONNX Runtime only reads its inputs, so lending it the const buffer is safe.
            check(api, api->CreateTensorWithDataAsOrtValue(memoryInfo, const_cast<float*>(tensor.data()), tensor.size() * sizeof(float), tensor.shape().data(), tensor.rank(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &value), _modelPath);
        }
        inputValues.values.push_back(value);
        inputNames.push_back(info.name.c_str());
    }

    FailureInjector inject;
    {
        std::lock_guard<std::mutex> lock(injectorMutex());
        inject = injector();
    }
    if (inject) {
        std::vector<std::vector<int64_t>> shapes;
        for (const AiTensorInfo& info : _inputs) {
            shapes.push_back(inputs.at(info.name)->shape());
        }
        const std::string injected = inject(*this, shapes);
        if (!injected.empty()) {
            throwFailure(_modelPath, injected);
        }
    }

    std::vector<const char*> outputNames;
    for (const AiTensorInfo& info : _outputs) {
        outputNames.push_back(info.name.c_str());
    }
    ValueList outputValues(api);
    outputValues.values.assign(_outputs.size(), nullptr);

    OrtRunOptions* runOptions = nullptr;
    check(api, api->CreateRunOptions(&runOptions), _modelPath);
    std::unique_ptr<OrtRunOptions, std::function<void(OrtRunOptions*)>> runOptionsGuard(runOptions, [api](OrtRunOptions* p) {
        api->ReleaseRunOptions(p);
    });

    OrtStatus* status = nullptr;
    std::atomic<bool> terminated(false);
    {
        GateLock gate(deviceGate(_device, _options.gpuIndex, _options.threads), abort);

        std::mutex watchMutex;
        std::condition_variable watchCond;
        bool finished = false;
        std::thread watcher;
        if (abort) {
            watcher = std::thread([&]() {
                std::unique_lock<std::mutex> lock(watchMutex);
                while (!watchCond.wait_for(lock, kAbortPollInterval, [&finished]() { return finished; })) {
                    if (abort()) {
                        terminated = true;
                        OrtStatus* s = api->RunOptionsSetTerminate(runOptions);
                        if (s) {
                            api->ReleaseStatus(s);
                        }

                        return;
                    }
                }
            });
        }
        status = api->Run(_session, runOptions, inputNames.data(), inputValues.values.data(), inputValues.values.size(),
                          outputNames.data(), outputNames.size(), outputValues.values.data());
        if (abort) {
            {
                std::lock_guard<std::mutex> lock(watchMutex);
                finished = true;
            }
            watchCond.notify_one();
            watcher.join();
        }
    }

    if (terminated) {
        if (status) {
            api->ReleaseStatus(status);
        }
        throw AiAborted();
    }
    check(api, status, _modelPath);

    std::map<std::string, Tensor> results;
    for (std::size_t i = 0; i < _outputs.size(); ++i) {
        OrtValue* value = outputValues.values[i];
        OrtTensorTypeAndShapeInfo* shapeInfo = nullptr;
        check(api, api->GetTensorTypeAndShape(value, &shapeInfo), _modelPath);
        std::unique_ptr<OrtTensorTypeAndShapeInfo, std::function<void(OrtTensorTypeAndShapeInfo*)>> shapeGuard(shapeInfo, [api](OrtTensorTypeAndShapeInfo* p) {
            api->ReleaseTensorTypeAndShapeInfo(p);
        });
        ONNXTensorElementDataType elementType = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
        check(api, api->GetTensorElementType(shapeInfo, &elementType), _modelPath);
        std::size_t rank = 0;
        check(api, api->GetDimensionsCount(shapeInfo, &rank), _modelPath);
        std::vector<int64_t> shape(rank);
        if (rank) {
            check(api, api->GetDimensions(shapeInfo, shape.data(), rank), _modelPath);
        }
        if (std::any_of(shape.begin(), shape.end(), [](int64_t d) { return d < 1; })) {
            throw AiError(_modelPath + ": output '" + _outputs[i].name + "' came back empty, shaped " + Tensor::shapeString(shape));
        }
        void* data = nullptr;
        check(api, api->GetTensorMutableData(value, &data), _modelPath);

        Tensor tensor(shape);
        float* dst = tensor.data();
        if (elementType == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16) {
            const uint16_t* src = static_cast<const uint16_t*>(data);
            for (std::size_t j = 0; j < tensor.size(); ++j) {
                dst[j] = halfToFloat(src[j]);
            }
        } else if (elementType == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
            std::copy(static_cast<const float*>(data), static_cast<const float*>(data) + tensor.size(), dst);
        } else {
            throw AiError(_modelPath + ": output '" + _outputs[i].name + "' is not a float32 or float16 tensor");
        }
        results.emplace(_outputs[i].name, std::move(tensor));
    }

    return results;
}

Tensor
InferenceSession::run(const Tensor& input,
                      const AiAbortPredicate& abort) const
{
    if (_inputs.size() != 1) {
        throw AiError(_modelPath + ": the model takes " + std::to_string(_inputs.size()) + " inputs, not one");
    }
    std::map<std::string, const Tensor*> inputs;
    inputs.emplace(_inputs.front().name, &input);
    std::map<std::string, Tensor> outputs = runNamed(inputs, abort);

    return std::move(outputs.at(_outputs.front().name));
}

NATRON_NAMESPACE_EXIT
