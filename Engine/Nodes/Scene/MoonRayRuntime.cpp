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

#include "MoonRayRuntime.h"

#include <cstdlib>
#include <iostream>

#include <sys/stat.h>

#include "Engine/Nodes/Scene/UsdIncludes.h"

NATRON_NAMESPACE_ENTER

namespace {
bool
isFile(const std::string& path)
{
    struct stat info;

    return (::stat(path.c_str(), &info) == 0) && S_ISREG(info.st_mode);
}

// MoonRay is compiled with -march=core-avx2 throughout, so loading any of it on an older CPU
// faults on the first such instruction.
bool
cpuRunsMoonRay()
{
    __builtin_cpu_init();

    return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma") && __builtin_cpu_supports("bmi2");
}

void
prependToEnv(const char* name,
             const std::string& dir)
{
    const char* current = std::getenv(name);
    std::string value = dir;

    if (current && *current) {
        value += ":";
        value += current;
    }
    ::setenv(name, value.c_str(), 1);
}
} // anon namespace

namespace MoonRayRuntime {
std::string
setUp(const std::string& applicationDirPath)
{
    static const std::string installed = [&applicationDirPath]() -> std::string {
        const std::string pluginInfo = "/plugin/pxr/plugInfo.json";
        std::string prefix = applicationDirPath + "/../moonray";

        if (!isFile(prefix + pluginInfo)) {
#ifdef NATRON_MOONRAY_PREFIX
            prefix = NATRON_MOONRAY_PREFIX;
            if (!isFile(prefix + pluginInfo)) {
                return std::string();
            }
#else
            return std::string();
#endif
        }
        if (char* resolved = ::realpath(prefix.c_str(), nullptr)) {
            prefix = resolved;
            std::free(resolved);
        }
        if (!cpuRunsMoonRay()) {
            std::cerr << "MoonRay needs a CPU with AVX2; its render delegate is not registered" << std::endl;

            return std::string();
        }

        // Ours first, so that execComp and the built-in DSOs match the delegate that loads them.
        prependToEnv("RDL2_DSO_PATH", prefix + "/rdl2dso");
        prependToEnv("ARRAS_SESSION_PATH", prefix + "/sessions");
        prependToEnv("PATH", prefix + "/bin");

        if (PXR_NS::PlugRegistry::GetInstance().RegisterPlugins(prefix + pluginInfo).empty()) {
            std::cerr << "MoonRay at " << prefix << " registered no USD plug-ins" << std::endl;
        }

        return prefix;
    }();

    return installed;
}
} // namespace MoonRayRuntime

NATRON_NAMESPACE_EXIT
