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

#include "NativeGenerator.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <QThread>

#include <ofxNatron.h>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Format.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/PoolParallelFor.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_ENTER

namespace {
// How many rows render() runs between two abort checks.
const int kAbortCheckRows = 16;

struct FormatResolution {
    const char* id;
    const char* label;
    int width;
    int height;
    double par;
};

// The openfx-misc format list (SupportExt/ofxsFormatResolution.h), in its order. The host
// replaces these entries with the project's formats once the node is created.
const FormatResolution kFormats[] = {
    { "PC_Video", "PC_Video 640x480", 640, 480, 1. },
    { "NTSC", "NTSC 720x486 0.91", 720, 486, 0.91 },
    { "PAL", "PAL 720x576 1.09", 720, 576, 1.09 },
    { "NTSC_16:9", "NTSC_16:9 720x486 1.21", 720, 486, 1.21 },
    { "PAL_16:9", "PAL_16:9 720x576 1.46", 720, 576, 1.46 },
    { "HD_720", "HD_720 1280x1720", 1280, 720, 1. },
    { "HD", "HD 1920x1080", 1920, 1080, 1. },
    { "UHD_4K", "UHD_4K 3840x2160", 3840, 2160, 1. },
    { "1K_Super35(full-ap)", "1K_Super35(full-ap) 1024x778", 1024, 778, 1. },
    { "1K_Cinemascope", "1K_Cinemascope 914x778 2", 914, 778, 2. },
    { "2K_Super35(full-ap)", "2K_Super35(full-ap) 2048x1556", 2048, 1556, 1. },
    { "2K_Cinemascope", "2K_Cinemascope 1828x1556 2", 1828, 1556, 2. },
    { "2K_DCP", "2K_DCP 2048x1080", 2048, 1080, 1. },
    { "4K_Super35(full-ap)", "4K_Super35(full-ap) 4096x3112", 4096, 3112, 1. },
    { "4K_Cinemascope", "4K_Cinemascope 3656x3112 2", 3656, 3112, 2. },
    { "4K_DCP", "4K_DCP 4096x2160", 4096, 2160, 1. },
    { "square_256", "square_256 256x256", 256, 256, 1. },
    { "square_512", "square_512 512x512", 512, 512, 1. },
    { "square_1K", "square_1K 1024x1024", 1024, 1024, 1. },
    { "square_2K", "square_2K 2048x2048", 2048, 2048, 1. },
};

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

// The bits written in a dstNComps-channel image holding `plane`. A one-channel plane rendered
// into a wider image sits where EffectInstance's copy back reads it: alpha for a colour plane,
// channel 0 otherwise.
std::bitset<4>
processedBitsForImage(const ImageLayerDesc& plane,
                      int dstNComps,
                      const std::bitset<4>& planeBits)
{
    if ((plane.getNumComponents() != 1) || (dstNComps == 1)) {
        return planeBits;
    }
    std::bitset<4> bits;
    bits[plane.isColorLayer() ? 3 : 0] = planeBits[3];

    return bits;
}

bool
writesEveryChannel(int nComps,
                   const std::bitset<4>& channels)
{
    for (int c = 0; (c < nComps) && (c < 4); ++c) {
        if (!channels[pixelKernelChannelBit(nComps, c)]) {
            return false;
        }
    }

    return true;
}

// Copies `width` pixels of row y starting at x0 into row (nComps floats per pixel), writing zero
// wherever the image has no pixel. An image in another layout is mapped channel by colour bit,
// a channel it lacks reading as zero.
void
readSourceRow(const Image* image,
              const Image::ReadAccess* access,
              const RectI& bounds,
              int x0,
              int y,
              int width,
              int nComps,
              float* row)
{
    std::fill(row, row + (std::size_t)width * nComps, 0.f);
    if (!image) {
        return;
    }
    if ((y < bounds.y1) || (y >= bounds.y2)) {
        return;
    }
    const int xStart = std::max(x0, bounds.x1);
    const int xEnd = std::min(x0 + width, bounds.x2);
    if (xStart >= xEnd) {
        return;
    }
    const int srcNComps = (int)image->getComponentsCount();
    const float* srcPix = (const float*)access->pixelAt(xStart, y);
    float* dstPix = row + (std::size_t)(xStart - x0) * nComps;
    if (srcNComps == nComps) {
        std::copy(srcPix, srcPix + (std::size_t)(xEnd - xStart) * nComps, dstPix);

        return;
    }
    int srcIndex[4] = { -1, -1, -1, -1 };
    for (int c = 0; (c < nComps) && (c < 4); ++c) {
        srcIndex[c] = channelIndexForBit(srcNComps, pixelKernelChannelBit(nComps, c));
    }
    for (int x = xStart; x < xEnd; ++x, srcPix += srcNComps, dstPix += nComps) {
        for (int c = 0; (c < nComps) && (c < 4); ++c) {
            if (srcIndex[c] >= 0) {
                dstPix[c] = srcPix[srcIndex[c]];
            }
        }
    }
}

struct GeneratorPlaneJob {
    ImagePtr dst;
    std::bitset<4> channels;
    ImagePtr src;
    bool passThrough;

