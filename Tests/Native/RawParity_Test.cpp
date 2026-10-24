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

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Raw/RawCACorrect.h"
#include "Engine/Nodes/Raw/RawColorCalibration.h"
#include "Engine/Nodes/Raw/RawColorCalibrationMath.h"
#include "Engine/Nodes/Raw/RawDemosaic.h"
#include "Engine/Nodes/Raw/RawDenoise.h"
#include "Engine/Nodes/Raw/RawHighlights.h"
#include "Engine/Nodes/Raw/RawHotPixels.h"
#include "Engine/Nodes/Raw/RawInputColor.h"
#include "Engine/Nodes/Raw/RawPrepare.h"
#include "Engine/Nodes/Raw/RawTemperature.h"

NATRON_NAMESPACE_USING

namespace {
namespace Cal = RawColorCalibrationMath;

const char* const kMappingPath = NATRON_TESTS_FIXTURES_DIR "/../../tools/raw/darktable-parity.json";

// One knob, or one option of a choice, that a darktable item maps to or that a node adds.
struct KnobRef {
    std::string what;
    std::string module;
    std::string knob;
    std::string option;
    QJsonValue expect;
    bool fromImage = false;
};

QJsonObject
loadMapping()
{
    QFile file(QString::fromUtf8(kMappingPath));

    if (!file.open(QIODevice::ReadOnly)) {
        ADD_FAILURE() << "cannot open " << kMappingPath;

        return QJsonObject();
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    EXPECT_EQ(QJsonParseError::NoError, error.error) << kMappingPath << ": " << error.errorString().toStdString();

    return document.object();
}

std::vector<KnobRef>
mappedRefs(const QJsonObject& mapping)
{
    std::vector<KnobRef> refs;
    const QJsonObject entries = mapping.value(QString::fromUtf8("entries")).toObject();

    for (auto it = entries.begin(); it != entries.end(); ++it) {
        const QJsonObject entry = it.value().toObject();
        if (entry.value(QString::fromUtf8("status")).toString() != QString::fromUtf8("mapped")) {
            continue;
        }
        const std::string key = it.key().toStdString();
        QJsonArray targets = entry.value(QString::fromUtf8("targets")).toArray();
        if (targets.isEmpty()) {
            targets.append(entry);
        }
        for (const QJsonValue& value : targets) {
            const QJsonObject target = value.toObject();
            KnobRef ref;
            ref.what = key;
            ref.module = key.substr(0, key.find('|'));
            ref.knob = target.value(QString::fromUtf8("knob")).toString().toStdString();
            ref.option = target.value(QString::fromUtf8("option")).toString().toStdString();
            ref.expect = target.contains(QString::fromUtf8("expect")) ? target.value(QString::fromUtf8("expect")) : entry.value(QString::fromUtf8("expect"));
            ref.fromImage = entry.contains(QString::fromUtf8("file"));
            refs.push_back(ref);
        }
    }

    return refs;
}

std::vector<KnobRef>
extraRefs(const QJsonObject& mapping)
{
    std::vector<KnobRef> refs;
    const QJsonObject extras = mapping.value(QString::fromUtf8("extras")).toObject();

    for (auto it = extras.begin(); it != extras.end(); ++it) {
        const std::string key = it.key().toStdString();
        const std::string target = key.substr(key.find('|') + 1);
        const std::size_t equals = target.find('=');
        KnobRef ref;
        ref.what = "extra " + key;
        ref.module = key.substr(0, key.find('|'));
        ref.knob = target.substr(0, equals);
        ref.option = (equals == std::string::npos) ? std::string() : target.substr(equals + 1);
        refs.push_back(ref);
    }

    return refs;
}

ImageMetadata
outputMetadata(const NodePtr& node)
{
    NativeEffectBase* effect = node ? dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get()) : nullptr;

    return effect ? effect->getOutputMetadata(1., ViewIdx(0)) : ImageMetadata();
}

template <typename T>
std::shared_ptr<T>
knob(const NodePtr& node,
     const char* name)
{
    std::shared_ptr<T> k = node->getEffectInstance()->getKnobByNameAndType<T>(name);
    EXPECT_TRUE(bool(k)) << name;

    return k;
}

std::string
activeId(const NodePtr& node,
         const char* name)
{
    KnobChoicePtr choice = knob<KnobChoice>(node, name);

    return choice ? choice->getActiveEntry().id : std::string();
}

std::string
optionId(const KnobChoicePtr& choice,
         int index)
{
    const std::vector<ChoiceOption> entries = choice->getEntries_mt_safe();

    return ((index >= 0) && (index < (int)entries.size())) ? entries[index].id : std::string();
}

// darktable's dt_xyY_to_Lch() at Y = 1, as the illuminant hue and chroma sliders show x, y:
// { chroma, hue in degrees }.
std::pair<double, double>
darktableHueChroma(double x,
                   double y)
{
    const double denominator = -2. * x + 12. * y + 3.;
    const double u = 4. * x / denominator;
    const double v = 9. * y / denominator;
    const double lightness = 100.;
    const double uStar = 13. * lightness * (u - 0.20915914598542354);
    const double vStar = 13. * lightness * (v - 0.488075320769787);
    double hue = std::atan2(vStar, uStar);
    if (hue < 0.) {
        hue += 2. * M_PI;
    }

    return { std::hypot(vStar, uStar), hue * 180. / M_PI };
}

// capture.c _get_variance_threshold(), from the image's sensor white level and ISO.
double
darktableCaptureThreshold(double sensorWhiteLevel,
                          double iso)
{
    double threshold = 0.4;

    if (sensorWhiteLevel > 4096.) {
        threshold -= 0.07;
    }
    const double clamped = std::min(1000., std::max(100., iso));

    return threshold - 0.012 * (600. - clamped) / 100.;
}

// A camera file and darktable's "[commit color calibration]" line for its scene-referred
// default job (Tests/fixtures/raw/README.md).
struct CameraCase {
    const RawFixture* fixture;
    const char* name;
    bool xtrans;
    bool dng;
    double temperature;
    double x;
    double y;
};
} // namespace

