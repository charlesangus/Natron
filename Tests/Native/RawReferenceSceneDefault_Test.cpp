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

#include <string>

#include <QString>

#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Raw/RawColorCalibration.h"
#include "Engine/Nodes/Raw/RawTemperature.h"

NATRON_NAMESPACE_USING

namespace {
// The linear DNG's crop (see the fixtures README).
RawReference::CropSite
dngSite()
{
    return { &linearFixture(), 3960, 2640, 2100, 1180, false };
}

ImageMetadata
outputMetadata(const NodePtr& node)
{
    NativeEffectBase* effect = dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());

    return effect ? effect->getOutputMetadata(1., ViewIdx(0)) : ImageMetadata();
}

QString
warningOf(const NodePtr& node)
{
    QString message;
    int type = 0;

    if (node->hasPersistentMessage()) {
        node->getPersistentMessage(&message, &type, false);
    }

    return message;
}
} // namespace

/**
 * The Develop RAW chain exactly as created, every node at its defaults, against darktable's
 * scene-referred default jobs (make-reference.py "scene-referred-default"): white balance as shot
 * to reference and color calibration at its file-derived defaults.
 **/
class RawReferenceSceneDefaultTest
    : public RawReferenceFixture {
protected:
    void SetUp() OVERRIDE
    {
        RawReferenceFixture::SetUp();
        setLinearRec709WorkingSpace();
    }

    // make-reference.py renders in darktable's linear Rec709 working space, which the CAT and the
    // gamut compression act in.
    void setLinearRec709WorkingSpace()
    {
        KnobChoicePtr workingSpace = getApp()->getProject()->getKnobByNameAndType<KnobChoice>("workingSpace");
        ASSERT_TRUE(bool(workingSpace));
        workingSpace->setValueFromID("Linear Rec.709 (sRGB)", 0);
        ASSERT_EQ(std::string("Linear Rec.709 (sRGB)"), getApp()->getProject()->getWorkingColorSpace());
    }

    static void expectSceneReferredDefaults(const RawDevelop::Chain& chain,
                                            const std::string& name)
    {
        EXPECT_EQ((int)eRawTemperatureModeAsShotToReference, chain.temperature->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawTemperatureParamMode)->getValue()) << name;
        EXPECT_FALSE(chain.colorCalibration->getEffectInstance()->getKnobByNameAndType<KnobBool>(kRawColorCalibrationParamCatOverride)->getValue()) << name;
        EXPECT_TRUE(RawMetadata::getCatApplied(outputMetadata(chain.colorCalibration))) << name;
        // The white balance is the reference and the CAT is the only one: nothing to warn about.
        EXPECT_TRUE(warningOf(chain.colorCalibration).isEmpty()) << name << ": " << warningOf(chain.colorCalibration).toStdString();
    }

    void compareWithReference(const RawReference::CropSite& site,
                              const char* name,
                              const char* reference,
                              const RawReference::Tolerance& tolerance)
    {
        const RawDevelop::Chain chain = createDefaultChain(site);
        ASSERT_TRUE(chain.complete()) << name;
        expectSceneReferredDefaults(chain, name);
        compareCrop(name, site, chain.colorCalibration, reference, tolerance);
    }
};

TEST_F(RawReferenceSceneDefaultTest, Cr2DefaultChainMatchesDarktable)
{
    compareWithReference(RawReference::cr2Site(), "CR2 scene-referred default", "canon-eos-5d-mark-iii-scene-referred-default.tif", RawReference::kRCDClass);
}

TEST_F(RawReferenceSceneDefaultTest, RafDefaultChainMatchesDarktable)
{
    compareWithReference(RawReference::rafSite(), "RAF scene-referred default", "fujifilm-x-pro2-scene-referred-default.tif", RawReference::kMarkesteijnClass);
}

// The linear DNG has no mosaic, so demosaic passes its channels through; the README gives it the
// RCD class.
TEST_F(RawReferenceSceneDefaultTest, LinearDngDefaultChainMatchesDarktable)
{
    compareWithReference(dngSite(), "DNG scene-referred default", "canon-eos-5d-mark-iii-linear-rgb-scene-referred-default.tif", RawReference::kRCDClass);
}
