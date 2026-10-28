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

#include "Crop.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <list>

#include "Engine/KnobTypes.h"
#include "Engine/NodeMetadata.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_ENTER

namespace {
bool
isEmptyRect(const RectD& r)
{
    return (r.x2 <= r.x1) || (r.y2 <= r.y1);
}

// The OpenFX support library's rectIntersection(): rectangles that merely touch still
// intersect, and an empty or disjoint pair gives an empty rectangle at the origin.
RectD
intersectRects(const RectD& a,
               const RectD& b)
{
    RectD empty(0., 0., 0., 0.);

    if (isEmptyRect(a) || isEmptyRect(b)) {
        return empty;
    }
    if ((a.x1 > b.x2) || (b.x1 > a.x2) || (a.y1 > b.y2) || (b.y1 > a.y2)) {
        return empty;
    }
    RectD ret;
    ret.x1 = std::max(a.x1, b.x1);
    ret.x2 = std::max(ret.x1, std::min(a.x2, b.x2));
    ret.y1 = std::max(a.y1, b.y1);
    ret.y2 = std::max(ret.y1, std::min(a.y2, b.y2));

    return ret;
}

RectI
toPixelEnclosing(const RectD& r,
                 double scale,
                 double par)
{
    if (isEmptyRect(r)) {
        return RectI(0, 0, 0, 0);
    }

    return RectI((int)std::floor(r.x1 * scale / par),
                 (int)std::floor(r.y1 * scale),
                 (int)std::ceil(r.x2 * scale / par),
                 (int)std::ceil(r.y2 * scale));
}

RectI
toPixelNearest(const RectD& r,
               double scale,
               double par)
{
    if (isEmptyRect(r)) {
        return RectI(0, 0, 0, 0);
    }

    return RectI((int)std::floor(r.x1 * scale / par + 0.5),
                 (int)std::floor(r.y1 * scale + 0.5),
                 (int)std::ceil(r.x2 * scale / par - 0.5),
                 (int)std::ceil(r.y2 * scale - 0.5));
}

RectD
toCanonical(const RectI& r,
            double scale,
            double par)
{
    if ((r.x2 <= r.x1) || (r.y2 <= r.y1)) {
        return RectD(0., 0., 0., 0.);
    }

    return RectD(r.x1 * par / scale, r.y1 / scale, r.x2 * par / scale, r.y2 / scale);
}

double
rampSmooth(double t)
{
    t *= 2.;
    if (t < 1) {
        return t * t / 2.;
    }
    t -= 1.;

    return -0.5 * (t * (t - 2) - 1);
}

ExtentPolicy
cropExtentPolicy()
{
    ExtentPolicy policy;

    policy.defaultExtent = ExtentKnobs::eExtentSize;
    policy.reformatToggle = false;
    policy.overlayFollowsExtent = false;
    policy.recenterOnSource = true;
    policy.frameRange = false;

    return policy;
}
} // anonymous namespace

CropOp::Params
CropOp::bake(const KnobReader& reader) const
{
    Params params;

    params.time = reader.time();
    params.view = reader.view();
    params.reformat = reader.value(reformat);
    params.blackOutside = reader.value(blackOutside);
    // Softness is in canonical units, so the render scale does not apply to it.
    params.softness = reader.value(softness);

    return params;
}

bool
CropOp::isIdentity(const Params& /*params*/,
                   const PassContext& /*context*/) const
{
    return false;
}

PixelOffset
CropOp::offset(const Params& params,
               const PassContext& context) const
{
    PixelOffset shift;

    if (!crop || !params.reformat) {
        return shift;
    }
    RectD cropFull;
    double par = 1.;
    crop->getCropRectangle(params.time, params.view, context.scale, false, false, false, false, &cropFull, &par);
    const RectI fullPixel = toPixelNearest(cropFull, context.scale.toOfxPointD().x, par);
    shift.x = -fullPixel.x1;
    shift.y = -fullPixel.y1;

    return shift;
}