/**
 * The parity audit's mapping (tools/raw/darktable-parity.json) against the nodes on each camera
 * file: every knob and option it names exists, static defaults are darktable's, and the defaults
 * darktable's reload_defaults() derives from the image are what the nodes show.
 **/
class RawParityTest
    : public RawReferenceFixture {
protected:
    // darktable's logged defaults were taken with linear Rec.709 as the working profile.
    void SetUp() OVERRIDE
    {
        RawReferenceFixture::SetUp();
        KnobChoicePtr workingSpace = getApp()->getProject()->getKnobByNameAndType<KnobChoice>("workingSpace");
        ASSERT_TRUE(bool(workingSpace));
        workingSpace->setValueFromID("Linear Rec.709 (sRGB)", 0);
        ASSERT_EQ(std::string("Linear Rec.709 (sRGB)"), getApp()->getProject()->getWorkingColorSpace());
    }

    // The Develop RAW chain on a file, with the modules it leaves out where darktable's pipe order
    // puts them: highlights, cacorrect, hotpixels, rawdenoise, demosaic.
    struct Develop {
        RawDevelop::Chain chain;
        NodePtr caCorrect;
        NodePtr hotPixels;
        NodePtr denoise;

        NodePtr forModule(const std::string& module) const
        {
            if (module == "rawprepare") {
                return chain.prepare;
            } else if (module == "temperature") {
                return chain.temperature;
            } else if (module == "highlights") {
                return chain.highlights;
            } else if (module == "demosaic") {
                return chain.demosaic;
            } else if (module == "colorin") {
                return chain.inputColor;
            } else if (module == "channelmixerrgb") {
                return chain.colorCalibration;
            } else if (module == "cacorrect") {
                return caCorrect;
            } else if (module == "hotpixels") {
                return hotPixels;
            } else if (module == "rawdenoise") {
                return denoise;
            }

            return NodePtr();
        }

        bool complete() const
        {
            return chain.complete() && caCorrect && hotPixels && denoise;
        }
    };

    Develop develop(const std::string& path)
    {
        Develop result;
        NodePtr read = createNode(QString::fromUtf8(PLUGINID_NATRON_READ));

        EXPECT_TRUE(bool(read)) << path;
        if (!read) {
            return result;
        }
        KnobFilePtr file = read->getEffectInstance()->getKnobByNameAndType<KnobFile>(kOfxImageEffectFileParamName);
        EXPECT_TRUE(bool(file)) << path;
        if (!file) {
            return result;
        }
        file->setValue(path);
        result.chain = RawDevelop::buildChain(read);
        EXPECT_TRUE(result.chain.complete()) << path;
        if (!result.chain.complete()) {
            return result;
        }
        result.caCorrect = insertBeforeDemosaic(result.chain, PLUGINID_NATRON_RAWCACORRECT);
        result.hotPixels = insertBeforeDemosaic(result.chain, PLUGINID_NATRON_RAWHOTPIXELS);
        result.denoise = insertBeforeDemosaic(result.chain, PLUGINID_NATRON_RAWDENOISE);

        return result;
    }

    NodePtr insertBeforeDemosaic(const RawDevelop::Chain& chain,
                                 const char* pluginID)
    {
        NodePtr node = createNode(QString::fromUtf8(pluginID));

        EXPECT_TRUE(bool(node)) << pluginID;
        if (node) {
            insertAbove(chain.demosaic, node);
        }

        return node;
    }

    static void expectValue(const KnobIPtr& k,
                            const QJsonValue& expect,
                            bool current,
                            const std::string& what)
    {
        if (expect.isBool()) {
            KnobBoolPtr b = std::dynamic_pointer_cast<KnobBool>(k);
            ASSERT_TRUE(bool(b)) << what << ": not a toggle";
            EXPECT_EQ(expect.toBool(), current ? b->getValue() : b->getDefaultValue(0)) << what;
        } else if (expect.isDouble()) {
            if (KnobDoublePtr d = std::dynamic_pointer_cast<KnobDouble>(k)) {
                EXPECT_NEAR(expect.toDouble(), current ? d->getValue() : d->getDefaultValue(0), 1e-6) << what;
            } else if (KnobIntPtr i = std::dynamic_pointer_cast<KnobInt>(k)) {
                EXPECT_EQ((int)std::lround(expect.toDouble()), current ? i->getValue() : i->getDefaultValue(0)) << what;
            } else {
                ADD_FAILURE() << what << ": not a number knob";
            }
        } else if (expect.isString()) {
            KnobChoicePtr c = std::dynamic_pointer_cast<KnobChoice>(k);
            ASSERT_TRUE(bool(c)) << what << ": not a choice";
            EXPECT_EQ(expect.toString().toStdString(), optionId(c, current ? c->getValue() : c->getDefaultValue(0))) << what;
        } else if (!expect.isUndefined() && !expect.isNull()) {
            ADD_FAILURE() << what << ": 'expect' is neither a bool, a number nor an option id";
        }
    }

    // Every mapped and EXTRA knob and option exists on the file's nodes, at its static default.
    // Defaults the node takes from the image are left to expectImageDefaults().
    void expectMapping(const Develop& nodes,
                       const char* name)
    {
        const QJsonObject mapping = loadMapping();
        std::vector<KnobRef> refs = mappedRefs(mapping);
        const std::vector<KnobRef> extras = extraRefs(mapping);

        ASSERT_FALSE(refs.empty()) << kMappingPath;
        refs.insert(refs.end(), extras.begin(), extras.end());
        for (const KnobRef& ref : refs) {
            const std::string what = std::string(name) + " " + ref.what + " -> " + ref.knob;
            const NodePtr node = nodes.forModule(ref.module);
            ASSERT_TRUE(bool(node)) << what << ": no node for " << ref.module;
            KnobIPtr k = node->getEffectInstance()->getKnobByName(ref.knob);
            // A name the node numbers, one knob per chart corner.
            if (!k) {
                k = node->getEffectInstance()->getKnobByName(ref.knob + "1");
            }
            EXPECT_TRUE(bool(k)) << what << ": no such knob";
            if (!k) {
                continue;
            }
            if (!ref.option.empty()) {
                KnobChoicePtr choice = std::dynamic_pointer_cast<KnobChoice>(k);
                EXPECT_TRUE(bool(choice)) << what << ": not a choice";
                if (choice) {
                    const std::vector<ChoiceOption> entries = choice->getEntries_mt_safe();
                    const bool found = std::any_of(entries.begin(), entries.end(), [&](const ChoiceOption& o) { return o.id == ref.option; });
                    EXPECT_TRUE(found) << what << ": no option " << ref.option;
                }
            }
            expectValue(k, ref.expect, false, what + " (default)");
            if (!ref.fromImage) {
                expectValue(k, ref.expect, true, what + " (value)");
            }
        }
    }

    // reload_defaults() of each module on a camera file, as the nodes show it until overridden.
    void expectImageDefaults(const Develop& nodes,
                             const CameraCase& c)
    {
        const std::string name = c.name;

        EXPECT_EQ("asShotToReference", activeId(nodes.chain.temperature, kRawTemperatureParamMode)) << name;

        EXPECT_EQ(c.xtrans ? "markesteijn1" : "rcd", activeId(nodes.chain.demosaic, c.xtrans ? kRawDemosaicParamXTransMethod : kRawDemosaicParamBayerMethod)) << name;
        const ImageMetadata demosaicSource = outputMetadata(nodes.chain.demosaic->getInput(0));
        const std::optional<double> sensorWhite = RawMetadata::getSensorWhiteLevel(demosaicSource);
        const std::optional<double> iso = RawMetadata::getIso(demosaicSource);
        EXPECT_TRUE(sensorWhite.has_value()) << name;
        EXPECT_TRUE(iso.has_value()) << name;
        EXPECT_FALSE(knob<KnobBool>(nodes.chain.demosaic, kRawDemosaicParamCaptureThresholdOverride)->getValue()) << name;
        EXPECT_NEAR(darktableCaptureThreshold(sensorWhite.value_or(0.), iso.value_or(0.)),
                    knob<KnobDouble>(nodes.chain.demosaic, kRawDemosaicParamCaptureThreshold)->getValue(), 1e-5)
            << name;

        EXPECT_FALSE(knob<KnobBool>(nodes.chain.inputColor, kRawInputColorParamProfileOverride)->getValue()) << name;
        EXPECT_EQ(c.dng ? "embeddedMatrix" : "standardMatrix", activeId(nodes.chain.inputColor, kRawInputColorParamProfile)) << name;

        EXPECT_FALSE(knob<KnobBool>(nodes.chain.highlights, kRawHighlightsParamClipOverride)->getValue()) << name;
        const double limit = RawMetadata::getLinearResponseLimit(outputMetadata(nodes.chain.highlights->getInput(0)));
        EXPECT_NEAR(std::min(1., limit), knob<KnobDouble>(nodes.chain.highlights, kRawHighlightsParamClip)->getValue(), 1e-6) << name;

        // No gain maps: the control keeps its default and is disabled, darktable hides it.
        EXPECT_EQ("embedded", activeId(nodes.chain.prepare, kRawPrepareParamFlatField)) << name;
        EXPECT_FALSE(knob<KnobChoice>(nodes.chain.prepare, kRawPrepareParamFlatField)->isEnabled(0)) << name;

        const NodePtr calibration = nodes.chain.colorCalibration;
        EXPECT_FALSE(knob<KnobBool>(calibration, kRawColorCalibrationParamCatOverride)->getValue()) << name;
        EXPECT_EQ("custom", activeId(calibration, kRawColorCalibrationParamIlluminant)) << name;
        EXPECT_EQ("cat16", activeId(calibration, kRawColorCalibrationParamAdaptation)) << name;
        EXPECT_NEAR(c.temperature, knob<KnobDouble>(calibration, kRawColorCalibrationParamTemperature)->getValue(), 1.) << name;
        const double x = knob<KnobDouble>(calibration, kRawColorCalibrationParamX)->getValue();
        const double y = knob<KnobDouble>(calibration, kRawColorCalibrationParamY)->getValue();
        EXPECT_NEAR(c.x, x, 1e-4) << name;
        EXPECT_NEAR(c.y, y, 1e-4) << name;
        const std::pair<double, double> shown = darktableHueChroma(x, y);
        const std::pair<double, double> logged = darktableHueChroma(c.x, c.y);
        const double chroma = knob<KnobDouble>(calibration, kRawColorCalibrationParamChroma)->getValue();
        const double hue = knob<KnobDouble>(calibration, kRawColorCalibrationParamHue)->getValue();
        EXPECT_NEAR(shown.first, chroma, 1e-3) << name;
        EXPECT_NEAR(shown.second, hue, 1e-3) << name;
        // The logged x, y are rounded to 1e-4, which moves the chroma by about 0.05 and the hue by
        // about 0.1 degree.
        EXPECT_NEAR(logged.first, chroma, 0.1) << name;
        EXPECT_NEAR(logged.second, hue, 0.2) << name;
    }

    void expectCamera(const CameraCase& c)
    {
        const Develop nodes = develop(c.fixture->path());
        ASSERT_TRUE(nodes.complete()) << c.name;
        expectMapping(nodes, c.name);
        expectImageDefaults(nodes, c);
    }
};

