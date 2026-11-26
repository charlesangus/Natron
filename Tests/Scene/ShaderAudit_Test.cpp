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

#include "Global/Macros.h"

#include <string>

#include <gtest/gtest.h>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Project.h"

NATRON_NAMESPACE_USING
NATRON_PYTHON_NAMESPACE_USING

namespace {

class ShaderAuditTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }
};

} // namespace

// tools/shaders/audit_shader_nodes.py creates every shader of both renderers and compares its
// knobs, ports and defaults with its manifest entry.
TEST_F(ShaderAuditTest, EveryShaderHasEveryManifestAttributeWithItsDefault)
{
    const std::string app = getApp()->getAppIDString();
    const std::string script = "import sys\n"
                               "sys.dont_write_bytecode = True\n"
                               "sys.path.insert(0, r'" NATRON_TESTS_ENGINE_DIR "/../tools/shaders')\n"
                               "import audit_shader_nodes\n"
                               "problems = audit_shader_nodes.audit("
        + app + ")\n"
                "missing = [p for p in problems if p.startswith('missing ')]\n"
                "if problems:\n"
                "    raise RuntimeError('%d problems, %d missing attributes:\\n' % (len(problems), len(missing)) + '\\n'.join(problems[:200]))\n";
    std::string error;
    std::string output;

    ASSERT_TRUE(interpretPythonScript(script, &error, &output)) << error;
    EXPECT_TRUE(error.empty()) << error;
}
