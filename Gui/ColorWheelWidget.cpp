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

#include "ColorWheelWidget.h"

#include <algorithm>
#include <cmath>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QAbstractSpinBox>
#include <QApplication>
#include <QBrush>
#include <QCursor>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPlainTextEdit>
#include <QPointer>
#include <QTextEdit>
#include <QWheelEvent>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/AppManager.h"
#include "Engine/Settings.h"

NATRON_NAMESPACE_ENTER

namespace {
const double kGrey = kColorPivotGrey;
const double kPi = 3.14159265358979323846;
const int kHueTableSize = 360;

const double kMargin = 3.0;
const double kRingWidth = 12.0;
const double kRingGap = 2.0;
const double kCrossArm = 5.0;
const double kCaretLength = 6.0;
const double kCaretHalfWidth = 4.0;
const double kPuckRadius = 5.0;
const double kParkedPuckRadius = 6.0;
const double kRingHitTolerance = 4.0;
const double kWheelNotch = 120.0;
const double kWheelMinPx = 160.0;
const double kWheelMaxPx = 320.0;

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

QPointF
screenDirection(const PlanePoint& d)
{
    return QPointF(d.x, -d.y);
}

QPointF
perpendicular(const QPointF& d)
{
    return QPointF(-d.y(), d.x());
}

void
drawOutlinedLine(QPainter& p,
                 const QPointF& a,
                 const QPointF& b)
{
    p.setPen(QPen(QColor(0, 0, 0, 200), 3.0, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(a, b);
    p.setPen(QPen(QColor(255, 255, 255), 1.0, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(a, b);
}

bool
isLockKey(int key)
{
    return (key == Qt::Key_V) || (key == Qt::Key_T);
}

bool
pointerOverWheel()
{
    QWidget* w = QApplication::widgetAt(QCursor::pos());

    while (w) {
        if (qobject_cast<ColorWheelWidget*>(w)) {
            return true;
        }
        w = w->parentWidget();
    }

    return false;
}

bool
isTextInput(const QObject* o)
{
    return qobject_cast<const QLineEdit*>(o) || qobject_cast<const QTextEdit*>(o) || qobject_cast<const QPlainTextEdit*>(o) || qobject_cast<const QAbstractSpinBox*>(o);
}

/**
 * @brief Installed once on qApp, so V/T reach the lock regardless of which widget has focus or
 * whether the pointer ever entered the wheel: Qt delivers key events to the focused widget, not
 * to whichever wheel a drag started on, once the pointer has left it.
 **/
class LockKeyTracker
    : public QObject {
public:
    LockKeyTracker()
        : QObject()
        , _held()
        , _draggingWheels()
    {
    }

    ColorAxisEnum activeLock() const
    {
        if (_held.empty()) {
            return eColorAxisTrackball;
        }

        return _held.back() == Qt::Key_V ? eColorAxisValue : eColorAxisTempTint;
    }

    void dragStarted(ColorWheelWidget* wheel)
    {
        _draggingWheels.push_back(wheel);
    }

    void dragFinished(ColorWheelWidget* wheel)
    {
        _draggingWheels.erase(std::remove(_draggingWheels.begin(), _draggingWheels.end(), wheel), _draggingWheels.end());
    }

protected:
    virtual bool eventFilter(QObject* watched,
                             QEvent* e) OVERRIDE
    {
        if (e->type() == QEvent::ApplicationStateChange) {
            if (qApp && (qApp->applicationState() == Qt::ApplicationInactive)) {
                _held.clear();
            }

            return QObject::eventFilter(watched, e);
        }

        bool shortcutOverride = e->type() == QEvent::ShortcutOverride;
        bool press = e->type() == QEvent::KeyPress;
        bool release = e->type() == QEvent::KeyRelease;

        if (!shortcutOverride && !press && !release) {
            return QObject::eventFilter(watched, e);
        }

        QKeyEvent* ke = static_cast<QKeyEvent*>(e);

        if (!isLockKey(ke->key())) {
            return QObject::eventFilter(watched, e);
        }

        bool consume = consumes(watched, ke);

        if (shortcutOverride) {
            if (consume) {
                e->accept();

                return true;
            }

            return QObject::eventFilter(watched, e);
        }
        if (!ke->isAutoRepeat()) {
            _held.erase(std::remove(_held.begin(), _held.end(), ke->key()), _held.end());
            if (press) {
                _held.push_back(ke->key());
            }
        }

        return consume;
    }

private:
    // A wheel that went away or was hidden mid-drag never sees its release, so only a live,
    // visible wheel that still reports a drag counts.
    bool anyWheelDragging() const
    {
        for (const QPointer<ColorWheelWidget>& w : _draggingWheels) {
            if (w && w->isVisible() && w->isDragging()) {
                return true;
            }
        }

        return false;
    }

    // Ctrl is only claimed mid-drag, where it is the gear: over an idle wheel (dragging or not)
    // Ctrl+V and Ctrl+T must still reach their application shortcuts. Idle, a text field that
    // has the key keeps it, so hovering a wheel never eats letters typed elsewhere.
    bool consumes(const QObject* receiver,
                  const QKeyEvent* e) const
    {
        const bool dragging = anyWheelDragging();
        Qt::KeyboardModifiers allowed = Qt::ShiftModifier | Qt::KeypadModifier;

        if (dragging) {
            allowed |= Qt::ControlModifier;
        }
        if (e->modifiers() & ~allowed) {
            return false;
        }
        if (dragging) {
            return true;
        }

        return !isTextInput(receiver) && pointerOverWheel();
    }

    std::vector<int> _held;
    std::vector<QPointer<ColorWheelWidget>> _draggingWheels;
};

LockKeyTracker*
lockKeyTracker()
{
    static LockKeyTracker* tracker = [] {
        LockKeyTracker* t = new LockKeyTracker();

        if (qApp) {
            qApp->installEventFilter(t);
        }

        return t;
    }();

    return tracker;
}
} // namespace

bool
ColorWheelWidget::DiscCacheKey::operator==(const DiscCacheKey& other) const
{
    return valid && other.valid && size == other.size && rim == other.rim && role == other.role && anchorX == other.anchorX && anchorY == other.anchorY && anchorLinear == other.anchorLinear && basisGeneration == other.basisGeneration && converterGeneration == other.converterGeneration;
}

bool
ColorWheelWidget::RingCacheKey::operator==(const RingCacheKey& other) const
{
    return valid && other.valid && size == other.size && basisGeneration == other.basisGeneration && converterGeneration == other.converterGeneration;
}

ColorWheelWidget::ColorWheelWidget(QWidget* parent)
    : QWidget(parent)
    , _basis(kRec709LumaWeights)
    , _role(eColorKnobRoleGain)
    , _value(neutralGainValue())
    , _anchor()
    , _rim(TrackballRates().wheelRim)
    , _converter()
    , _dragOffset()
    , _hasDragOffset(false)
    , _basisGeneration(0)
    , _converterGeneration(0)
    , _preferredDiameter(minimumDiameter())
    , _dragZone(eHitZoneNone)
    , _lastDragPos()
    , _gestureOffset()
    , _gestureMoved(false)
{
    QSizePolicy policy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    policy.setHeightForWidth(true);
    setSizePolicy(policy);
    setMaximumSize(maximumDiameter(), maximumDiameter());
    setFocusPolicy(Qt::StrongFocus);
    lockKeyTracker();
    updateAnchor();
}

ColorWheelWidget::~ColorWheelWidget()
{
    // No gestureFinished from here: the owner may already be half destroyed, and it ends its
    // own edit session when it goes.
    if (isDragging()) {
        lockKeyTracker()->dragFinished(this);
    }
}

void
ColorWheelWidget::setBasis(const LumaBasis& basis)
{
    _basis = basis;
    ++_basisGeneration;
    updateAnchor();
    update();
}

void
ColorWheelWidget::setRole(ColorKnobRoleEnum role)
{
    if (role == _role) {
        return;
    }
    _role = role;
    updateAnchor();
    update();
}

void
ColorWheelWidget::setValue(const KnobColorValue& value)
{
    _value = value;
    _hasDragOffset = false;
    _dragOffset = PlanePoint();
    updateAnchor();
    update();
}

void
ColorWheelWidget::setRim(double rim)
{
    if (!std::isfinite(rim)) {
        return;
    }

    TrackballRates rates;
    double clamped = std::min(rates.zoomMax, std::max(rates.zoomMin, rim));

    if (clamped == _rim) {
        return;
    }
    _rim = clamped;
    update();
}

void
ColorWheelWidget::setDisplayConverter(const DisplayConverter& converter)
{
    _converter = converter;
    ++_converterGeneration;
    update();
}

void
ColorWheelWidget::setDragOffset(const PlanePoint& dq)
{
    _dragOffset = planePoint(dq.x, dq.y);
    _hasDragOffset = true;
    update();
}

void
ColorWheelWidget::clearDragOffset()
{
    _dragOffset = PlanePoint();
    _hasDragOffset = false;
    update();
}

const LumaBasis&
ColorWheelWidget::getBasis() const
{
    return _basis;
}

ColorKnobRoleEnum
ColorWheelWidget::getRole() const
{
    return _role;
}

const KnobColorValue&
ColorWheelWidget::getValue() const
{
    return _value;
}

double
ColorWheelWidget::getRim() const
{
    return _rim;
}

PlanePoint
ColorWheelWidget::getAnchor() const
{
    return _anchor;
}

PlanePoint
ColorWheelWidget::getDragOffset() const
{
    return _hasDragOffset ? _dragOffset : planePoint(0.0, 0.0);
}

bool
ColorWheelWidget::hasDragOffset() const
{
    return _hasDragOffset;
}

void
ColorWheelWidget::updateAnchor()
{
    _anchor = toPlane(_role, _basis, _value);
    if (!_anchor.valid) {
        _anchor.x = 0.0;
        _anchor.y = 0.0;
    }
}

QPointF
ColorWheelWidget::getDiscCentre() const
{
    return QPointF(width() / 2.0, height() / 2.0);
}

double
ColorWheelWidget::getRingOuterRadius() const
{
    return std::max(0.0, std::min(width(), height()) / 2.0 - TO_DPIX(kMargin));
}

double
ColorWheelWidget::getRingInnerRadius() const
{
    return std::max(0.0, getRingOuterRadius() - TO_DPIX(kRingWidth));
}

double
ColorWheelWidget::getDiscRadius() const
{
    return std::max(0.0, getRingInnerRadius() - TO_DPIX(kRingGap));
}

QPointF
ColorWheelWidget::planeToWidget(const PlanePoint& p) const
{
    QPointF c = getDiscCentre();
    double scale = getDiscRadius() / _rim;

    return QPointF(c.x() + p.x * scale, c.y() - p.y * scale);
}

PlanePoint
ColorWheelWidget::widgetToPlane(const QPointF& pos) const
{
    QPointF c = getDiscCentre();
    double R = getDiscRadius();

    if (R <= 0.0) {
        return planePoint(0.0, 0.0);
    }

    double scale = _rim / R;

    return planePoint((pos.x() - c.x()) * scale, (c.y() - pos.y()) * scale);
}

DiscClamp
ColorWheelWidget::getWhiteMarker() const
{
    return clampToDisc(whitePoint(_anchor), _rim);
}

QPointF
ColorWheelWidget::getWhiteMarkerPosition() const
{
    DiscClamp marker = getWhiteMarker();

    if (!marker.offDisc) {
        return planeToWidget(marker.point);
    }

    double inset = TO_DPIX(kCaretLength + kCrossArm + 3.0);

    return getDiscCentre() + screenDirection(marker.direction) * std::max(0.0, getDiscRadius() - inset);
}

QPolygonF
ColorWheelWidget::getWhiteMarkerCaret() const
{
    QPolygonF caret;
    DiscClamp marker = getWhiteMarker();

    if (!marker.offDisc) {
        return caret;
    }

    QPointF c = getDiscCentre();
    QPointF d = screenDirection(marker.direction);
    QPointF n = perpendicular(d);
    double R = getDiscRadius();
    QPointF tip = c + d * (R - TO_DPIX(1.0));
    QPointF base = c + d * (R - TO_DPIX(1.0 + kCaretLength));

    caret << tip << base + n * TO_DPIX(kCaretHalfWidth) << base - n * TO_DPIX(kCaretHalfWidth);

    return caret;
}

DiscClamp
ColorWheelWidget::getPuck() const
{
    return clampToDisc(getDragOffset(), _rim);
}

QPointF
ColorWheelWidget::getPuckPosition() const
{
    return planeToWidget(getPuck().point);
}

QPolygonF
ColorWheelWidget::getGamutTrianglePolygon() const
{
    QPolygonF polygon;
    GamutTriangle triangle = gamutTriangle(_basis);

    if (!triangle.valid) {
        return polygon;
    }
    for (int i = 0; i < 3; ++i) {
        polygon << planeToWidget(planePoint(triangle.vertex[i].x - _anchor.x, triangle.vertex[i].y - _anchor.y));
    }

    return polygon;
}

QImage
ColorWheelWidget::getDiscImage() const
{
    return discImage();
}

int
ColorWheelWidget::minimumDiameter()
{
    return (int)TO_DPIX(kWheelMinPx);
}

int
ColorWheelWidget::maximumDiameter()
{
    return (int)TO_DPIX(kWheelMaxPx);
}

void
ColorWheelWidget::setPreferredDiameter(int pixels)
{
    int d = std::min(std::max(pixels, minimumDiameter()), maximumDiameter());

    if (d == _preferredDiameter) {
        return;
    }
    _preferredDiameter = d;
    updateGeometry();
}

int
ColorWheelWidget::getPreferredDiameter() const
{
    return _preferredDiameter;
}

QSize
ColorWheelWidget::sizeHint() const
{
    return QSize(_preferredDiameter, _preferredDiameter);
}

QSize
ColorWheelWidget::minimumSizeHint() const
{
    return QSize(minimumDiameter(), minimumDiameter());
}

bool
ColorWheelWidget::hasHeightForWidth() const
{
    return true;
}

int
ColorWheelWidget::heightForWidth(int width) const
{
    return width;
}

double
ColorWheelWidget::devicePixelRatioForCache() const
{
    double dpr = devicePixelRatio();

    return dpr > 0.0 ? dpr : 1.0;
}

void
ColorWheelWidget::convertToDisplay(std::vector<float>& rgb) const
{
    if (_converter && !rgb.empty()) {
        _converter(rgb.data(), (int)(rgb.size() / 3));
    }
}

bool
ColorWheelWidget::discColour(const PlanePoint& q,
                             double rgb[3]) const
{
    if (isRatioRole(_role)) {
        ChromaEditResult r = fromPlane(eColorKnobRoleGain, _basis, neutralGainValue(), q, kGrey);

        for (int i = 0; i < 3; ++i) {
            rgb[i] = r.value.rgb[i];
        }

        return r.valid;
    }

    // Held luma 0 puts the 18% grey pivot plus the offset at luma 0.18, the same level the ratio
    // roles are shown at; _value only decides which plane, projective or linear, it inverts.
    ChromaEditResult r = fromPlane(_role, _basis, _value, q, 0.0);

    for (int i = 0; i < 3; ++i) {
        rgb[i] = kGrey + r.value.rgb[i];
    }

    return r.valid;
}

void
ColorWheelWidget::buildHueTable(std::vector<float>& table) const
{
    table.assign(3 * kHueTableSize, (float)kGrey);

    for (int k = 0; k < kHueTableSize; ++k) {
        double theta = 2.0 * kPi * k / kHueTableSize;
        double cx = std::cos(theta);
        double cy = std::sin(theta);
        double rgb[3];

        auto inGamut = [&](double s, double out[3]) {
            ChromaEditResult r = fromPlane(eColorKnobRoleGain, _basis, neutralGainValue(), planePoint(s * cx, s * cy), kGrey);

            for (int i = 0; i < 3; ++i) {
                out[i] = r.value.rgb[i];
            }

            return r.valid && out[0] >= 0.0 && out[1] >= 0.0 && out[2] >= 0.0;
        };

        double lo = 0.0;
        double hi = 1.0;

        if (inGamut(hi, rgb)) {
            lo = hi;
        } else {
            for (int iter = 0; iter < 32; ++iter) {
                double mid = 0.5 * (lo + hi);

                if (inGamut(mid, rgb)) {
                    lo = mid;
                } else {
                    hi = mid;
                }
            }
        }

        // Backing off the gamut edge keeps a little of the other channels, so neighbouring
        // hues stay distinguishable instead of collapsing onto the three primaries' corners.
        if (!inGamut(0.8 * lo, rgb)) {
            continue;
        }

        double m = std::max(rgb[0], std::max(rgb[1], rgb[2]));

        if (m <= 0.0) {
            continue;
        }
        for (int i = 0; i < 3; ++i) {
            table[3 * k + i] = (float)(rgb[i] / m);
        }
    }
    convertToDisplay(table);
}

const QImage&
ColorWheelWidget::discImage() const
{
    double dpr = devicePixelRatioForCache();
    double R = getDiscRadius();
    int n = (int)std::ceil(2.0 * R * dpr);
    DiscCacheKey key;

    key.size = QSize(n, n);
    key.rim = _rim;
    key.role = _role;
    key.anchorX = _anchor.x;
    key.anchorY = _anchor.y;
    key.anchorLinear = _anchor.linear;
    key.basisGeneration = _basisGeneration;
    key.converterGeneration = _converterGeneration;
    key.valid = true;

    if ((key == _discKey) || (n <= 0)) {
        return _disc;
    }

    QImage image(n, n, QImage::Format_ARGB32);

    image.fill(Qt::transparent);

    std::vector<int> indices;
    std::vector<int> alphas;
    std::vector<float> rgb;
    double Rdev = R * dpr;

    indices.reserve(n * n);
    alphas.reserve(n * n);
    rgb.reserve(3 * n * n);
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            double dx = (i + 0.5) - Rdev;
            double dy = (j + 0.5) - Rdev;
            double coverage = clamp01(Rdev - std::hypot(dx, dy) + 0.5);

            if (coverage <= 0.0) {
                continue;
            }

            PlanePoint q = planePoint(_anchor.x + dx / Rdev * _rim, _anchor.y - dy / Rdev * _rim);
            double c[3];
            bool ok = discColour(q, c);

            if (!ok || (c[0] < 0.0) || (c[1] < 0.0) || (c[2] < 0.0)) {
                double level = (((i + j) / 3) % 2 == 0) ? 0.5 * kGrey : 0.9 * kGrey;

                c[0] = c[1] = c[2] = level;
            }
            indices.push_back(j * n + i);
            alphas.push_back((int)(coverage * 255.0 + 0.5));
            for (int k = 0; k < 3; ++k) {
                rgb.push_back((float)c[k]);
            }
        }
    }
    convertToDisplay(rgb);
    for (std::size_t k = 0; k < indices.size(); ++k) {
        int i = indices[k] % n;
        int j = indices[k] / n;

        image.setPixel(i, j, qRgba(toByte(rgb[3 * k]), toByte(rgb[3 * k + 1]), toByte(rgb[3 * k + 2]), alphas[k]));
    }
    image.setDevicePixelRatio(dpr);

    _disc = image;
    _discKey = key;

    return _disc;
}

const QImage&
ColorWheelWidget::ringImage() const
{
    double dpr = devicePixelRatioForCache();
    double Ro = getRingOuterRadius();
    double Ri = getRingInnerRadius();
    int n = (int)std::ceil(2.0 * Ro * dpr);
    RingCacheKey key;

    key.size = QSize(n, n);
    key.basisGeneration = _basisGeneration;
    key.converterGeneration = _converterGeneration;
    key.valid = true;

    if ((key == _ringKey) || (n <= 0)) {
        return _ring;
    }

    std::vector<float> table;

    buildHueTable(table);

    QImage image(n, n, QImage::Format_ARGB32);

    image.fill(Qt::transparent);

    double RoDev = Ro * dpr;
    double RiDev = Ri * dpr;

    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            double dx = (i + 0.5) - RoDev;
            double dy = (j + 0.5) - RoDev;
            double r = std::hypot(dx, dy);
            double coverage = std::min(clamp01(RoDev - r + 0.5), clamp01(r - RiDev + 0.5));

            if (coverage <= 0.0) {
                continue;
            }

            double degrees = std::atan2(-dy, dx) * 180.0 / kPi;

            if (degrees < 0.0) {
                degrees += 360.0;
            }

            int k = (int)std::floor(degrees * kHueTableSize / 360.0 + 0.5) % kHueTableSize;

            image.setPixel(i, j, qRgba(toByte(table[3 * k]), toByte(table[3 * k + 1]), toByte(table[3 * k + 2]), (int)(coverage * 255.0 + 0.5)));
        }
    }
    image.setDevicePixelRatio(dpr);

