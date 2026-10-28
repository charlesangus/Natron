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

#ifndef NATRON_TESTS_SCENE_CYCLESTESTUTILS_H
#define NATRON_TESTS_SCENE_CYCLESTESTUTILS_H

// Include after Python.h and UsdIncludes.h, as the tests that use it do.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#include <gtest/gtest.h>

#include <pxr/base/plug/registry.h>
#include <pxr/base/vt/value.h>

namespace CyclesTestUtils {
// The Cycles delegate comes from the build image (tools/ci/local/build-cycles.sh), not from this
// tree, so a build in an older image has none. The bundled gtest has no GTEST_SKIP, so the test
// returns early on false; NATRON_TEST_REQUIRE_CYCLES=1 turns that skip into a failure.
inline bool
cyclesOrSkip()
{
    if (PXR_NS::PlugRegistry::GetInstance().GetPluginWithName("hdCycles")) {
        return true;
    }
    const char* reason = "OpenUSD has no hdCycles plug-in registered; this build image predates the "
                         "Cycles render delegate (tools/ci/local/build-cycles.sh)";
    const char* require = std::getenv("NATRON_TEST_REQUIRE_CYCLES");
    if (require && (std::strcmp(require, "1") == 0)) {
        ADD_FAILURE() << "NATRON_TEST_REQUIRE_CYCLES=1 but " << reason;
    } else {
        std::printf("[  SKIPPED ] %s\n", reason);
    }

    return false;
}

// A sample count and seed fixed for every Cycles engine the tests make, and the adaptive sampler,
// which stops pixels at a noise level rather than a count, off.
inline std::map<std::string, PXR_NS::VtValue>
fixedSampling(int samples)
{
    std::map<std::string, PXR_NS::VtValue> settings;
    settings["cycles:samples"] = PXR_NS::VtValue(samples);
    settings["cycles:integrator:seed"] = PXR_NS::VtValue(7);
    settings["cycles:integrator:use_adaptive_sampling"] = PXR_NS::VtValue(false);

    return settings;
}
} // namespace CyclesTestUtils

#endif // NATRON_TESTS_SCENE_CYCLESTESTUTILS_H