    GeneratorPlaneJob()
        : dst()
        , channels()
        , src()
        , passThrough(false)
    {
    }
};

struct GeneratorBand {
    std::size_t job;
    int y1;
    int y2;

    GeneratorBand(std::size_t jobIndex,
                  int firstRow,
                  int endRow)
        : job(jobIndex)
        , y1(firstRow)
        , y2(endRow)
    {
    }
};

bool
isAnimatedKnob(const KnobIPtr& knob)
{
    if (!knob) {
        return false;
    }
    for (int d = 0; d < knob->getDimension(); ++d) {
        if (knob->isAnimated(d)) {
            return true;
        }
    }

    return false;
}

NativeImageTraits
generatorTraits()
{
    NativeImageTraits traits;

    traits.generator = true;

    return traits;
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

NativeGenerator::NativeGenerator(NodePtr node)
    : NativeImageEffect(node, generatorTraits())
{
}

NativeGenerator::~NativeGenerator()
{
}

void
NativeGenerator::describeSourceInput(NativePluginDescription* desc)
{
    desc->inputs.push_back(NativeInputDescription(kNativeGeneratorSourceInputLabel, true, eDataKindImage));
}

void
NativeGenerator::initializeGeneratorKnobs(const KnobPagePtr& page)
{
    KnobChoicePtr extent = createKnob<KnobChoice>(tr("Extent"));
    extent->setName(kNativeGeneratorParamExtent);
    extent->setHintToolTip(tr("Extent (size and offset) of the output."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kNativeGeneratorExtentFormat, tr("Format").toStdString(), tr("Use a pre-defined image format.").toStdString()));
        options.push_back(ChoiceOption(kNativeGeneratorExtentSize, tr("Size").toStdString(), tr("Use a specific extent (size and offset).").toStdString()));
        options.push_back(ChoiceOption(kNativeGeneratorExtentProject, tr("Project").toStdString(), tr("Use the project extent (size and offset).").toStdString()));
        options.push_back(ChoiceOption(kNativeGeneratorExtentDefault, tr("Default").toStdString(), tr("Use the default extent (e.g. the source clip extent, if connected).").toStdString()));
        extent->populateChoices(options);
    }
    extent->setDefaultValue((int)eExtentDefault);
    extent->setAddNewLine(false);
    extent->setAnimationEnabled(false);
    extent->setIsMetadataSlave(true);
    page->addKnob(extent);
    _extent = extent;

    KnobButtonPtr recenter = createKnob<KnobButton>(tr("Center"));
    recenter->setName(kNativeGeneratorParamRecenter);
    recenter->setHintToolTip(tr("Centers the region of definition to the input region of definition. "
                                "If there is no input, then the region of definition is centered to the project window."));
    recenter->setAddNewLine(false);
    page->addKnob(recenter);
    _recenter = recenter;

    KnobBoolPtr reformat = createKnob<KnobBool>(tr("Reformat"));
    reformat->setName(kNativeGeneratorParamReformat);
    reformat->setHintToolTip(tr("Set the output format to the given extent, except if the Bottom Left or Size parameters is animated."));
    reformat->setDefaultValue(false);
    reformat->setAddNewLine(false);
    reformat->setAnimationEnabled(false);
    reformat->setIsMetadataSlave(true);
    page->addKnob(reformat);
    _reformat = reformat;

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
    formatSize->setDefaultValue(kFormats[0].width, 0);
    formatSize->setDefaultValue(kFormats[0].height, 1);
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
    formatPar->setMaximum(DBL_MAX);
    formatPar->setDisplayMinimum(0.5);
    formatPar->setDisplayMaximum(2.);
    formatPar->setDefaultValue(kFormats[0].par);
    formatPar->setAnimationEnabled(false);
    formatPar->setIsMetadataSlave(true);
    formatPar->setSecretByDefault(true);
    formatPar->setDefaultAllDimensionsEnabled(false);
    page->addKnob(formatPar);
    _formatPar = formatPar;

    KnobDoublePtr bottomLeft = createKnob<KnobDouble>(tr("Bottom Left"), 2);
    bottomLeft->setName(kNativeGeneratorParamBottomLeft);
    bottomLeft->setHintToolTip(tr("Coordinates of the bottom left corner of the size rectangle."));
    bottomLeft->setSpatial(true);
    bottomLeft->disableSlider();
    bottomLeft->setDefaultValuesAreNormalized(true);
    for (int d = 0; d < 2; ++d) {
        bottomLeft->setMinimum(-DBL_MAX, d);
        bottomLeft->setMaximum(DBL_MAX, d);
        bottomLeft->setDisplayMinimum(-10000., d);
        bottomLeft->setDisplayMaximum(10000., d);
        bottomLeft->setIncrement(1., d);
        bottomLeft->setDecimals(0, d);
        bottomLeft->setDefaultValue(0., d);
    }
    bottomLeft->setAddNewLine(false);
    bottomLeft->setIsMetadataSlave(true);
    page->addKnob(bottomLeft);
    _bottomLeft = bottomLeft;

    KnobDoublePtr size = createKnob<KnobDouble>(tr("Size"), 2);
    size->setName(kNativeGeneratorParamSize);
    size->setHintToolTip(tr("Width and height of the size rectangle."));
    size->setSpatial(true);
    size->setCanAutoFoldDimensions(true);
    size->setDefaultValuesAreNormalized(true);
    size->setDimensionName(0, "w");
    size->setDimensionName(1, "h");
    for (int d = 0; d < 2; ++d) {
        size->setMinimum(0., d);
        size->setMaximum(DBL_MAX, d);
        size->setDisplayMinimum(0., d);
        size->setDisplayMaximum(10000., d);
        size->setIncrement(1., d);
        size->setDecimals(0, d);
        size->setDefaultValue(1., d);
    }
    size->setIsMetadataSlave(true);
    page->addKnob(size);
    _size = size;

    KnobBoolPtr interactive = createKnob<KnobBool>(tr("Interactive Update"));
    interactive->setName(kNativeGeneratorParamInteractive);
    interactive->setHintToolTip(tr("If checked, update the parameter values during interaction with the image viewer, else update the values when pen is released."));
    interactive->setDefaultValue(false);
    interactive->setEvaluateOnChange(false);
    page->addKnob(interactive);
    _interactive = interactive;

    // Kept for knob parity with the OpenFX generators; the host overlay sizes itself.
    KnobBoolPtr hiDPI = createKnob<KnobBool>(tr("HiDPI"));
    hiDPI->setName(kNativeGeneratorParamHiDPI);
    hiDPI->setHintToolTip(tr("Should be checked when the display area is High-DPI (a.k.a Retina). Draws OpenGL overlays twice larger."));
    hiDPI->setDefaultValue(false);
    hiDPI->setAnimationEnabled(false);
    hiDPI->setEvaluateOnChange(false);
    page->addKnob(hiDPI);
    _hiDPI = hiDPI;

    KnobIntPtr frameRange = createKnob<KnobInt>(tr("Frame Range"), 2);
    frameRange->setName(kNativeGeneratorParamFrameRange);
    frameRange->setHintToolTip(tr("Time domain."));
    frameRange->setDimensionName(0, "min");
    frameRange->setDimensionName(1, "max");
    frameRange->setDefaultValue(1, 0);
    frameRange->setDefaultValue(1, 1);
    frameRange->setAnimationEnabled(false);
    page->addKnob(frameRange);
    _frameRange = frameRange;

    // The rectangle overlay has no per-handle hooks, so this is what hides it outside Size extent.
    KnobBoolPtr rectangleEnable = createKnob<KnobBool>(std::string("Rectangle Interact Enable"));
    rectangleEnable->setName(kNativeGeneratorParamRectangleEnable);
    rectangleEnable->setDefaultValue(false);
    rectangleEnable->setAnimationEnabled(false);
    rectangleEnable->setEvaluateOnChange(false);
    rectangleEnable->setIsPersistent(false);
    rectangleEnable->setSecretByDefault(true);
    page->addKnob(rectangleEnable);
    _rectangleEnable = rectangleEnable;

    updateExtentKnobsVisibility();

    NodePtr node = getNode();
    if (node) {
        node->addRectangleInteract(bottomLeft, size, interactive, rectangleEnable);
    }
} // NativeGenerator::initializeGeneratorKnobs

NativeGenerator::ExtentEnum
NativeGenerator::getExtent() const
{
    KnobChoicePtr extent = _extent.lock();

    return extent ? (ExtentEnum)extent->getValue() : eExtentDefault;
}

void
NativeGenerator::updateExtentKnobsVisibility()
{
    const ExtentEnum extent = getExtent();
    const bool hasFormat = (extent == eExtentFormat);
    const bool hasSize = (extent == eExtentSize);

    setSecretAndDisabled(_format.lock(), !hasFormat);
    setSecretAndDisabled(_reformat.lock(), !hasSize);
    setSecretAndDisabled(_size.lock(), !hasSize);
    setSecretAndDisabled(_recenter.lock(), !hasSize);
    setSecretAndDisabled(_bottomLeft.lock(), !hasSize);
    setSecretAndDisabled(_interactive.lock(), !hasSize);
    setSecretAndDisabled(_hiDPI.lock(), !hasSize);

    KnobBoolPtr rectangleEnable = _rectangleEnable.lock();
    if (rectangleEnable && (rectangleEnable->getValue() != hasSize)) {
        rectangleEnable->setValue(hasSize);
    }
}

RectD
NativeGenerator::getProjectExtentRect() const
{
    Format format;

    getApp()->getProject()->getProjectDefaultFormat(&format);

    return format.toCanonicalFormat();
}

void
NativeGenerator::recenter()
{
    KnobDoublePtr size = _size.lock();
    KnobDoublePtr bottomLeft = _bottomLeft.lock();

    if (!size || !bottomLeft) {
        return;
    }

    // The openfx-misc generators never pass a source clip here, so the project window is the
    // reference even with Source connected.
    const RectD project = getProjectExtentRect();
    const double centerX = (project.x2 + project.x1) / 2.;
    const double centerY = (project.y2 + project.y1) / 2.;
    const double width = size->getValue(0);
    const double height = size->getValue(1);
    const double x1 = centerX - width / 2.;
    const double y1 = centerY - height / 2.;

    beginChanges();
    size->setValue((x1 + width) - x1, ViewSpec::all(), 0);
    size->setValue((y1 + height) - y1, ViewSpec::all(), 1);
    bottomLeft->setValue(x1, ViewSpec::all(), 0);
    bottomLeft->setValue(y1, ViewSpec::all(), 1);
    endChanges();
}

bool
NativeGenerator::knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec /*view*/,
                             double /*time*/,
                             bool /*originatedFromMainThread*/)
{
    KnobChoicePtr extent = _extent.lock();
    if (extent && (k == extent.get())) {
        if (reason != eValueChangedReasonTimeChanged) {
            updateExtentKnobsVisibility();
        }

        return true;
    }
    KnobButtonPtr recenterButton = _recenter.lock();
    if (recenterButton && (k == recenterButton.get())) {
        recenter();

        return true;
    }

    return false;
}

