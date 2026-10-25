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

#include "Reformat.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <ofxNatron.h>

#include "Engine/AppInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/KnobReader.h"
#include "Engine/Project.h"
#include "Engine/RenderScale.h"
#include "Engine/TimeLine.h"
#include "Engine/Transform.h"

NATRON_NAMESPACE_ENTER

namespace {
// The rectangle helpers below repeat the OpenFX support library's Coords functions, which the
// output format and the transform are computed with, so the results are the same to the bit.

template <typename Rect>
bool
rectIsEmpty(const Rect& r)
{
    return (r.x2 <= r.x1) || (r.y2 <= r.y1);
}

// Rectangles that merely touch still intersect; an empty or disjoint pair gives an empty
// rectangle at the origin.
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

// Pixels at render scale 1 to canonical.
RectD
toCanonical(const RectD& r,
            double par)
{
    if (rectIsEmpty(r)) {
        return RectD(0., 0., 0., 0.);
    }

    return RectD(r.x1 * par / 1., r.y1 / 1., r.x2 * par / 1., r.y2 / 1.);
}

RectD
toCanonical(const RectI& r,
            double par)
{
    if (rectIsEmpty(r)) {
        return RectD(0., 0., 0., 0.);
    }

    return RectD(r.x1 * par / 1., r.y1 / 1., r.x2 * par / 1., r.y2 / 1.);
}

// Canonical to pixels at render scale 1, without rounding.
RectD
toPixelSub(const RectD& r,
           double par)
{
    if (rectIsEmpty(r)) {
        return RectD(0., 0., 0., 0.);
    }

    return RectD(r.x1 * 1. / par, r.y1 * 1., r.x2 * 1. / par, r.y2 * 1.);
}

// Canonical to pixels at render scale 1, each edge rounded to the nearest pixel boundary.
RectI
toPixelNearest(const RectD& r,
               double par)
{
    if (rectIsEmpty(r)) {
        return RectI(0, 0, 0, 0);
    }

    return RectI((int)std::floor(r.x1 * 1. / par + 0.5),
                 (int)std::floor(r.y1 * 1. + 0.5),
                 (int)std::ceil(r.x2 * 1. / par - 0.5),
                 (int)std::ceil(r.y2 * 1. - 0.5));
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
} // anonymous namespace

ReformatOp::Params
ReformatOp::bake(const KnobReader& reader) const
{
    Params p;

    p.time = reader.time();
    p.view = reader.view();
    const bool moved = reader.value(center) || reader.value(flip) || reader.value(flop) || reader.value(turn);
    p.identity = !moved && ((Reformat::ResizeEnum)reader.value(resize) == Reformat::eResizeNone);
    p.filter = (Resampler::FilterEnum)reader.value(filter);
    p.clamp = reader.value(clamp);
    p.blackOutside = reader.value(blackOutside);

    return p;
}

bool
ReformatOp::isIdentity(const Params& params,
                       const PassContext& /*context*/) const
{
    return params.identity;
}

ResampleSettings
ReformatOp::settings(const Params& params,
                     const PassContext& /*context*/) const
{
    ResampleSettings s;

    s.filter = params.filter;
    s.clamp = params.clamp;
    s.blackOutside = params.blackOutside;
    s.invert = false;
    s.transformIsIdentity = params.identity;

    return s;
}

bool
ReformatOp::transform(const Params& params,
                      const PassContext& context,
                      double /*amount*/,
                      bool invert,
                      TransformMath::Mat3* matrix) const
{
    return reformat && reformat->getInverseTransformCanonical(params.time, params.view, invert, context.pixelAspectRatio, matrix);
}

bool
ReformatOp::canConcatenate() const
{
    KnobBoolPtr knob = preserveBB.lock();

    return knob && knob->getValue();
}

std::optional<GpuCostHint>
ReformatOp::costHint(const Params& /*params*/,
                     const PassContext& /*context*/) const
{
    return GpuCostHint();
}

Reformat::Reformat(NodePtr node)
    : ImageNode<ReformatOp>(node)
{
    op().reformat = this;
}

Reformat::~Reformat()
{
}

void
Reformat::addAcceptedComponents(int inputNb,
                                std::list<ImageLayerDesc>* comps)
{
    ImageNode<ReformatOp>::addAcceptedComponents(inputNb, comps);
    comps->push_back(ImageLayerDesc::getXYComponents());
}

void
Reformat::initializeKnobs()
{
    const KnobPagePtr page = createControlsPage();

    KnobBoolPtr useRoD = createKnob<KnobBool>(tr("Use Source RoD"));
    useRoD->setName(kReformatParamUseRoD);
    useRoD->setHintToolTip(tr("Use the region of definition of the source as the source format."));
    useRoD->setDefaultValue(false);
    useRoD->setAnimationEnabled(false);
    useRoD->setIsMetadataSlave(true);
    page->addKnob(useRoD);
    _useRoD = useRoD;

    KnobChoicePtr type = createKnob<KnobChoice>(tr("Type"));
    type->setName(kReformatParamType);
    type->setHintToolTip(tr("Selects how the output format is computed."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kReformatParamTypeOptionToFormat, tr("To Format").toStdString(), tr("Resize to predefined format.").toStdString()));
        options.push_back(ChoiceOption(kReformatParamTypeOptionToBox, tr("To Box").toStdString(), tr("Resize to fit into a box of a given width and height.").toStdString()));
        options.push_back(ChoiceOption(kReformatParamTypeOptionScale, tr("Scale").toStdString(), tr("Apply scale (rounding to integer pixel sizes).").toStdString()));
        options.push_back(ChoiceOption(kReformatParamTypeOptionToProjectFormat, tr("To Project Format").toStdString(), tr("Resize to project format.").toStdString()));
        type->populateChoices(options);
    }
    type->setDefaultValue((int)eReformatTypeToProjectFormat);
    type->setAnimationEnabled(false);
    type->setIsMetadataSlave(true);
    page->addKnob(type);
    _type = type;

