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

#include "RawReferenceSupport.h"

#include "Engine/Nodes/Raw/RawDemosaic.h"

NATRON_NAMESPACE_USING

namespace {
/**
 * One demosaic method on one camera file, compared at the tolerance class of
 * Tests/fixtures/raw/README.md.
 **/
struct ReferenceCase {
    const char* name;
    RawReference::CropSite site;
    int method;
    const char* reference;
    RawReference::Tolerance tolerance;
};
} // namespace

class RawReferenceTest
    : public RawReferenceFixture {
protected:
    void compareWithReference(const ReferenceCase& c)
    {
        const RawDevelop::Chain chain = createChain(c.site);
        ASSERT_TRUE(chain.complete()) << c.name;
        EffectInstancePtr demosaic = chain.demosaic->getEffectInstance();
        demosaic->getKnobByNameAndType<KnobChoice>(c.site.xtrans ? kRawDemosaicParamXTransMethod : kRawDemosaicParamBayerMethod)->setValue(c.method);
        compareCrop(c.name, c.site, chain.inputColor, c.reference, c.tolerance);
    }
};

TEST_F(RawReferenceTest, BayerPPGMatchesDarktable)
{
    compareWithReference({ "Bayer PPG", RawReference::cr2Site(), (int)eRawDemosaicBayerPPG, "canon-eos-5d-mark-iii-ppg.tif", RawReference::kRCDClass });
}

TEST_F(RawReferenceTest, BayerRCDMatchesDarktable)
{
    compareWithReference({ "Bayer RCD", RawReference::cr2Site(), (int)eRawDemosaicBayerRCD, "canon-eos-5d-mark-iii-rcd.tif", RawReference::kRCDClass });
}

TEST_F(RawReferenceTest, BayerAMaZEMatchesDarktable)
{
    compareWithReference({ "Bayer AMaZE", RawReference::cr2Site(), (int)eRawDemosaicBayerAMaZE, "canon-eos-5d-mark-iii-amaze.tif", RawReference::kAMaZEClass });
}

TEST_F(RawReferenceTest, XTransMarkesteijnMatchesDarktable)
{
    compareWithReference({ "X-Trans Markesteijn 1-pass", RawReference::rafSite(), (int)eRawDemosaicXTransMarkesteijn1, "fujifilm-x-pro2-markesteijn.tif", RawReference::kMarkesteijnClass });
}

TEST_F(RawReferenceTest, XTransMarkesteijn3MatchesDarktable)
{
    compareWithReference({ "X-Trans Markesteijn 3-pass", RawReference::rafSite(), (int)eRawDemosaicXTransMarkesteijn3, "fujifilm-x-pro2-markesteijn3.tif", RawReference::kMarkesteijnClass });
}