void
NativeGenerator::onKnobsLoaded()
{
    updateExtentKnobsVisibility();
}

bool
NativeGenerator::getExtentRegionOfDefinition(double time,
                                             ViewIdx view,
                                             RectD* rod) const
{
    switch (getExtent()) {
    case eExtentFormat: {
        KnobIntPtr formatSize = _formatSize.lock();
        KnobDoublePtr formatPar = _formatPar.lock();
        if (!formatSize || !formatPar) {
            return false;
        }
        const int w = formatSize->getValueAtTime(time, 0, view);
        const int h = formatSize->getValueAtTime(time, 1, view);
        const double par = formatPar->getValueAtTime(time, 0, view);
        if ((w <= 0) || (h <= 0)) {
            rod->x1 = rod->y1 = rod->x2 = rod->y2 = 0.;
        } else {
            rod->x1 = 0.;
            rod->y1 = 0.;
            rod->x2 = w * par;
            rod->y2 = h;
        }

        return true;
    }
    case eExtentSize: {
        KnobDoublePtr size = _size.lock();
        KnobDoublePtr bottomLeft = _bottomLeft.lock();
        if (!size || !bottomLeft) {
            return false;
        }
        rod->x1 = bottomLeft->getValueAtTime(time, 0, view);
        rod->y1 = bottomLeft->getValueAtTime(time, 1, view);
        rod->x2 = size->getValueAtTime(time, 0, view) + rod->x1;
        rod->y2 = size->getValueAtTime(time, 1, view) + rod->y1;

        return true;
    }
    case eExtentProject:
        *rod = getProjectExtentRect();

        return true;
    case eExtentDefault:

        return false;
    }

    return false;
}

