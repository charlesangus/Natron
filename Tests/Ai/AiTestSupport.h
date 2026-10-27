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

#ifndef Tests_Ai_AiTestSupport_h
#define Tests_Ai_AiTestSupport_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>
#include <string>
#include <utility>

#include "Engine/Ai/InferenceSession.h"
#include "Engine/Ai/OrtLoader.h"

NATRON_NAMESPACE_ENTER

namespace AiTest {

// A model written by tools/ai/make-test-models.py.
inline std::string
modelPath(const char* name)
{
    return std::string(NATRON_TESTS_FIXTURES_DIR "/ai/") + name;
}

// The directory fetch-assets.sh unpacks the pinned ONNX Runtime release into.
inline std::string
ortDir()
{
    return NATRON_TESTS_ONNXRUNTIME_DIR;
}

/**
 * @brief The build tree's ONNX Runtime, loaded once for the whole process, independently of the
 * Preferences "AI Enabled" switch that OrtLoader::process() obeys.
 **/
inline const std::shared_ptr<const OrtLoader>&
loader()
{
    static const std::shared_ptr<const OrtLoader> loaded = OrtLoader::load({ OrtLoader::Candidate { ortDir(), true } });

    return loaded;
}

inline AiSessionOptions
cpuOptions(int threads = 0)
{
    AiSessionOptions options;

    options.device = eAiDeviceCpu;
    options.threads = threads;

    return options;
}

// Installs a failure injector for the life of the guard.
class InjectorGuard {
public:
    explicit InjectorGuard(InferenceSession::FailureInjector injector)
    {
        InferenceSession::setFailureInjectorForTesting(std::move(injector));
    }

    ~InjectorGuard()
    {
        InferenceSession::setFailureInjectorForTesting(InferenceSession::FailureInjector());
    }

    InjectorGuard(const InjectorGuard&) = delete;
    InjectorGuard& operator=(const InjectorGuard&) = delete;
};

} // namespace AiTest

NATRON_NAMESPACE_EXIT

#endif // Tests_Ai_AiTestSupport_h