    KnobChoicePtr format = createKnob<KnobChoice>(tr("Format"));
    format->setName(kNatronParamFormatChoice);
    format->setHintToolTip(tr("The output format"));
    {
        std::vector<ChoiceOption> options;
        int projectFormatIndex = 0;
        getApp()->getProject()->getProjectFormatEntries(&options, &projectFormatIndex);
        format->populateChoices(options);
        // The host's refresh moves the default without touching the value, so the value is
        // seeded here to start on the project's own format.
        format->setDefaultValue(projectFormatIndex);
    }
    format->setAnimationEnabled(false);
    format->setIsMetadataSlave(true);
    page->addKnob(format);
    _format = format;

    KnobIntPtr formatSize = createKnob<KnobInt>(tr("Size"), 2);
    formatSize->setName(kNatronParamFormatSize);
    formatSize->setHintToolTip(tr("The output dimensions of the image in pixels."));
    formatSize->setDefaultValue(200, 0);
    formatSize->setDefaultValue(200, 1);
    formatSize->setAnimationEnabled(false);
    formatSize->setIsMetadataSlave(true);
    formatSize->setSecretByDefault(true);
    formatSize->setDefaultAllDimensionsEnabled(false);
    page->addKnob(formatSize);
    _formatSize = formatSize;

    KnobDoublePtr formatPar = createKnob<KnobDouble>(tr("Pixel Aspect Ratio"));
    formatPar->setName(kNatronParamFormatPar);
    formatPar->setHintToolTip(tr("Output pixel aspect ratio."));
    formatPar->setMinimum(0.);
    formatPar->setMaximum(10.);
    formatPar->setDisplayMinimum(0.5);
    formatPar->setDisplayMaximum(2.);
    formatPar->setDefaultValue(1.);
    formatPar->setAnimationEnabled(false);
    formatPar->setIsMetadataSlave(true);
    formatPar->setSecretByDefault(true);
    formatPar->setDefaultAllDimensionsEnabled(false);
    page->addKnob(formatPar);
    _formatPar = formatPar;

    KnobIntPtr boxSize = createKnob<KnobInt>(tr("Size"), 2);
    boxSize->setName(kReformatParamBoxSize);
    boxSize->setHintToolTip(tr("The output dimensions of the image in pixels."));
    boxSize->setDefaultValue(200, 0);
    boxSize->setDefaultValue(200, 1);
    boxSize->setAnimationEnabled(false);
    boxSize->setAddNewLine(false);
    boxSize->setSpacingBetweenItems(1);
    boxSize->setIsMetadataSlave(true);
    page->addKnob(boxSize);
    _boxSize = boxSize;

