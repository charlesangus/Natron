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

#include <array>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Nodes/Filter/NoiseProfiles.h"
#include "Engine/Nodes/IO/RawDecodeSupport.h"
#include "Engine/Nodes/IO/RawMetadata.h"

#include "RawFixtures.h"

NATRON_NAMESPACE_USING

namespace {
// The Canon EOS 5D Mark III rows of Engine/Nodes/Filter/data/noiseprofiles.json.
const std::array<double, 3> kIso100A = { 2.35525873489583e-06, 1.55413089907708e-06, 8.60663421379715e-07 };
const std::array<double, 3> kIso100B = { 1.61191836573806e-08, -3.4355760673268e-08, 4.46630475780481e-07 };
// ISO 110 lies 10/25 of the way from ISO 100 to ISO 125.
const std::array<double, 3> kIso110A = { 2.560459414e-06, 1.690862643e-06, 8.866552686e-07 };
const std::array<double, 3> kIso110B = { 2.174457041e-08, -3.493144711e-08, 4.972541987e-07 };

void
expectNear(const std::array<double, 3>& expected,
           const std::array<float, 3>& actual,
           const char* what)
{
    for (int c = 0; c < 3; ++c) {
        EXPECT_NEAR(expected[c], (double)actual[c], std::fabs(expected[c]) * 1e-6) << what << "[" << c << "]";
    }
}

NoiseProfiles::Entry
entry(const char* name,
      double iso,
      double a,
      bool skip = false)
{
    NoiseProfiles::Entry e;

    e.name = name;
    e.iso = iso;
    e.a = { a, a, a };
    e.b = { 0., 0., 0. };
    e.skip = skip;

    return e;
}

NoiseProfiles::Database
syntheticDatabase()
{
    NoiseProfiles::Model model;
    model.model = "Cam";
    model.profiles = {
        entry("first 400", 400., 4e-5),
        entry("100", 100., 1e-5),
        entry("skipped 200", 200., 9e-5, true),
        entry("second 400", 400., 5e-5),
        entry("200", 200.7, 2e-5),
    };
    NoiseProfiles::Maker maker;
    maker.maker = "Maker";
    maker.models = { model };

    return { maker };
}
} // namespace

TEST(NoiseProfilesTest, TheDatabaseIsInstalledBesideTheExecutable)
{
    EXPECT_FALSE(NoiseProfiles::profilesJsonPath().empty());
    EXPECT_FALSE(NoiseProfiles::database().empty());
}

