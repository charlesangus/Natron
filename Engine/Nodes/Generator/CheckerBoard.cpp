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
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Image/PixelKernel.h"
#include "Engine/RectD.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

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

void
fillRun(const GeneratorRowChannels& channels,
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
readColor(const KnobReader& reader,
          const KnobColorWPtr& knob,
          float out[4])
{
    for (int i = 0; i < 4; ++i) {
        out[i] = (float)reader.valueOr(knob, 0., i);
    }
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

void
CheckerBoardOp::createKnobs(KnobHolder* holder,
                            const KnobPagePtr& page)
{
    KnobDoublePtr boxSizeKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Box Size").toStdString(), 2);
    boxSizeKnob->setName(kCheckerBoardParamBoxSize);
    boxSizeKnob->setHintToolTip(EffectInstance::tr("Size of the checkerboard boxes in pixels.").toStdString());
    boxSizeKnob->setSpatial(true);
    boxSizeKnob->setCanAutoFoldDimensions(true);
    for (int d = 0; d < 2; ++d) {
        boxSizeKnob->setMinimum(1., d);
        boxSizeKnob->setMaximum(DBL_MAX, d);
        boxSizeKnob->setDisplayMinimum(0., d);
        boxSizeKnob->setDisplayMaximum(100., d);
        boxSizeKnob->setDefaultValue(64., d);
    }
    page->addKnob(boxSizeKnob);
    boxSize = boxSizeKnob;

    color0 = addCheckerBoardColorKnob(holder, page, kCheckerBoardParamColor0, EffectInstance::tr("Color 0").toStdString(),
                                      EffectInstance::tr("Color to fill the box on top-left of image center and every other row and column.").toStdString(), 0.1, 0.1, 0.1, 1.);
    color1 = addCheckerBoardColorKnob(holder, page, kCheckerBoardParamColor1, EffectInstance::tr("Color 1").toStdString(),
                                      EffectInstance::tr("Color to fill the box on top-right of image center and every other row and column.").toStdString(), 0.5, 0.5, 0.5, 1.);
    color2 = addCheckerBoardColorKnob(holder, page, kCheckerBoardParamColor2, EffectInstance::tr("Color 2").toStdString(),
                                      EffectInstance::tr("Color to fill the box on bottom-right of image center and every other row and column.").toStdString(), 0.1, 0.1, 0.1, 1.);
    color3 = addCheckerBoardColorKnob(holder, page, kCheckerBoardParamColor3, EffectInstance::tr("Color 3").toStdString(),
                                      EffectInstance::tr("Color to fill the box on bottom-left of image center and every other row and column.").toStdString(), 0.5, 0.5, 0.5, 1.);
    lineColor = addCheckerBoardColorKnob(holder, page, kCheckerBoardParamLineColor, EffectInstance::tr("Line Color").toStdString(),
                                         EffectInstance::tr("Color of the line drawn between boxes.").toStdString(), 1., 1., 1., 1.);
    lineWidth = addCheckerBoardWidthKnob(holder, page, kCheckerBoardParamLineWidth, EffectInstance::tr("Line Width").toStdString(),
                                         EffectInstance::tr("Width, in pixels, of the lines drawn between boxes.").toStdString(), 0.);
    centerlineColor = addCheckerBoardColorKnob(holder, page, kCheckerBoardParamCenterLineColor, EffectInstance::tr("Centerline Color").toStdString(),
                                               EffectInstance::tr("Color of the center lines.").toStdString(), 1., 1., 0., 1.);
    centerlineWidth = addCheckerBoardWidthKnob(holder, page, kCheckerBoardParamCenterLineWidth, EffectInstance::tr("Centerline Width").toStdString(),
                                               EffectInstance::tr("Width, in pixels, of the center lines.").toStdString(), 1.);
} // CheckerBoardOp::createKnobs

CheckerBoardOp::Params
CheckerBoardOp::bake(const KnobReader& reader) const
{
    Params params;
    const RectD rod = generator ? generator->getRegionOfDefinition(reader.time(), reader.view()) : RectD();

    params.boxSizeX = reader.valueOr(boxSize, 64., 0);
    params.boxSizeY = reader.valueOr(boxSize, 64., 1);
    params.lineWidth = reader.valueOr(lineWidth, 0., 0);
    params.centerlineWidth = reader.valueOr(centerlineWidth, 1., 0);
    params.rodX1 = rod.x1;
    params.rodY1 = rod.y1;
    params.rodX2 = rod.x2;
    params.rodY2 = rod.y2;
    params.par = generator ? generator->getPixelAspectRatio(reader.time(), reader.view()) : 1.;
    readColor(reader, color0, params.colors[eCheckerBoardColor0]);
    readColor(reader, color1, params.colors[eCheckerBoardColor1]);
    readColor(reader, color2, params.colors[eCheckerBoardColor2]);
    readColor(reader, color3, params.colors[eCheckerBoardColor3]);
    readColor(reader, lineColor, params.colors[eCheckerBoardColorLine]);
    readColor(reader, centerlineColor, params.colors[eCheckerBoardColorCenterline]);

    return params;
}

CheckerBoardGeometry
CheckerBoardOp::geometry(const Params& params,
                         const PassContext& context)
{
    const OfxPointD renderScale = context.scale.toOfxPointD();
    const RectD rod(params.rodX1, params.rodY1, params.rodX2, params.rodY2);

    return checkerBoardGeometry(params.boxSizeX, params.boxSizeY, params.lineWidth, params.centerlineWidth, rod, renderScale.x, renderScale.y, params.par);
}

bool
CheckerBoardOp::isIdentity(const Params& /*params*/,
                           const PassContext& /*context*/) const
{
    return false;
}

void
CheckerBoardOp::row(const Params& params,
                    const PassContext& context,
                    const RowIO& io) const
{
    const CheckerBoardGeometry g = geometry(params, context);
    const GeneratorRowChannels channels(io);
    const CheckerBoardRow row = checkerBoardRow(g, io.y);
    const int xEnd = io.x0 + io.width;
    float* dst = io.dst;

    for (int x = io.x0; x < xEnd;) {
        CheckerBoardColorEnum color = eCheckerBoardColor0;
        const int end = checkerBoardRunEnd(g, row, x, xEnd, &color);
        fillRun(channels, params.colors[color], io.nComps, end - x, dst);
        dst += (std::size_t)(end - x) * io.nComps;
        x = end;
    }
}

GpuOpRef
CheckerBoardOp::gpu(const Params& params,
                    const PassContext& context) const
{
#ifdef NATRON_HAVE_GPU
    const CheckerBoardGeometry g = geometry(params, context);

    // Pixels only exist inside the region of definition; the margin keeps the tables clear of
    // the rounding of its edges.
    const double par = params.par;
    const double rsX = context.scale.toOfxPointD().x;
    const double rsY = context.scale.toOfxPointD().y;
    const double x1 = std::floor(params.rodX1 * rsX / par) - 2.;
    const double x2 = std::ceil(params.rodX2 * rsX / par) + 2.;
    const double y1 = std::floor(params.rodY1 * rsY) - 2.;
    const double y2 = std::ceil(params.rodY2 * rsY) + 2.;
    const double limit = 65536.;
    if (!(x2 - x1 >= 1.) || !(y2 - y1 >= 1.) || !(x2 - x1 <= limit) || !(y2 - y1 <= limit) || !(std::fabs(x1) < 1e9) || !(std::fabs(y1) < 1e9)) {
        return GpuOpRef::none();
    }
    const int baseX = (int)x1;
    const int baseY = (int)y1;
    const int countX = (int)(x2 - x1);
    const int countY = (int)(y2 - y1);
    const std::size_t wordsX = ((std::size_t)countX + 15) / 16;
    const std::size_t wordsY = ((std::size_t)countY + 15) / 16;
    const std::size_t header = 32;
    std::vector<std::uint32_t> words(header + wordsX + wordsY, 0u);

    std::memcpy(words.data(), params.colors, sizeof(float) * 24);
    words[24] = (std::uint32_t)baseX;
    words[25] = (std::uint32_t)baseY;
    words[26] = (std::uint32_t)countX;
    words[27] = (std::uint32_t)countY;
    words[28] = (std::uint32_t)header;
    words[29] = (std::uint32_t)(header + wordsX);

    // Columns are classed with the per-pixel test of a box row, whose even and odd box colours
    // stand for the box parities.
    CheckerBoardRow boxRow;
    boxRow.uniform = false;
    boxRow.uniformColor = eCheckerBoardColor0;
    boxRow.evenColor = eCheckerBoardColor0;
    boxRow.oddColor = eCheckerBoardColor1;
    const int xEnd = baseX + countX;
    for (int x = baseX; x < xEnd;) {
        CheckerBoardColorEnum color = eCheckerBoardColor0;
        const int end = checkerBoardRunEnd(g, boxRow, x, xEnd, &color);
        std::uint32_t cls = 2u;
        if (color == eCheckerBoardColorCenterline) {
            cls = 0u;
        } else if (color == eCheckerBoardColorLine) {
            cls = 1u;
        } else if (color == eCheckerBoardColor1) {
            cls = 3u;
        }
        for (int i = x; i < end; ++i) {
            const std::size_t n = (std::size_t)(i - baseX);
            words[header + n / 16] |= cls << ((n % 16) * 2);
        }
        x = end;
    }
    for (int y = baseY; y < baseY + countY; ++y) {
        const CheckerBoardRow row = checkerBoardRow(g, y);
        std::uint32_t cls;
        if (row.uniform) {
            cls = (row.uniformColor == eCheckerBoardColorCenterline) ? 0u : 1u;
        } else {
            cls = (row.evenColor == eCheckerBoardColor0) ? 2u : 3u;
        }
        const std::size_t n = (std::size_t)(y - baseY);
        words[header + wordsX + n / 16] |= cls << ((n % 16) * 2);
    }

    GpuOpRef ref;
    ref.kind = GpuOpRef::Kind::Kernel;
    ref.kernel.kernel = (int)gpu::GpuKernelId::checkerboard;
    ref.kernel.params.resize(words.size() * sizeof(std::uint32_t));
    std::memcpy(ref.kernel.params.data(), words.data(), ref.kernel.params.size());

    return ref;
#else
    Q_UNUSED(params);
    Q_UNUSED(context);

    return GpuOpRef::none();
#endif
} // CheckerBoardOp::gpu

NATRON_NAMESPACE_EXIT
