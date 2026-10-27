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
 * Knobs, defaults, the output size and the warp follow darktable's rotate and perspective module,
 * DARKTABLE_REF release-5.6.2 (commit 20891f6c6fa7), src/iop/ashift.c, Copyright (C) 2016-2026
 * darktable developers, distributed under the GNU GPL version 3 or later.
 */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "AutoStraighten.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include "Engine/AppInstance.h"
#include "Engine/Format.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/KnobReader.h"
#include "Engine/Project.h"
#include "Engine/RenderScale.h"
#include "Engine/TimeLine.h"

NATRON_NAMESPACE_ENTER

namespace {
constexpr unsigned int kOutlierSeed = 1;
constexpr int kMinimumOutputSize = 4;

const char* const kNotEnoughStructure = "not enough structure for automatic correction\nminimum 2 lines in each relevant direction";
const char* const kFitFailed = "automatic correction failed, please correct manually";

bool
rectIsEmpty(const RectD& r)
{
    return (r.x2 <= r.x1) || (r.y2 <= r.y1);
}

RectD
rectIntersection(const RectD& a,
                 const RectD& b)
{
    if (rectIsEmpty(a) || rectIsEmpty(b)) {
        return RectD(0., 0., 0., 0.);
    }
    if ((a.x1 > b.x2) || (b.x1 > a.x2) || (a.y1 > b.y2) || (b.y1 > a.y2)) {
        return RectD(0., 0., 0., 0.);
    }
    RectD ret;
    ret.x1 = (std::max)(a.x1, b.x1);
    ret.x2 = (std::max)(ret.x1, (std::min)(a.x2, b.x2));
    ret.y1 = (std::max)(a.y1, b.y1);
    ret.y2 = (std::max)(ret.y1, (std::min)(a.y2, b.y2));

    return ret;
}

RectD
toCanonical(const RectI& r,
            double par)
{
    if ((r.x2 <= r.x1) || (r.y2 <= r.y1)) {
        return RectD(0., 0., 0., 0.);
    }

    return RectD(r.x1 * par, r.y1, r.x2 * par, r.y2);
}

void
setSecretAndDisabled(const KnobIPtr& knob,
                     bool secret)
{
    if (!knob) {
        return;
    }
    knob->setSecret(secret);
    knob->setAllDimensionsEnabled(!secret);
}

// darktable's commit_params(): the generic model ignores the focal length, the lens dependence
// and the aspect.
Ashift::Model
makeModel(double rotation,
          double lensShiftV,
          double lensShiftH,
          double shear,
          bool specific,
          double focalLength,
          double cropFactor,
          double lensDependence,
          double aspectAdjust)
{
    Ashift::Model m;

    m.rotation = -rotation;
    m.lensShiftV = lensShiftV;
    m.lensShiftH = lensShiftH;
    m.shear = shear;
    if (specific) {
        m.focalLength = focalLength * cropFactor;
        m.orthocorr = lensDependence;
        m.aspect = aspectAdjust;
    } else {
        m.focalLength = Ashift::kDefaultFocalLength;
        m.orthocorr = 0.;
        m.aspect = 1.;
    }

    return m;
}

bool
sameModel(const Ashift::Model& a,
          const Ashift::Model& b)
{
    return (a.rotation == b.rotation) && (a.lensShiftV == b.lensShiftV) && (a.lensShiftH == b.lensShiftH) && (a.shear == b.shear)
        && (a.focalLength == b.focalLength) && (a.orthocorr == b.orthocorr) && (a.aspect == b.aspect);
}
} // anonymous namespace

AutoStraightenOp::Params
AutoStraightenOp::bake(const KnobReader& reader) const
{
    Params p;

    p.time = reader.time();
    p.view = reader.view();
    p.model = makeModel(reader.value(rotation), reader.value(lensShiftV), reader.value(lensShiftH), reader.value(shear),
                        reader.value(lensModel) == (int)AutoStraighten::eLensModelSpecific, reader.value(focalLength),
                        reader.value(cropFactor), reader.value(lensDependence), reader.value(aspectAdjust));
    p.crop = (Ashift::CropModeEnum)reader.value(autoCrop);
    p.filter = (Resampler::FilterEnum)reader.value(filter);
    p.clamp = reader.value(clamp);
    p.blackOutside = reader.value(blackOutside);

    return p;
}

