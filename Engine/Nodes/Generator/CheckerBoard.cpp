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

#include "CheckerBoard.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <memory>
#include <string>

#include "Engine/KnobTypes.h"
#include "Engine/RectD.h"

NATRON_NAMESPACE_ENTER

namespace {
// Everything the kernel needs, in pixel coordinates at the render scale, as the openfx-misc
// CheckerBoardProcessorBase::setValues() derives it.
struct CheckerBoardValues {
    double boxSizeX;
    double boxSizeY;
    float color0[4];
    float color1[4];
    float color2[4];
    float color3[4];
    float lineColor[4];
    float centerlineColor[4];
    double lineInfX;
    double lineSupX;
    double lineInfY;
    double lineSupY;
    double centerlineInfX;
    double centerlineSupX;
    double centerlineInfY;
    double centerlineSupY;
    double centerX;
    double centerY;
};

class CheckerBoardKernel
    : public PixelKernel {
public:
    explicit CheckerBoardKernel(const CheckerBoardValues& values)
        : _v(values)
    {
    }

    virtual void processRow(const RowIO& io) const OVERRIDE FINAL
    {
        const GeneratorRowChannels channels(io);
        const int y = io.y;
        float* dst = io.dst;

        if (((_v.centerY - _v.centerlineInfY) <= y) && (y < (_v.centerY + _v.centerlineSupY))) {
            fill(channels, _v.centerlineColor, io, dst);

            return;
        }
        const double yline = _v.centerY + _v.boxSizeY * std::floor((y - _v.centerY) / _v.boxSizeY + 0.5);
        if (((yline - _v.lineInfY) <= y) && (y < (yline + _v.lineSupY))) {
            fill(channels, _v.lineColor, io, dst);

            return;
        }
        const int ybox = (int)std::floor((y - _v.centerY) / _v.boxSizeY);
        const float* c0 = (ybox & 1) ? _v.color3 : _v.color0;
        const float* c1 = (ybox & 1) ? _v.color2 : _v.color1;
        const int xEnd = io.x0 + io.width;
        for (int x = io.x0; x < xEnd; ++x, dst += io.nComps) {
            if (((_v.centerX - _v.centerlineInfX) <= x) && (x < (_v.centerX + _v.centerlineSupX))) {
                channels.write(_v.centerlineColor, dst);
                continue;
            }
            const double xline = _v.centerX + _v.boxSizeX * std::floor((x - _v.centerX) / _v.boxSizeX + 0.5);
            if (((xline - _v.lineInfX) <= x) && (x < (xline + _v.lineSupX))) {
                channels.write(_v.lineColor, dst);
                continue;
            }
            const int xbox = static_cast<int>(std::floor((x - _v.centerX) / _v.boxSizeX));
            channels.write((xbox & 1) ? c1 : c0, dst);
        }
    }

private:
    static void fill(const GeneratorRowChannels& channels,
                     const float rgba[4],
                     const RowIO& io,
                     float* dst)
    {
        for (int x = 0; x < io.width; ++x, dst += io.nComps) {
            channels.write(rgba, dst);
        }
    }

    CheckerBoardValues _v;
};

KnobColorPtr
addCheckerBoardColorKnob(KnobHolder* holder,
                         const KnobPagePtr& page,
                         const std::string& name,
                         const std::string& label,
                         const std::string& hint,
                         double r,
                         double g,
                         double b,
                         double a)
{
    KnobColorPtr knob = AppManager::createKnob<KnobColor>(holder, label, 4);
    const double defaults[4] = { r, g, b, a };

    knob->setName(name);
    knob->setHintToolTip(hint);
    for (int i = 0; i < 4; ++i) {
        knob->setDefaultValue(defaults[i], i);
        knob->setMinimum(-DBL_MAX, i);
        knob->setMaximum(DBL_MAX, i);
        knob->setDisplayMinimum(0., i);
        knob->setDisplayMaximum(1., i);
    }
    page->addKnob(knob);

    return knob;
}

KnobDoublePtr
addCheckerBoardWidthKnob(KnobHolder* holder,
                         const KnobPagePtr& page,
                         const std::string& name,
                         const std::string& label,
                         const std::string& hint,
                         double defaultValue)
{
    KnobDoublePtr knob = AppManager::createKnob<KnobDouble>(holder, label, 1);

    knob->setName(name);
    knob->setHintToolTip(hint);
    knob->setMinimum(0.);
    knob->setMaximum(DBL_MAX);
    knob->setDisplayMinimum(0.);
    knob->setDisplayMaximum(10.);
    knob->setDefaultValue(defaultValue);
    page->addKnob(knob);

    return knob;
}

void
readColor(const KnobColorWPtr& weak,
          const KernelContext& context,
          float out[4])
{
    KnobColorPtr knob = weak.lock();

    for (int i = 0; i < 4; ++i) {
        out[i] = knob ? (float)knob->getValueAtTime(context.time, i, context.view) : 0.f;
    }
}

double
readDouble(const KnobDoubleWPtr& weak,
           const KernelContext& context,
           int dimension,
           double defaultValue)
{
    KnobDoublePtr knob = weak.lock();

    return knob ? knob->getValueAtTime(context.time, dimension, context.view) : defaultValue;
}
} // anonymous namespace

