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

NATRON_NAMESPACE_ENTER

namespace {
const double kGrey = kColorPivotGrey;
const double kPi = 3.14159265358979323846;

const double kPuckRadius = 5.0;
const double kSizeHint = 240.0;

const double kHueReachDegrees = 30.0;
const double kGamutReach = 0.9;
const int kLattice = 48;
const double kNeutralSaturation = 1e-9;

const double kRec709ToXYZ[9] = {
    0.4124564, 0.3575761, 0.1804375,
    0.2126729, 0.7151522, 0.0721750,
    0.0193339, 0.1191920, 0.9503041
};

PlanePoint
planePoint(double x,
           double y)
{
    PlanePoint p;

    p.x = x;
    p.y = y;
    p.valid = true;

    return p;
}

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
normalise(double rgb[3])
{
    double m = std::max(rgb[0], std::max(rgb[1], rgb[2]));

    if (m > 0.0) {
        for (int i = 0; i < 3; ++i) {
            rgb[i] /= m;
        }
    }
}

// The colour a drag of (dx, dy), in units of the disc radius with dy running down as screen
// pixels do, produces. Each axis takes globalModeEdit's sign for the same pixel delta, so the
// disc shows where a drag that way goes.
struct DragSampler {
    ColorGlobalModeEnum mode;
    const LumaBasis& basis;
    double centreHue;
    ColorWheelHueTable hues;
    TempTintAxes axes;

    DragSampler(ColorGlobalModeEnum m,
                const LumaBasis& b,
                double hue)
        : mode(m)
        , basis(b)
        , centreHue(hue)
    {
        if (mode == eColorGlobalModeS) {
            hues = buildColorWheelHueTable(basis);
        } else if (mode == eColorGlobalModeT) {
            axes = linearTempTintAxes(basis);
        }
    }

    bool sample(double dx,
                double dy,
                double rgb[3]) const
    {
        rgb[0] = rgb[1] = rgb[2] = kGrey;

        switch (mode) {
        case eColorGlobalModeS: {
            double degrees = centreHue + dx * kHueReachDegrees;
            double theta = degrees * kPi / 180.0;
            double s = clamp01((1.0 - dy) / 2.0) * kGamutReach * hues.limit[colorWheelHueIndex(degrees)];
            ChromaEditResult r = fromPlane(eColorKnobRoleGain, basis, neutralGainValue(), planePoint(s * std::cos(theta), s * std::sin(theta)), kGrey);

            if (!r.valid) {
                return false;
            }
            for (int i = 0; i < 3; ++i) {
                rgb[i] = std::max(0.0, r.value.rgb[i]);
            }
            normalise(rgb);

            return true;
        }
        case eColorGlobalModeT: {
            if (!axes.valid) {
                return false;
            }

            double distance = std::hypot(dx, dy);

            if (!(distance > 0.0)) {
                rgb[0] = rgb[1] = rgb[2] = 1.0;

                return true;
            }

            double temp = dx / distance;
            double tint = -dy / distance;
            double a = temp * axes.temp[0] + tint * axes.tint[0];
            double b = temp * axes.temp[1] + tint * axes.tint[1];
            double unit[3];

            join(basis, 0.0, a, b, unit);

            // Scaled per direction to just inside the gamut at mid-grey, so every rim colour
            // is as vivid as the working space allows instead of fading where one axis is short.
            double reach = 0.0;

            for (int i = 0; i < 3; ++i) {
                if (unit[i] < 0.0) {
                    double limit = kGrey / -unit[i];

                    reach = (reach > 0.0) ? std::min(reach, limit) : limit;
                }
            }
            if (!(reach > 0.0)) {
                return false;
            }
            for (int i = 0; i < 3; ++i) {
                rgb[i] = std::max(0.0, kGrey + std::min(1.0, distance) * kGamutReach * reach * unit[i]);
            }
            normalise(rgb);

            return true;
        }
        case eColorGlobalModeV: {
            KnobColorValue c = setValue(eColorKnobRoleGain, basis, neutralGainValue(), clamp01(0.5 + 0.5 * (dx - dy)));

            for (int i = 0; i < 3; ++i) {
                rgb[i] = std::max(0.0, c.rgb[i]);
            }

            return true;
        }
        case eColorGlobalModeC:
        default:
            return false;
        }
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

    DragSampler sampler(_mode, _basis, _hue);
    double RoDev = Ro * dpr;
    double Rdev = R * dpr;
    std::vector<int> indices;
    std::vector<int> alphas;
    std::vector<float> rgb;

    // Evaluated on a lattice over the unit disc's bounding square and interpolated: an S drag
    // rebuilds this on every degree of hue, and a per-pixel plane solve is too slow for that.
    const int L = kLattice;
    std::vector<double> lattice(3 * L * L, kGrey);

    for (int j = 0; j < L; ++j) {
        for (int i = 0; i < L; ++i) {
            double c[3];

            if (sampler.sample(2.0 * i / (L - 1) - 1.0, 2.0 * j / (L - 1) - 1.0, c)) {
                for (int k = 0; k < 3; ++k) {
                    lattice[3 * (j * L + i) + k] = c[k];
                }
            }
        }
    }

    indices.reserve(n * n);
    alphas.reserve(n * n);
    rgb.reserve(3 * n * n);
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

            indices.push_back(j * n + i);
            alphas.push_back((int)(coverage * 255.0 + 0.5));
            for (int k = 0; k < 3; ++k) {
                double a = lattice[3 * (y0 * L + x0) + k] * (1.0 - tx) + lattice[3 * (y0 * L + x0 + 1) + k] * tx;
                double b = lattice[3 * ((y0 + 1) * L + x0) + k] * (1.0 - tx) + lattice[3 * ((y0 + 1) * L + x0 + 1) + k] * tx;

                rgb.push_back((float)(a * (1.0 - ty) + b * ty));
            }
        }
    }
    if (_converter && !rgb.empty()) {
        _converter(rgb.data(), (int)(rgb.size() / 3));
    }
    for (std::size_t k = 0; k < indices.size(); ++k) {
        int i = indices[k] % n;
        int j = indices[k] / n;

        image.setPixel(i, j, qRgba(toByte(rgb[3 * k]), toByte(rgb[3 * k + 1]), toByte(rgb[3 * k + 2]), alphas[k]));
    }
    image.setDevicePixelRatio(dpr);

    _gradient = image;
    _gradientKey = key;

    return _gradient;
} // ColorModeDiscWidget::gradientImage

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
