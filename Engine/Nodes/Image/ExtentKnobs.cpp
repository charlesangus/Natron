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

#include "ExtentKnobs.h"

#include <cfloat>
#include <cstddef>
#include <string>
#include <vector>

#include <ofxNatron.h>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Project.h"

NATRON_NAMESPACE_ENTER

namespace {
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

ExtentKnobs::ExtentKnobs(EffectInstance* effect,
                         const ExtentPolicy& policy)
    : _effect(effect)
    , _policy(policy)
{
}

void
ExtentKnobs::createRectangleEnable(const KnobPagePtr& page)
{
    // The rectangle overlay has no per-handle hooks, so this is what hides it.
    KnobBoolPtr rectangleEnable = AppManager::createKnob<KnobBool>(_effect, std::string("Rectangle Interact Enable"));
    rectangleEnable->setName(kNativeGeneratorParamRectangleEnable);
    rectangleEnable->setDefaultValue(!_policy.overlayFollowsExtent);
    rectangleEnable->setAnimationEnabled(false);
    rectangleEnable->setEvaluateOnChange(false);
    rectangleEnable->setIsPersistent(false);
    rectangleEnable->setSecretByDefault(true);
    page->addKnob(rectangleEnable);
    _rectangleEnable = rectangleEnable;
}

void
ExtentKnobs::createKnobs(const KnobPagePtr& page)
{
    if (!_policy.overlayFollowsExtent) {
        createRectangleEnable(page);
    }

    KnobChoicePtr extent = AppManager::createKnob<KnobChoice>(_effect, EffectInstance::tr("Extent"));
    extent->setName(kNativeGeneratorParamExtent);
    extent->setHintToolTip(EffectInstance::tr("Extent (size and offset) of the output."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kNativeGeneratorExtentFormat, EffectInstance::tr("Format").toStdString(), EffectInstance::tr("Use a pre-defined image format.").toStdString()));
        options.push_back(ChoiceOption(kNativeGeneratorExtentSize, EffectInstance::tr("Size").toStdString(), EffectInstance::tr("Use a specific extent (size and offset).").toStdString()));
        options.push_back(ChoiceOption(kNativeGeneratorExtentProject, EffectInstance::tr("Project").toStdString(), EffectInstance::tr("Use the project extent (size and offset).").toStdString()));
        options.push_back(ChoiceOption(kNativeGeneratorExtentDefault, EffectInstance::tr("Default").toStdString(), EffectInstance::tr("Use the default extent (e.g. the source clip extent, if connected).").toStdString()));
        extent->populateChoices(options);
    }
    extent->setDefaultValue(_policy.defaultExtent);
    extent->setAddNewLine(false);
    extent->setAnimationEnabled(false);
    extent->setIsMetadataSlave(true);
    page->addKnob(extent);
    _extent = extent;

    KnobButtonPtr recenter = AppManager::createKnob<KnobButton>(_effect, EffectInstance::tr("Center"));
    recenter->setName(kNativeGeneratorParamRecenter);
    recenter->setHintToolTip(EffectInstance::tr("Centers the region of definition to the input region of definition. "
                                                "If there is no input, then the region of definition is centered to the project window."));
    recenter->setAddNewLine(false);
    page->addKnob(recenter);
    _recenter = recenter;

    if (_policy.reformatToggle) {
        KnobBoolPtr reformat = AppManager::createKnob<KnobBool>(_effect, EffectInstance::tr("Reformat"));
        reformat->setName(kNativeGeneratorParamReformat);
        reformat->setHintToolTip(EffectInstance::tr("Set the output format to the given extent, except if the Bottom Left or Size parameters is animated."));
        reformat->setDefaultValue(false);
        reformat->setAddNewLine(false);
        reformat->setAnimationEnabled(false);
        reformat->setIsMetadataSlave(true);
        page->addKnob(reformat);
        _reformat = reformat;
    }

    KnobChoicePtr format = AppManager::createKnob<KnobChoice>(_effect, EffectInstance::tr("Format"));
    format->setName(kNatronParamFormatChoice);
    format->setHintToolTip(EffectInstance::tr("The output format"));
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

