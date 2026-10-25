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

#include "ColorModeDiscWidget.h"

#include <algorithm>
#include <cmath>
#include <vector>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/AppManager.h"

#include "Gui/ColorWheelWidget.h"
#include "Gui/DisplayLegendColour.h"

NATRON_NAMESPACE_ENTER

namespace {
const double kGrey = kColorPivotGrey;
const double kPi = 3.14159265358979323846;

const double kPuckRadius = 5.0;
const double kSizeHint = 240.0;

const double kHueReachDegrees = 60.0;
const int kLattice = 48;
const double kNeutralSaturation = 1e-9;
const double kSqrt2 = 1.41421356237309504880;

// T's four patches sit on the screen axes and blend by cos(angle off the patch)^sharpness,
// fading linearly from the neutral centre to full strength at kTRadialFull of the radius.
const double kTSharpness = 3.0;
const double kTChroma = 0.145;
const double kTRadialFull = 0.85;
const double kTTempStepMired = 100.0;
const double kTTintStep = 10.0;
const double kTAxisStep = 0.05;

// V's ramp spans these OKLab lightnesses at the diagonal's ends, about 0.003 and 0.96 linear.
const double kVLightnessSpan = 0.42;

const double kRec709ToXYZ[9] = {
    0.4124564, 0.3575761, 0.1804375,
    0.2126729, 0.7151522, 0.0721750,
    0.0193339, 0.1191920, 0.9503041
};

int
toByte(float v)
{
    if (!(v > 0.f)) {
        return 0;
    }
    if (v >= 1.f) {
        return 255;
    }

    return (int)(v * 255.f + 0.5f);
}

double
clamp01(double v)
{
    return std::min(1.0, std::max(0.0, v));
}

void
encodeLinear(const double linear[3],
             double encoded[3])
{
    for (int i = 0; i < 3; ++i) {
        encoded[i] = srgbEncode(linear[i]);
    }
}

double
signOf(double v)
{
    return (v > 0.0) ? 1.0 : ((v < 0.0) ? -1.0 : 0.0);
}

double
midGreyLightness()
{
    const double grey[3] = { kGrey, kGrey, kGrey };

    return linearSrgbToOkLab(grey).L;
}

KnobColorValue
greyValue()
{
    KnobColorValue v = neutralGainValue();

    v.rgb[0] = v.rgb[1] = v.rgb[2] = kGrey;

    return v;
}

// Working-space colours a step from mid grey toward warm, magenta, cool and green, in that
// order (screen angles 0, 90, 180 and 270 degrees). False when the basis has no way to walk
// either the locus or its linear axes.
bool
tempTintDirections(const LumaBasis& basis,
                   float rgb[12])
{
    const double steps[4][2] = {
        { kTTempStepMired, 0.0 }, { 0.0, kTTintStep }, { -kTTempStepMired, 0.0 }, { 0.0, -kTTintStep }
    };
    TempTintAxes axes;
    bool axesRead = false;

    for (int k = 0; k < 4; ++k) {
        ChromaEditResult r = fromTempTint(eColorKnobRoleGain, basis, greyValue(), steps[k][0], steps[k][1]);
        double c[3];

        if (r.valid) {
            for (int i = 0; i < 3; ++i) {
                c[i] = r.value.rgb[i];
            }
        } else {
            if (!axesRead) {
                axes = linearTempTintAxes(basis);
                axesRead = true;
            }
            if (!axes.valid) {
                return false;
            }

            const double t = signOf(steps[k][0]);
            const double n = signOf(steps[k][1]);

            join(basis, kGrey, kTAxisStep * (t * axes.temp[0] + n * axes.tint[0]), kTAxisStep * (t * axes.temp[1] + n * axes.tint[1]), c);
        }
        for (int i = 0; i < 3; ++i) {
            rgb[3 * k + i] = (float)std::max(0.0, c[i]);
        }
    }

    return true;
}

// What a drag of (dx, dy) does, in units of the disc radius with dy running down as screen
// pixels do, as display-encoded sRGB. Each axis takes globalModeEdit's sign for the same pixel
// delta. Only the hues come from the working space and the display conversion; lightness and
// chroma are the legend's own, so the disc reads as a map of directions, not as swatches.
struct LegendSampler {
    ColorGlobalModeEnum mode;
    double centreHue;
    std::vector<double> hues;
    OkLab patches[4];
    bool patchesValid;
    double greyL;

