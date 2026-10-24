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

/*
 * The area color mapping and the built-in presets of darktable's color calibration module,
 * DARKTABLE_REF release-5.6.2 (commit 20891f6c6fa7), src/iop/channelmixerrgb.c:
 * _auto_set_illuminant(), _spot_settings_changed_callback(), the mapping section of gui_init()
 * and init_presets(), Copyright (C) 2020-2026 darktable developers, distributed under the GNU
 * GPL version 3 or later.
 */

/*
 * The knobs and the Calibrate and Validate buttons of the color checker section of darktable's
 * color calibration module, DARKTABLE_REF release-5.6.2 (commit 20891f6c6fa7): gui_init(),
 * _commit_profile_callback() and the run_profile and run_validation branches of process() in
 * src/iop/channelmixerrgb.c, Copyright (C) 2018-2026 darktable developers, distributed under the
 * GNU GPL version 3 or later.
 */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "RawColorCalibration.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/Format.h"
#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/KnobReader.h"
#include "Engine/Nodes/Raw/RawColorMath.h"
#include "Engine/Nodes/Raw/RawInputColor.h"
#include "Engine/Project.h"

NATRON_NAMESPACE_ENTER

using namespace RawColorCalibrationMath;
using namespace RawColorCalibrationChecker;
using RawColorCheckerData::ColorCheckerTarget;

namespace {
const char* const kChartIds[] = { "xrite24_2000", "xrite24_2014", "spyder24", "spyder24v2", "spyder48", "spyder48v2", "spyderPhoto" };
const char* const kOptimizationIds[] = { "none", "neutral", "saturated", "skin", "foliage", "sky", "averageDeltaE", "maximumDeltaE" };
static_assert(sizeof(kChartIds) / sizeof(kChartIds[0]) == (size_t)ColorCheckerTarget::Last);
static_assert(sizeof(kOptimizationIds) / sizeof(kOptimizationIds[0]) == (size_t)Optimization::Last);

const char* const kCornerNames[4] = {
    kRawColorCalibrationParamCheckerTopLeft,
    kRawColorCalibrationParamCheckerTopRight,
    kRawColorCalibrationParamCheckerBottomRight,
    kRawColorCalibrationParamCheckerBottomLeft,
};

// The corners 2% inside rect, in the order of the ideal box, which is y-down: its top is the
// rect's y2 since Natron's pixels are y-up.
RawColorCalibrationChecker::Point
defaultCorner(int corner,
              double x1,
              double y1,
              double width,
              double height)
{
    const bool right = (corner == 1) || (corner == 2);
    const bool bottom = (corner == 2) || (corner == 3);

    return { (float)(right ? x1 + 0.98 * width : x1 + 0.02 * width),
             (float)(bottom ? y1 + 0.02 * height : y1 + 0.98 * height) };
}

void
setIfChanged(const KnobDoublePtr& knob,
             double value)
{
    if (knob && (knob->getValue() != value)) {
        knob->setValue(value);
    }
}

void
setIfChanged(const KnobBoolPtr& knob,
             bool value)
{
    if (knob && (knob->getValue() != value)) {
        knob->setValue(value);
    }
}

void
setIfChanged(const KnobChoicePtr& knob,
             int value)
{
    if (knob && (knob->getValue() != value)) {
        knob->setValue(value);
    }
}

Mat3
toFloat(const RawColorMath::Mat3& m)
{
    Mat3 r;
    for (std::size_t i = 0; i < 9; ++i) {
        r[i] = (float)m[i];
    }

    return r;
}

// darktable's XYZ_to_CAM is the inverse of the input profile's matrix to the D50 connection
// space. The profile Raw Input Color resolves by default stands in for it: the node cannot see an
// override made on Raw Input Color. Without one, the working space is taken as the camera space.
Mat3
xyzToCamera(const ImageMetadata& source,
            const Mat3& workingToXyz)
{
    RawColorMath::Mat3 camToXyz;
    const RawInputColorProfileEnum profile = rawInputColorResolveProfile(rawInputColorDefaultProfile(source), source, &camToXyz);
    if (profile != eRawInputColorProfileCustom) {
        if (const std::optional<RawColorMath::Mat3> adapt = RawColorMath::bradford(RawColorMath::d65White(), RawColorMath::lcmsD50White())) {
            if (const std::optional<RawColorMath::Mat3> inverse = RawColorMath::invert(RawColorMath::multiply(*adapt, camToXyz))) {
                return toFloat(*inverse);
            }
        }
    }
    RawColorMath::Mat3 working;
    for (std::size_t i = 0; i < 9; ++i) {
        working[i] = workingToXyz[i];
    }
    if (const std::optional<RawColorMath::Mat3> inverse = RawColorMath::invert(working)) {
        return toFloat(*inverse);
    }

    return workingToXyz;
}

} // namespace