TEST_F(RawParityTest, Cr2)
{
    expectCamera({ &bayerFixture(), "CR2", false, false, 3222., 0.4251, 0.4102 });
}

TEST_F(RawParityTest, Raf)
{
    expectCamera({ &xtransFixture(), "RAF", true, false, 3678., 0.3989, 0.3969 });
}

TEST_F(RawParityTest, Dng)
{
    expectCamera({ &linearFixture(), "DNG", false, true, 3182., 0.4280, 0.4123 });
}

// rawprepare reload_defaults(): FLAT_FIELD_EMBEDDED when the file has gain maps.
TEST_F(RawParityTest, GainMapDngDefaultsToEmbeddedFlatField)
{
    const Develop nodes = develop(NATRON_TESTS_FIXTURES_DIR "/raw/reference/gainmap-bayer.dng");
    ASSERT_TRUE(nodes.complete());
    EXPECT_EQ("embedded", activeId(nodes.chain.prepare, kRawPrepareParamFlatField));
    EXPECT_TRUE(knob<KnobChoice>(nodes.chain.prepare, kRawPrepareParamFlatField)->isEnabled(0));
}

// highlights reload_defaults(): clip at most the file's LinearResponseLimit, 0.8 here.
TEST_F(RawParityTest, LinearResponseLimitLowersClip)
{
    const Develop nodes = develop(NATRON_TESTS_FIXTURES_DIR "/raw/reference/linear-response-limit-bayer.dng");
    ASSERT_TRUE(nodes.complete());
    EXPECT_FALSE(knob<KnobBool>(nodes.chain.highlights, kRawHighlightsParamClipOverride)->getValue());
    EXPECT_NEAR(0.8, knob<KnobDouble>(nodes.chain.highlights, kRawHighlightsParamClip)->getValue(), 1e-6);
}