StatusEnum
NativeGenerator::getRegionOfDefinition(U64 /*hash*/,
                                       double time,
                                       const RenderScale& scale,
                                       ViewIdx view,
                                       RectD* rod)
{
    if (getExtentRegionOfDefinition(time, view, rod)) {
        return eStatusOK;
    }

    // The host's default for an OpenFX general-context effect: the Source clip when it is
    // connected, else the project extent from the origin.
    EffectInstancePtr source = getInput(0);
    if (source) {
        const RenderScale sourceScale = source->supportsRenderScale() ? scale : RenderScale::identity;
        bool isProjectFormat = false;

        return source->getRegionOfDefinition_public(source->getRenderHash(), time, sourceScale, view, rod, &isProjectFormat);
    }
    const RectD project = getProjectExtentRect();
    rod->x1 = 0.;
    rod->y1 = 0.;
    rod->x2 = project.x2;
    rod->y2 = project.y2;

    return eStatusOK;
}

void
NativeGenerator::getFrameRange(double* first,
                               double* last)
{
    KnobIntPtr frameRange = _frameRange.lock();

    *first = frameRange ? frameRange->getValue(0) : 1.;
    *last = frameRange ? frameRange->getValue(1) : 1.;
}

StatusEnum
NativeGenerator::getPreferredMetadata(NodeMetadata& metadata)
{
    metadata.setIsContinuous(true);
    metadata.setNComps(-1, 4);
    metadata.setComponentsType(-1, kNatronColorLayerID);

    double par = 0.;
    switch (getExtent()) {
    case eExtentFormat: {
        KnobDoublePtr formatPar = _formatPar.lock();
        par = formatPar ? formatPar->getValue() : 1.;
        break;
    }
    case eExtentProject:
    case eExtentDefault: {
        Format format;
        getApp()->getProject()->getProjectDefaultFormat(&format);
        par = format.getPixelAspectRatio();
        break;
    }
    case eExtentSize: {
        KnobBoolPtr reformat = _reformat.lock();
        if (reformat && reformat->getValue() && !isAnimatedKnob(_bottomLeft.lock()) && !isAnimatedKnob(_size.lock())) {
            par = 1.;
        }
        break;
    }
    }

    if (par != 0.) {
        metadata.setPixelAspectRatio(-1, par);
        RectD rod;
        if (getExtentRegionOfDefinition(0., ViewIdx(0), &rod)) {
            RectI format(0, 0, 0, 0);
            if ((rod.x2 > rod.x1) && (rod.y2 > rod.y1)) {
                format.x1 = (int)std::floor(rod.x1 / par + 0.5);
                format.y1 = (int)std::floor(rod.y1 + 0.5);
                format.x2 = (int)std::ceil(rod.x2 / par - 0.5);
                format.y2 = (int)std::ceil(rod.y2 - 0.5);
            }
            metadata.setOutputFormat(format);
        }
    }

    return eStatusOK;
} // NativeGenerator::getPreferredMetadata