bool
AutoStraightenOp::isIdentity(const Params& params,
                             const PassContext& /*context*/) const
{
    if (!node) {
        return true;
    }
    RectI format;
    double par = 1.;
    node->getSourceFormat(params.time, params.view, &format, &par);

    return node->computeGeometry(format, par, params.model, params.crop).bypass;
}

ResampleSettings
AutoStraightenOp::settings(const Params& params,
                           const PassContext& context) const
{
    ResampleSettings s;

    s.filter = params.filter;
    s.clamp = params.clamp;
    s.blackOutside = params.blackOutside;
    s.invert = false;
    s.transformIsIdentity = isIdentity(params, context);

    return s;
}

bool
AutoStraightenOp::transform(const Params& params,
                            const PassContext& context,
                            double /*amount*/,
                            bool invert,
                            TransformMath::Mat3* matrix) const
{
    if (!node) {
        return false;
    }
    RectI format;
    double par = 1.;
    node->getSourceFormat(params.time, params.view, &format, &par);
    if (context.pixelAspectRatio > 0.) {
        par = context.pixelAspectRatio;
    }
    const AutoStraighten::Geometry geometry = node->computeGeometry(format, par, params.model, params.crop);

    return AutoStraighten::canonicalMatrix(geometry, params.model, invert, matrix);
}

bool
AutoStraightenOp::canConcatenate() const
{
    return false;
}

std::optional<GpuCostHint>
AutoStraightenOp::costHint(const Params& /*params*/,
                           const PassContext& /*context*/) const
{
    return GpuCostHint();
}

AutoStraighten::AutoStraighten(NodePtr node)
    : ImageNode<AutoStraightenOp>(node)
{
    op().node = this;
}

AutoStraighten::~AutoStraighten()
{
}

void
AutoStraighten::addAcceptedComponents(int inputNb,
                                      std::list<ImageLayerDesc>* comps)
{
    ImageNode<AutoStraightenOp>::addAcceptedComponents(inputNb, comps);
    comps->push_back(ImageLayerDesc::getXYComponents());
}

