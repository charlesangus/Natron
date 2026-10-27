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

#include <algorithm>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Ai/OrtLoader.h"
#include "Tests/Ai/AiTestSupport.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kBogusPath = "/nonexistent/natron-ort-loader-test/onnxruntime";

} // namespace

TEST(AiOrtLoader, LoadsTheBuildTreeLibrary)
{
    const std::shared_ptr<const OrtLoader>& ort = AiTest::loader();

    ASSERT_TRUE(ort->available()) << ort->unavailableReason();
    EXPECT_TRUE(ort->unavailableReason().empty());
    EXPECT_NE(nullptr, ort->api());
    EXPECT_NE(nullptr, ort->env());
    EXPECT_GE(ort->apiVersion(), OrtLoader::kMinimumApiVersion);
    EXPECT_FALSE(ort->versionString().empty());
    EXPECT_NE("unknown", ort->versionString());
    EXPECT_EQ(0u, ort->libraryPath().find(AiTest::ortDir())) << ort->libraryPath();
    EXPECT_TRUE(ort->hasProvider("CPUExecutionProvider"));
    EXPECT_NE(ort->providers().end(), std::find(ort->providers().begin(), ort->providers().end(), "CPUExecutionProvider"));
}

TEST(AiOrtLoader, BogusPathIsUnavailableAndNamesThePath)
{
    const std::shared_ptr<const OrtLoader> ort = OrtLoader::load({ OrtLoader::Candidate { kBogusPath, true } });

    EXPECT_FALSE(ort->available());
    EXPECT_EQ(nullptr, ort->api());
    EXPECT_TRUE(ort->providers().empty());
    EXPECT_NE(std::string::npos, ort->unavailableReason().find(kBogusPath)) << ort->unavailableReason();
}

TEST(AiOrtLoader, NoCandidatesIsUnavailable)
{
    const std::shared_ptr<const OrtLoader> ort = OrtLoader::load(std::vector<OrtLoader::Candidate>());

    EXPECT_FALSE(ort->available());
    EXPECT_FALSE(ort->unavailableReason().empty());
}

TEST(AiOrtLoader, AFileThatIsNotALibraryIsUnavailable)
{
    const std::string model = AiTest::modelPath("identity.onnx");
    const std::shared_ptr<const OrtLoader> ort = OrtLoader::load({ OrtLoader::Candidate { model, true } });

    EXPECT_FALSE(ort->available());
    EXPECT_NE(std::string::npos, ort->unavailableReason().find(model)) << ort->unavailableReason();
}

TEST(AiOrtLoader, ARequiredCandidateThatFailsEndsTheSearch)
{
    const std::shared_ptr<const OrtLoader> ort = OrtLoader::load({ OrtLoader::Candidate { kBogusPath, true },
                                                                   OrtLoader::Candidate { AiTest::ortDir(), false } });

    EXPECT_FALSE(ort->available());
    EXPECT_NE(std::string::npos, ort->unavailableReason().find(kBogusPath)) << ort->unavailableReason();
}

TEST(AiOrtLoader, AnOptionalCandidateThatFailsFallsThrough)
{
    const std::shared_ptr<const OrtLoader> ort = OrtLoader::load({ OrtLoader::Candidate { kBogusPath, false },
                                                                   OrtLoader::Candidate { AiTest::ortDir(), false } });

    ASSERT_TRUE(ort->available()) << ort->unavailableReason();
    EXPECT_TRUE(ort->hasProvider("CPUExecutionProvider"));
}

TEST(AiOrtLoader, SearchOrderIsPreferenceThenUserDataThenBundle)
{
    const std::vector<OrtLoader::Candidate> all = OrtLoader::searchCandidates("/prefs/ort", "/home/u/.local/share/Natron", "/opt/Natron/bin");

    ASSERT_EQ(3u, all.size());
    EXPECT_EQ("/prefs/ort", all[0].path);
    EXPECT_TRUE(all[0].required);
    EXPECT_EQ("/home/u/.local/share/Natron/onnxruntime", all[1].path);
    EXPECT_FALSE(all[1].required);
    EXPECT_EQ("/opt/Natron/lib/onnxruntime", all[2].path);
    EXPECT_FALSE(all[2].required);

    const std::vector<OrtLoader::Candidate> noPreference = OrtLoader::searchCandidates(std::string(), "/data", "/opt/Natron/bin");
    ASSERT_EQ(2u, noPreference.size());
    EXPECT_EQ("/data/onnxruntime", noPreference[0].path);
    EXPECT_FALSE(noPreference[0].required);
}