    KnobBoolPtr boxFixed = createKnob<KnobBool>(tr("Force This Shape"));
    boxFixed->setName(kReformatParamBoxFixed);
    boxFixed->setHintToolTip(tr("If checked, the output image is cropped to this size. Else, image is resized according to the resize type but the whole image is kept."));
    boxFixed->setDefaultValue(false);
    boxFixed->setAnimationEnabled(false);
    boxFixed->setIsMetadataSlave(true);
    page->addKnob(boxFixed);
    _boxFixed = boxFixed;

    KnobDoublePtr boxPar = createKnob<KnobDouble>(tr("Pixel Aspect Ratio"));
    boxPar->setName(kReformatParamBoxPar);
    boxPar->setHintToolTip(tr("Output pixel aspect ratio."));
    boxPar->setMinimum(0.);
    boxPar->setMaximum(10.);
    boxPar->setDisplayMinimum(0.5);
    boxPar->setDisplayMaximum(2.);
    boxPar->setDefaultValue(1.);
    boxPar->setAnimationEnabled(false);
    boxPar->setIsMetadataSlave(true);
    page->addKnob(boxPar);
    _boxPar = boxPar;

    KnobDoublePtr scale = createKnob<KnobDouble>(tr("Scale"), 2);
    scale->setName(kReformatParamScale);
    scale->setCanAutoFoldDimensions(true);
    scale->setHintToolTip(tr("The scale factor to apply to the image. The scale factor is rounded slightly, so that the output image is an integer number of pixels in the direction chosen under resize type."));
    for (int d = 0; d < 2; ++d) {
        scale->setMinimum(-DBL_MAX, d);
        scale->setMaximum(DBL_MAX, d);
        scale->setDisplayMinimum(0.1, d);
        scale->setDisplayMaximum(10., d);
        scale->setIncrement(0.01, d);
        scale->setDefaultValue(1., d);
    }
    scale->setAnimationEnabled(false);
    scale->setAddNewLine(false);
    scale->setSpacingBetweenItems(1);
    scale->setIsMetadataSlave(true);
    page->addKnob(scale);
    _scale = scale;

    KnobBoolPtr scaleUniform = createKnob<KnobBool>(tr("Uniform"));
    scaleUniform->setName(kReformatParamScaleUniform);
    scaleUniform->setHintToolTip(tr("Use the X scale for both directions"));
    scaleUniform->setDefaultValue(false);
    scaleUniform->setAnimationEnabled(false);
    scaleUniform->setIsMetadataSlave(true);
    page->addKnob(scaleUniform);
    _scaleUniform = scaleUniform;

    // The divider an OpenFX host draws under a knob whose layout hint asks for one, with the name
    // that host gives it.
    {
        KnobSeparatorPtr separator = createKnob<KnobSeparator>(std::string());
        separator->setName(std::string(kReformatParamScaleUniform) + "_separator");
        page->addKnob(separator);
    }

