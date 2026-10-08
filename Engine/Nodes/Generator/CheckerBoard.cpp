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
#include <cstddef>
#include <memory>
#include <string>

#include "Engine/KnobTypes.h"
#include "Engine/RectD.h"

NATRON_NAMESPACE_ENTER

namespace {
// The smallest integer at or above v, for a v above x, clamped to at most xEnd.
int
firstIntegerFrom(double v,
                 int x,
                 int xEnd)
{
    if (!(v < xEnd)) {
        return xEnd;
    }

    return (std::max)(x + 1, (int)std::ceil(v));
}

// The first n in (x, bound] where step(n) differs from step(x), or bound; step must be
// non-decreasing in n and guess close to that n. A NaN step makes every pixel its own run.
template <typename Step>
int
nextStep(const Step& step,
         double stepAtX,
         int guess,
         int x,
         int bound)
{
    if (stepAtX != stepAtX) {
        return x + 1;
    }
    int n = (std::min)((std::max)(guess, x + 1), bound);

    while ((n > x + 1) && (step(n - 1) != stepAtX)) {
        --n;
    }
    while ((n < bound) && (step(n) == stepAtX)) {
        ++n;
    }

    return n;
}

class CheckerBoardKernel
    : public PixelKernel {
public:
    CheckerBoardKernel(const CheckerBoardGeometry& geometry,
                       const float colors[6][4])
        : _g(geometry)
    {
        for (int i = 0; i < 6; ++i) {
            for (int c = 0; c < 4; ++c) {
                _colors[i][c] = colors[i][c];
            }
        }
    }

    virtual void processRow(const RowIO& io) const OVERRIDE FINAL
    {
        const GeneratorRowChannels channels(io);
        const CheckerBoardRow row = checkerBoardRow(_g, io.y);
        const int xEnd = io.x0 + io.width;
        float* dst = io.dst;

        for (int x = io.x0; x < xEnd;) {
            CheckerBoardColorEnum color = eCheckerBoardColor0;
            const int end = checkerBoardRunEnd(_g, row, x, xEnd, &color);
            fillRun(channels, _colors[color], io.nComps, end - x, dst);
            dst += (std::size_t)(end - x) * io.nComps;
            x = end;
        }
    }

private:
    static void fillRun(const GeneratorRowChannels& channels,
                        const float rgba[4],
                        int nComps,
                        int count,
                        float* dst)
    {
        if (channels.count != nComps) {
            for (int i = 0; i < count; ++i, dst += nComps) {
                channels.write(rgba, dst);
            }

            return;
        }
        float pixel[4] = { 0.f, 0.f, 0.f, 0.f };
        channels.write(rgba, pixel);
        for (int i = 0; i < count; ++i, dst += nComps) {
            for (int c = 0; c < nComps; ++c) {
                dst[c] = pixel[c];
            }
        }
    }

    CheckerBoardGeometry _g;
    float _colors[6][4];
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

CheckerBoardGeometry
checkerBoardGeometry(double boxSizeX,
                     double boxSizeY,
                     double lineWidth,
                     double centerlineWidth,
                     const RectD& rod,
                     double renderScaleX,
                     double renderScaleY,
                     double par)
{
    const double rsX = renderScaleX;
    const double rsY = renderScaleY;
    CheckerBoardGeometry g;

    g.boxSizeX = (std::max)(1., boxSizeX * rsX / par);
    g.boxSizeY = (std::max)(1., boxSizeY * rsY);
    g.lineInfX = lineWidth > 0. ? ((std::max)(0., lineWidth * rsX / 2 / par) + 0.25) : 0.;
    g.lineSupX = lineWidth > 0. ? ((std::max)(lineWidth, par) * rsX / 2 / par - 0.25) : 0.;
    g.lineInfY = lineWidth > 0. ? ((std::max)(0., lineWidth * rsY / 2) + 0.25) : 0.;
    g.lineSupY = lineWidth > 0. ? ((std::max)(lineWidth, 1.) * rsY / 2 - 0.25) : 0.;
    // The centre lines are drawn at every render scale, at least a pixel wide.
    g.centerlineInfX = centerlineWidth > 0. ? ((std::max)(0., centerlineWidth * rsX / 2 / par) + 0.25) : 0.;
    g.centerlineSupX = centerlineWidth > 0. ? ((std::max)(centerlineWidth * rsX, par) / 2 / par - 0.25) : 0.;
    g.centerlineInfY = centerlineWidth > 0. ? ((std::max)(0., centerlineWidth * rsY / 2) + 0.25) : 0.;
    g.centerlineSupY = centerlineWidth > 0. ? ((std::max)(centerlineWidth * rsY, 1.) / 2 - 0.25) : 0.;
    const double rodX1 = rod.x1 * rsX / par;
    const double rodX2 = rod.x2 * rsX / par;
    const double rodY1 = rod.y1 * rsY;
    const double rodY2 = rod.y2 * rsY;
    g.centerX = (rodX1 + rodX2) / 2;
    g.centerY = (rodY1 + rodY2) / 2;

    return g;
}

CheckerBoardRow
checkerBoardRow(const CheckerBoardGeometry& g,
                int y)
{
    CheckerBoardRow row;

    row.uniform = true;
    row.uniformColor = eCheckerBoardColorCenterline;
    row.evenColor = eCheckerBoardColor0;
    row.oddColor = eCheckerBoardColor1;
    if (((g.centerY - g.centerlineInfY) <= y) && (y < (g.centerY + g.centerlineSupY))) {
        return row;
    }
    const double yline = g.centerY + g.boxSizeY * std::floor((y - g.centerY) / g.boxSizeY + 0.5);
    if (((yline - g.lineInfY) <= y) && (y < (yline + g.lineSupY))) {
        row.uniformColor = eCheckerBoardColorLine;

        return row;
    }
    const int ybox = (int)std::floor((y - g.centerY) / g.boxSizeY);
    row.uniform = false;
    row.evenColor = (ybox & 1) ? eCheckerBoardColor3 : eCheckerBoardColor0;
    row.oddColor = (ybox & 1) ? eCheckerBoardColor2 : eCheckerBoardColor1;

    return row;
}

// Pixel x's colour comes from three tests: inside the centre line, inside the line of the
// nearest box edge k(x), and the parity of the box xbox(x). The run ends where any input of
// those tests can change: a bound of the centre line, a bound of line k(x), or a step of k or
// xbox. Bounds are compared as the per-pixel test compares them, and k and xbox are evaluated
// with its expressions, so each run holds exactly the pixels that test gives one colour.
int
checkerBoardRunEnd(const CheckerBoardGeometry& g,
                   const CheckerBoardRow& row,
                   int x,
                   int xEnd,
                   CheckerBoardColorEnum* color)
{
    if (row.uniform) {
        *color = row.uniformColor;

        return xEnd;
    }
    const double centerlineInf = g.centerX - g.centerlineInfX;
    const double centerlineSup = g.centerX + g.centerlineSupX;
    if ((centerlineInf <= x) && (x < centerlineSup)) {
        *color = eCheckerBoardColorCenterline;

        return firstIntegerFrom(centerlineSup, x, xEnd);
    }
    int end = xEnd;
    if (x < centerlineInf) {
        end = firstIntegerFrom(centerlineInf, x, end);
    }

    const auto nearestEdge = [&g](int n) {
        return std::floor((n - g.centerX) / g.boxSizeX + 0.5);
    };
    const double k = nearestEdge(x);
    end = nextStep(nearestEdge, k, firstIntegerFrom(g.centerX + g.boxSizeX * (k + 0.5), x, end), x, end);

    const double xline = g.centerX + g.boxSizeX * k;
    const double lineInf = xline - g.lineInfX;
    const double lineSup = xline + g.lineSupX;
    if ((lineInf <= x) && (x < lineSup)) {
        *color = eCheckerBoardColorLine;

        return firstIntegerFrom(lineSup, x, end);
    }
    if (x < lineInf) {
        end = firstIntegerFrom(lineInf, x, end);
    }

    const auto box = [&g](int n) {
        return std::floor((n - g.centerX) / g.boxSizeX);
    };
    const double xbox = box(x);
    end = nextStep(box, xbox, firstIntegerFrom(g.centerX + g.boxSizeX * (xbox + 1), x, end), x, end);
    *color = (static_cast<int>(xbox) & 1) ? row.oddColor : row.evenColor;

    return end;
} // checkerBoardRunEnd

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

    const CheckerBoardGeometry g = checkerBoardGeometry(boxSizeX, boxSizeY, lineWidth, centerlineWidth, rod, rsX, rsY, par);
    float colors[6][4];
    readColor(_color0, context, colors[eCheckerBoardColor0]);
    readColor(_color1, context, colors[eCheckerBoardColor1]);
    readColor(_color2, context, colors[eCheckerBoardColor2]);
    readColor(_color3, context, colors[eCheckerBoardColor3]);
    readColor(_lineColor, context, colors[eCheckerBoardColorLine]);
    readColor(_centerlineColor, context, colors[eCheckerBoardColorCenterline]);

    return std::make_shared<CheckerBoardKernel>(g, colors);
} // CheckerBoard::makeKernel

NATRON_NAMESPACE_EXIT