void
AutoStraighten::initializeKnobs()
{
    const KnobPagePtr page = createControlsPage();

    const auto makeDouble = [&](const QString& label, const char* name, const QString& hint, double def, double mini, double maxi,
                                double dispMin, double dispMax) {
        KnobDoublePtr knob = createKnob<KnobDouble>(label);
        knob->setName(name);
        knob->setHintToolTip(hint);
        knob->setMinimum(mini);
        knob->setMaximum(maxi);
        knob->setDisplayMinimum(dispMin);
        knob->setDisplayMaximum(dispMax);
        knob->setDefaultValue(def);
        knob->setIsMetadataSlave(true);
        page->addKnob(knob);

        return knob;
    };

    KnobDoublePtr rotation = makeDouble(tr("Rotation"), kAutoStraightenParamRotation,
                                        tr("Rotate the image, in degrees."),
                                        0., -Ashift::kRotationRange, Ashift::kRotationRange, -10., 10.);
    op().rotation = rotation;

    KnobDoublePtr lensShiftV = makeDouble(tr("Lens Shift V"), kAutoStraightenParamLensShiftV,
                                          tr("Apply lens shift correction in the vertical direction: corrects converging verticals."),
                                          0., -Ashift::kLensShiftRange, Ashift::kLensShiftRange, -1., 1.);
    lensShiftV->setDecimals(3);
    op().lensShiftV = lensShiftV;

    KnobDoublePtr lensShiftH = makeDouble(tr("Lens Shift H"), kAutoStraightenParamLensShiftH,
                                          tr("Apply lens shift correction in the horizontal direction: corrects converging horizontals."),
                                          0., -Ashift::kLensShiftRange, Ashift::kLensShiftRange, -1., 1.);
    lensShiftH->setDecimals(3);
    op().lensShiftH = lensShiftH;

    KnobDoublePtr shear = makeDouble(tr("Shear"), kAutoStraightenParamShear,
                                     tr("Shear the image along one diagonal."),
                                     0., -Ashift::kShearRange, Ashift::kShearRange, -0.2, 0.2);
    op().shear = shear;

    KnobChoicePtr autoCrop = createKnob<KnobChoice>(tr("Auto Crop"));
    autoCrop->setName(kAutoStraightenParamAutoCrop);
    autoCrop->setHintToolTip(tr("Automatically crop to avoid black edges."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("off", tr("Off").toStdString(), tr("Keep the whole corrected image, with its empty borders.").toStdString()));
        options.push_back(ChoiceOption("largest", tr("Largest Area").toStdString(), tr("Crop to the largest rectangle inside the corrected image.").toStdString()));
        options.push_back(ChoiceOption("original", tr("Original Format").toStdString(), tr("Crop to the largest rectangle of the source's aspect ratio inside the corrected image.").toStdString()));
        autoCrop->populateChoices(options);
    }
    autoCrop->setDefaultValue((int)Ashift::eCropLargest);
    autoCrop->setAnimationEnabled(false);
    autoCrop->setIsMetadataSlave(true);
    page->addKnob(autoCrop);
    op().autoCrop = autoCrop;

    KnobChoicePtr lensModel = createKnob<KnobChoice>(tr("Lens Model"));
    lensModel->setName(kAutoStraightenParamLensModel);
    lensModel->setHintToolTip(tr("Lens model of the perspective correction: generic, or according to the focal length."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("generic", tr("Generic").toStdString(), tr("A lens of 28 mm on a full-frame sensor.").toStdString()));
        options.push_back(ChoiceOption("specific", tr("Specific").toStdString(), tr("The focal length, crop factor, lens dependence and aspect adjust below.").toStdString()));
        lensModel->populateChoices(options);
    }
    lensModel->setDefaultValue((int)eLensModelGeneric);
    lensModel->setAnimationEnabled(false);
    lensModel->setIsMetadataSlave(true);
    page->addKnob(lensModel);
    op().lensModel = lensModel;

    KnobDoublePtr focalLength = makeDouble(tr("Focal Length"), kAutoStraightenParamFocalLength,
                                           tr("Focal length of the lens, in millimetres."),
                                           Ashift::kDefaultFocalLength, 1., 2000., 10., 1000.);
    focalLength->setDecimals(0);
    op().focalLength = focalLength;

    KnobDoublePtr cropFactor = makeDouble(tr("Crop Factor"), kAutoStraightenParamCropFactor,
                                          tr("Crop factor of the camera sensor."),
                                          1., 0.5, 10., 1., 2.);
    op().cropFactor = cropFactor;

    KnobDoublePtr lensDependence = makeDouble(tr("Lens Dependence"), kAutoStraightenParamLensDependence,
                                              tr("The level of lens dependent correction, in percent: 100 for full lens dependency, 0 for the generic case."),
                                              100., 0., 100., 0., 100.);
    op().lensDependence = lensDependence;

    KnobDoublePtr aspectAdjust = makeDouble(tr("Aspect Adjust"), kAutoStraightenParamAspectAdjust,
                                            tr("Adjust the aspect ratio of the image by horizontal and vertical scaling."),
                                            1., 0.5, 2., 0.5, 2.);
    op().aspectAdjust = aspectAdjust;

    KnobChoicePtr structure = createKnob<KnobChoice>(tr("Structure"));
    structure->setName(kAutoStraightenParamStructure);
    structure->setHintToolTip(tr("What the fit buttons straighten: the lines detected in the source, or the four sides of a rectangle placed in the viewer."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("lines", tr("Detected Lines").toStdString(), tr("Automatically analyse the line structure of the image.").toStdString()));
        options.push_back(ChoiceOption("rectangle", tr("Rectangle").toStdString(), tr("Manually define the perspective rectangle with the four corners.").toStdString()));
        structure->populateChoices(options);
    }
    structure->setDefaultValue((int)eStructureDetectedLines);
    structure->setAnimationEnabled(false);
    page->addKnob(structure);
    _structure = structure;

    KnobChoicePtr enhance = createKnob<KnobChoice>(tr("Enhance"));
    enhance->setName(kAutoStraightenParamEnhance);
    enhance->setHintToolTip(tr("The enhancement applied to the source before the lines are detected."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("none", tr("None").toStdString(), ""));
        options.push_back(ChoiceOption("edges", tr("Edges").toStdString(), tr("An additional edge enhancement.").toStdString()));
        options.push_back(ChoiceOption("detail", tr("Detail").toStdString(), tr("An additional detail enhancement.").toStdString()));
        options.push_back(ChoiceOption("edgesAndDetail", tr("Edges And Detail").toStdString(), tr("A combination of both.").toStdString()));
        enhance->populateChoices(options);
    }
    enhance->setDefaultValue(0);
    enhance->setAnimationEnabled(false);
    page->addKnob(enhance);
    _enhance = enhance;

    KnobChoicePtr fitParameters = createKnob<KnobChoice>(tr("Fit Parameters"));
    fitParameters->setName(kAutoStraightenParamFitParameters);
    fitParameters->setHintToolTip(tr("The parameters the fit buttons solve for. All fits rotation and lens shift, and with Fit Both also shear."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("all", tr("All").toStdString(), ""));
        options.push_back(ChoiceOption("rotation", tr("Rotation Only").toStdString(), ""));
        options.push_back(ChoiceOption("lensShift", tr("Lens Shift Only").toStdString(), ""));
        options.push_back(ChoiceOption("rotationAndLensShift", tr("Rotation And Lens Shift").toStdString(), ""));
        fitParameters->populateChoices(options);
    }
    fitParameters->setDefaultValue((int)eFitParametersAll);
    fitParameters->setAnimationEnabled(false);
    page->addKnob(fitParameters);
    _fitParameters = fitParameters;

    KnobButtonPtr fitVertical = createKnob<KnobButton>(tr("Fit Vertical"));
    fitVertical->setName(kAutoStraightenParamFitVertical);
    fitVertical->setHintToolTip(tr("Automatically correct for vertical perspective distortion at the current frame."));
    fitVertical->setAddNewLine(false);
    page->addKnob(fitVertical);
    _fitVertical = fitVertical;

    KnobButtonPtr fitHorizontal = createKnob<KnobButton>(tr("Fit Horizontal"));
    fitHorizontal->setName(kAutoStraightenParamFitHorizontal);
    fitHorizontal->setHintToolTip(tr("Automatically correct for horizontal perspective distortion at the current frame."));
    fitHorizontal->setAddNewLine(false);
    page->addKnob(fitHorizontal);
    _fitHorizontal = fitHorizontal;

    KnobButtonPtr fitBoth = createKnob<KnobButton>(tr("Fit Both"));
    fitBoth->setName(kAutoStraightenParamFitBoth);
    fitBoth->setHintToolTip(tr("Automatically correct for vertical and horizontal perspective distortions at the current frame, fitting rotation, lens shift in both directions, and shear."));
    page->addKnob(fitBoth);
    _fitBoth = fitBoth;

    // darktable's default rectangle, at 20% and 80% of the image.
    const char* const cornerNames[4] = { kAutoStraightenParamTopLeft, kAutoStraightenParamTopRight, kAutoStraightenParamBottomLeft, kAutoStraightenParamBottomRight };
    const QString cornerLabels[4] = { tr("Top Left"), tr("Top Right"), tr("Bottom Left"), tr("Bottom Right") };
    const double cornerDefaults[4][2] = { { 0.2, 0.8 }, { 0.8, 0.8 }, { 0.2, 0.2 }, { 0.8, 0.2 } };
    for (int i = 0; i < 4; ++i) {
        KnobDoublePtr corner = createKnob<KnobDouble>(cornerLabels[i], 2);
        corner->setName(cornerNames[i]);
        corner->setHintToolTip(tr("A corner of the rectangle whose sides the fit makes vertical and horizontal, in Rectangle mode."));
        corner->setSpatial(true);
        corner->disableSlider();
        corner->setDefaultValuesAreNormalized(true);
        for (int d = 0; d < 2; ++d) {
            corner->setMinimum(-DBL_MAX, d);
            corner->setMaximum(DBL_MAX, d);
            corner->setDisplayMinimum(-10000., d);
            corner->setDisplayMaximum(10000., d);
            corner->setDefaultValue(cornerDefaults[i][d], d);
        }
        corner->setAddNewLine((i % 2) == 1);
        page->addKnob(corner);
        corner->setHasHostOverlayHandle(true);
        _corners[i] = corner;
    }

    KnobChoicePtr filter = createKnob<KnobChoice>(tr(kResamplerParamFilterTypeLabel));
    filter->setName(kResamplerParamFilterType);
    filter->setHintToolTip(tr(kResamplerParamFilterTypeHint));
    {
        const std::vector<Resampler::ChoiceOption>& filters = Resampler::filterOptions();
        std::vector<ChoiceOption> options;
        for (std::size_t i = 0; i < filters.size(); ++i) {
            options.push_back(ChoiceOption(filters[i].id, filters[i].label, filters[i].hint));
        }
        filter->populateChoices(options);
    }
    filter->setDefaultValue((int)Resampler::eFilterCubic);
    filter->setAddNewLine(false);
    filter->setSpacingBetweenItems(1);
    page->addKnob(filter);
    op().filter = filter;

    KnobBoolPtr clamp = createKnob<KnobBool>(tr(kResamplerParamFilterClampLabel));
    clamp->setName(kResamplerParamFilterClamp);
    clamp->setHintToolTip(tr(kResamplerParamFilterClampHint));
    clamp->setDefaultValue(false);
    clamp->setAddNewLine(false);
    clamp->setSpacingBetweenItems(1);
    page->addKnob(clamp);
    op().clamp = clamp;

    KnobBoolPtr blackOutside = createKnob<KnobBool>(tr(kResamplerParamFilterBlackOutsideLabel));
    blackOutside->setName(kResamplerParamFilterBlackOutside);
    blackOutside->setHintToolTip(tr(kResamplerParamFilterBlackOutsideHint));
    blackOutside->setDefaultValue(true);
    page->addKnob(blackOutside);
    op().blackOutside = blackOutside;

    addStageKnobs(page);

    refreshVisibility();
    updateClampEnabled(getApp()->getTimeLine()->currentFrame(), ViewIdx(0));
} // AutoStraighten::initializeKnobs