    KnobChoicePtr resize = createKnob<KnobChoice>(tr("Resize Type"));
    resize->setName(kReformatParamResize);
    resize->setHintToolTip(tr("Format: Converts between formats, the image is resized to fit in the target format. "
                              "Size: Scales to fit into a box of a given width and height. "
                              "Scale: Scales the image."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kReformatParamResizeOptionNone, tr("None").toStdString(), tr("Do not resize the original.").toStdString()));
        options.push_back(ChoiceOption(kReformatParamResizeOptionWidth, tr("Width").toStdString(), tr("Scale the original so that its width fits the output width, while preserving the aspect ratio.").toStdString()));
        options.push_back(ChoiceOption(kReformatParamResizeOptionHeight, tr("Height").toStdString(), tr("Scale the original so that its height fits the output height, while preserving the aspect ratio.").toStdString()));
        options.push_back(ChoiceOption(kReformatParamResizeOptionFit, tr("Fit").toStdString(), tr("Scale the original so that its smallest size fits the output width or height, while preserving the aspect ratio.").toStdString()));
        options.push_back(ChoiceOption(kReformatParamResizeOptionFill, tr("Fill").toStdString(), tr("Scale the original so that its longest size fits the output width or height, while preserving the aspect ratio.").toStdString()));
        options.push_back(ChoiceOption(kReformatParamResizeOptionDistort, tr("Distort").toStdString(), tr("Scale the original so that both sides fit the output dimensions. This does not preserve the aspect ratio.").toStdString()));
        resize->populateChoices(options);
    }
    resize->setDefaultValue((int)eResizeWidth);
    resize->setAnimationEnabled(false);
    resize->setIsMetadataSlave(true);
    page->addKnob(resize);
    op().resize = resize;

    KnobBoolPtr center = createKnob<KnobBool>(tr("Center"));
    center->setName(kReformatParamCenter);
    center->setHintToolTip(tr("Translate the center of the image to the center of the output. Otherwise, the lower left corner is left untouched."));
    center->setDefaultValue(true);
    center->setAnimationEnabled(false);
    center->setAddNewLine(false);
    center->setSpacingBetweenItems(1);
    center->setIsMetadataSlave(true);
    page->addKnob(center);
    op().center = center;

    KnobBoolPtr flip = createKnob<KnobBool>(tr("Flip"));
    flip->setName(kReformatParamFlip);
    flip->setHintToolTip(tr("Mirror the image vertically."));
    flip->setDefaultValue(false);
    flip->setAnimationEnabled(false);
    flip->setAddNewLine(false);
    flip->setSpacingBetweenItems(1);
    page->addKnob(flip);
    op().flip = flip;

    KnobBoolPtr flop = createKnob<KnobBool>(tr("Flop"));
    flop->setName(kReformatParamFlop);
    flop->setHintToolTip(tr("Mirror the image horizontally."));
    flop->setDefaultValue(false);
    flop->setAnimationEnabled(false);
    flop->setAddNewLine(false);
    flop->setSpacingBetweenItems(1);
    page->addKnob(flop);
    op().flop = flop;

    KnobBoolPtr turn = createKnob<KnobBool>(tr("Turn"));
    turn->setName(kReformatParamTurn);
    turn->setHintToolTip(tr("Rotate the image by 90 degrees counter-clockwise."));
    turn->setDefaultValue(false);
    turn->setAnimationEnabled(false);
    turn->setIsMetadataSlave(true);
    page->addKnob(turn);
    op().turn = turn;

    KnobBoolPtr preserveBB = createKnob<KnobBool>(tr("Preserve BBox"));
    preserveBB->setName(kReformatParamPreserveBoundingBox);
    preserveBB->setHintToolTip(tr("If checked, preserve the whole image bounding box and concatenate transforms downstream.\n"
                                  "Normally, all pixels outside of the outside format are clipped off. If this is checked, the whole image RoD is kept.\n"
                                  "By default, transforms are only concatenated upstream, i.e. the image is rendered by this effect by concatenating upstream transforms (e.g. CornerPin, Transform...), and the original image is resampled only once. If checked, and there are concatenating transform effects downstream, the image is rendered by the last consecutive concatenating effect."));
    preserveBB->setDefaultValue(false);
    preserveBB->setAnimationEnabled(false);
    preserveBB->setIsMetadataSlave(true);
    page->addKnob(preserveBB);
    op().preserveBB = preserveBB;

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
    blackOutside->setDefaultValue(false);
    page->addKnob(blackOutside);
    op().blackOutside = blackOutside;

    addStageKnobs(page);

    refreshVisibility();
    updateClampEnabled(getApp()->getTimeLine()->currentFrame(), ViewIdx(0));
} // Reformat::initializeKnobs

Reformat::ReformatTypeEnum
Reformat::getReformatType() const
{
    KnobChoicePtr type = _type.lock();

    return type ? (ReformatTypeEnum)type->getValue() : eReformatTypeToProjectFormat;
}

void
Reformat::refreshVisibility()
{
    const ReformatTypeEnum type = getReformatType();
    const bool isFormat = (type == eReformatTypeToFormat);
    const bool isBox = (type == eReformatTypeToBox);
    const bool isScale = (type == eReformatTypeScale);

    setSecretAndDisabled(_format.lock(), !isFormat);
    setSecretAndDisabled(_boxSize.lock(), !isBox);
    setSecretAndDisabled(_boxPar.lock(), !isBox);
    setSecretAndDisabled(_boxFixed.lock(), !isBox);
    setSecretAndDisabled(_scale.lock(), !isScale);
    setSecretAndDisabled(_scaleUniform.lock(), !isScale);
    setSecretAndDisabled(_formatSize.lock(), true);
    setSecretAndDisabled(_formatPar.lock(), true);
}