void
RawColorCalibrationOp::createCheckerKnobs(KnobHolder* knobHolder,
                                          const KnobGroupPtr& tab)
{
    KnobGroupPtr group = AppManager::createKnob<KnobGroup>(knobHolder, EffectInstance::tr("Calibrate with a Color Checker").toStdString());
    group->setName(kRawColorCalibrationGroupChecker);
    group->setHintToolTip(EffectInstance::tr("Use a color checker target to autoset CAT and channels. Place the four corners on the "
                                             "chart's guides in the viewer, then Calibrate.")
                              .toStdString());
    tab->addKnob(group);

    std::vector<ChoiceOption> charts;
    for (int t = 0; t < (int)ColorCheckerTarget::Last; ++t) {
        charts.push_back(ChoiceOption(kChartIds[t], chartLabel((ColorCheckerTarget)t), std::string()));
    }
    KnobChoicePtr chart = AppManager::createKnob<KnobChoice>(knobHolder, EffectInstance::tr("Chart").toStdString());
    chart->setName(kRawColorCalibrationParamCheckerChart);
    chart->setHintToolTip(EffectInstance::tr("Choose the vendor and the type of your chart.").toStdString());
    chart->populateChoices(charts);
    chart->setDefaultValue((int)ColorCheckerTarget::Xrite24_2000);
    chart->setAnimationEnabled(false);
    chart->setEvaluateOnChange(false);
    group->addKnob(chart);
    checkerChart = chart;

    std::vector<ChoiceOption> optimizations;
    for (int o = 0; o < (int)Optimization::Last; ++o) {
        optimizations.push_back(ChoiceOption(kOptimizationIds[o], optimizationLabel((Optimization)o), std::string()));
    }
    KnobChoicePtr optimize = AppManager::createKnob<KnobChoice>(knobHolder, EffectInstance::tr("Optimize For").toStdString());
    optimize->setName(kRawColorCalibrationParamCheckerOptimize);
    optimize->setHintToolTip(EffectInstance::tr("Choose the colors that will be optimized with higher priority.\n"
                                                "Neutral colors gives the lowest average delta E but a high maximum delta E.\n"
                                                "Saturated colors gives the lowest maximum delta E but a high average delta E.\n"
                                                "None is a trade-off between both.\n"
                                                "The others are special behaviors to protect some hues.")
                                 .toStdString());
    optimize->populateChoices(optimizations);
    optimize->setDefaultValue((int)Optimization::None);
    optimize->setAnimationEnabled(false);
    optimize->setEvaluateOnChange(false);
    group->addKnob(optimize);
    checkerOptimize = optimize;

    KnobDoublePtr scale = AppManager::createKnob<KnobDouble>(knobHolder, EffectInstance::tr("Patch Scale").toStdString());
    scale->setName(kRawColorCalibrationParamCheckerPatchScale);
    scale->setHintToolTip(EffectInstance::tr("Reduce the radius of the patches to select the more or less central part.\n"
                                             "Useful when the perspective correction is sloppy or\n"
                                             "the patches frame cast a shadows on the edges of the patch.")
                              .toStdString());
    scale->setDefaultValue(0.5);
    scale->setMinimum(0.);
    scale->setMaximum(1.);
    scale->setDisplayMinimum(0.);
    scale->setDisplayMaximum(1.);
    scale->setAnimationEnabled(false);
    scale->setEvaluateOnChange(false);
    group->addKnob(scale);
    checkerPatchScale = scale;

    Format format;
    if (AppInstancePtr app = knobHolder->getApp()) {
        app->getProject()->getProjectDefaultFormat(&format);
    }
    const QString cornerLabels[4] = {
        EffectInstance::tr("Top Left"),
        EffectInstance::tr("Top Right"),
        EffectInstance::tr("Bottom Right"),
        EffectInstance::tr("Bottom Left"),
    };
    for (int i = 0; i < 4; ++i) {
        KnobDoublePtr corner = AppManager::createKnob<KnobDouble>(knobHolder, cornerLabels[i].toStdString(), 2);
        corner->setName(kCornerNames[i]);
        corner->setHintToolTip(EffectInstance::tr("A corner of the chart, on its guide (white dot), in pixels of the input.").toStdString());
        corner->setSpatial(true);
        corner->disableSlider();
        corner->setAnimationEnabled(false);
        corner->setEvaluateOnChange(false);
        const RawColorCalibrationChecker::Point p = defaultCorner(i, format.x1, format.y1, format.width(), format.height());
        for (int d = 0; d < 2; ++d) {
            corner->setDecimals(0, d);
            corner->setIncrement(1., d);
            corner->setDefaultValue((d == 0) ? (double)p.x : (double)p.y, d);
        }
        group->addKnob(corner);
        checkerCorners[i] = corner;

        // The corner pin interact draws only the corners these enable.
        KnobBoolPtr enable = AppManager::createKnob<KnobBool>(knobHolder, cornerLabels[i].toStdString());
        enable->setName(std::string(kRawColorCalibrationParamCheckerCornerEnable) + std::to_string(i + 1));
        enable->setDefaultValue(true);
        enable->setSecretByDefault(true);
        enable->setIsPersistent(false);
        enable->setAnimationEnabled(false);
        enable->setEvaluateOnChange(false);
        group->addKnob(enable);
        checkerCornerEnable[i] = enable;
    }

    std::vector<ChoiceOption> points;
    points.push_back(ChoiceOption("to", "to", std::string()));
    points.push_back(ChoiceOption("from", "from", std::string()));
    KnobChoicePtr overlay = AppManager::createKnob<KnobChoice>(knobHolder, EffectInstance::tr("Overlay Points").toStdString());
    overlay->setName(kRawColorCalibrationParamCheckerOverlayPoints);
    overlay->populateChoices(points);
    overlay->setDefaultValue(0);
    overlay->setSecretByDefault(true);
    overlay->setIsPersistent(false);
    overlay->setAnimationEnabled(false);
    overlay->setEvaluateOnChange(false);
    group->addKnob(overlay);
    checkerOverlayPoints = overlay;

    KnobButtonPtr calibrateKnob = AppManager::createKnob<KnobButton>(knobHolder, EffectInstance::tr("Calibrate").toStdString());
    calibrateKnob->setName(kRawColorCalibrationParamCalibrate);
    calibrateKnob->setHintToolTip(EffectInstance::tr("Compute the illuminant and the channel mix that map the chart onto its reference "
                                                     "and set them in the node, with Override File Defaults on.")
                                      .toStdString());
    calibrateKnob->setEvaluateOnChange(false);
    calibrateKnob->setAddNewLine(false);
    group->addKnob(calibrateKnob);
    calibrate = calibrateKnob;

    KnobButtonPtr validateKnob = AppManager::createKnob<KnobButton>(knobHolder, EffectInstance::tr("Validate").toStdString());
    validateKnob->setName(kRawColorCalibrationParamValidate);
    validateKnob->setHintToolTip(EffectInstance::tr("Check the output delta E.").toStdString());
    validateKnob->setEvaluateOnChange(false);
    group->addKnob(validateKnob);
    validate = validateKnob;

    KnobStringPtr report = AppManager::createKnob<KnobString>(knobHolder, EffectInstance::tr("Report").toStdString());
    report->setName(kRawColorCalibrationParamCheckerReport);
    report->setHintToolTip(EffectInstance::tr("The fit of the last Calibrate or Validate. The delta E is using the CIE 2000 formula.").toStdString());
    report->setAsLabel();
    report->setIsPersistent(false);
    report->setEvaluateOnChange(false);
    group->addKnob(report);
    checkerReport = report;
} // RawColorCalibrationOp::createCheckerKnobs