void
AutoStraighten::refreshVisibility()
{
    KnobChoicePtr lensModel = op().lensModel.lock();
    KnobChoicePtr structure = _structure.lock();
    const bool specific = lensModel && (lensModel->getValue() == (int)eLensModelSpecific);
    const bool rectangle = structure && (structure->getValue() == (int)eStructureRectangle);

    setSecretAndDisabled(op().focalLength.lock(), !specific);
    setSecretAndDisabled(op().cropFactor.lock(), !specific);
    setSecretAndDisabled(op().lensDependence.lock(), !specific);
    setSecretAndDisabled(op().aspectAdjust.lock(), !specific);
    setSecretAndDisabled(_enhance.lock(), rectangle);
    for (int i = 0; i < 4; ++i) {
        setSecretAndDisabled(_corners[i].lock(), !rectangle);
    }
}

void
AutoStraighten::updateClampEnabled(double time,
                                   ViewSpec view)
{
    KnobChoicePtr filter = op().filter.lock();
    KnobBoolPtr clamp = op().clamp.lock();

    if (!filter || !clamp) {
        return;
    }
    clamp->setAllDimensionsEnabled(Resampler::filterHonoursClamp((Resampler::FilterEnum)filter->getValueAtTime(time, 0, view)));
}

void
AutoStraighten::onKnobsLoaded()
{
    refreshVisibility();
    updateClampEnabled(getApp()->getTimeLine()->currentFrame(), ViewIdx(0));
}