void
Reformat::updateClampEnabled(double time, ViewSpec view)
{
    KnobChoicePtr filter = op().filter.lock();
    KnobBoolPtr clamp = op().clamp.lock();

    if (!filter || !clamp) {
        return;
    }
    clamp->setAllDimensionsEnabled(Resampler::filterHonoursClamp((Resampler::FilterEnum)filter->getValueAtTime(time, 0, view)));
}

void
Reformat::onKnobsLoaded()
{
    refreshVisibility();
    updateClampEnabled(getApp()->getTimeLine()->currentFrame(), ViewIdx(0));
}

bool
Reformat::knobChanged(KnobI* k,
                      ValueChangedReasonEnum reason,
                      ViewSpec view,
                      double time,
                      bool /*originatedFromMainThread*/)
{
    KnobChoicePtr filterKnob = op().filter.lock();
    if (filterKnob && (k == filterKnob.get())) {
        updateClampEnabled(time, (view.isAll() || view.isCurrent()) ? ViewIdx(0) : ViewIdx(view));

        return true;
    }
    if (reason == eValueChangedReasonTimeChanged) {
        return false;
    }
    KnobChoicePtr type = _type.lock();
    KnobChoicePtr format = _format.lock();
    KnobDoublePtr scale = _scale.lock();
    KnobBoolPtr scaleUniform = _scaleUniform.lock();

    const bool isType = type && (k == type.get());
    if (isType) {
        refreshVisibility();
    }
    if (isType || (format && (k == format.get())) || (scale && (k == scale.get())) || (scaleUniform && (k == scaleUniform.get()))) {
        int w = 0;
        int h = 0;
        double par = 1.;
        bool boxFixed = false;
        if (getBoxValues(time, ViewIdx(0), getAspectRatio(0), &w, &h, &par, &boxFixed)) {
            KnobIntPtr boxSizeKnob = _boxSize.lock();
            KnobDoublePtr boxParKnob = _boxPar.lock();
            KnobBoolPtr boxFixedKnob = _boxFixed.lock();
            beginChanges();
            if (boxSizeKnob) {
                boxSizeKnob->setValues(w, h, ViewSpec::all(), eValueChangedReasonPluginEdited);
            }
            if (boxParKnob) {
                boxParKnob->setValue(par);
            }
            if (boxFixedKnob) {
                boxFixedKnob->setValue(boxFixed);
            }
            endChanges();
        }

        return true;
    }

    return false;
}

RectD
Reformat::getProjectRect(double* par) const
{
    Format f;

    getApp()->getProject()->getProjectDefaultFormat(&f);
    const double projectPar = f.getPixelAspectRatio();
    if (par) {
        *par = projectPar;
    }
    const RectI pixel(f.x1, f.y1, f.x2, f.y2);

    return pixel.toCanonical_noClipping(0, projectPar);
}

bool
Reformat::getSourceRegionOfDefinition(double time,
                                      ViewIdx view,
                                      const RenderScale& scale,
                                      RectD* rod) const
{
    *rod = RectD(0., 0., 0., 0.);
    EffectInstancePtr input = getInput(0);
    if (!input) {
        return false;
    }
    const RenderScale inputScale = input->supportsRenderScale() ? scale : RenderScale::identity;
    bool isProjectFormat = false;
    RectD srcRoD;
    if (input->getRegionOfDefinition_public(input->getRenderHash(), time, inputScale, view, &srcRoD, &isProjectFormat) != eStatusFailed) {
        *rod = srcRoD;
    }

    return true;
}

void
Reformat::getInputFormat(double time,
                         ViewIdx view,
                         double srcPar,
                         double* par,
                         RectD* rect) const
{
    *par = srcPar;
    KnobBoolPtr useRoD = _useRoD.lock();
    if (!useRoD || !useRoD->getValueAtTime(time, 0, view)) {
        // The format an OpenFX source clip reports: that of the nearest upstream node that is not
        // an identity, or the project's without a source.
        RectI format;
        EffectInstancePtr input = getInput(0);
        if (input) {
            input = input->getNearestNonIdentity(time);
        }
        if (input) {
            format = input->getOutputFormat();
        } else {
            Format f;
            getApp()->getProject()->getProjectDefaultFormat(&f);
            format = RectI(f.x1, f.y1, f.x2, f.y2);
        }
        if (!rectIsEmpty(format)) {
            *rect = RectD(format.x1, format.y1, format.x2, format.y2);

            return;
        }
    }
    RectD srcRoD;
    getSourceRegionOfDefinition(time, view, RenderScale::identity, &srcRoD);
    *rect = toPixelSub(srcRoD, *par);
}