ResampleMatte
CropOp::matte(const Params& params,
              const PassContext& context) const
{
    const bool blackOutside = params.blackOutside;
    const double softness = params.softness;

    if (!crop || (!blackOutside && (softness == 0))) {
        return ResampleMatte();
    }
    const double scale = context.scale.toOfxPointD().x;
    NodeMetadataConstPtr own = crop->getMetadata(params.time, params.view);
    const double dstPar = own ? own->getPixelAspectRatio(-1) : 1.;
    RectD cropFull;
    double par = 1.;
    crop->getCropRectangle(params.time, params.view, context.scale, false, false, false, false, &cropFull, &par);
    const RectI fullPixel = toPixelNearest(cropFull, scale, par);
    const PixelOffset shift = offset(params, context);

    // The weight of one axis, from the distance d of the pixel centre inside the crop rectangle.
    const auto weight = [softness](double d) -> double {
        if ((softness == 0) || (d >= softness)) {
            return 1.;
        }

        return rampSmooth(d / softness);
    };
    const auto columnDistance = [scale, dstPar, cropFull, tx = shift.x](int x) {
        const double canonicalX = ((x - tx) + 0.5) * dstPar / scale;

        return std::min(canonicalX - cropFull.x1, cropFull.x2 - canonicalX);
    };
    const auto rowDistance = [scale, cropFull, ty = shift.y](int y) {
        const double canonicalY = ((y - ty) + 0.5) / scale;

        return std::min(canonicalY - cropFull.y1, cropFull.y2 - canonicalY);
    };

    ResampleMatte matte;
    matte.column = [weight, columnDistance](int x) {
        return weight(columnDistance(x));
    };
    matte.row = [weight, rowDistance](int y) {
        return weight(rowDistance(y));
    };
    if (blackOutside) {
        matte.blackColumn = [columnDistance, fullPixel, tx = shift.x](int x) {
            return (x == (fullPixel.x1 + tx)) || (x == (fullPixel.x2 - 1 + tx)) || (columnDistance(x) <= 0);
        };
        matte.blackRow = [rowDistance, fullPixel, ty = shift.y](int y) {
            return (y == (fullPixel.y1 + ty)) || (y == (fullPixel.y2 - 1 + ty)) || (rowDistance(y) <= 0);
        };
    }

    return matte;
} // CropOp::matte

Crop::Crop(NodePtr node)
    : ImageNode<CropOp>(node)
    , _extentKnobs(this, cropExtentPolicy())
{
    op().crop = this;
}

Crop::~Crop()
{
}

void
Crop::addAcceptedComponents(int inputNb,
                            std::list<ImageLayerDesc>* comps)
{
    ImageNode<CropOp>::addAcceptedComponents(inputNb, comps);
    comps->push_back(ImageLayerDesc::getXYComponents());
}

void
Crop::initializeKnobs()
{
    const KnobPagePtr page = createControlsPage();

    _extentKnobs.createKnobs(page);

    KnobDoublePtr softness = createKnob<KnobDouble>(tr("Softness"));
    softness->setName(kCropParamSoftness);
    softness->setHintToolTip(tr("Size of the fade to black around edges to apply."));
    softness->setDefaultValue(0.);
    softness->setMinimum(0.);
    softness->setMaximum(1000.);
    softness->setDisplayMinimum(0.);
    softness->setDisplayMaximum(100.);
    softness->setIncrement(1.);
    page->addKnob(softness);
    op().softness = softness;

    KnobBoolPtr reformat = createKnob<KnobBool>(tr("Reformat"));
    reformat->setName(kCropParamReformat);
    reformat->setHintToolTip(tr("Translates the bottom left corner of the crop rectangle to be in (0,0), and makes the crop rectangle the output format. "
                                "With the 'Size' or 'Default' extent, the output format is the crop rectangle at the current frame: "
                                "the rectangle may be animated, but the output format can not."));
    reformat->setDefaultValue(false);
    reformat->setAnimationEnabled(false);
    reformat->setAddNewLine(false);
    reformat->setSpacingBetweenItems(1);
    reformat->setIsMetadataSlave(true);
    page->addKnob(reformat);
    op().reformat = reformat;

    KnobBoolPtr intersect = createKnob<KnobBool>(tr("Intersect"));
    intersect->setName(kCropParamIntersect);
    intersect->setHintToolTip(tr("Intersects the crop rectangle with the input region of definition instead of extending it."));
    intersect->setDefaultValue(false);
    intersect->setAddNewLine(false);
    intersect->setSpacingBetweenItems(1);
    page->addKnob(intersect);
    _intersect = intersect;

    KnobBoolPtr blackOutside = createKnob<KnobBool>(tr("Black Outside"));
    blackOutside->setName(kCropParamBlackOutside);
    blackOutside->setHintToolTip(tr("Add 1 black and transparent pixel to the region of definition so that all the area outside the crop rectangle is black."));
    blackOutside->setDefaultValue(false);
    page->addKnob(blackOutside);
    op().blackOutside = blackOutside;

    addStageKnobs(page);
    _extentKnobs.finishKnobs();
} // Crop::initializeKnobs

