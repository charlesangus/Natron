/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
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

#include <set>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Optics/LensDatabase.h"
#include "Engine/Optics/Prescription.h"
#include "Engine/Optics/ZemaxParser.h"

using namespace Natron::Optics;

namespace {

const char* const kArchive = NATRON_TESTS_FIXTURES_DIR "/../../Gui/Resources/Optics/lenses.zmx.gz";
const char* const kNikonFile = NATRON_TESTS_FIXTURES_DIR "/optics/nikon-1-nikkor-10mm-f2.8.zmx";
const char* const kNikonId = "Nikon/Nikon 1 Nikkor 10mm f2.8";

LensDatabase&
database()
{
    static LensDatabase db(kArchive);

    return db;
}

} // namespace

TEST(OpticsDatabase, TheArchiveLoadsWithoutFailures)
{
    std::string error;
    ASSERT_TRUE(database().load(&error)) << error;
    EXPECT_TRUE(database().loadFailures().empty()) << database().loadFailures().front();
    EXPECT_EQ(std::string("1b007269eda014b9e4d23670d5d5d7dd2dfe6de5"), database().sourceCommit());
    // 986 OpticsDatabase lenses at the pinned commit, plus the 9 Classic Designs.
    EXPECT_GE(database().lenses().size(), 995u);
}

TEST(OpticsDatabase, IdsAreUniqueAndStartWithTheVendor)
{
    std::set<std::string> ids;
    for (const LensInfo& lens : database().lenses()) {
        EXPECT_TRUE(ids.insert(lens.id).second) << lens.id;
        EXPECT_EQ(lens.vendor + "/", lens.id.substr(0, lens.vendor.size() + 1)) << lens.id;
        EXPECT_EQ(lens.id, database().find(lens.id)->id);
    }
}

TEST(OpticsDatabase, ClassicDesignsComeFirstWithNineLenses)
{
    const std::vector<std::string> vendors = database().vendors();
    ASSERT_FALSE(vendors.empty());
    EXPECT_EQ(std::string(kClassicDesignsVendor), vendors.front());
    EXPECT_GT(vendors.size(), 30u);

    int classic = 0;
    for (const LensInfo& lens : database().lenses()) {
        if (lens.vendor == kClassicDesignsVendor) {
            ++classic;
            EXPECT_TRUE(lens.sourcePath.empty()) << lens.id;
            EXPECT_FALSE(lens.source.empty()) << lens.id;
        }
    }
    EXPECT_EQ(9, classic);
    EXPECT_EQ(builtinLensIds().size(), (size_t)classic);

    Prescription planar;
    std::string error;
    ASSERT_TRUE(database().prescription(std::string(kClassicDesignsVendor) + "/planar50", &planar, &error)) << error;
    Prescription builtin;
    ASSERT_TRUE(builtinLens("planar50", &builtin));
    EXPECT_EQ(builtin.surfaces.size(), planar.surfaces.size());
}

TEST(OpticsDatabase, MetadataDescribesTheLens)
{
    const LensInfo* nikon = database().find(kNikonId);
    ASSERT_NE(nullptr, nikon);
    EXPECT_EQ(std::string("Nikon"), nikon->vendor);
    EXPECT_EQ(std::string("Nikon 1 Nikkor 10mm f2.8"), nikon->name);
    EXPECT_NE(std::string::npos, nikon->title.find("JP 2011-248340"));
    EXPECT_NE(std::string::npos, nikon->source.find("Bill Claff"));
    EXPECT_EQ(std::string("Optics/Photography/Nikon/Nikon 1/Nikon 1 Nikkor 10mm f2.8.zmx"), nikon->sourcePath);
    EXPECT_NEAR(10., nikon->focalLength, 0.5);
    EXPECT_DOUBLE_EQ(2.92, nikon->fNumber);
    EXPECT_TRUE(nikon->aspheric);
    EXPECT_FALSE(nikon->anamorphic);
    EXPECT_GT(nikon->elementCount, 4);

    const LensInfo* triplet = database().find(std::string(kClassicDesignsVendor) + "/triplet50");
    ASSERT_NE(nullptr, triplet);
    EXPECT_EQ(3, triplet->elementCount);
    EXPECT_NEAR(50., triplet->focalLength, 0.5);
    EXPECT_FALSE(triplet->aspheric);
}