void
RawColorCalibration::refreshCheckerDefaults()
{
    EffectInstancePtr input = getInput(getSourceInput());

    if (!input) {
        return;
    }
    const RectI format = input->getOutputFormat();
    if (format.isNull()) {
        return;
    }
    for (int i = 0; i < 4; ++i) {
        const KnobDoublePtr corner = op().checkerCorners[i].lock();
        if (!corner) {
            continue;
        }
        const RawColorCalibrationChecker::Point p = defaultCorner(i, format.x1, format.y1, format.width(), format.height());
        for (int d = 0; d < 2; ++d) {
            if (!corner->hasModifications(d)) {
                corner->setDefaultValue((d == 0) ? (double)p.x : (double)p.y, d);
            }
        }
    }
}

ChartPlacement
RawColorCalibration::checkerPlacement(double time) const
{
    const RawColorCalibrationOp& o = op();
    ChartPlacement placement;
    const KnobChoicePtr chart = o.checkerChart.lock();

    placement.checker = &RawColorCheckerData::colorChecker(chart ? (ColorCheckerTarget)chart->getValue() : ColorCheckerTarget::Xrite24_2000);
    if (const KnobDoublePtr scale = o.checkerPatchScale.lock()) {
        placement.patchScale = (float)scale->getValueAtTime(time);
    }
    for (int i = 0; i < 4; ++i) {
        if (const KnobDoublePtr corner = o.checkerCorners[i].lock()) {
            placement.box[i] = { (float)corner->getValueAtTime(time, 0), (float)corner->getValueAtTime(time, 1) };
        }
    }

    return placement;
}