namespace {
// The PAR of source's output at (time, view), which is what this node's input clip carries there.
double
sourcePixelAspectRatio(EffectInstance& source,
                       double time,
                       ViewIdx view)
{
    NodeMetadataConstPtr metadata = source.getMetadata(time, view);

    return metadata ? metadata->getPixelAspectRatio(-1) : 1.;
}
} // anonymous namespace

Crop::ExtentEnum
Crop::getExtent() const
{
    return _extentKnobs.getExtent();
}

void
Crop::updateRectangleEnable()
{
    KnobBoolPtr reformat = op().reformat.lock();
    KnobBoolPtr rectangleEnable = _extentKnobs.getRectangleEnableKnob();

    if (!reformat || !rectangleEnable) {
        return;
    }
    const bool enable = !reformat->getValue();
    if (rectangleEnable->getValue() != enable) {
        rectangleEnable->setValue(enable);
    }
}

void
Crop::onKnobsLoaded()
{
    _extentKnobs.updateVisibility();
    updateRectangleEnable();
}

bool
Crop::knobChanged(KnobI* k,
                  ValueChangedReasonEnum reason,
                  ViewSpec /*view*/,
                  double time,
                  bool /*originatedFromMainThread*/)
{
    if (_extentKnobs.onKnobChanged(k, reason, time)) {
        return true;
    }
    KnobBoolPtr reformat = op().reformat.lock();
    if (reformat && (k == reformat.get())) {
        updateRectangleEnable();
        if (reason == eValueChangedReasonUserEdited) {
            KnobBoolPtr blackOutside = op().blackOutside.lock();
            if (blackOutside) {
                blackOutside->setValue(!reformat->getValue());
            }
        }

        return true;
    }

    return false;
}

void
Crop::getCropRectangle(double time,
                       ViewIdx view,
                       const RenderScale& scale,
                       bool useIntersect,
                       bool forceIntersect,
                       bool useBlackOutside,
                       bool useReformat,
                       RectD* cropRect,
                       double* par) const
{
    KnobBoolPtr intersectKnob = _intersect.lock();
    KnobBoolPtr blackOutsideKnob = op().blackOutside.lock();
    KnobBoolPtr reformatKnob = op().reformat.lock();

    bool intersect = false;
    if (useIntersect) {
        intersect = forceIntersect || (intersectKnob && intersectKnob->getValueAtTime(time, 0, view));
    }
    const bool blackOutside = useBlackOutside && blackOutsideKnob && blackOutsideKnob->getValueAtTime(time, 0, view);
    const bool reformat = useReformat && reformatKnob && reformatKnob->getValueAtTime(time, 0, view);

    EffectInstancePtr source = getInput(0);
    RectD sourceRoD;
    bool hasSourceRoD = false;
    if (source) {
        const RenderScale sourceScale = source->supportsRenderScale() ? scale : RenderScale::identity;
        bool isProjectFormat = false;
        hasSourceRoD = (source->getRegionOfDefinition_public(source->getRenderHash(), time, sourceScale, view, &sourceRoD, &isProjectFormat) != eStatusFailed);
    }

    RectD rod;
    double rodPar = 1.;
    switch (_extentKnobs.getExtent(time, view)) {
    case eExtentFormat: {
        KnobIntPtr formatSize = _extentKnobs.getFormatSizeKnob();
        KnobDoublePtr formatPar = _extentKnobs.getFormatParKnob();
        const int w = formatSize ? formatSize->getValueAtTime(time, 0, view) : 0;
        const int h = formatSize ? formatSize->getValueAtTime(time, 1, view) : 0;
        rodPar = formatPar ? formatPar->getValueAtTime(time, 0, view) : 1.;
        rod = toCanonical(RectI(0, 0, w, h), 1., rodPar);
        break;
    }
    case eExtentSize: {
        KnobDoublePtr size = _extentKnobs.getSizeKnob();
        KnobDoublePtr bottomLeft = _extentKnobs.getBottomLeftKnob();
        if (source) {
            rodPar = sourcePixelAspectRatio(*source, time, view);
        } else {
            _extentKnobs.getProjectExtentRect(&rodPar);
        }
        if (size && bottomLeft) {
            rod.x1 = bottomLeft->getValueAtTime(time, 0, view);
            rod.y1 = bottomLeft->getValueAtTime(time, 1, view);
            rod.x2 = size->getValueAtTime(time, 0, view) + rod.x1;
            rod.y2 = size->getValueAtTime(time, 1, view) + rod.y1;
        }
        break;
    }
    case eExtentProject:
        rod = _extentKnobs.getProjectExtentRect(&rodPar);
        break;
    case eExtentDefault:
        if (source && hasSourceRoD) {
            rod = sourceRoD;
            rodPar = sourcePixelAspectRatio(*source, time, view);
        } else {
            rod = _extentKnobs.getProjectExtentRect(&rodPar);
        }
        break;
    }

    if (reformat) {
        rod.x2 -= rod.x1;
        rod.y2 -= rod.y1;
        rod.x1 = 0.;
        rod.y1 = 0.;
    }
    if (intersect && hasSourceRoD) {
        rod = intersectRects(rod, sourceRoD);
    }
    if (blackOutside) {
        const double s = scale.toOfxPointD().x;
        RectI rodPixel = toPixelEnclosing(rod, s, rodPar);
        rodPixel.x1 -= 1;
        rodPixel.y1 -= 1;
        rodPixel.x2 += 1;
        rodPixel.y2 += 1;
        rod = toCanonical(rodPixel, s, rodPar);
    }

    if (cropRect) {
        *cropRect = rod;
    }
    if (par) {
        *par = rodPar;
    }
} // Crop::getCropRectangle