bool
AutoStraighten::knobChanged(KnobI* k,
                            ValueChangedReasonEnum /*reason*/,
                            ViewSpec view,
                            double time,
                            bool /*originatedFromMainThread*/)
{
    const ViewIdx viewIdx = (view.isAll() || view.isCurrent()) ? ViewIdx(0) : ViewIdx(view);
    KnobChoicePtr filterKnob = op().filter.lock();

    if (filterKnob && (k == filterKnob.get())) {
        updateClampEnabled(time, viewIdx);

        return true;
    }
    KnobChoicePtr lensModel = op().lensModel.lock();
    KnobChoicePtr structure = _structure.lock();
    if ((lensModel && (k == lensModel.get())) || (structure && (k == structure.get()))) {
        refreshVisibility();

        return true;
    }
    const KnobButtonPtr buttons[3] = { _fitVertical.lock(), _fitHorizontal.lock(), _fitBoth.lock() };
    for (int b = 0; b < 3; ++b) {
        if (buttons[b] && (k == buttons[b].get())) {
            (void)fit((FitButtonEnum)b, time, viewIdx);

            return true;
        }
    }

    return false;
}

Ashift::Model
AutoStraighten::getModel(double time,
                         ViewIdx view) const
{
    const AutoStraightenOp& o = op();
    const auto value = [&](const KnobDoubleWPtr& w, double fallback) {
        KnobDoublePtr knob = w.lock();
        return knob ? knob->getValueAtTime(time, 0, view) : fallback;
    };
    KnobChoicePtr lensModel = o.lensModel.lock();

    return makeModel(value(o.rotation, 0.), value(o.lensShiftV, 0.), value(o.lensShiftH, 0.), value(o.shear, 0.),
                     lensModel && (lensModel->getValueAtTime(time, 0, view) == (int)eLensModelSpecific),
                     value(o.focalLength, Ashift::kDefaultFocalLength), value(o.cropFactor, 1.), value(o.lensDependence, 100.),
                     value(o.aspectAdjust, 1.));
}