std::optional<RgbImage>
RawColorCalibration::readChecker(double time,
                                 ViewIdx view,
                                 const ChartPlacement& placement,
                                 const Kernel* kernel)
{
    const int inputNb = getSourceInput();

    if (!getInput(inputNb)) {
        return std::nullopt;
    }
    double left = placement.box[0].x, right = left, bottom = placement.box[0].y, top = bottom;
    for (const RawColorCalibrationChecker::Point& p : placement.box) {
        left = (std::min)(left, (double)p.x);
        right = (std::max)(right, (double)p.x);
        bottom = (std::min)(bottom, (double)p.y);
        top = (std::max)(top, (double)p.y);
    }
    RectI box;
    box.x1 = (int)std::floor(left) - 2;
    box.y1 = (int)std::floor(bottom) - 2;
    box.x2 = (int)std::ceil(right) + 2;
    box.y2 = (int)std::ceil(top) + 2;
    const RectD bounds(box.x1, box.y1, box.x2, box.y2);
    RectI roi;
    ImagePtr src = getImage(inputNb, time, RenderScale::identity, view, &bounds, NULL, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &roi);
    if (!src || (src->getBitDepth() != eImageBitDepthFloat)) {
        return std::nullopt;
    }
    const RectI area = box.intersect(src->getBounds());
    if (area.isNull()) {
        return std::nullopt;
    }
    RgbImage image;
    image.x1 = area.x1;
    image.y1 = area.y1;
    image.width = area.width();
    image.height = area.height();
    image.rgb.assign((std::size_t)image.width * (std::size_t)image.height * 3, 0.f);
    const int nComps = (int)src->getComponentsCount();
    Image::ReadAccess access(src.get());
    for (int py = area.y1; py < area.y2; ++py) {
        const float* pix = (const float*)access.pixelAt(area.x1, py);
        if (!pix) {
            continue;
        }
        float* dst = &image.rgb[(std::size_t)(py - area.y1) * (std::size_t)image.width * 3];
        for (int px = area.x1; px < area.x2; ++px, pix += nComps, dst += 3) {
            float in[4] = { 0.f, 0.f, 0.f, 1.f };
            for (int c = 0; c < 3; ++c) {
                in[c] = pix[(nComps >= 3) ? c : 0];
            }
            if (kernel) {
                float out[4] = { 0.f, 0.f, 0.f, 1.f };
                processPixel(*kernel, in, out);
                std::copy(out, out + 3, dst);
            } else {
                std::copy(in, in + 3, dst);
            }
        }
    }

    return image;
} // RawColorCalibration::readChecker

