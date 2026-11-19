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

#include <cmath>
#include <filesystem>
#include <string>

#include <gtest/gtest.h>

#include "Engine/Optics/Glass.h"

using namespace Natron::Optics;

namespace {

const char* const kGlassJson = NATRON_TESTS_FIXTURES_DIR "/../../Gui/Resources/Optics/glass.json";

const GlassCatalogue&
catalogue()
{
    static const GlassCatalogue c = [] {
        GlassCatalogue loaded;
        std::string error;
        loaded.load(kGlassJson, &error);

        return loaded;
    }();

    return c;
}

} // namespace

TEST(OpticsGlass, CatalogueLoadsAllSixVendors)
{
    GlassCatalogue c;
    std::string error;
    ASSERT_TRUE(c.load(kGlassJson, &error)) << error;
    const std::vector<std::string> vendors = c.vendors();
    const std::vector<std::string> expected = { "CDGM", "Hikari", "Hoya", "Ohara", "Schott", "Sumita" };
    EXPECT_EQ(vendors, expected);
    EXPECT_GT(c.glasses().size(), 1000u);
}

TEST(OpticsGlass, CommittedJsonIsUnderOneMegabyte)
{
    EXPECT_LT(std::filesystem::file_size(kGlassJson), 1024u * 1024u);
}

TEST(OpticsGlass, SchottNBK7AtTheDLine)
{
    const Glass* bk7 = catalogue().find("Schott", "N-BK7");
    ASSERT_TRUE(bk7 != nullptr);
    EXPECT_NEAR(bk7->ior(587.6), 1.5168, 1e-4);
    EXPECT_GT(bk7->ior(486.1), bk7->ior(656.3));
}

TEST(OpticsGlass, EveryFormulaReproducesItsCatalogueIndex)
{
    for (const Glass& g : catalogue().glasses()) {
        EXPECT_NEAR(g.ior(kWavelengthDNm), g.nd, 2e-3) << g.vendor << " " << g.name;
    }
}

TEST(OpticsGlass, MatchingFindsTheNearestGlass)
{
    const Dispersion d = matchGlass(catalogue(), "Schott", 1.5168, 64.17, 0.);
    ASSERT_TRUE(d.glass != nullptr);
    EXPECT_EQ(d.glass->name, "N-BK7");
    EXPECT_NEAR(d.ior(587.6), 1.5168, 1e-4);
}

TEST(OpticsGlass, AbbeOffsetPicksADifferentGlass)
{
    const Dispersion d = matchGlass(catalogue(), "Schott", 1.5168, 64.17, 20.);
    ASSERT_TRUE(d.glass != nullptr);
    EXPECT_NE(d.glass->name, "N-BK7");
    EXPECT_GT(d.glass->vd, 75.);
}

TEST(OpticsGlass, FarOffGlassFallsBackToCauchy)
{
    const Dispersion d = matchGlass(catalogue(), "Schott", 1.25, 110., 0.);
    EXPECT_TRUE(d.glass == nullptr);
    EXPECT_FALSE(d.isAir());
    EXPECT_DOUBLE_EQ(d.ior(486.1), cauchyIor(1.25, 110., 486.1));
    EXPECT_NEAR(d.ior(587.6), 1.25, 1e-4);
    EXPECT_GT(d.ior(486.1), d.ior(656.3));
}

TEST(OpticsGlass, UnknownVendorFallsBackToCauchy)
{
    const Dispersion d = matchGlass(catalogue(), "NoSuchVendor", 1.5168, 64.17, 0.);
    EXPECT_TRUE(d.glass == nullptr);
    EXPECT_DOUBLE_EQ(d.ior(486.1), cauchyIor(1.5168, 64.17, 486.1));
}

TEST(OpticsGlass, MissingAbbeNumberIsEstimated)
{
    const Dispersion d = matchGlass(catalogue(), "NoSuchVendor", 1.62, 0., 0.);
    EXPECT_DOUBLE_EQ(d.vd, abbeFor(1.62));
}

TEST(OpticsGlass, AirIsOne)
{
    const Dispersion d = matchGlass(catalogue(), "Schott", 1., 0., 20.);
    EXPECT_TRUE(d.isAir());
    EXPECT_DOUBLE_EQ(d.ior(400.), 1.);
}

TEST(OpticsGlass, MalformedJsonIsAnError)
{
    GlassCatalogue c;
    std::string error;
    EXPECT_FALSE(c.parse("{\"glasses\": [{\"vendor\": \"X\"}]}", &error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(c.parse("{\"glasses\": [", &error));
}