bool
Reformat::getBoxValues(double time,
                       ViewIdx view,
                       double srcPar,
                       int* w,
                       int* h,
                       double* par,
                       bool* boxFixed) const
{
    switch (getReformatType()) {
    case eReformatTypeToFormat: {
        // The host fills the size and the pixel aspect ratio from the chosen format.
        KnobIntPtr formatSize = _formatSize.lock();
        KnobDoublePtr formatPar = _formatPar.lock();
        *w = formatSize ? formatSize->getValueAtTime(time, 0, view) : 0;
        *h = formatSize ? formatSize->getValueAtTime(time, 1, view) : 0;
        *par = formatPar ? formatPar->getValueAtTime(time, 0, view) : 1.;
        *boxFixed = true;
        break;
    }
    case eReformatTypeToBox: {
        KnobIntPtr boxSize = _boxSize.lock();
        KnobDoublePtr boxPar = _boxPar.lock();
        KnobBoolPtr boxFixedKnob = _boxFixed.lock();
        *w = boxSize ? boxSize->getValueAtTime(time, 0, view) : 0;
        *h = boxSize ? boxSize->getValueAtTime(time, 1, view) : 0;
        *par = boxPar ? boxPar->getValueAtTime(time, 0, view) : 1.;
        *boxFixed = boxFixedKnob && boxFixedKnob->getValueAtTime(time, 0, view);

        return false;
    }
    case eReformatTypeScale: {
        KnobDoublePtr scaleKnob = _scale.lock();
        KnobBoolPtr scaleUniform = _scaleUniform.lock();
        double scaleX = scaleKnob ? scaleKnob->getValueAtTime(time, 0, view) : 1.;
        double scaleY = scaleKnob ? scaleKnob->getValueAtTime(time, 1, view) : 1.;
        if (scaleUniform && scaleUniform->getValueAtTime(time, 0, view)) {
            scaleY = scaleX;
        }
        RectD srcRod;
        if (getInput(0)) {
            *par = srcPar;
            getSourceRegionOfDefinition(time, view, RenderScale::identity, &srcRod);
        } else {
            srcRod = getProjectRect(par);
        }
        KnobBoolPtr turn = op().turn.lock();
        if (turn && turn->getValueAtTime(time, 0, view)) {
            std::swap(srcRod.x1, srcRod.y1);
            std::swap(srcRod.x2, srcRod.y2);
        }
        srcRod.x1 *= scaleX;
        srcRod.x2 *= scaleX;
        srcRod.y1 *= scaleY;
        srcRod.y2 *= scaleY;
        const RectI srcRodPixel = toPixelNearest(srcRod, *par);
        *w = srcRodPixel.x2 - srcRodPixel.x1;
        *h = srcRodPixel.y2 - srcRodPixel.y1;
        *boxFixed = true;
        break;
    }
    case eReformatTypeToProjectFormat: {
        double projectPar = 1.;
        const RectD project = getProjectRect(&projectPar);
        *w = (int)(project.width() / projectPar);
        *h = (int)project.height();
        *par = projectPar;
        *boxFixed = true;
        break;
    }
    }

    return true;
} // Reformat::getBoxValues