    KnobIntPtr formatSize = AppManager::createKnob<KnobInt>(_effect, EffectInstance::tr("Size"), 2);
    formatSize->setName(kNatronParamFormatSize);
    formatSize->setHintToolTip(EffectInstance::tr("The output dimensions of the image in pixels."));
    formatSize->setDefaultValue(kFormats[0].width, 0);
    formatSize->setDefaultValue(kFormats[0].height, 1);
    formatSize->setAnimationEnabled(false);
    formatSize->setIsMetadataSlave(true);
    formatSize->setSecretByDefault(true);
    formatSize->setDefaultAllDimensionsEnabled(false);
    page->addKnob(formatSize);
    _formatSize = formatSize;

    KnobDoublePtr formatPar = AppManager::createKnob<KnobDouble>(_effect, EffectInstance::tr("Pixel Aspect Ratio"));
    formatPar->setName(kNatronParamFormatPar);
    formatPar->setHintToolTip(EffectInstance::tr("Output pixel aspect ratio."));
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

    KnobDoublePtr bottomLeft = AppManager::createKnob<KnobDouble>(_effect, EffectInstance::tr("Bottom Left"), 2);
    bottomLeft->setName(kNativeGeneratorParamBottomLeft);
    bottomLeft->setHintToolTip(EffectInstance::tr("Coordinates of the bottom left corner of the size rectangle."));
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

    KnobDoublePtr size = AppManager::createKnob<KnobDouble>(_effect, EffectInstance::tr("Size"), 2);
    size->setName(kNativeGeneratorParamSize);
    size->setHintToolTip(EffectInstance::tr("Width and height of the size rectangle."));
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

    KnobBoolPtr interactive = AppManager::createKnob<KnobBool>(_effect, EffectInstance::tr("Interactive Update"));
    interactive->setName(kNativeGeneratorParamInteractive);
    interactive->setHintToolTip(EffectInstance::tr("If checked, update the parameter values during interaction with the image viewer, else update the values when pen is released."));
    interactive->setDefaultValue(false);
    interactive->setEvaluateOnChange(false);
    page->addKnob(interactive);
    _interactive = interactive;

    // Kept for knob parity with the OpenFX plug-ins; the host overlay sizes itself.
    KnobBoolPtr hiDPI = AppManager::createKnob<KnobBool>(_effect, EffectInstance::tr("HiDPI"));
    hiDPI->setName(kNativeGeneratorParamHiDPI);
    hiDPI->setHintToolTip(EffectInstance::tr("Should be checked when the display area is High-DPI (a.k.a Retina). Draws OpenGL overlays twice larger."));
    hiDPI->setDefaultValue(false);
    hiDPI->setAnimationEnabled(false);
    hiDPI->setEvaluateOnChange(false);
    page->addKnob(hiDPI);
    _hiDPI = hiDPI;

    KnobIntPtr frameRange = AppManager::createKnob<KnobInt>(_effect, EffectInstance::tr("Frame Range"), 2);
    frameRange->setName(kNativeGeneratorParamFrameRange);
    frameRange->setHintToolTip(EffectInstance::tr("Time domain."));
    frameRange->setDimensionName(0, "min");
    frameRange->setDimensionName(1, "max");
    frameRange->setDefaultValue(1, 0);
    frameRange->setDefaultValue(1, 1);
    frameRange->setAnimationEnabled(false);
    page->addKnob(frameRange);
    _frameRange = frameRange;

    if (_policy.overlayFollowsExtent) {
        createRectangleEnable(page);
    }
} // ExtentKnobs::createKnobs

void
ExtentKnobs::finishKnobs()
{
    updateVisibility();

    NodePtr node = _effect->getNode();
    KnobDoublePtr bottomLeft = _bottomLeft.lock();
    KnobDoublePtr size = _size.lock();
    KnobBoolPtr interactive = _interactive.lock();
    KnobBoolPtr rectangleEnable = _rectangleEnable.lock();
    if (node && bottomLeft && size && interactive && rectangleEnable) {
        node->addRectangleInteract(bottomLeft, size, interactive, rectangleEnable);
    }
}

ExtentKnobs::ExtentEnum
ExtentKnobs::getExtent() const
{
    KnobChoicePtr extent = _extent.lock();

    return extent ? (ExtentEnum)extent->getValue() : (ExtentEnum)_policy.defaultExtent;
}

