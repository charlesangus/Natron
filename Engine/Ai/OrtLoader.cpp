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

#include "OrtLoader.h"

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <system_error>

#include <dlfcn.h>

#include <onnxruntime_c_api.h>

#include "Engine/AppManager.h"
#include "Engine/Settings.h"
#include "Engine/StandardPaths.h"

NATRON_NAMESPACE_ENTER

namespace {

const char* const kLibraryName = "libonnxruntime.so.1";
const char* const kLibraryLinkName = "libonnxruntime.so";

// Execution providers some builds export a legacy C append function for, by ONNX Runtime name.
// GetAvailableProviders covers current builds; this catches a library whose list omits one.
const struct
{
    const char* symbol;
    const char* ortName;
} kProviderAppendSymbols[] = {
    { "OrtSessionOptionsAppendExecutionProvider_CUDA", "CUDAExecutionProvider" },
    { "OrtSessionOptionsAppendExecutionProvider_MIGraphX", "MIGraphXExecutionProvider" },
    { "OrtSessionOptionsAppendExecutionProvider_ROCM", "ROCMExecutionProvider" },
    { "OrtSessionOptionsAppendExecutionProvider_OpenVINO", "OpenVINOExecutionProvider" },
};

// The library file a candidate names: the path itself when it is a file, otherwise the first
// library found in it or its lib/. Empty, with the reason in *error, when there is none.
std::string
resolveLibraryFile(const std::string& candidate,
                   std::string* error)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path path(candidate);

    if (fs::is_regular_file(path, ec)) {
        return path.string();
    }
    if (!fs::is_directory(path, ec)) {
        *error = candidate + " does not exist";

        return std::string();
    }
    for (const fs::path& dir : { path, path / "lib" }) {
        for (const char* name : { kLibraryName, kLibraryLinkName }) {
            const fs::path file = dir / name;
            if (fs::is_regular_file(file, ec)) {
                return file.string();
            }
        }
    }
    *error = "no " + std::string(kLibraryName) + " in " + candidate + " or its lib/";

    return std::string();
}

std::string
statusMessage(const OrtApi* api,
              OrtStatus* status)
{
    std::string message = api->GetErrorMessage(status);
    api->ReleaseStatus(status);

    return message;
}

std::string
executableDir()
{
    std::error_code ec;
    const std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec);

    return ec ? std::string() : exe.parent_path().string();
}

} // namespace

std::vector<OrtLoader::Candidate>
OrtLoader::searchCandidates(const std::string& preferencePath,
                            const std::string& userDataDir,
                            const std::string& executableDir)
{
    std::vector<Candidate> candidates;

    if (!preferencePath.empty()) {
        candidates.push_back(Candidate { preferencePath, true });
    }
    if (!userDataDir.empty()) {
        candidates.push_back(Candidate { (std::filesystem::path(userDataDir) / "onnxruntime").string(), false });
    }
    if (!executableDir.empty()) {
        candidates.push_back(Candidate { (std::filesystem::path(executableDir) / ".." / "lib" / "onnxruntime").lexically_normal().string(), false });
    }

    return candidates;
}

std::shared_ptr<const OrtLoader>
OrtLoader::load(const std::vector<Candidate>& candidates)
{
    std::shared_ptr<OrtLoader> loader(new OrtLoader);
    std::vector<std::string> failures;

    for (const Candidate& candidate : candidates) {
        std::string error;
        const std::string file = resolveLibraryFile(candidate.path, &error);
        if (!file.empty() && loader->loadLibrary(file, &error)) {
            return loader;
        }
        failures.push_back(error);
        if (candidate.required) {
            break;
        }
    }

    loader->_reason = "ONNX Runtime could not be loaded";
    if (failures.empty()) {
        loader->_reason += ": there was nowhere to look for it";
    }
    for (std::size_t i = 0; i < failures.size(); ++i) {
        loader->_reason += (i == 0 ? ": " : "; ") + failures[i];
    }

    return loader;
}