CheckerBoard::CheckerBoard(NodePtr node)
    : NativeGenerator(node)
{
}

CheckerBoard::~CheckerBoard()
{
}

NativePluginDescription
CheckerBoard::getNativePluginDescription() const
{
    NativePluginDescription desc;

    desc.id = PLUGINID_NATRON_CHECKERBOARD;
    desc.label = "CheckerBoard";
    desc.description = tr("Generate an image with a checkerboard.\n"
                          "A frame range may be specified for operators that need it.\n"
                          "See also: https://web.archive.org/web/20220807183203/http://www.opticalenquiry.com/nuke/index.php?title=Constant,_CheckerBoard,_ColorBars,_ColorWheel")
                           .toStdString();
    desc.grouping = PLUGIN_GROUP_IMAGE;
    desc.majorVersion = PLUGIN_MAJOR_NATRON_CHECKERBOARD;
    desc.minorVersion = 0;
    describeSourceInput(&desc);
    desc.outputKind = eDataKindImage;

    return desc;
}

void
CheckerBoard::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    initializeGeneratorKnobs(page);

    KnobDoublePtr boxSize = createKnob<KnobDouble>(tr("Box Size"), 2);
    boxSize->setName(kCheckerBoardParamBoxSize);
    boxSize->setHintToolTip(tr("Size of the checkerboard boxes in pixels."));
    boxSize->setSpatial(true);
    boxSize->setCanAutoFoldDimensions(true);
    for (int d = 0; d < 2; ++d) {
        boxSize->setMinimum(1., d);
        boxSize->setMaximum(DBL_MAX, d);
        boxSize->setDisplayMinimum(0., d);
        boxSize->setDisplayMaximum(100., d);
        boxSize->setDefaultValue(64., d);
    }
    page->addKnob(boxSize);
    _boxSize = boxSize;

    _color0 = addCheckerBoardColorKnob(this, page, kCheckerBoardParamColor0, tr("Color 0").toStdString(),
                                       tr("Color to fill the box on top-left of image center and every other row and column.").toStdString(), 0.1, 0.1, 0.1, 1.);
    _color1 = addCheckerBoardColorKnob(this, page, kCheckerBoardParamColor1, tr("Color 1").toStdString(),
                                       tr("Color to fill the box on top-right of image center and every other row and column.").toStdString(), 0.5, 0.5, 0.5, 1.);
    _color2 = addCheckerBoardColorKnob(this, page, kCheckerBoardParamColor2, tr("Color 2").toStdString(),
                                       tr("Color to fill the box on bottom-right of image center and every other row and column.").toStdString(), 0.1, 0.1, 0.1, 1.);
    _color3 = addCheckerBoardColorKnob(this, page, kCheckerBoardParamColor3, tr("Color 3").toStdString(),
                                       tr("Color to fill the box on bottom-left of image center and every other row and column.").toStdString(), 0.5, 0.5, 0.5, 1.);
    _lineColor = addCheckerBoardColorKnob(this, page, kCheckerBoardParamLineColor, tr("Line Color").toStdString(),
                                          tr("Color of the line drawn between boxes.").toStdString(), 1., 1., 1., 1.);
    _lineWidth = addCheckerBoardWidthKnob(this, page, kCheckerBoardParamLineWidth, tr("Line Width").toStdString(),
                                          tr("Width, in pixels, of the lines drawn between boxes.").toStdString(), 0.);
    _centerlineColor = addCheckerBoardColorKnob(this, page, kCheckerBoardParamCenterLineColor, tr("Centerline Color").toStdString(),
                                                tr("Color of the center lines.").toStdString(), 1., 1., 0., 1.);
    _centerlineWidth = addCheckerBoardWidthKnob(this, page, kCheckerBoardParamCenterLineWidth, tr("Centerline Width").toStdString(),
                                                tr("Width, in pixels, of the center lines.").toStdString(), 1.);
} // CheckerBoard::initializeKnobs