void
ExtentKnobs::updateVisibility()
{
    const ExtentEnum extent = getExtent();
    const bool hasFormat = (extent == eExtentFormat);
    const bool hasSize = (extent == eExtentSize);

    setSecretAndDisabled(_format.lock(), !hasFormat);
    if (_policy.reformatToggle) {
        setSecretAndDisabled(_reformat.lock(), !hasSize);
    }
    setSecretAndDisabled(_size.lock(), !hasSize);
    setSecretAndDisabled(_recenter.lock(), !hasSize);
    setSecretAndDisabled(_bottomLeft.lock(), !hasSize);
    setSecretAndDisabled(_interactive.lock(), !hasSize);
    setSecretAndDisabled(_hiDPI.lock(), !hasSize);

    if (_policy.overlayFollowsExtent) {
        KnobBoolPtr rectangleEnable = _rectangleEnable.lock();
        if (rectangleEnable && (rectangleEnable->getValue() != hasSize)) {
            rectangleEnable->setValue(hasSize);
        }
    }
}

RectD
ExtentKnobs::getProjectExtentRect(double* par) const
{
    Format format;

    _effect->getApp()->getProject()->getProjectDefaultFormat(&format);
    if (par) {
        *par = format.getPixelAspectRatio();
    }

    return format.toCanonicalFormat();
}

void
ExtentKnobs::recenter(double time,
                      ViewIdx view)
{
    KnobDoublePtr size = _size.lock();
    KnobDoublePtr bottomLeft = _bottomLeft.lock();

    if (!size || !bottomLeft) {
        return;
    }

    RectD reference = getProjectExtentRect();
    if (_policy.recenterOnSource) {
        EffectInstancePtr source = _effect->getInput(0);
        if (source) {
            bool isProjectFormat = false;
            RectD sourceRoD;
            if (source->getRegionOfDefinition_public(source->getRenderHash(), time, RenderScale::identity, view, &sourceRoD, &isProjectFormat) != eStatusFailed) {
                reference = sourceRoD;
            }
        }
    }
    const double centerX = (reference.x2 + reference.x1) / 2.;
    const double centerY = (reference.y2 + reference.y1) / 2.;

    if (_policy.recenterOnSource) {
        const double width = size->getValueAtTime(time, 0, view);
        const double height = size->getValueAtTime(time, 1, view);

        _effect->beginChanges();
        bottomLeft->setValue(centerX - width / 2., ViewSpec::all(), 0);
        bottomLeft->setValue(centerY - height / 2., ViewSpec::all(), 1);
        _effect->endChanges();
    } else {
        const double width = size->getValue(0);
        const double height = size->getValue(1);
        const double x1 = centerX - width / 2.;
        const double y1 = centerY - height / 2.;

        _effect->beginChanges();
        size->setValue((x1 + width) - x1, ViewSpec::all(), 0);
        size->setValue((y1 + height) - y1, ViewSpec::all(), 1);
        bottomLeft->setValue(x1, ViewSpec::all(), 0);
        bottomLeft->setValue(y1, ViewSpec::all(), 1);
        _effect->endChanges();
    }
}

bool
ExtentKnobs::onKnobChanged(KnobI* k,
                           ValueChangedReasonEnum reason,
                           double time)
{
    KnobChoicePtr extent = _extent.lock();
    if (extent && (k == extent.get())) {
        if (reason != eValueChangedReasonTimeChanged) {
            updateVisibility();
        }

        return true;
    }
    KnobButtonPtr recenterButton = _recenter.lock();
    if (recenterButton && (k == recenterButton.get())) {
        recenter(time, ViewIdx(0));

        return true;
    }

    return false;
}

bool
ExtentKnobs::getExtentRegionOfDefinition(double time,
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

KnobChoicePtr
ExtentKnobs::getExtentKnob() const
{
    return _extent.lock();
}

KnobBoolPtr
ExtentKnobs::getReformatKnob() const
{
    return _reformat.lock();
}

KnobIntPtr
ExtentKnobs::getFormatSizeKnob() const
{
    return _formatSize.lock();
}

KnobDoublePtr
ExtentKnobs::getFormatParKnob() const
{
    return _formatPar.lock();
}

KnobDoublePtr
ExtentKnobs::getBottomLeftKnob() const
{
    return _bottomLeft.lock();
}

KnobDoublePtr
ExtentKnobs::getSizeKnob() const
{
    return _size.lock();
}

KnobBoolPtr
ExtentKnobs::getRectangleEnableKnob() const
{
    return _rectangleEnable.lock();
}

KnobIntPtr
ExtentKnobs::getFrameRangeKnob() const
{
    return _frameRange.lock();
}

NATRON_NAMESPACE_EXIT