TEST(OpticsDatabase, APrescriptionMatchesItsOriginalFile)
{
    Prescription archived;
    std::string error;
    ASSERT_TRUE(database().prescription(kNikonId, &archived, &error)) << error;
    Prescription original;
    LensFileError fileError;
    ASSERT_TRUE(loadLensFile(kNikonFile, &original, &fileError)) << fileError.text();

    EXPECT_EQ(kNikonId, archived.id);
    EXPECT_EQ(original.name, archived.name);
    EXPECT_EQ(original.source, archived.source);
    EXPECT_EQ(original.fNumber, archived.fNumber);
    EXPECT_EQ(original.imageDistance, archived.imageDistance);
    ASSERT_EQ(original.surfaces.size(), archived.surfaces.size());
    for (size_t i = 0; i < original.surfaces.size(); ++i) {
        const Surface& a = archived.surfaces[i];
        const Surface& o = original.surfaces[i];
        EXPECT_EQ(o.type, a.type) << i;
        EXPECT_EQ(o.curvature, a.curvature) << i;
        EXPECT_EQ(o.spacing, a.spacing) << i;
        EXPECT_EQ(o.semiDiameter, a.semiDiameter) << i;
        EXPECT_EQ(o.conic, a.conic) << i;
        EXPECT_EQ(o.coefficients, a.coefficients) << i;
        EXPECT_EQ(o.nd, a.nd) << i;
        EXPECT_EQ(o.vd, a.vd) << i;
    }

    Prescription again;
    ASSERT_TRUE(database().prescription(kNikonId, &again, &error)) << error;
    EXPECT_EQ(archived.surfaces.size(), again.surfaces.size());
}

TEST(OpticsDatabase, AMissingIdIsAnErrorNamingIt)
{
    Prescription lens;
    std::string error;
    EXPECT_FALSE(database().prescription("Nikon/No Such Lens", &lens, &error));
    EXPECT_NE(std::string::npos, error.find("Nikon/No Such Lens")) << error;
    EXPECT_EQ(nullptr, database().find("Nikon/No Such Lens"));
}

TEST(OpticsDatabase, ConcurrentFirstLoadsReadTheArchiveOnce)
{
    LensDatabase db(kArchive);
    std::vector<size_t> counts(8, 0);
    std::vector<std::thread> threads;
    for (size_t i = 0; i < counts.size(); ++i) {
        threads.emplace_back([&db, &counts, i] { counts[i] = db.lenses().size(); });
    }
    for (std::thread& t : threads) {
        t.join();
    }
    EXPECT_EQ(1, db.archiveReads());
    for (size_t count : counts) {
        EXPECT_EQ(db.lenses().size(), count);
    }
}

TEST(OpticsDatabase, AMissingArchiveStillListsTheClassicDesigns)
{
    LensDatabase db(NATRON_TESTS_FIXTURES_DIR "/optics/no-such-archive.zmx.gz");
    std::string error;
    EXPECT_FALSE(db.load(&error));
    EXPECT_NE(std::string::npos, error.find("no-such-archive")) << error;
    EXPECT_EQ(9u, db.lenses().size());
}

TEST(OpticsDatabase, SearchMatchesEveryWord)
{
    const LensInfo* nikon = database().find(kNikonId);
    ASSERT_NE(nullptr, nikon);
    EXPECT_TRUE(lensMatches(*nikon, ""));
    EXPECT_TRUE(lensMatches(*nikon, "nikkor 10MM"));
    EXPECT_TRUE(lensMatches(*nikon, "claff"));
    EXPECT_FALSE(lensMatches(*nikon, "nikkor canon"));
}