    LegendSampler(ColorGlobalModeEnum m,
                  const LumaBasis& basis,
                  const ColorModeDiscWidget::DisplayConverter& converter,
                  double hue,
                  const std::vector<double>& planeHues)
        : mode(m)
        , centreHue(hue)
        , hues(planeHues)
        , patchesValid(false)
        , greyL(midGreyLightness())
    {
        if (mode != eColorGlobalModeT) {
            return;
        }

        std::vector<float> directions(12, (float)kGrey);

        if (!tempTintDirections(basis, directions.data())) {
            return;
        }

        const std::vector<double> h = displayLegendHues(directions, converter);

        for (int k = 0; k < 4; ++k) {
            double rgb[3];

            okLchToGamutLinearSrgb(displayLegendHueLightness(h[k]), kTChroma, h[k], rgb);
            patches[k] = linearSrgbToOkLab(rgb);
        }
        patchesValid = true;
    }

    void sample(double dx,
                double dy,
                double encoded[3]) const
    {
        double rgb[3] = { kGrey, kGrey, kGrey };

        switch (mode) {
        case eColorGlobalModeS: {
            if (hues.empty()) {
                break;
            }

            const double h = hues[colorWheelHueIndex(centreHue + dx * kHueReachDegrees)];

            okLchToGamutLinearSrgb(displayLegendHueLightness(h), kDisplayLegendChroma * clamp01((1.0 - dy) / 2.0), h, rgb);
            break;
        }
        case eColorGlobalModeT: {
            if (!patchesValid) {
                break;
            }

            const double phi = std::atan2(-dy, dx);
            OkLab lab;
            double sum = 0.0;

            for (int k = 0; k < 4; ++k) {
                const double w = std::pow(std::max(0.0, std::cos(phi - k * kPi / 2.0)), kTSharpness);

                lab.L += w * patches[k].L;
                lab.a += w * patches[k].a;
                lab.b += w * patches[k].b;
                sum += w;
            }
            if (sum > 0.0) {
                lab.L /= sum;
                lab.a /= sum;
                lab.b /= sum;
            }

            const double fade = clamp01(std::hypot(dx, dy) / kTRadialFull);
            const double L = kDisplayLegendNeutralLightness + (lab.L - kDisplayLegendNeutralLightness) * fade;

            okLchToGamutLinearSrgb(L, fade * std::hypot(lab.a, lab.b), std::atan2(lab.b, lab.a), rgb);
            break;
        }
        case eColorGlobalModeV: {
            // Divided by sqrt 2 so the ends land on the rim's diagonal instead of plateauing
            // before it.
            const double t = std::max(-1.0, std::min(1.0, (dx - dy) / kSqrt2));
            OkLab lab;

            lab.L = greyL + kVLightnessSpan * t;
            okLabToLinearSrgb(lab, rgb);
            for (int i = 0; i < 3; ++i) {
                rgb[i] = clamp01(rgb[i]);
            }
            break;
        }
        case eColorGlobalModeC:
        default:
            break;
        }
        encodeLinear(rgb, encoded);
    }
};

int
wholeDegrees(double degrees)
{
    if (!std::isfinite(degrees)) {
        return 0;
    }

    int d = (int)std::lround(std::fmod(degrees, 360.0));

    return ((d % 360) + 360) % 360;
}
} // anonymous namespace

bool
ColorModeDiscWidget::CacheKey::operator==(const CacheKey& other) const
{
    return valid == other.valid && size == other.size && dpr == other.dpr && mode == other.mode && hue == other.hue && generation == other.generation;
}

ColorModeDiscWidget::ColorModeDiscWidget(QWidget* parent)
    : QWidget(parent)
    , _mode(eColorGlobalModeS)
    , _basis(kRec709LumaWeights, kRec709ToXYZ)
    , _converter()
    , _generation(0)
    , _hue(0)
    , _hasDefinedHue(false)
    , _lastDefinedHue(0.0)
    , _puck(0.0, 0.0)
    , _hues()
    , _huesGeneration(0)
    , _huesValid(false)
{
    setAttribute(Qt::WA_TranslucentBackground, true);
}