void
AutoStraighten::getSourceFormat(double time,
                                ViewIdx view,
                                RectI* format,
                                double* par) const
{
    NodeMetadataConstPtr metadata = getInput(0) ? getInputMetadataAt(0, time, view) : NodeMetadataConstPtr();

    if (metadata) {
        *format = metadata->getOutputFormat();
        *par = metadata->getPixelAspectRatio(-1);
    } else {
        Format f;
        getApp()->getProject()->getProjectDefaultFormat(&f);
        *format = RectI(f.x1, f.y1, f.x2, f.y2);
        *par = f.getPixelAspectRatio();
    }
    if (*par <= 0.) {
        *par = 1.;
    }
}

AutoStraighten::Geometry
AutoStraighten::computeGeometry(const RectI& source,
                                double par,
                                const Ashift::Model& model,
                                Ashift::CropModeEnum crop) const
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_hasCachedGeometry && (_cachedGeometry.source == source) && (_cachedGeometry.par == par) && sameModel(_cachedModel, model)
            && (_cachedCrop == crop)) {
            return _cachedGeometry;
        }
    }

    Geometry g;
    g.source = source;
    g.par = par;
    g.width = (int)std::lround(source.width() * par);
    g.height = source.height();
    g.output = source;
    g.outWidth = g.width;
    g.outHeight = g.height;
    if ((g.width > 0) && (g.height > 0) && !Ashift::isNeutral(model, 0., 1., 0., 1.)) {
        CropOverride cropOverride;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            cropOverride = _cropOverride;
        }
        Ashift::CropBox box;
        if (cropOverride) {
            (void)cropOverride(model, g.width, g.height, crop, &box);
        } else if (!Ashift::computeCrop(model, g.width, g.height, crop, &box)) {
            box = Ashift::CropBox();
        }
        int ow = 0, oh = 0;
        Ashift::outputSize(model, g.width, g.height, box.cl, box.cr, box.ct, box.cb, &ow, &oh);
        // darktable's modify_roi_out() leaves an output under 4 pixels unprocessed.
        if ((ow >= kMinimumOutputSize) && (oh >= kMinimumOutputSize)) {
            g.crop = box;
            g.outWidth = ow;
            g.outHeight = oh;
            const int outPixels = (std::max)(1, (int)std::lround(ow / par));
            g.output = RectI(source.x1, source.y1, source.x1 + outPixels, source.y1 + oh);
            g.bypass = false;
        }
    }

    std::lock_guard<std::mutex> lock(_mutex);
    _cachedGeometry = g;
    _cachedModel = model;
    _cachedCrop = crop;
    _hasCachedGeometry = true;

    return g;
} // AutoStraighten::computeGeometry

AutoStraighten::Geometry
AutoStraighten::getGeometry(double time,
                            ViewIdx view) const
{
    RectI format;
    double par = 1.;
    getSourceFormat(time, view, &format, &par);
    KnobChoicePtr autoCrop = op().autoCrop.lock();
    const Ashift::CropModeEnum crop = autoCrop ? (Ashift::CropModeEnum)autoCrop->getValueAtTime(time, 0, view) : Ashift::eCropLargest;

    return computeGeometry(format, par, getModel(time, view), crop);
}

bool
AutoStraighten::canonicalMatrix(const Geometry& g,
                                const Ashift::Model& model,
                                bool invert,
                                TransformMath::Mat3* matrix)
{
    if (g.bypass) {
        return false;
    }
    using TransformMath::Mat3;
    const double par = g.par;
    // darktable's process(): output pixel (i, j) samples the input at hinv * (i + cx, j + cy), with
    // pixel centres at integers, y down, in square pixels.
    const double cx = (g.crop.cr > g.crop.cl) ? g.outWidth / (g.crop.cr - g.crop.cl) * g.crop.cl : 0.;
    const double cy = (g.crop.cb > g.crop.ct) ? g.outHeight / (g.crop.cb - g.crop.ct) * g.crop.ct : 0.;
    const Mat3 toOutput(1., 0., -(0.5 + g.output.x1) * par + cx,
                        0., -1., (g.output.y2 - 0.5) + cy,
                        0., 0., 1.);
    const Mat3 fromInput(1., 0., (0.5 + g.source.x1) * par,
                         0., -1., g.source.y2 - 0.5,
                         0., 0., 1.);
    const Mat3 m = fromInput * Ashift::homography(model, g.width, g.height, true) * toOutput;

    if (!invert) {
        *matrix = m;

        return true;
    }

    return m.inverse(matrix);
}

