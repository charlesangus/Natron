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

#ifndef Engine_Ai_OrtLoader_h
#define Engine_Ai_OrtLoader_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>
#include <string>
#include <vector>

struct OrtApi;
struct OrtEnv;

NATRON_NAMESPACE_ENTER

/**
 * @brief ONNX Runtime, loaded with dlopen() through its C API so the CPU build in the bundle and a
 * GPU build the user installs are interchangeable. Nothing links the library: only
 * onnxruntime_c_api.h is used at build time.
 **/
class OrtLoader {
public:
    /**
     * @brief The oldest OrtApi version accepted from a library older than the headers this build
     * uses. Code calling through api() must only use functions that version already has.
     **/
    static const int kMinimumApiVersion = 18;

    /**
     * @brief A place to look for the library: a file, or a directory holding libonnxruntime.so.1
     * directly or in its lib/. A required candidate that fails ends the search: it is what the
     * user asked for, and silently loading another library instead would hide the mistake.
     **/
    struct Candidate {
        std::string path;
        bool required = false;
    };

    /**
     * @brief The search order: the Preferences path (required) when set, then
     * <userDataDir>/onnxruntime, then <executable dir>/../lib/onnxruntime, the bundled copy.
     **/
    static std::vector<Candidate> searchCandidates(const std::string& preferencePath,
                                                   const std::string& userDataDir,
                                                   const std::string& executableDir);

    /**
     * @brief Loads the first usable candidate and creates its OrtEnv. Never throws: on failure the
     * loader is unavailable and unavailableReason() names every path tried and why it failed.
     **/
    static std::shared_ptr<const OrtLoader> load(const std::vector<Candidate>& candidates);

    /**
     * @brief The process-wide loader. Nothing is loaded until the first call made while "AI
     * Enabled" is on in Preferences; while it is off, the loader returned is unavailable and says
     * so. A failed load is retried when the Preferences library path changes. A successful one is
     * kept for the life of the process.
     **/
    static std::shared_ptr<const OrtLoader> process();

    /**
     * @brief The reason process() gives while AI is disabled in Preferences.
     **/
    static const char* disabledReason();

    ~OrtLoader();

    OrtLoader(const OrtLoader&) = delete;
    OrtLoader& operator=(const OrtLoader&) = delete;

    bool available() const
    {
        return _api != nullptr;
    }

    const std::string& unavailableReason() const
    {
        return _reason;
    }

    // The library's own version string, for example "1.31.1".
    const std::string& versionString() const
    {
        return _version;
    }

    // The file the library was loaded from, resolved from its candidate.
    const std::string& libraryPath() const
    {
        return _libraryPath;
    }

    // The OrtApi version in use: the headers' own, or an older one the library offers instead.
    int apiVersion() const
    {
        return _apiVersion;
    }

    /**
     * @brief The execution providers this library offers, by their ONNX Runtime names
     * (CPUExecutionProvider, CUDAExecutionProvider, ...): the library's own list, plus any EP
     * whose append function it exports but that list omits.
     **/
    const std::vector<std::string>& providers() const
    {
        return _providers;
    }

    bool hasProvider(const std::string& ortName) const;

    const OrtApi* api() const
    {
        return _api;
    }

    OrtEnv* env() const
    {
        return _env;
    }

private:
    OrtLoader() = default;

    bool loadLibrary(const std::string& file, std::string* error);

    void* _handle = nullptr;
    const OrtApi* _api = nullptr;
    OrtEnv* _env = nullptr;
    int _apiVersion = 0;
    std::string _reason;
    std::string _version;
    std::string _libraryPath;
    std::vector<std::string> _providers;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Ai_OrtLoader_h