ColorModeDiscWidget::~ColorModeDiscWidget()
{
}

void
ColorModeDiscWidget::setMode(ColorGlobalModeEnum mode)
{
    if (mode == _mode) {
        return;
    }
    _mode = mode;
    update();
}

void
ColorModeDiscWidget::setBasis(const LumaBasis& basis)
{
    _basis = basis;
    ++_generation;
    update();
}

void
ColorModeDiscWidget::setDisplayConverter(const DisplayConverter& converter)
{
    _converter = converter;
    ++_generation;
    update();
}

void
ColorModeDiscWidget::setCentreValue(ColorKnobRoleEnum role,
                                    const KnobColorValue& value)
{
    PlanePoint p = toPlane(role, _basis, value);

    if (p.valid && (planeSaturation(p) > kNeutralSaturation)) {
        _lastDefinedHue = planeHue(p);
        _hasDefinedHue = true;
    }
    setCentreHue(_hasDefinedHue ? _lastDefinedHue : 0.0);
}

void
ColorModeDiscWidget::forgetHue()
{
    _hasDefinedHue = false;
    _lastDefinedHue = 0.0;
}

void
ColorModeDiscWidget::setCentreHue(double degrees)
{
    int hue = wholeDegrees(degrees);

    if (hue == _hue) {
        return;
    }
    _hue = hue;
    if (_mode == eColorGlobalModeS) {
        update();
    }
}

void
ColorModeDiscWidget::setPuck(const QPointF& puck)
{
    QPointF clamped = puck;
    double len = std::hypot(clamped.x(), clamped.y());

    if (len > 1.0) {
        clamped /= len;
    }
    if (clamped == _puck) {
        return;
    }
    _puck = clamped;
    update();
}

QPointF
ColorModeDiscWidget::getDiscCentre() const
{
    return QPointF(width() / 2.0, height() / 2.0);
}

double
ColorModeDiscWidget::getRingOuterRadius() const
{
    return colorWheelRingOuterRadius(size());
}

double
ColorModeDiscWidget::getRingInnerRadius() const
{
    return colorWheelRingInnerRadius(size());
}

double
ColorModeDiscWidget::getDiscRadius() const
{
    return colorWheelDiscRadius(size());
}

QPointF
ColorModeDiscWidget::getPuckWidgetPos() const
{
    QPointF c = getDiscCentre();
    double R = getDiscRadius();

    return QPointF(c.x() + _puck.x() * R, c.y() - _puck.y() * R);
}

QSize
ColorModeDiscWidget::sizeHint() const
{
    return QSize((int)TO_DPIX(kSizeHint), (int)TO_DPIY(kSizeHint));
}

QSize
ColorModeDiscWidget::minimumSizeHint() const
{
    return QSize((int)TO_DPIX(80.0), (int)TO_DPIY(80.0));
}

double
ColorModeDiscWidget::devicePixelRatioForCache() const
{
    double dpr = devicePixelRatio();

    return dpr > 0.0 ? dpr : 1.0;
}

ColorModeDiscWidget::CacheKey
ColorModeDiscWidget::cacheKey(int size,
                              bool withMode) const
{
    CacheKey key;

    key.size = size;
    key.dpr = devicePixelRatioForCache();
    key.mode = withMode ? _mode : eColorGlobalModeS;
    key.hue = (withMode && (_mode == eColorGlobalModeS)) ? _hue : 0;
    key.generation = _generation;
    key.valid = true;

    return key;
}

const QImage&
ColorModeDiscWidget::ringImage() const
{
    double dpr = devicePixelRatioForCache();
    double Ro = getRingOuterRadius();
    int n = (int)std::ceil(2.0 * Ro * dpr);
    CacheKey key = cacheKey(n, false);

    if ((key == _ringKey) || (n <= 0)) {
        return _ring;
    }
    _ring = renderColorWheelRing(_basis, Ro, getRingInnerRadius(), dpr, _converter);
    _ringKey = key;

    return _ring;
}