void
AutoStraighten::setCropOverride(const CropOverride& cropOverride)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _cropOverride = cropOverride;
    _hasCachedGeometry = false;
}

int
AutoStraighten::fitAxes(FitButtonEnum button,
                        FitParametersEnum parameters)
{
    switch (parameters) {
    case eFitParametersRotation:
        return (button == eFitButtonVertical)  ? Ashift::eFitRotationVerticalLines
            : (button == eFitButtonHorizontal) ? Ashift::eFitRotationHorizontalLines
                                               : Ashift::eFitRotationBothLines;
    case eFitParametersLensShift:
        return (button == eFitButtonVertical)  ? Ashift::eFitVerticallyNoRotation
            : (button == eFitButtonHorizontal) ? Ashift::eFitHorizontallyNoRotation
                                               : Ashift::eFitBothNoRotation;
    case eFitParametersRotationAndLensShift:
        return (button == eFitButtonVertical)  ? Ashift::eFitVertically
            : (button == eFitButtonHorizontal) ? Ashift::eFitHorizontally
                                               : Ashift::eFitBoth;
    case eFitParametersAll:
    default:
        return (button == eFitButtonVertical)  ? Ashift::eFitVertically
            : (button == eFitButtonHorizontal) ? Ashift::eFitHorizontally
                                               : Ashift::eFitBothShear;
    }
}

Ashift::LineSet
AutoStraighten::rectangleLines(double time,
                               ViewIdx view) const
{
    RectI format;
    double par = 1.;
    getSourceFormat(time, view, &format, &par);
    // Canonical to darktable's square-pixel input coordinates.
    const double x0 = (0.5 + format.x1) * par;
    const double y0 = format.y2 - 0.5;
    // darktable's order: top left, bottom left, top right, bottom right.
    const int order[4] = { 0, 2, 1, 3 };
    double pts[8];
    for (int i = 0; i < 4; ++i) {
        KnobDoublePtr corner = _corners[order[i]].lock();
        const double x = corner ? corner->getValueAtTime(time, 0, view) : 0.;
        const double y = corner ? corner->getValueAtTime(time, 1, view) : 0.;
        pts[2 * i] = x - x0;
        pts[2 * i + 1] = y0 - y;
    }

    return Ashift::quadLines(pts, (int)std::lround(format.width() * par), format.height());
}

bool
AutoStraighten::detectStructure(double time,
                                ViewIdx view,
                                Ashift::LineSet* lines,
                                std::string* error)
{
    if (!getInput(0)) {
        *error = "could not detect structural data in image";

        return false;
    }
    RectI format;
    double par = 1.;
    getSourceFormat(time, view, &format, &par);
    const int width = format.width();
    const int height = format.height();
    if ((width <= 0) || (height <= 0)) {
        *error = "could not detect structural data in image";

        return false;
    }
    const RectD bounds = toCanonical(format, par);
    RectI roi;
    ImagePtr src = getImage(0, time, RenderScale::identity, view, &bounds, NULL, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &roi);
    if (!src || (src->getBitDepth() != eImageBitDepthFloat)) {
        *error = "could not detect structural data in image";

        return false;
    }
    // Top row first, as darktable's buffers are.
    std::vector<float> rgba((std::size_t)width * height * 4, 0.f);
    const int nComps = (int)src->getComponentsCount();
    const RectI area = format.intersect(src->getBounds());
    {
        Image::ReadAccess access(src.get());
        for (int py = area.y1; py < area.y2; ++py) {
            const float* pix = (const float*)access.pixelAt(area.x1, py);
            if (!pix) {
                continue;
            }
            float* dst = &rgba[4 * ((std::size_t)(format.y2 - 1 - py) * width + (area.x1 - format.x1))];
            for (int px = area.x1; px < area.x2; ++px, pix += nComps, dst += 4) {
                for (int c = 0; c < 3; ++c) {
                    dst[c] = pix[(nComps >= 3) ? c : 0];
                }
                dst[3] = (nComps == 4) ? pix[3] : 1.f;
            }
        }
    }
    KnobChoicePtr enhanceKnob = _enhance.lock();
    const int enhance = enhanceKnob ? enhanceKnob->getValue() : 0;
    if (!Ashift::detectLines(std::move(rgba), width, height, par, enhance, Ashift::SegmentDetector(), lines, error)) {
        if (!Ashift::hasSegmentDetector()) {
            *error = "line detector unavailable: use Rectangle mode";
        }

        return false;
    }
    Ashift::removeOutliers(lines, kOutlierSeed);

    return true;
} // AutoStraighten::detectStructure