void
Reformat::computeOutputFormat(double time,
                              ViewIdx view,
                              double srcPar,
                              double* par,
                              RectD* rect,
                              RectI* format) const
{
    int boxW = 0;
    int boxH = 0;
    double boxPAR = 1.;
    bool boxFixed = false;
    getBoxValues(time, view, srcPar, &boxW, &boxH, &boxPAR, &boxFixed);

    KnobChoicePtr resizeKnob = op().resize.lock();
    KnobBoolPtr centerKnob = op().center.lock();
    KnobBoolPtr turnKnob = op().turn.lock();
    ResizeEnum resize = resizeKnob ? (ResizeEnum)resizeKnob->getValueAtTime(time, 0, view) : eResizeWidth;
    const bool center = centerKnob && centerKnob->getValueAtTime(time, 0, view);
    const bool turn = turnKnob && turnKnob->getValueAtTime(time, 0, view);

    if (format && boxFixed) {
        *format = RectI(0, 0, boxW, boxH);
    }

    if ((boxW == 0) && (boxH == 0)) {
        *rect = RectD(0., 0., 0., 0.);
        *par = 1.;
        if (format) {
            *format = RectI(0, 0, 0, 0);
        }

        return;
    }
    const RectD boxRod(0., 0., (double)boxW * boxPAR, (double)boxH);

    RectD srcRod;
    {
        double inputPar = 1.;
        RectD inputFormat;
        getInputFormat(time, view, srcPar, &inputPar, &inputFormat);
        srcRod = toCanonical(inputFormat, inputPar);
    }
    if (rectIsEmpty(srcRod)) {
        *rect = RectD(0., 0., 0., 0.);
        *par = 1.;
        if (format) {
            *format = RectI(0, 0, 0, 0);
        }

        return;
    }
    if (turn) {
        std::swap(srcRod.x1, srcRod.y1);
        std::swap(srcRod.x2, srcRod.y2);
    }

    const double srcw = srcRod.x2 - srcRod.x1;
    const double srch = srcRod.y2 - srcRod.y1;
    if (resize == eResizeFit) {
        resize = (boxRod.x2 * srch > boxRod.y2 * srcw) ? eResizeHeight : eResizeWidth;
    } else if (resize == eResizeFill) {
        resize = (boxRod.x2 * srch > boxRod.y2 * srcw) ? eResizeWidth : eResizeHeight;
    }

    RectD dstRod(0., 0., 0., 0.);
    if (resize == eResizeNone) {
        if (center) {
            const double xoff = ((boxRod.x1 + boxRod.x2) - (srcRod.x1 + srcRod.x2)) / 2;
            const double yoff = ((boxRod.y1 + boxRod.y2) - (srcRod.y1 + srcRod.y2)) / 2;
            dstRod.x1 = srcRod.x1 + xoff;
            dstRod.x2 = srcRod.x2 + xoff;
            dstRod.y1 = srcRod.y1 + yoff;
            dstRod.y2 = srcRod.y2 + yoff;
        } else {
            dstRod = srcRod;
        }
    } else if (resize == eResizeDistort) {
        dstRod.x2 = boxRod.x2;
        dstRod.y2 = boxRod.y2;
    } else if (resize == eResizeWidth) {
        const double scale = boxRod.x2 / srcw;
        dstRod.x2 = boxRod.x2;
        const double dsth = srch * scale;
        const double offset = (center && boxFixed) ? (boxRod.y2 - dsth) / 2 : 0;
        dstRod.y1 = offset;
        dstRod.y2 = offset + dsth;
    } else if (resize == eResizeHeight) {
        const double scale = boxRod.y2 / srch;
        const double dstw = srcw * scale;
        const double offset = (center && boxFixed) ? (boxRod.x2 - dstw) / 2 : 0;
        dstRod.x1 = offset;
        dstRod.x2 = offset + dstw;
        dstRod.y2 = boxRod.y2;
    }
    *par = boxPAR;
    *rect = toPixelSub(dstRod, *par);
    if (format && !boxFixed) {
        *format = toPixelNearest(dstRod, *par);
    }
} // Reformat::computeOutputFormat

