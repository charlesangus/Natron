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

#include "Global/Macros.h"

#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "Tests/BaseTest.h"

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/KnobReader.h"
#include "Engine/Optics/LensDatabase.h"
#include "Engine/Optics/LensKnobs.h"

NATRON_NAMESPACE_USING

using Optics::LensKnobOptions;
using Optics::LensKnobs;
using Optics::LensModelOption;
using Optics::Prescription;

namespace {

const char* const kArchive = NATRON_TESTS_FIXTURES_DIR "/../../Gui/Resources/Optics/lenses.zmx.gz";
const char* const kBadFile = NATRON_TESTS_FIXTURES_DIR "/optics/bad-curvature.zmx";
const char* const kTriplet = NATRON_TESTS_FIXTURES_DIR "/optics/cooke-triplet-50mm-f4.zmx";

KnobPagePtr
addPage(EffectInstance* effect)
{
    return AppManager::createKnob<KnobPage>(effect, std::string("Lens"));
}

int
modelIndex(const KnobChoicePtr& model,
           const std::string& id)
{
    const std::vector<ChoiceOption> entries = model->getEntries_mt_safe();
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].id == id) {
            return (int)i;
        }
    }

    return -1;
}

} // namespace

class OpticsLensKnobsTest
    : public BaseTest {
protected:
    void SetUp() override
    {
        BaseTest::SetUp();
        Optics::LensDatabase::setSharedArchivePath(kArchive);
        ASSERT_TRUE(Optics::LensDatabase::shared().find("Nikon/Nikon 1 Nikkor 10mm f2.8") != nullptr);
    }
};

TEST_F(OpticsLensKnobsTest, KnobsHaveTheirNamesAndDefaults)
{
    NodePtr node = createNode(_generatorPluginID);
    ASSERT_TRUE(bool(node));
    EffectInstance* effect = node->getEffectInstance().get();
    LensKnobOptions options;
    options.leadingModels.push_back(LensModelOption { "procedural", "Procedural", "" });
    options.defaultModel = "procedural";
    const LensKnobs knobs = LensKnobs::create(effect, addPage(effect), options);

    KnobChoicePtr model = std::dynamic_pointer_cast<KnobChoice>(effect->getKnobByName("lensModel"));
    ASSERT_TRUE(bool(model));
    EXPECT_EQ(std::string("Lens Model"), model->getLabel());
    const std::vector<ChoiceOption> entries = model->getEntries_mt_safe();
    ASSERT_EQ(3u, entries.size());
    EXPECT_EQ(std::string("procedural"), entries[0].id);
    EXPECT_EQ(std::string(kLensModelDatabase), entries[1].id);
    EXPECT_EQ(std::string(kLensModelFromFile), entries[2].id);
    EXPECT_EQ(0, model->getDefaultValue(0));
    EXPECT_EQ(std::string("procedural"), knobs.model(KnobReader(0., ViewIdx(0))));

    KnobStringPtr lens = std::dynamic_pointer_cast<KnobString>(effect->getKnobByName("lens"));
    ASSERT_TRUE(bool(lens));
    EXPECT_EQ(std::string("Lens"), lens->getLabel());
    EXPECT_EQ(std::string("Classic Designs/dgauss50"), lens->getDefaultValue(0));
    EXPECT_TRUE(bool(std::dynamic_pointer_cast<KnobButton>(effect->getKnobByName("lensBrowse"))));
    KnobFilePtr file = std::dynamic_pointer_cast<KnobFile>(effect->getKnobByName("lensFile"));
    ASSERT_TRUE(bool(file));
    EXPECT_EQ(std::string("Lens File"), file->getLabel());

    KnobChoicePtr vendor = std::dynamic_pointer_cast<KnobChoice>(effect->getKnobByName("glassVendor"));
    ASSERT_TRUE(bool(vendor));
    EXPECT_EQ(std::string("Glass Vendor"), vendor->getLabel());
    EXPECT_EQ(6u, vendor->getEntries_mt_safe().size());
    EXPECT_EQ(std::string("Schott"), knobs.glassVendor(KnobReader(0., ViewIdx(0))));

    KnobDoublePtr abbe = std::dynamic_pointer_cast<KnobDouble>(effect->getKnobByName("abbeOffset"));
    ASSERT_TRUE(bool(abbe));
    EXPECT_EQ(std::string("Abbe Offset"), abbe->getLabel());
    EXPECT_EQ(0., abbe->getDefaultValue(0));
    EXPECT_EQ(-20., abbe->getMinimum(0));
    EXPECT_EQ(20., abbe->getMaximum(0));

    KnobBoolPtr aspheric = std::dynamic_pointer_cast<KnobBool>(effect->getKnobByName("useAspheric"));
    ASSERT_TRUE(bool(aspheric));
    EXPECT_EQ(std::string("Use Aspheric"), aspheric->getLabel());
    EXPECT_TRUE(aspheric->getDefaultValue(0));
    EXPECT_TRUE(knobs.useAspheric(KnobReader(0., ViewIdx(0))));

    EXPECT_TRUE(lens->getIsSecret());
    EXPECT_TRUE(file->getIsSecret());
    model->setValue(modelIndex(model, kLensModelDatabase));
    EXPECT_TRUE(knobs.knobChanged(model.get()));
    EXPECT_FALSE(lens->getIsSecret());
    EXPECT_TRUE(file->getIsSecret());
}

