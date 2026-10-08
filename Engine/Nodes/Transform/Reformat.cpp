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
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <ofxNatron.h>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Format.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/PoolParallelFor.h"
#include "Engine/Project.h"
#include "Engine/RenderScale.h"
#include "Engine/Transform.h"

NATRON_NAMESPACE_ENTER

namespace {
// How many rows render() runs between two abort checks.
const int kAbortCheckRows = 16;

struct FormatEntry {
    const char* id;
    const char* label;
};

// The openfx-misc format list (SupportExt/ofxsFormatResolution.h), in its order. The host
// replaces these entries with the project's formats once the node is created.
const FormatEntry kFormats[] = {
    { "PC_Video", "PC_Video 640x480" },
    { "NTSC", "NTSC 720x486 0.91" },
    { "PAL", "PAL 720x576 1.09" },
    { "NTSC_16:9", "NTSC_16:9 720x486 1.21" },
    { "PAL_16:9", "PAL_16:9 720x576 1.46" },
    { "HD_720", "HD_720 1280x1720" },
    { "HD", "HD 1920x1080" },
    { "UHD_4K", "UHD_4K 3840x2160" },
    { "1K_Super35(full-ap)", "1K_Super35(full-ap) 1024x778" },
    { "1K_Cinemascope", "1K_Cinemascope 914x778 2" },
    { "2K_Super35(full-ap)", "2K_Super35(full-ap) 2048x1556" },
    { "2K_Cinemascope", "2K_Cinemascope 1828x1556 2" },
    { "2K_DCP", "2K_DCP 2048x1080" },
    { "4K_Super35(full-ap)", "4K_Super35(full-ap) 4096x3112" },
    { "4K_Cinemascope", "4K_Cinemascope 3656x3112 2" },
    { "4K_DCP", "4K_DCP 4096x2160" },
    { "square_256", "square_256 256x256" },
    { "square_512", "square_512 512x512" },
    { "square_1K", "square_1K 1024x1024" },
    { "square_2K", "square_2K 2048x2048" },
};

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

// The index in an nComps-channel pixel of the channel on colour bit `bit`, or -1.
int
channelIndexForBit(int nComps,
                   int bit)
{
    if (nComps == 1) {
        return (bit == 3) ? 0 : -1;
    }

    return (bit < nComps) ? bit : -1;
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

NativeImageTraits
reformatTraits()
{
    NativeImageTraits traits;

    traits.processesAllLayers = true;

    return traits;
}

struct ReformatPlaneJob {
    ImagePtr dst;
    ImagePtr src;
    Transform::Matrix3x3Ptr inputTransform;
    std::shared_ptr<Image::ReadAccess> srcAccess;
    std::shared_ptr<Image::WriteAccess> dstAccess;
    // Filled on the calling thread: Image::getBounds() takes the image's lock, which a band thread
    // must never ask for, since behind a writer waiting on an image this render holds for reading
    // it blocks forever.
    Resampler::SourceImage source;
    Resampler::SamplingTransforms transforms;
    Resampler::ResampleParams params;
    int dstNComps;
    int srcIndex[4];

    ReformatPlaneJob()
        : dstNComps(0)
    {
        for (int c = 0; c < 4; ++c) {
            srcIndex[c] = -1;
        }
    }
};

struct ReformatBand {
    std::size_t job;
    int y1;
    int y2;

    ReformatBand(std::size_t jobIndex,
                 int firstRow,
                 int endRow)
        : job(jobIndex)
        , y1(firstRow)
        , y2(endRow)
    {
    }
};
} // anonymous namespace

Reformat::Reformat(NodePtr node)
    : NativeImageEffect(node, reformatTraits())
{
}

Reformat::~Reformat()
{
}

void
Reformat::addAcceptedComponents(int inputNb,
                                std::list<ImageLayerDesc>* comps)
{
    NativeImageEffect::addAcceptedComponents(inputNb, comps);
    comps->push_back(ImageLayerDesc::getXYComponents());
}

NativePluginDescription
Reformat::getNativePluginDescription() const
{
    NativePluginDescription desc;

    desc.id = PLUGINID_NATRON_REFORMAT;
    desc.label = "Reformat";
    desc.description = tr("Convert the image to another format or size.\n"
                          "An image transform is computed that goes from the input format, regardless of the region of definition (RoD), to the selected format. The Resize Type parameter adjust the way the transform is computed.\n"
                          "The output format is set by this effect.\n"
                          "In order to set the output format without transforming the image content, use the NoOp effect.\n"
                          "This plugin concatenates transforms.\n"
                          "See also: https://web.archive.org/web/20220627014216/http://www.opticalenquiry.com/nuke/index.php?title=Reformat")
                           .toStdString();
    desc.grouping = PLUGIN_GROUP_TRANSFORM;
    desc.majorVersion = PLUGIN_MAJOR_NATRON_REFORMAT;
    desc.minorVersion = 0;
    desc.inputs.push_back(NativeInputDescription("Source", false, eDataKindImage));
    desc.outputKind = eDataKindImage;

    return desc;
}

void
Reformat::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

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
        for (std::size_t i = 0; i < sizeof(kFormats) / sizeof(kFormats[0]); ++i) {
            options.push_back(ChoiceOption(kFormats[i].id, kFormats[i].label, std::string()));
        }
        format->populateChoices(options);
    }
    format->setDefaultValue(0);
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
    _resize = resize;

    KnobBoolPtr center = createKnob<KnobBool>(tr("Center"));
    center->setName(kReformatParamCenter);
    center->setHintToolTip(tr("Translate the center of the image to the center of the output. Otherwise, the lower left corner is left untouched."));
    center->setDefaultValue(true);
    center->setAnimationEnabled(false);
    center->setAddNewLine(false);
    center->setSpacingBetweenItems(1);
    center->setIsMetadataSlave(true);
    page->addKnob(center);
    _center = center;

    KnobBoolPtr flip = createKnob<KnobBool>(tr("Flip"));
    flip->setName(kReformatParamFlip);
    flip->setHintToolTip(tr("Mirror the image vertically."));
    flip->setDefaultValue(false);
    flip->setAnimationEnabled(false);
    flip->setAddNewLine(false);
    flip->setSpacingBetweenItems(1);
    page->addKnob(flip);
    _flip = flip;

    KnobBoolPtr flop = createKnob<KnobBool>(tr("Flop"));
    flop->setName(kReformatParamFlop);
    flop->setHintToolTip(tr("Mirror the image horizontally."));
    flop->setDefaultValue(false);
    flop->setAnimationEnabled(false);
    flop->setAddNewLine(false);
    flop->setSpacingBetweenItems(1);
    page->addKnob(flop);
    _flop = flop;

    KnobBoolPtr turn = createKnob<KnobBool>(tr("Turn"));
    turn->setName(kReformatParamTurn);
    turn->setHintToolTip(tr("Rotate the image by 90 degrees counter-clockwise."));
    turn->setDefaultValue(false);
    turn->setAnimationEnabled(false);
    turn->setIsMetadataSlave(true);
    page->addKnob(turn);
    _turn = turn;

    KnobBoolPtr preserveBB = createKnob<KnobBool>(tr("Preserve BBox"));
    preserveBB->setName(kReformatParamPreserveBoundingBox);
    preserveBB->setHintToolTip(tr("If checked, preserve the whole image bounding box and concatenate transforms downstream.\n"
                                  "Normally, all pixels outside of the outside format are clipped off. If this is checked, the whole image RoD is kept.\n"
                                  "By default, transforms are only concatenated upstream, i.e. the image is rendered by this effect by concatenating upstream transforms (e.g. CornerPin, Transform...), and the original image is resampled only once. If checked, and there are concatenating transform effects downstream, the image is rendered by the last consecutive concatenating effect."));
    preserveBB->setDefaultValue(false);
    preserveBB->setAnimationEnabled(false);
    preserveBB->setIsMetadataSlave(true);
    page->addKnob(preserveBB);
    _preserveBB = preserveBB;

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
    _filter = filter;

    KnobBoolPtr clamp = createKnob<KnobBool>(tr(kResamplerParamFilterClampLabel));
    clamp->setName(kResamplerParamFilterClamp);
    clamp->setHintToolTip(tr(kResamplerParamFilterClampHint));
    clamp->setDefaultValue(false);
    clamp->setAddNewLine(false);
    clamp->setSpacingBetweenItems(1);
    page->addKnob(clamp);
    _clamp = clamp;

    KnobBoolPtr blackOutside = createKnob<KnobBool>(tr(kResamplerParamFilterBlackOutsideLabel));
    blackOutside->setName(kResamplerParamFilterBlackOutside);
    blackOutside->setHintToolTip(tr(kResamplerParamFilterBlackOutsideHint));
    blackOutside->setDefaultValue(false);
    page->addKnob(blackOutside);
    _blackOutside = blackOutside;

    refreshVisibility();
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
Reformat::onKnobsLoaded()
{
    refreshVisibility();
}

