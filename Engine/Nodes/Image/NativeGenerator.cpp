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

#include <cmath>

#include <ofxMetadata.h>
#include <ofxNatron.h>

#include "Engine/AppInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_ENTER

namespace {
ExtentPolicy
generatorExtentPolicy()
{
    ExtentPolicy policy;

    policy.defaultExtent = ExtentKnobs::eExtentDefault;
    policy.reformatToggle = true;
    policy.overlayFollowsExtent = true;
    policy.recenterOnSource = false;

    return policy;
}
} // anonymous namespace

NativeGenerator::NativeGenerator(EffectInstance* effect)
    : _effect(effect)
    , _extentKnobs(effect, generatorExtentPolicy())
{
}

void
NativeGenerator::createKnobs(const KnobPagePtr& page)
{
    _extentKnobs.createKnobs(page);
    _extentKnobs.finishKnobs();
}

NativeGenerator::ExtentEnum
NativeGenerator::getExtent() const
{
    return _extentKnobs.getExtent();
}

RectD
NativeGenerator::getProjectExtentRect() const
{
    return _extentKnobs.getProjectExtentRect();
}

bool
NativeGenerator::knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             double time)
{
    return _extentKnobs.onKnobChanged(k, reason, time);
}

void
NativeGenerator::onKnobsLoaded()
{
    _extentKnobs.updateVisibility();
}

bool
NativeGenerator::getExtentRegionOfDefinition(double time,
                                             ViewIdx view,
                                             RectD* rod) const
{
    return _extentKnobs.getExtentRegionOfDefinition(time, view, rod);
}

RectD
NativeGenerator::getRegionOfDefinition(double time,
                                       ViewIdx view) const
{
    RectD rod;

    if (!getExtentRegionOfDefinition(time, view, &rod)) {
        rod = getProjectExtentRect();
    }

    return rod;
}

double
NativeGenerator::getPixelAspectRatio(double time,
                                     ViewIdx view) const
{
    const double par = _effect->getRenderMetadata(time, view)->getPixelAspectRatio(-1);

    return (par == 0.) ? 1. : par;
}

void
NativeGenerator::getFrameRange(double* first,
                               double* last) const
{
    KnobIntPtr frameRange = _extentKnobs.getFrameRangeKnob();

    *first = frameRange ? frameRange->getValue(0) : 1.;
    *last = frameRange ? frameRange->getValue(1) : 1.;
}

ImageMetadata
NativeGenerator::deriveOutputMetadata(double time,
                                      ViewIdx view) const
{
    ImageMetadata metadata;

    metadata.setDouble(kOfxMetadataKeyFrameRate, _effect->getApp()->getProjectFrameRate());
    NodeMetadataConstPtr own = _effect->getMetadata(time, view);
    metadata.setDouble(kOfxMetadataKeyPixelAspect, own ? own->getPixelAspectRatio(-1) : 1.);

    return metadata;
}

StatusEnum
NativeGenerator::getPreferredMetadataAt(double time,
                                        ViewIdx view,
                                        NodeMetadata& metadata) const
{
    metadata.setIsContinuous(true);
    metadata.setNComps(-1, 4);
    metadata.setComponentsType(-1, kNatronColorLayerID);

    double par = 0.;
    switch (_extentKnobs.getExtent(time, view)) {
    case ExtentKnobs::eExtentFormat: {
        KnobDoublePtr formatPar = _extentKnobs.getFormatParKnob();
        par = formatPar ? formatPar->getValueAtTime(time, 0, view) : 1.;
        break;
    }
    case ExtentKnobs::eExtentProject:
    case ExtentKnobs::eExtentDefault: {
        _extentKnobs.getProjectExtentRect(&par);
        break;
    }
    case ExtentKnobs::eExtentSize: {
        KnobBoolPtr reformat = _extentKnobs.getReformatKnob();
        // Asked at a frame, an animated rectangle reformats to that frame's size, where a
        // timeless answer could only leave the format alone.
        if (reformat && reformat->getValueAtTime(time, 0, view)) {
            par = 1.;
        }
        break;
    }
    }

    if (par != 0.) {
        metadata.setPixelAspectRatio(-1, par);
        RectD rod;
        if (getExtentRegionOfDefinition(time, view, &rod)) {
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
} // NativeGenerator::getPreferredMetadataAt

NATRON_NAMESPACE_EXIT