StatusEnum
NativeGenerator::render(const RenderActionArgs& args)
{
    KernelContext context;
    context.time = args.time;
    context.view = args.view;
    context.mappedScale = args.mappedScale;
    context.processChannels = args.processChannels;
    const PixelKernelPtr kernel = makeKernel(context);
    if (!kernel) {
        return eStatusFailed;
    }

    // Every source image is fetched before any is locked: fetching renders upstream, which may
    // write into a cached image this render would otherwise already hold a read lock on.
    std::vector<GeneratorPlaneJob> jobs;
    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        GeneratorPlaneJob job;
        job.dst = it->second;
        if (!job.dst) {
            jobs.push_back(job);
            continue;
        }
        if (job.dst->getBitDepth() != eImageBitDepthFloat) {
            return eStatusFailed;
        }
        const int dstNComps = (int)job.dst->getComponentsCount();
        job.channels = processedBitsForImage(it->first, dstNComps, args.processChannels);
        job.passThrough = !writesEveryChannel(dstNComps, job.channels);

        ImageLayerDesc sourceLayer;
        if (job.passThrough && getInput(0) && resolveInputPlaneForRender(0, args.time, args.view, &sourceLayer, NULL)) {
            RectI sourceRoI;
            // Mapped to the clip's components, which is the layout the output plane is rendered in.
            job.src = getImage(0, args.time, args.mappedScale, args.view, NULL, &sourceLayer, true /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &sourceRoI);
            if (job.src && (job.src->getBitDepth() != eImageBitDepthFloat)) {
                return eStatusFailed;
            }
        }
        jobs.push_back(job);
    }

    const RectI& roi = args.roi;
    const int width = roi.width();
    if ((width <= 0) || (roi.height() <= 0)) {
        return eStatusOK;
    }

    // Images are locked here, on the calling thread, for the whole render; the band threads only
    // compute pixel addresses through these accesses. Their bounds are read here too, because
    // Image::getBounds() takes the lock, which a band thread must not ask for: behind a writer
    // waiting on an image this render holds for reading, it blocks forever.
    std::vector<std::shared_ptr<Image::ReadAccess>> srcAccesses(jobs.size());
    std::vector<RectI> srcBounds(jobs.size());
    std::vector<std::shared_ptr<Image::WriteAccess>> dstAccesses(jobs.size());
    std::vector<GeneratorBand> bands;
    const int nThreads = appPTR->getNCPUsAvailableForEffect();
    std::vector<RectI> bandRects;
    makeRowBands(roi, nThreads, &bandRects);
    for (std::size_t j = 0; j < jobs.size(); ++j) {
        if (!jobs[j].dst) {
            continue;
        }
        if (jobs[j].src) {
            srcBounds[j] = jobs[j].src->getBounds();
            srcAccesses[j] = std::make_shared<Image::ReadAccess>(jobs[j].src.get());
        }
        dstAccesses[j] = std::make_shared<Image::WriteAccess>(jobs[j].dst.get());
        for (std::size_t b = 0; b < bandRects.size(); ++b) {
            bands.push_back(GeneratorBand(j, bandRects[b].y1, bandRects[b].y2));
        }
    }

    QThread* const callingThread = QThread::currentThread();
    std::atomic<bool> wasAborted(false);

    const std::function<void(int)> renderBand = [&](int bandIndex) {
        const GeneratorBand& band = bands[bandIndex];
        const GeneratorPlaneJob& job = jobs[band.job];
        const int nComps = (int)job.dst->getComponentsCount();

        RowIO io;
        io.nSrc = 0;
        io.x0 = roi.x1;
        io.width = width;
        io.nComps = nComps;
        io.channels = job.channels;

        for (int y = band.y1; y < band.y2; ++y) {
            if (((y - band.y1) % kAbortCheckRows) == 0) {
                if (wasAborted.load(std::memory_order_relaxed)) {
                    return;
                }
                // Only the calling thread carries the render's TLS, so only it may ask.
                if ((QThread::currentThread() == callingThread) && aborted()) {
                    wasAborted = true;

                    return;
                }
            }

            float* dstPix = (float*)dstAccesses[band.job]->pixelAt(roi.x1, y);
            if (!dstPix) {
                continue;
            }
            if (job.passThrough) {
                readSourceRow(job.src.get(), srcAccesses[band.job].get(), srcBounds[band.job], roi.x1, y, width, nComps, dstPix);
            }
            io.y = y;
            io.dst = dstPix;
            kernel->processRow(io);
        }
    };
    parallelForOnGlobalPool((int)bands.size(), nThreads, renderBand);

    return eStatusOK;
} // NativeGenerator::render

NATRON_NAMESPACE_EXIT