TEST(NoiseProfilesTest, LoadRejectsAMissingFile)
{
    NoiseProfiles::Database database;
    std::string error;

    EXPECT_FALSE(NoiseProfiles::load("/nonexistent/noiseprofiles.json", &database, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_TRUE(database.empty());
}

TEST(NoiseProfilesTest, AnExactIsoIsFound)
{
    const std::vector<NoiseProfiles::Profile> profiles = NoiseProfiles::matching(NoiseProfiles::database(), "Canon", "EOS 5D Mark III");

    ASSERT_FALSE(profiles.empty());
    const NoiseProfiles::AutoProfile found = NoiseProfiles::autoProfile(profiles, 100);
    EXPECT_TRUE(found.detected);
    EXPECT_EQ("found ISO 100", found.name);
    EXPECT_EQ(100, found.profile.iso);
    expectNear(kIso100A, found.profile.a, "a");
    expectNear(kIso100B, found.profile.b, "b");
}

TEST(NoiseProfilesTest, AnIsoBetweenEntriesIsInterpolated)
{
    const std::vector<NoiseProfiles::Profile> profiles = NoiseProfiles::matching(NoiseProfiles::database(), "Canon", "EOS 5D Mark III");
    const NoiseProfiles::AutoProfile found = NoiseProfiles::autoProfile(profiles, 110);

    EXPECT_TRUE(found.detected);
    EXPECT_EQ("interpolated ISO 110", found.name);
    EXPECT_EQ(110, found.profile.iso);
    expectNear(kIso110A, found.profile.a, "a");
    expectNear(kIso110B, found.profile.b, "b");
}

TEST(NoiseProfilesTest, AnIsoOutsideTheProfiledRangeGetsTheGenericProfile)
{
    const std::vector<NoiseProfiles::Profile> profiles = NoiseProfiles::matching(NoiseProfiles::database(), "Canon", "EOS 5D Mark III");

    for (int iso : { 25, 204800 }) {
        const NoiseProfiles::AutoProfile found = NoiseProfiles::autoProfile(profiles, iso);
        EXPECT_FALSE(found.detected) << iso;
        EXPECT_EQ("generic poissonian", found.name) << iso;
    }
}

TEST(NoiseProfilesTest, AnUnknownCameraGetsTheGenericProfile)
{
    const std::vector<NoiseProfiles::Profile> profiles = NoiseProfiles::matching(NoiseProfiles::database(), "Nobody", "Nothing 1");

    EXPECT_TRUE(profiles.empty());
    const NoiseProfiles::AutoProfile found = NoiseProfiles::autoProfile(profiles, 100);
    EXPECT_FALSE(found.detected);
    EXPECT_EQ("generic poissonian", found.name);
    for (int c = 0; c < 3; ++c) {
        EXPECT_EQ(0.0001f, found.profile.a[c]);
        EXPECT_EQ(0.f, found.profile.b[c]);
    }
}

TEST(NoiseProfilesTest, TheMakerMatchesAsASubstringAndTheModelExactly)
{
    const NoiseProfiles::Database db = syntheticDatabase();

    EXPECT_FALSE(NoiseProfiles::matching(db, "The Maker Co.", "Cam").empty());
    EXPECT_TRUE(NoiseProfiles::matching(db, "Make", "Cam").empty());
    EXPECT_TRUE(NoiseProfiles::matching(db, "Maker", "Cam 2").empty());
}

TEST(NoiseProfilesTest, MatchingSkipsSortsByIsoAndReversesTies)
{
    const std::vector<NoiseProfiles::Profile> profiles = NoiseProfiles::matching(syntheticDatabase(), "Maker", "Cam");

    ASSERT_EQ(4u, profiles.size());
    EXPECT_EQ("100", profiles[0].name);
    EXPECT_EQ("200", profiles[1].name);
    EXPECT_EQ(200, profiles[1].iso);
    EXPECT_EQ("second 400", profiles[2].name);
    EXPECT_EQ("first 400", profiles[3].name);
}

TEST(NoiseProfilesTest, TheFixtureCameraResolvesFromItsMetadata)
{
    std::string error;
    const std::shared_ptr<const RawDecodeSupport::Header> header = RawDecodeSupport::readHeader(bayerFixture().path(), &error);

    ASSERT_TRUE(bool(header)) << error;
    const std::optional<std::string> make = RawMetadata::getMake(header->metadata);
    const std::optional<std::string> model = RawMetadata::getModel(header->metadata);
    const std::optional<double> iso = RawMetadata::getIso(header->metadata);
    ASSERT_TRUE(make && model && iso);
    const std::vector<NoiseProfiles::Profile> profiles = NoiseProfiles::matching(NoiseProfiles::database(), *make, *model);
    EXPECT_EQ(24u, profiles.size());
    const NoiseProfiles::AutoProfile found = NoiseProfiles::autoProfile(profiles, (int)*iso);
    EXPECT_TRUE(found.detected) << *iso;
}

TEST(NoiseProfilesTest, TheXTransFixtureCameraIsInTheDatabase)
{
    std::string error;
    const std::shared_ptr<const RawDecodeSupport::Header> header = RawDecodeSupport::readHeader(xtransFixture().path(), &error);

    ASSERT_TRUE(bool(header)) << error;
    const std::vector<NoiseProfiles::Profile> profiles = NoiseProfiles::matching(NoiseProfiles::database(), RawMetadata::getMake(header->metadata).value_or(""), RawMetadata::getModel(header->metadata).value_or(""));
    EXPECT_EQ(19u, profiles.size());
}