// channelmixerrgb reload_defaults(): an input a CAT already adapted, or a monochrome one, gets
// the plain channel mixer, illuminant pipe and adaptation none.
TEST_F(RawParityTest, AdaptedOrMonochromeInputDefaultsToPipe)
{
    const Develop nodes = develop(bayerFixture().path());
    ASSERT_TRUE(nodes.complete());

    NodePtr second = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWCOLORCALIBRATION));
    ASSERT_TRUE(bool(second));
    connectNodes(nodes.chain.colorCalibration, second, 0, true);
    EXPECT_TRUE(RawMetadata::getCatApplied(outputMetadata(nodes.chain.colorCalibration)));
    EXPECT_EQ("pipe", activeId(second, kRawColorCalibrationParamIlluminant));
    EXPECT_EQ("none", activeId(second, kRawColorCalibrationParamAdaptation));

    knob<KnobChoice>(nodes.chain.demosaic, kRawDemosaicParamBayerMethod)->setValue((int)eRawDemosaicBayerPassthroughMonochrome);
    const ImageMetadata monochrome = outputMetadata(nodes.chain.inputColor);
    EXPECT_TRUE(RawMetadata::getMonochrome(monochrome));
    const RawColorCalibrationSourceDefaults d = rawColorCalibrationSourceDefaults(monochrome);
    EXPECT_EQ(Cal::Illuminant::Pipe, d.illuminant);
    EXPECT_EQ(Cal::Adaptation::Rgb, d.adaptation);
}