bool
Reformat::knobChanged(KnobI* k,
                      ValueChangedReasonEnum reason,
                      ViewSpec /*view*/,
                      double time,
                      bool /*originatedFromMainThread*/)
{
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

bool
Reformat::getCanTransform() const
{
    KnobBoolPtr preserveBB = _preserveBB.lock();

    return preserveBB && preserveBB->getValue();
}

bool
Reformat::getInputsHoldingTransform(std::list<int>* inputs) const
{
    inputs->push_back(0);

    return true;
}

bool
Reformat::isIdentityParams(double time,
                           ViewIdx view) const
{
    KnobBoolPtr center = _center.lock();
    KnobBoolPtr flip = _flip.lock();
    KnobBoolPtr flop = _flop.lock();
    KnobBoolPtr turn = _turn.lock();
    KnobChoicePtr resize = _resize.lock();

    if (!center || !flip || !flop || !turn || !resize) {
        return false;
    }
    if (center->getValueAtTime(time, 0, view) || flip->getValueAtTime(time, 0, view) || flop->getValueAtTime(time, 0, view) || turn->getValueAtTime(time, 0, view)) {
        return false;
    }

    return (ResizeEnum)resize->getValueAtTime(time, 0, view) == eResizeNone;
}

bool
Reformat::isIdentityOp(double time,
                       const RenderScale& /*scale*/,
                       const RectI& /*roi*/,
                       ViewIdx view)
{
    KnobBoolPtr clamp = _clamp.lock();

    // Clamping changes values above the source's range even without any movement.
    if (clamp && clamp->getValueAtTime(time, 0, view)) {
        return false;
    }

    return isIdentityParams(time, view);
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
            input = input->getNearestNonIdentity(getCurrentTime());
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
        *w = formatSize ? formatSize->getValue(0) : 0;
        *h = formatSize ? formatSize->getValue(1) : 0;
        *par = formatPar ? formatPar->getValue() : 1.;
        *boxFixed = true;
        break;
    }
    case eReformatTypeToBox: {
        KnobIntPtr boxSize = _boxSize.lock();
        KnobDoublePtr boxPar = _boxPar.lock();
        KnobBoolPtr boxFixedKnob = _boxFixed.lock();
        *w = boxSize ? boxSize->getValue(0) : 0;
        *h = boxSize ? boxSize->getValue(1) : 0;
        *par = boxPar ? boxPar->getValue() : 1.;
        *boxFixed = boxFixedKnob && boxFixedKnob->getValue();

        return false;
    }
    case eReformatTypeScale: {
        KnobDoublePtr scaleKnob = _scale.lock();
        KnobBoolPtr scaleUniform = _scaleUniform.lock();
        double scaleX = scaleKnob ? scaleKnob->getValue(0) : 1.;
        double scaleY = scaleKnob ? scaleKnob->getValue(1) : 1.;
        if (scaleUniform && scaleUniform->getValue()) {
            scaleY = scaleX;
        }
        RectD srcRod;
        if (getInput(0)) {
            *par = srcPar;
            getSourceRegionOfDefinition(time, view, RenderScale::identity, &srcRod);
        } else {
            srcRod = getProjectRect(par);
        }
        KnobBoolPtr turn = _turn.lock();
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

    KnobChoicePtr resizeKnob = _resize.lock();
    KnobBoolPtr centerKnob = _center.lock();
    KnobBoolPtr turnKnob = _turn.lock();
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
    KnobBoolPtr flipKnob = _flip.lock();
    KnobBoolPtr flopKnob = _flop.lock();
    KnobBoolPtr turnKnob = _turn.lock();
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
    if (!getInput(0)) {
        return NativeImageEffect::getRegionOfDefinition(hash, time, scale, view, rod);
    }
    RectD srcRoD;
    getSourceRegionOfDefinition(time, view, scale, &srcRoD);

    const double srcPar = getAspectRatio(0);
    const OfxPointD s = scale.toOfxPointD();
    KnobBoolPtr blackOutside = _blackOutside.lock();
    Resampler::RegionParams params;
    params.invert = false;
    params.blackOutside = blackOutside && blackOutside->getValueAtTime(time, 0, view);
    params.isIdentity = isIdentityParams(time, view);
    const Resampler::CanonicalTransformFn fn = [this, view, srcPar](double t, double /*amount*/, bool invert, TransformMath::Mat3* matrix) {
        return getInverseTransformCanonical(t, view, invert, srcPar, matrix);
    };
    Resampler::getRegionOfDefinition(fn, srcRoD, time, getAspectRatio(-1), s.x, s.y, params, rod);

    KnobBoolPtr preserveBB = _preserveBB.lock();
    if (!preserveBB || !preserveBB->getValue()) {
        double par = 1.;
        RectD rect;
        RectI format;
        computeOutputFormat(time, view, srcPar, &par, &rect, &format);
        *rod = rectIntersection(*rod, toCanonical(format, par));
    }

    return eStatusOK;
}

void
Reformat::getRegionsOfInterest(double time,
                               const RenderScale& scale,
                               const RectD& /*outputRoD*/,
                               const RectD& renderWindow,
                               ViewIdx view,
                               RoIMap* ret)
{
    EffectInstancePtr input = getInput(0);

    if (!input) {
        return;
    }
    RectD srcRoD;
    getSourceRegionOfDefinition(time, view, scale, &srcRoD);

    const double srcPar = getAspectRatio(0);
    const OfxPointD s = scale.toOfxPointD();
    KnobChoicePtr filter = _filter.lock();
    KnobBoolPtr blackOutside = _blackOutside.lock();
    Resampler::RegionParams params;
    params.invert = false;
    params.filter = filter ? (Resampler::FilterEnum)filter->getValueAtTime(time, 0, view) : Resampler::eFilterCubic;
    params.blackOutside = blackOutside && blackOutside->getValueAtTime(time, 0, view);
    params.isIdentity = isIdentityParams(time, view);
    const Resampler::CanonicalTransformFn fn = [this, view, srcPar](double t, double /*amount*/, bool invert, TransformMath::Mat3* matrix) {
        return getInverseTransformCanonical(t, view, invert, srcPar, matrix);
    };
    RectD srcRoI;
    Resampler::getRegionOfInterest(fn, renderWindow, srcRoD, getProjectRect(NULL), time, srcPar, s.x, s.y, params, &srcRoI);

    ret->insert(std::make_pair(input, srcRoI));
}

StatusEnum
Reformat::getTransform(double time,
                       const RenderScale& renderScale,
                       bool /*draftRender*/,
                       ViewIdx view,
                       EffectInstancePtr* inputToTransform,
                       Transform::Matrix3x3* transform)
{
    EffectInstancePtr input = getInput(0);

    if (!input) {
        return eStatusReplyDefault;
    }
    const double srcPar = getAspectRatio(0);
    TransformMath::Mat3 invCanonical;
    if (!getInverseTransformCanonical(time, view, false, srcPar, &invCanonical)) {
        return eStatusReplyDefault;
    }
    TransformMath::Mat3 canonical;
    if (!invCanonical.inverse(&canonical)) {
        return eStatusReplyDefault;
    }
    const OfxPointD s = renderScale.toOfxPointD();
    const TransformMath::Mat3 pixel = TransformMath::forwardToPixel(canonical, srcPar, getAspectRatio(-1), s.x, s.y);

    *inputToTransform = input;
    *transform = TransformMath::toEngineMatrix(pixel);

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

    computeOutputFormat(0., ViewIdx(0), srcPar, &par, &rect, &format);
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

StatusEnum
Reformat::render(const RenderActionArgs& args)
{
    KnobChoicePtr filterKnob = _filter.lock();
    KnobBoolPtr clampKnob = _clamp.lock();
    KnobBoolPtr blackOutsideKnob = _blackOutside.lock();
    if (!filterKnob || !clampKnob || !blackOutsideKnob) {
        return eStatusFailed;
    }
    const Resampler::FilterEnum filter = args.draftMode ? Resampler::eFilterImpulse : (Resampler::FilterEnum)filterKnob->getValueAtTime(args.time, 0, args.view);
    const bool clamp = clampKnob->getValueAtTime(args.time, 0, args.view);
    const bool blackOutside = blackOutsideKnob->getValueAtTime(args.time, 0, args.view);
    const double srcPar = getAspectRatio(0);
    const double dstPar = getAspectRatio(-1);
    const OfxPointD s = args.mappedScale.toOfxPointD();
    const ViewIdx view = args.view;
    const Resampler::CanonicalTransformFn fn = [this, view, srcPar](double t, double /*amount*/, bool invert, TransformMath::Mat3* matrix) {
        return getInverseTransformCanonical(t, view, invert, srcPar, matrix);
    };

    // Every source image is fetched before any is locked: fetching renders upstream, which may
    // write into a cached image this render would otherwise already hold a read lock on.
    std::vector<ReformatPlaneJob> jobs;
    std::vector<RectI> sourceWindows;
    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        ReformatPlaneJob job;
        RectI sourceWindow;
        job.dst = it->second;
        if (job.dst) {
            if (job.dst->getBitDepth() != eImageBitDepthFloat) {
                return eStatusFailed;
            }
            ImageLayerDesc sourceLayer;
            if (getInput(0) && resolveInputPlaneForRender(0, args.time, args.view, &sourceLayer, NULL)) {
                // Mapped to the clip's components, which is the layout the output plane is rendered in.
                job.src = getImage(0, args.time, args.mappedScale, args.view, NULL, &sourceLayer, true /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &sourceWindow, &job.inputTransform);
                if (job.src && (job.src->getBitDepth() != eImageBitDepthFloat)) {
                    return eStatusFailed;
                }
            }
        }
        jobs.push_back(job);
        sourceWindows.push_back(sourceWindow);
    }

    const RectI& roi = args.roi;
    const int width = roi.width();
    if ((width <= 0) || (roi.height() <= 0)) {
        return eStatusOK;
    }

    // Images are locked here, on the calling thread, for the whole render; the band threads only
    // read through the pointers and bounds captured below.
    const int nThreads = appPTR->getNCPUsAvailableForEffect();
    std::vector<RectI> bandRects;
    makeRowBands(roi, nThreads, &bandRects);
    std::vector<ReformatBand> bands;
    for (std::size_t j = 0; j < jobs.size(); ++j) {
        ReformatPlaneJob& job = jobs[j];
        if (!job.dst) {
            continue;
        }
        job.dstNComps = (int)job.dst->getComponentsCount();
        job.dstAccess = std::make_shared<Image::WriteAccess>(job.dst.get());
        if (job.src) {
            const RectI srcBounds = job.src->getBounds();
            const int srcNComps = (int)job.src->getComponentsCount();
            job.srcAccess = std::make_shared<Image::ReadAccess>(job.src.get());
            // An OpenFX plug-in sees the source only over the window it asked for, and the edge
            // clamping of the filters (black_outside off) happens at that window's edge.
            RectI seen = srcBounds;
            if (!sourceWindows[j].isNull()) {
                seen = sourceWindows[j].intersect(srcBounds);
            }
            if (!seen.isNull() && (srcNComps >= 1) && (srcNComps <= 4)) {
                job.source.data = (const float*)job.srcAccess->pixelAt(seen.x1, seen.y1);
                job.source.bounds = seen;
                job.source.nComps = srcNComps;
                job.source.rowStride = (std::size_t)srcBounds.width() * (std::size_t)srcNComps;
            }
            for (int c = 0; (c < job.dstNComps) && (c < 4); ++c) {
                job.srcIndex[c] = channelIndexForBit(srcNComps, pixelKernelChannelBit(job.dstNComps, c));
            }
            Resampler::buildSamplingTransforms(fn, args.time, false, Resampler::BlurSettings(), s.x, s.y, false, srcPar, dstPar, &job.transforms);
            if (job.inputTransform) {
                Resampler::concatenateInputTransform(TransformMath::fromEngineMatrix(*job.inputTransform), &job.transforms);
            }
            job.params = Resampler::makeResampleParams(job.transforms, filter, clamp, blackOutside);
        }
        for (std::size_t b = 0; b < bandRects.size(); ++b) {
            bands.push_back(ReformatBand(j, bandRects[b].y1, bandRects[b].y2));
        }
    }

    RenderCancellation cancel(this);

    const std::function<void(int)> renderBand = [&](int bandIndex) {
        const ReformatBand& band = bands[bandIndex];
        const ReformatPlaneJob& job = jobs[band.job];
        const int dstNComps = job.dstNComps;
        const bool hasSource = job.source.isValid();
        const int srcNComps = hasSource ? job.source.nComps : 0;
        std::vector<float> row(hasSource ? (std::size_t)width * srcNComps : 0);

        for (int y = band.y1; y < band.y2; ++y) {
            if ((((y - band.y1) % kAbortCheckRows) == 0) && cancel.check()) {
                return;
            }

            float* dstPix = (float*)job.dstAccess->pixelAt(roi.x1, y);
            if (!dstPix) {
                continue;
            }
            if (!hasSource) {
                std::fill(dstPix, dstPix + (std::size_t)width * dstNComps, 0.f);
                continue;
            }
            Resampler::resampleRow(job.params, job.source, y, roi.x1, roi.x2, &row[0]);
            if (srcNComps == dstNComps) {
                std::copy(row.begin(), row.end(), dstPix);
                continue;
            }
            const float* srcPix = &row[0];
            for (int i = 0; i < width; ++i, srcPix += srcNComps, dstPix += dstNComps) {
                for (int c = 0; c < dstNComps; ++c) {
                    const int index = (c < 4) ? job.srcIndex[c] : -1;
                    dstPix[c] = (index >= 0) ? srcPix[index] : 0.f;
                }
            }
        }
    };
    parallelForCancellable((int)bands.size(), nThreads, cancel, renderBand);

    return eStatusOK;
} // Reformat::render

NATRON_NAMESPACE_EXIT