const QImage&
ColorModeDiscWidget::gradientImage() const
{
    double dpr = devicePixelRatioForCache();
    double R = getDiscRadius();
    double Ro = getRingOuterRadius();
    int n = (int)std::ceil(2.0 * Ro * dpr);
    CacheKey key = cacheKey(n, true);

    if ((key == _gradientKey) || (n <= 0) || (R <= 0.0)) {
        return _gradient;
    }

    QImage image(n, n, QImage::Format_ARGB32);

    image.fill(Qt::transparent);

    LegendSampler sampler(_mode, _basis, _converter, _hue, (_mode == eColorGlobalModeS) ? planeHues() : std::vector<double>());
    double RoDev = Ro * dpr;
    double Rdev = R * dpr;

    // Evaluated on a lattice over the unit disc's bounding square and interpolated: an S drag
    // rebuilds this on every degree of hue, and a per-pixel plane solve is too slow for that.
    const int L = kLattice;
    std::vector<double> lattice(3 * L * L, 0.0);

    for (int j = 0; j < L; ++j) {
        for (int i = 0; i < L; ++i) {
            sampler.sample(2.0 * i / (L - 1) - 1.0, 2.0 * j / (L - 1) - 1.0, &lattice[3 * (j * L + i)]);
        }
    }

    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            double px = (i + 0.5) - RoDev;
            double py = (j + 0.5) - RoDev;
            double r = std::hypot(px, py);
            double coverage = clamp01(RoDev - r + 0.5);

            if (coverage <= 0.0) {
                continue;
            }

            // The ring holds the rim's colour in each direction.
            double scale = (r > Rdev) ? 1.0 / r : 1.0 / Rdev;
            double fx = clamp01((px * scale + 1.0) / 2.0) * (L - 1);
            double fy = clamp01((py * scale + 1.0) / 2.0) * (L - 1);
            int x0 = std::min((int)fx, L - 2);
            int y0 = std::min((int)fy, L - 2);
            double tx = fx - x0;
            double ty = fy - y0;
            int bytes[3];

            for (int k = 0; k < 3; ++k) {
                double a = lattice[3 * (y0 * L + x0) + k] * (1.0 - tx) + lattice[3 * (y0 * L + x0 + 1) + k] * tx;
                double b = lattice[3 * ((y0 + 1) * L + x0) + k] * (1.0 - tx) + lattice[3 * ((y0 + 1) * L + x0 + 1) + k] * tx;

                bytes[k] = toByte((float)(a * (1.0 - ty) + b * ty));
            }
            image.setPixel(i, j, qRgba(bytes[0], bytes[1], bytes[2], (int)(coverage * 255.0 + 0.5)));
        }
    }
    image.setDevicePixelRatio(dpr);

    _gradient = image;
    _gradientKey = key;

    return _gradient;
} // ColorModeDiscWidget::gradientImage

const std::vector<double>&
ColorModeDiscWidget::planeHues() const
{
    if (!_huesValid || (_huesGeneration != _generation)) {
        _hues = displayLegendHues(buildColorWheelHueTable(_basis).rgb, _converter);
        _huesGeneration = _generation;
        _huesValid = true;
    }

    return _hues;
}

void
ColorModeDiscWidget::paintEvent(QPaintEvent* /*e*/)
{
    QPointF c = getDiscCentre();
    double R = getDiscRadius();
    double Ri = getRingInnerRadius();
    double Ro = getRingOuterRadius();

    if (R <= 1.0) {
        return;
    }

    QPainter p(this);

    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.drawImage(QPointF(c.x() - Ro, c.y() - Ro), (_mode == eColorGlobalModeC) ? ringImage() : gradientImage());
    drawColorWheelRingOutlines(p, c, R, Ri, Ro);

    QPointF pos = getPuckWidgetPos();
    double r = TO_DPIX(kPuckRadius);

    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(0, 0, 0), 4.0));
    p.drawEllipse(pos, r, r);
    p.setPen(QPen(QColor(255, 255, 255), 2.0));
    p.drawEllipse(pos, r, r);
}

NATRON_NAMESPACE_EXIT