void
RawColorCalibration::showCheckerReport(const std::string& report)
{
    if (const KnobStringPtr knob = op().checkerReport.lock()) {
        const bool wasSyncing = _syncing;
        _syncing = true;
        knob->setValue(report);
        _syncing = wasSyncing;
    }
}

bool
RawColorCalibration::calibrateChecker(double time,
                                      ViewIdx view)
{
    const RawColorCalibrationOp& o = op();
    const ChartPlacement placement = checkerPlacement(time);
    const std::optional<RgbImage> image = readChecker(time, view, placement, nullptr);

    if (!image) {
        showCheckerReport(std::string("Calibrate needs a source to read the chart from."));

        return false;
    }
    const Mat3 toXyz = RawColorCalibrationOp::workingToXyzD50(this);
    const ImageMetadata source = getSourceMetadata(time, view);
    const RawColorCalibrationMath::Settings s = o.effectiveSettings(KnobReader(time, view), source);
    const Adaptation kind = rawColorCalibrationEffectiveAdaptation(s);
    const KnobChoicePtr optimize = o.checkerOptimize.lock();
    const Optimization optimization = optimize ? (Optimization)optimize->getValue() : Optimization::None;
    const std::optional<Profile> profile = extractColorChecker(*image, placement, optimization, kind, toXyz, xyzToCamera(source, toXyz));
    if (!profile) {
        showCheckerReport(std::string("The chart could not be fitted: check that the corners are on its guides."));

        return false;
    }

    AppInstancePtr app = getApp();
    const bool oneUndoStep = app && !app->isCreatingPythonGroup();
    if (oneUndoStep) {
        setMultipleParamsEditLevel(KnobHolder::eMultipleParamsEditOnCreateNewCommand);
    }
    const bool wasSyncing = _syncing;
    _syncing = true;
    // The profile was fitted with this adaptation, which the file defaults may be supplying.
    setIfChanged(o.catOverride.lock(), true);
    setIfChanged(o.adaptation.lock(), static_cast<int>(kind));
    setIfChanged(o.illuminant.lock(), rawColorCalibrationIlluminantIndex(Illuminant::Custom));
    setIfChanged(o.x.lock(), (double)profile->illuminant.x);
    setIfChanged(o.y.lock(), (double)profile->illuminant.y);
    setIfChanged(o.temperature.lock(), (double)profile->temperature);
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            setIfChanged(o.rows[r][c].lock(), (double)profile->mix[r * 3 + c]);
        }
    }
    _lastIlluminant = Illuminant::Custom;
    _syncing = wasSyncing;
    if (oneUndoStep) {
        setMultipleParamsEditLevel(KnobHolder::eMultipleParamsEditOff);
    }
    showCheckerReport(profileReport(*profile));
    refreshAfterWrite();

    return true;
} // RawColorCalibration::calibrateChecker

bool
RawColorCalibration::validateChecker(double time,
                                     ViewIdx view)
{
    const RawColorCalibrationOp& o = op();
    const ChartPlacement placement = checkerPlacement(time);
    const ImageMetadata source = getSourceMetadata(time, view);
    const Kernel kernel = o.bake(KnobReader(time, view), source);
    const std::optional<RgbImage> image = readChecker(time, view, placement, o.isIdentity(kernel) ? nullptr : &kernel);

    if (!image) {
        showCheckerReport(std::string("Validate needs a source to read the chart from."));

        return false;
    }
    const Mat3 toXyz = RawColorCalibrationOp::workingToXyzD50(this);
    const std::optional<Validation> validation = validateColorChecker(*image, placement, toXyz, xyzToCamera(source, toXyz));
    if (!validation) {
        showCheckerReport(std::string("The chart could not be read: check that the corners are on its guides."));

        return false;
    }
    showCheckerReport(validationReport(*validation));

    return true;
}

NATRON_NAMESPACE_EXIT