bool
Reformat::getInverseTransformCanonical(double time,
                                       ViewIdx view,
                                       bool invert,
                                       double srcPar,
                                       TransformMath::Mat3* matrix) const
{
    if (!getInput(0)) {
        return false;
    }

    RectD srcRod;
    RectD dstRod;
    {
        double par = 1.;
        RectD format;
        getInputFormat(time, view, srcPar, &par, &format);
        srcRod = toCanonical(format, par);
        computeOutputFormat(time, view, srcPar, &par, &format, NULL);
        dstRod = toCanonical(format, par);
    }
    KnobBoolPtr flipKnob = op().flip.lock();
    KnobBoolPtr flopKnob = op().flop.lock();
    KnobBoolPtr turnKnob = op().turn.lock();
    const bool flip = flipKnob && flipKnob->getValueAtTime(time, 0, view);
    const bool flop = flopKnob && flopKnob->getValueAtTime(time, 0, view);
    const bool turn = turnKnob && turnKnob->getValueAtTime(time, 0, view);

    // Swapped bounds leave srcRod "empty", which is what mirrors the mapping.
    if (flip) {
        std::swap(srcRod.y1, srcRod.y2);
    }
    if (flop) {
        std::swap(srcRod.x1, srcRod.x2);
    }
    TransformMath::Mat3& m = *matrix;
    if (!invert) {
        if ((dstRod.x1 == dstRod.x2) || (dstRod.y1 == dstRod.y2)) {
            return false;
        }
        if (!turn) {
            const double ax = (srcRod.x2 - srcRod.x1) / (dstRod.x2 - dstRod.x1);
            const double ay = (srcRod.y2 - srcRod.y1) / (dstRod.y2 - dstRod.y1);
            m(0, 0) = ax;
            m(0, 1) = 0;
            m(0, 2) = srcRod.x1 - dstRod.x1 * ax;
            m(1, 0) = 0;
            m(1, 1) = ay;
            m(1, 2) = srcRod.y1 - dstRod.y1 * ay;
        } else {
            // A quarter turn counter-clockwise.
            const double ax = (srcRod.x2 - srcRod.x1) / (dstRod.y2 - dstRod.y1);
            const double ay = (srcRod.y2 - srcRod.y1) / (dstRod.x2 - dstRod.x1);
            m(0, 0) = 0;
            m(0, 1) = ax;
            m(0, 2) = srcRod.x1 - dstRod.y1 * ax;
            m(1, 0) = -ay;
            m(1, 1) = 0;
            m(1, 2) = srcRod.y1 + dstRod.x2 * ay;
        }
    } else {
        if ((srcRod.x1 == srcRod.x2) || (srcRod.y1 == srcRod.y2)) {
            return false;
        }
        if (!turn) {
            const double ax = (dstRod.x2 - dstRod.x1) / (srcRod.x2 - srcRod.x1);
            const double ay = (dstRod.y2 - dstRod.y1) / (srcRod.y2 - srcRod.y1);
            m(0, 0) = ax;
            m(0, 1) = 0;
            m(0, 2) = dstRod.x1 - srcRod.x1 * ax;
            m(1, 0) = 0;
            m(1, 1) = ay;
            m(1, 2) = dstRod.y1 - srcRod.y1 * ay;
        } else {
            const double ax = (dstRod.x2 - dstRod.x1) / (srcRod.y2 - srcRod.y1);
            const double ay = (dstRod.y2 - dstRod.y1) / (srcRod.x2 - srcRod.x1);
            m(0, 0) = 0;
            m(0, 1) = -ax;
            m(0, 2) = dstRod.x1 + srcRod.y2 * ax;
            m(1, 0) = ay;
            m(1, 1) = 0;
            m(1, 2) = dstRod.y1 - srcRod.x1 * ay;
        }
    }
    m(2, 0) = 0;
    m(2, 1) = 0;
    m(2, 2) = 1.;

    return true;
} // Reformat::getInverseTransformCanonical

StatusEnum
Reformat::getRegionOfDefinition(U64 hash,
                                double time,
                                const RenderScale& scale,
                                ViewIdx view,
                                RectD* rod)
{
    const StatusEnum status = ImageNode<ReformatOp>::getRegionOfDefinition(hash, time, scale, view, rod);

    if ((status != eStatusOK) || !getInput(0) || op().canConcatenate()) {
        return status;
    }
    double par = 1.;
    RectD rect;
    RectI format;
    computeOutputFormat(time, view, getAspectRatio(0), &par, &rect, &format);
    *rod = rectIntersection(*rod, toCanonical(format, par));

    return eStatusOK;
}

StatusEnum
Reformat::getPreferredMetadata(NodeMetadata& metadata)
{
    // The source's pixel aspect ratio is read from the metadata being computed, as the effect's
    // own copy is the previous one.
    const double srcPar = metadata.getPixelAspectRatio(0);
    double par = 1.;
    RectD rect;
    RectI format;

    computeOutputFormat(getCurrentTime(), ViewIdx(0), srcPar, &par, &rect, &format);
    switch (getReformatType()) {
    case eReformatTypeToFormat:
    case eReformatTypeToBox:
    case eReformatTypeToProjectFormat:
        metadata.setPixelAspectRatio(-1, par);
        break;
    case eReformatTypeScale:
        break;
    }
    metadata.setOutputFormat(format);

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