TEST_F(OpticsLensKnobsTest, ResolveReportsEachModel)
{
    NodePtr node = createNode(_generatorPluginID);
    ASSERT_TRUE(bool(node));
    EffectInstance* effect = node->getEffectInstance().get();
    const LensKnobs knobs = LensKnobs::create(effect, addPage(effect));
    KnobChoicePtr model = knobs.lensModelKnob();
    KnobStringPtr lens = knobs.lensKnob();
    KnobFilePtr file = knobs.lensFileKnob();
    ASSERT_TRUE(bool(model));
    ASSERT_TRUE(bool(lens));
    ASSERT_TRUE(bool(file));
    EXPECT_EQ(std::string(kLensModelDatabase), knobs.model(KnobReader(0., ViewIdx(0))));

    Prescription out;
    std::string error;
    EXPECT_TRUE(knobs.resolve(KnobReader(0., ViewIdx(0)), &out, &error)) << error;
    EXPECT_EQ(std::string("Classic Designs/dgauss50"), out.id);

    lens->setValue(std::string("Nikon/Nikon 1 Nikkor 10mm f2.8"));
    error.clear();
    EXPECT_TRUE(knobs.resolve(KnobReader(0., ViewIdx(0)), &out, &error)) << error;
    EXPECT_EQ(std::string("Nikon/Nikon 1 Nikkor 10mm f2.8"), out.id);
    EXPECT_TRUE(out.hasAspheres());

    lens->setValue(std::string("Nikon/Renamed Upstream"));
    error.clear();
    EXPECT_FALSE(knobs.resolve(KnobReader(0., ViewIdx(0)), &out, &error));
    EXPECT_NE(std::string::npos, error.find("Nikon/Renamed Upstream")) << error;
    EXPECT_EQ(std::string("Nikon/Renamed Upstream"), lens->getValue());

    model->setValue(modelIndex(model, kLensModelFromFile));
    file->setValue(std::string(kBadFile));
    error.clear();
    EXPECT_FALSE(knobs.resolve(KnobReader(0., ViewIdx(0)), &out, &error));
    EXPECT_NE(std::string::npos, error.find("line 5")) << error;
    EXPECT_NE(std::string::npos, error.find("CURV")) << error;

    file->setValue(std::string(kTriplet));
    error.clear();
    EXPECT_TRUE(knobs.resolve(KnobReader(0., ViewIdx(0)), &out, &error)) << error;
    EXPECT_EQ(1, out.stopCount());
}

TEST_F(OpticsLensKnobsTest, BrowseCallsTheHandlerWithTheLensKnob)
{
    NodePtr node = createNode(_generatorPluginID);
    ASSERT_TRUE(bool(node));
    EffectInstance* effect = node->getEffectInstance().get();
    const LensKnobs knobs = LensKnobs::create(effect, addPage(effect));

    KnobStringPtr seen;
    LensKnobs::setBrowseHandler([&seen](const KnobStringPtr& lens) {
        seen = lens;
        lens->setValue(std::string("Classic Designs/planar50"));
    });
    EXPECT_TRUE(knobs.knobChanged(knobs.lensBrowseKnob().get()));
    LensKnobs::setBrowseHandler(LensKnobs::BrowseHandler());

    EXPECT_EQ(knobs.lensKnob(), seen);
    EXPECT_EQ(std::string("Classic Designs/planar50"), knobs.lensKnob()->getValue());
}