PixelKernelPtr
CheckerBoard::makeKernel(const KernelContext& context)
{
    const OfxPointD renderScale = context.mappedScale.toOfxPointD();
    const double rsX = renderScale.x;
    const double rsY = renderScale.y;
    double par = getAspectRatio(-1);
    if (par == 0) {
        par = 1.;
    }

    RectD rod;
    if (!getExtentRegionOfDefinition(context.time, context.view, &rod)) {
        rod = getProjectExtentRect();
    }

    const double boxSizeX = readDouble(_boxSize, context, 0, 64.);
    const double boxSizeY = readDouble(_boxSize, context, 1, 64.);
    const double lineWidth = readDouble(_lineWidth, context, 0, 0.);
    const double centerlineWidth = readDouble(_centerlineWidth, context, 0, 1.);

    CheckerBoardValues v;
    v.boxSizeX = (std::max)(1., boxSizeX * rsX / par);
    v.boxSizeY = (std::max)(1., boxSizeY * rsY);
    readColor(_color0, context, v.color0);
    readColor(_color1, context, v.color1);
    readColor(_color2, context, v.color2);
    readColor(_color3, context, v.color3);
    readColor(_lineColor, context, v.lineColor);
    readColor(_centerlineColor, context, v.centerlineColor);
    v.lineInfX = lineWidth > 0. ? ((std::max)(0., lineWidth * rsX / 2 / par) + 0.25) : 0.;
    v.lineSupX = lineWidth > 0. ? ((std::max)(lineWidth, par) * rsX / 2 / par - 0.25) : 0.;
    v.lineInfY = lineWidth > 0. ? ((std::max)(0., lineWidth * rsY / 2) + 0.25) : 0.;
    v.lineSupY = lineWidth > 0. ? ((std::max)(lineWidth, 1.) * rsY / 2 - 0.25) : 0.;
    // The centre lines are drawn at every render scale, at least a pixel wide.
    v.centerlineInfX = centerlineWidth > 0. ? ((std::max)(0., centerlineWidth * rsX / 2 / par) + 0.25) : 0.;
    v.centerlineSupX = centerlineWidth > 0. ? ((std::max)(centerlineWidth * rsX, par) / 2 / par - 0.25) : 0.;
    v.centerlineInfY = centerlineWidth > 0. ? ((std::max)(0., centerlineWidth * rsY / 2) + 0.25) : 0.;
    v.centerlineSupY = centerlineWidth > 0. ? ((std::max)(centerlineWidth * rsY, 1.) / 2 - 0.25) : 0.;
    const double rodX1 = rod.x1 * rsX / par;
    const double rodX2 = rod.x2 * rsX / par;
    const double rodY1 = rod.y1 * rsY;
    const double rodY2 = rod.y2 * rsY;
    v.centerX = (rodX1 + rodX2) / 2;
    v.centerY = (rodY1 + rodY2) / 2;

    return std::make_shared<CheckerBoardKernel>(v);
} // CheckerBoard::makeKernel

NATRON_NAMESPACE_EXIT