bool
OrtLoader::loadLibrary(const std::string& file,
                       std::string* error)
{
    // RTLD_LOCAL keeps ONNX Runtime's own copies of protobuf and friends out of the global
    // namespace, where they would collide with any other library carrying them.
    void* handle = dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL);

    if (!handle) {
        const char* why = dlerror();
        *error = file + ": " + (why ? why : "dlopen failed");

        return false;
    }

    typedef const OrtApiBase* (*GetApiBaseFn)();
    GetApiBaseFn getApiBase = reinterpret_cast<GetApiBaseFn>(dlsym(handle, "OrtGetApiBase"));
    const OrtApiBase* base = getApiBase ? getApiBase() : nullptr;
    if (!base) {
        dlclose(handle);
        *error = file + " is not an ONNX Runtime library (it has no OrtGetApiBase)";

        return false;
    }

    // From here on the library has run its own code, so it is never unloaded: its static state
    // and threads cannot be assumed safe to tear down.
    const char* version = base->GetVersionString ? base->GetVersionString() : nullptr;
    const std::string versionString = version ? version : "unknown";

    const OrtApi* api = nullptr;
    int apiVersion = ORT_API_VERSION;
    for (; apiVersion >= kMinimumApiVersion; --apiVersion) {
        api = base->GetApi(apiVersion);
        if (api) {
            break;
        }
    }
    if (!api) {
        *error = file + " is ONNX Runtime " + versionString + ", which offers none of the API versions "
            + std::to_string(kMinimumApiVersion) + " to " + std::to_string(ORT_API_VERSION)
            + " this build of Natron can use";

        return false;
    }

    OrtEnv* env = nullptr;
    OrtStatus* status = api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "Natron", &env);
    if (status) {
        *error = file + ": ONNX Runtime " + versionString + " could not create its environment: " + statusMessage(api, status);

        return false;
    }

    std::vector<std::string> providers;
    char** names = nullptr;
    int count = 0;
    status = api->GetAvailableProviders(&names, &count);
    if (status) {
        api->ReleaseStatus(status);
    } else if (names) {
        for (int i = 0; i < count; ++i) {
            providers.push_back(names[i]);
        }
        status = api->ReleaseAvailableProviders(names, count);
        if (status) {
            api->ReleaseStatus(status);
        }
    }
    for (const auto& probe : kProviderAppendSymbols) {
        if (dlsym(handle, probe.symbol) && (std::find(providers.begin(), providers.end(), probe.ortName) == providers.end())) {
            providers.push_back(probe.ortName);
        }
    }
    if (std::find(providers.begin(), providers.end(), "CPUExecutionProvider") == providers.end()) {
        providers.push_back("CPUExecutionProvider");
    }

    _handle = handle;
    _api = api;
    _env = env;
    _apiVersion = apiVersion;
    _version = versionString;
    _libraryPath = file;
    _providers = providers;
    _reason.clear();

    return true;
}

OrtLoader::~OrtLoader()
{
    if (_api && _env) {
        _api->ReleaseEnv(_env);
    }
}

bool
OrtLoader::hasProvider(const std::string& ortName) const
{
    return std::find(_providers.begin(), _providers.end(), ortName) != _providers.end();
}

const char*
OrtLoader::disabledReason()
{
    return "AI is disabled in Preferences";
}

std::shared_ptr<const OrtLoader>
OrtLoader::process()
{
    static std::mutex mutex;
    // Never destroyed: the loaded library's threads may outlive static destruction at exit.
    static std::shared_ptr<const OrtLoader>* loaded = new std::shared_ptr<const OrtLoader>();
    static std::string attemptedPreferencePath;

    SettingsPtr settings = appPTR ? appPTR->getCurrentSettings() : SettingsPtr();
    if (!settings || !settings->isAiEnabled()) {
        static const std::shared_ptr<const OrtLoader> disabled = []() {
            std::shared_ptr<OrtLoader> loader(new OrtLoader);
            loader->_reason = disabledReason();

            return std::shared_ptr<const OrtLoader>(loader);
        }();

        return disabled;
    }

    const std::string preferencePath = settings->getAiOrtLibraryPath();
    std::lock_guard<std::mutex> lock(mutex);
    if (*loaded && ((*loaded)->available() || (preferencePath == attemptedPreferencePath))) {
        return *loaded;
    }
    attemptedPreferencePath = preferencePath;
    *loaded = load(searchCandidates(preferencePath,
                                    StandardPaths::writableLocation(StandardPaths::eStandardLocationData).toStdString(),
                                    executableDir()));

    return *loaded;
}

NATRON_NAMESPACE_EXIT