    _ring = image;
    _ringKey = key;

    return _ring;
}

QColor
ColorWheelWidget::activeColour() const
{
    QColor colour(243, 149, 0);
    SettingsPtr settings = appPTR ? appPTR->getCurrentSettings() : SettingsPtr();

    if (settings) {
        double r, g, b;

        settings->getSelectionColor(&r, &g, &b);
        colour.setRgbF(clamp01(r), clamp01(g), clamp01(b));
    }

    return colour;
}

void
ColorWheelWidget::drawOverlays(QPainter& p) const
{
    QPolygonF triangle = getGamutTrianglePolygon();

    if (!triangle.isEmpty()) {
        p.setPen(QPen(QColor(255, 140, 60, 200), 1.0, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawPolygon(triangle);
    }

    if (isRatioRole(_role)) {
        std::vector<PlanePoint> locus = drawLocus(_basis, _anchor);

        if (locus.size() >= 2) {
            QPainterPath path;

            path.moveTo(planeToWidget(locus[0]));
            for (std::size_t i = 1; i < locus.size(); ++i) {
                path.lineTo(planeToWidget(locus[i]));
            }
            p.setPen(QPen(QColor(255, 255, 255, 110), 1.2));
            p.setBrush(Qt::NoBrush);
            p.drawPath(path);
        }

        return;
    }

    TempTintAxes axes = linearTempTintAxes(_basis);

    if (!axes.valid) {
        return;
    }

    PlanePoint white = whitePoint(_anchor);
    double halfLength = planeSaturation(white) + 2.0 * _rim;
    const double* directions[2] = { axes.temp, axes.tint };
    const int alpha[2] = { 90, 60 };

    for (int k = 0; k < 2; ++k) {
        // The axes are directions in (a, b); a small offset along each, taken through toPlane,
        // gives the matching direction in the plane the wheel is drawn in.
        const double eps = 1e-3;
        KnobColorValue o;

        join(_basis, 0.0, directions[k][0] * eps, directions[k][1] * eps, o.rgb);
        o.a = 0.0;
        o.hasAlpha = false;

        PlanePoint d = toPlane(eColorKnobRoleOffset, _basis, o);
        double len = planeSaturation(d);

        if (!d.valid || !(len > 0.0)) {
            continue;
        }

        PlanePoint a = planePoint(white.x - d.x / len * halfLength, white.y - d.y / len * halfLength);
        PlanePoint b = planePoint(white.x + d.x / len * halfLength, white.y + d.y / len * halfLength);

        p.setPen(QPen(QColor(255, 255, 255, alpha[k]), 1.0, Qt::DotLine));
        p.drawLine(planeToWidget(a), planeToWidget(b));
    }
}

void
ColorWheelWidget::drawWhiteMarker(QPainter& p) const
{
    QPointF m = getWhiteMarkerPosition();
    double arm = TO_DPIX(kCrossArm);

    drawOutlinedLine(p, m - QPointF(arm, 0.0), m + QPointF(arm, 0.0));
    drawOutlinedLine(p, m - QPointF(0.0, arm), m + QPointF(0.0, arm));

    QPolygonF caret = getWhiteMarkerCaret();

    if (!caret.isEmpty()) {
        p.setPen(QPen(QColor(0, 0, 0, 200), 1.0));
        p.setBrush(QColor(255, 255, 255));
        p.drawPolygon(caret);
    }
}

void
ColorWheelWidget::drawPuck(QPainter& p) const
{
    DiscClamp puck = getPuck();
    QPointF pos = getPuckPosition();
    QColor fill = _hasDragOffset ? activeColour() : QColor(232, 232, 232);

    if (!puck.offDisc) {
        double r = TO_DPIX(kPuckRadius);

        p.setPen(QPen(QColor(0, 0, 0), 1.5));
        p.setBrush(fill);
        p.drawEllipse(pos, r, r);

        return;
    }

    QPointF d = screenDirection(puck.direction);
    QPointF n = perpendicular(d);
    double r = TO_DPIX(kParkedPuckRadius);
    QPointF tip = pos + d * TO_DPIX(13.0);
    QPointF left = pos + d * TO_DPIX(6.0) + n * TO_DPIX(5.0);
    QPointF right = pos + d * TO_DPIX(6.0) - n * TO_DPIX(5.0);
    QPolygonF chevron;

    chevron << left << tip << right;
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(0, 0, 0), 4.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawEllipse(pos, r, r);
    p.drawPolyline(chevron);
    p.setPen(QPen(fill, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawEllipse(pos, r, r);
    p.drawPolyline(chevron);
}

void
ColorWheelWidget::paintEvent(QPaintEvent* /*e*/)
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
    p.drawImage(QPointF(c.x() - Ro, c.y() - Ro), ringImage());
    p.drawImage(QPointF(c.x() - R, c.y() - R), discImage());

    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(21, 21, 21), 1.0));
    p.drawEllipse(c, R, R);
    p.drawEllipse(c, Ri, Ri);
    p.drawEllipse(c, Ro, Ro);

    p.save();
    QPainterPath clip;

    clip.addEllipse(c, R, R);
    p.setClipPath(clip);
    drawOverlays(p);
    p.restore();

    drawWhiteMarker(p);
    drawPuck(p);
}

ColorAxisEnum
ColorWheelWidget::getActiveLock() const
{
    return lockKeyTracker()->activeLock();
}

bool
ColorWheelWidget::isDragging() const
{
    return _dragZone != eHitZoneNone;
}

ColorWheelWidget::HitZoneEnum
ColorWheelWidget::hitTest(const QPointF& pos) const
{
    QPointF c = getDiscCentre();
    double r = std::hypot(pos.x() - c.x(), pos.y() - c.y());
    double tolerance = TO_DPIX(kRingHitTolerance);

    if (getDiscRadius() <= 1.0) {
        return eHitZoneNone;
    }
    if (r <= getDiscRadius()) {
        return eHitZoneDisc;
    }
    if ((r >= getRingInnerRadius() - tolerance) && (r <= getRingOuterRadius() + tolerance)) {
        return eHitZoneRing;
    }

    return eHitZoneNone;
}

double
ColorWheelWidget::ringAngleAt(const QPointF& pos) const
{
    QPointF c = getDiscCentre();
    double degrees = std::atan2(-(pos.y() - c.y()), pos.x() - c.x()) * 180.0 / kPi;

    return degrees < 0.0 ? degrees + 360.0 : degrees;
}

void
ColorWheelWidget::emitStep(const ColorEdit& edit,
                           const QString& axisLabel)
{
    _gestureMoved = true;
    Q_EMIT gestureMoved(edit, axisLabel);
}

void
ColorWheelWidget::emitRingHue(const QPointF& pos)
{
    ColorEdit edit;

    edit.axis = eColorAxisHue;
    edit.d1 = ringAngleAt(pos);
    edit.absolute = true;
    emitStep(edit, tr("Hue"));
}

void
ColorWheelWidget::dragTo(const QPointF& pos,
                         Qt::KeyboardModifiers modifiers)
{
    double dx = pos.x() - _lastDragPos.x();
    double dy = pos.y() - _lastDragPos.y();

    _lastDragPos = pos;
    if ((dx == 0.0) && (dy == 0.0)) {
        return;
    }

    if (_dragZone == eHitZoneRing) {
        emitRingHue(pos);

        return;
    }

    TrackballRates rates;
    double g = gear(modifiers.testFlag(Qt::ControlModifier), modifiers.testFlag(Qt::ShiftModifier));
    ColorEdit edit;

    edit.axis = getActiveLock();
    switch (edit.axis) {
    case eColorAxisValue:
        edit.d1 = (dx - dy) * valueRatePerPixel(rates, _role) * g;
        emitStep(edit, tr("Value"));
        break;

    case eColorAxisTempTint:
        edit.d1 = dx * tempRatePerPixel(rates, _role) * g;
        edit.d2 = -dy * tintRatePerPixel(rates, _role) * g;
        emitStep(edit, tr("Temp/Tint"));
        break;

    default: {
        double R = getDiscRadius();

        if (R <= 0.0) {
            return;
        }

        double scale = _rim / R * g;

        edit.axis = eColorAxisTrackball;
        edit.d1 = dx * scale;
        edit.d2 = -dy * scale;
        _gestureOffset.x += edit.d1;
        _gestureOffset.y += edit.d2;
        setDragOffset(_gestureOffset);
        emitStep(edit, tr("Trackball"));
        break;
    }
    }
}

void
ColorWheelWidget::finishGesture()
{
    if (!isDragging()) {
        return;
    }

    bool moved = _gestureMoved;

    _dragZone = eHitZoneNone;
    _gestureMoved = false;
    _gestureOffset = planePoint(0.0, 0.0);
    lockKeyTracker()->dragFinished(this);
    Q_EMIT gestureFinished(moved);
    clearDragOffset();
}

bool
ColorWheelWidget::event(QEvent* e)
{
    // V/T are the app-wide LockKeyTracker's job; only Escape, specific to this wheel's own
    // drag, is overridden here.
    if ((e->type() == QEvent::ShortcutOverride) && isDragging() && (static_cast<QKeyEvent*>(e)->key() == Qt::Key_Escape)) {
        e->accept();

        return true;
    }

    return QWidget::event(e);
}

void
ColorWheelWidget::mousePressEvent(QMouseEvent* e)
{
    if ((e->button() != Qt::LeftButton) || isDragging()) {
        QWidget::mousePressEvent(e);

        return;
    }

    QPointF pos = e->position();
    HitZoneEnum zone = hitTest(pos);

    if (zone == eHitZoneNone) {
        QWidget::mousePressEvent(e);

        return;
    }

    e->accept();
    // Only Esc mid-drag needs focus here; V/T come through the app-wide LockKeyTracker.
    setFocus(Qt::MouseFocusReason);
    _dragZone = zone;
    _lastDragPos = pos;
    _gestureMoved = false;
    _gestureOffset = planePoint(0.0, 0.0);
    lockKeyTracker()->dragStarted(this);
    Q_EMIT gestureStarted();

    if (zone == eHitZoneRing) {
        emitRingHue(pos);
    } else {
        setDragOffset(_gestureOffset);
    }
}

void
ColorWheelWidget::mouseMoveEvent(QMouseEvent* e)
{
    if (!isDragging()) {
        QWidget::mouseMoveEvent(e);

        return;
    }
    e->accept();
    dragTo(e->position(), e->modifiers());
}

void
ColorWheelWidget::mouseReleaseEvent(QMouseEvent* e)
{
    if (!isDragging() || (e->button() != Qt::LeftButton)) {
        QWidget::mouseReleaseEvent(e);

        return;
    }
    e->accept();
    dragTo(e->position(), e->modifiers());
    finishGesture();
}

void
ColorWheelWidget::mouseDoubleClickEvent(QMouseEvent* e)
{
    if ((e->button() != Qt::LeftButton) || (hitTest(e->position()) == eHitZoneNone)) {
        QWidget::mouseDoubleClickEvent(e);

        return;
    }
    e->accept();
    finishGesture();

    ColorEdit edit;

    edit.axis = eColorAxisSat;
    edit.d1 = 0.0;
    edit.absolute = true;
    Q_EMIT gestureStarted();
    Q_EMIT gestureMoved(edit, tr("Reset"));
    Q_EMIT gestureFinished(true);
}

void
ColorWheelWidget::wheelEvent(QWheelEvent* e)
{
    double notches = e->angleDelta().y() / kWheelNotch;
    // A bare scroll passing over the wheel belongs to the properties bin it sits in.
    bool zoom = hasFocus() || isDragging() || e->modifiers().testFlag(Qt::ControlModifier);

    if ((notches == 0.0) || !zoom) {
        e->ignore();

        return;
    }
    e->accept();
    setRim(_rim * std::pow(TrackballRates().zoomFactorPerNotch, notches));
}

void
ColorWheelWidget::hideEvent(QHideEvent* e)
{
    finishGesture();
    QWidget::hideEvent(e);
}

void
ColorWheelWidget::keyPressEvent(QKeyEvent* e)
{
    if (isDragging() && (e->key() == Qt::Key_Escape)) {
        e->accept();
        finishGesture();

        return;
    }
    QWidget::keyPressEvent(e);
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_ColorWheelWidget.cpp"