StatusEnum
Crop::getRegionOfDefinition(U64 /*hash*/,
                            double time,
                            const RenderScale& scale,
                            ViewIdx view,
                            RectD* rod)
{
    getCropRectangle(time, view, scale, true, false, true, true, rod, NULL);

    return eStatusOK;
}

void
Crop::getRegionsOfInterest(double time,
                           const RenderScale& scale,
                           const RectD& /*outputRoD*/,
                           const RectD& renderWindow,
                           ViewIdx view,
                           RoIMap* ret)
{
    EffectInstancePtr source = getInput(0);

    if (!source) {
        return;
    }
    RectD cropRect;
    getCropRectangle(time, view, scale, true, true, false, false, &cropRect, NULL);

    RectD roi = renderWindow;
    KnobBoolPtr reformat = op().reformat.lock();
    if (reformat && reformat->getValueAtTime(time, 0, view)) {
        // The crop rectangle is rendered at the origin, so the window maps back by its offset.
        roi.x1 += cropRect.x1;
        roi.y1 += cropRect.y1;
        roi.x2 += cropRect.x1;
        roi.y2 += cropRect.y1;
    }
    ret->insert(std::make_pair(source, intersectRects(cropRect, roi)));
}

StatusEnum
Crop::getPreferredMetadataAt(double time,
                             ViewIdx view,
                             NodeMetadata& metadata)
{
    KnobBoolPtr reformat = op().reformat.lock();

    if (!reformat || !reformat->getValueAtTime(time, 0, view)) {
        return eStatusOK;
    }

    RectI pixelFormat(0, 0, 0, 0);
    double par = 0.;
    const ExtentEnum extent = _extentKnobs.getExtent(time, view);
    if (extent == eExtentFormat) {
        KnobIntPtr formatSize = _extentKnobs.getFormatSizeKnob();
        KnobDoublePtr formatPar = _extentKnobs.getFormatParKnob();
        if (formatSize && formatPar) {
            par = formatPar->getValueAtTime(time, 0, view);
            pixelFormat = RectI(0, 0, formatSize->getValueAtTime(time, 0, view), formatSize->getValueAtTime(time, 1, view));
        }
    } else if (extent == eExtentProject) {
        const RectD project = _extentKnobs.getProjectExtentRect(&par);
        pixelFormat = toPixelNearest(project, 1., par);
    } else {
        // Unlike the OpenFX Crop, which leaves the source's format around the smaller image for
        // these extents. The format is the rectangle at time, so an animated rectangle gives
        // each frame its own format.
        RectD rect;
        getCropRectangle(time, view, RenderScale::identity, false, false, false, true, &rect, &par);
        pixelFormat = toPixelNearest(rect, 1., par);
    }
    if (par != 0.) {
        metadata.setPixelAspectRatio(-1, par);
    }
    if ((pixelFormat.x2 > pixelFormat.x1) && (pixelFormat.y2 > pixelFormat.y1)) {
        metadata.setOutputFormat(pixelFormat);
    }

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