bool
AutoStraighten::fit(FitButtonEnum button,
                    double time,
                    ViewIdx view)
{
    clearPersistentMessage(false);

    KnobChoicePtr structure = _structure.lock();
    Ashift::LineSet lines;
    std::string error;
    if (structure && (structure->getValue() == (int)eStructureRectangle)) {
        lines = rectangleLines(time, view);
    } else if (!detectStructure(time, view, &lines, &error)) {
        setPersistentMessage(eMessageTypeError, error);

        return false;
    }
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _lastLines = lines;
        _lastLinesTime = time;
        _hasLastLines = true;
    }

    KnobChoicePtr fitParameters = _fitParameters.lock();
    const FitParametersEnum parameters = fitParameters ? (FitParametersEnum)fitParameters->getValue() : eFitParametersAll;
    Ashift::Model model = getModel(time, view);
    switch (Ashift::fit(lines, fitAxes(button, parameters), &model)) {
    case Ashift::eNmsSuccess:
        break;
    case Ashift::eNmsNotEnoughLines:
        setPersistentMessage(eMessageTypeError, kNotEnoughStructure);

        return false;
    case Ashift::eNmsDidNotConverge:
    case Ashift::eNmsInsane:
        setPersistentMessage(eMessageTypeError, kFitFailed);

        return false;
    }

    const AutoStraightenOp& o = op();
    const KnobDoublePtr knobs[4] = { o.rotation.lock(), o.lensShiftV.lock(), o.lensShiftH.lock(), o.shear.lock() };
    const double values[4] = { -model.rotation, model.lensShiftV, model.lensShiftH, model.shear };
    AppInstancePtr app = getApp();
    const bool oneUndoStep = app && !app->isCreatingPythonGroup();
    if (oneUndoStep) {
        setMultipleParamsEditLevel(KnobHolder::eMultipleParamsEditOnCreateNewCommand);
    }
    beginChanges();
    for (int i = 0; i < 4; ++i) {
        if (!knobs[i]) {
            continue;
        }
        if (knobs[i]->isAnimated(0, ViewSpec::all())) {
            knobs[i]->setValueAtTime(time, values[i], ViewSpec::all(), 0, eValueChangedReasonPluginEdited, NULL);
        } else {
            knobs[i]->setValue(values[i], ViewSpec::all(), 0, eValueChangedReasonPluginEdited, NULL);
        }
    }
    endChanges();
    if (oneUndoStep) {
        setMultipleParamsEditLevel(KnobHolder::eMultipleParamsEditOff);
    }

    return true;
} // AutoStraighten::fit

Ashift::LineSet
AutoStraighten::lines(double time) const
{
    std::lock_guard<std::mutex> lock(_mutex);

    return (_hasLastLines && (_lastLinesTime == time)) ? _lastLines : Ashift::LineSet();
}

StatusEnum
AutoStraighten::getRegionOfDefinition(U64 hash,
                                      double time,
                                      const RenderScale& scale,
                                      ViewIdx view,
                                      RectD* rod)
{
    const StatusEnum status = ImageNode<AutoStraightenOp>::getRegionOfDefinition(hash, time, scale, view, rod);

    if ((status != eStatusOK) || !getInput(0)) {
        return status;
    }
    const Geometry g = getGeometry(time, view);
    if (!g.bypass) {
        *rod = rectIntersection(*rod, toCanonical(g.output, g.par));
    }

    return eStatusOK;
}

StatusEnum
AutoStraighten::getPreferredMetadataAt(double time,
                                       ViewIdx view,
                                       NodeMetadata& metadata)
{
    // The metadata being computed hold the source's format and pixel aspect ratio.
    const RectI source = metadata.getOutputFormat();
    const double par = (metadata.getPixelAspectRatio(0) > 0.) ? metadata.getPixelAspectRatio(0) : 1.;
    KnobChoicePtr autoCrop = op().autoCrop.lock();
    const Ashift::CropModeEnum crop = autoCrop ? (Ashift::CropModeEnum)autoCrop->getValueAtTime(time, 0, view) : Ashift::eCropLargest;
    const Geometry g = computeGeometry(source, par, getModel(time, view), crop);

    metadata.setOutputFormat(g.output);

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
