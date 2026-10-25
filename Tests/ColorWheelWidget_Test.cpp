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

#include "Global/Macros.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

#include <QApplication>
#include <QColor>
#include <QCursor>
#include <QDir>
#include <QImage>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPixmap>
#include <QPoint>
#include <QPointF>
#include <QPolygonF>
#include <QString>
#include <QWheelEvent>

#include <gtest/gtest.h>

#include "Engine/AppManager.h"
#include "Engine/ColorWheelMath.h"

#include "Gui/ColorAdjustPanel.h"
#include "Gui/ColorWheelWidget.h"
#include "Gui/DisplayLegendColour.h"

NATRON_NAMESPACE_USING

namespace {
const double kAP1Weights[3] = { 0.2722287168, 0.6740817658, 0.0536895174 };
const double kAP1ToXYZ[9] = {
    0.6624541811, 0.1340042065, 0.1561876870,
    0.2722287168, 0.6740817658, 0.0536895174,
    -0.0055746495, 0.0040607335, 1.0103391003
};
const int kWidgetSize = 160;
const double kPi = 3.14159265358979323846;

LumaBasis
acescg()
{
    return LumaBasis(kAP1Weights, kAP1ToXYZ);
}

KnobColorValue
makeValue(double r,
          double g,
          double b)
{
    KnobColorValue v;

    v.rgb[0] = r;
    v.rgb[1] = g;
    v.rgb[2] = b;
    v.a = 1.0;
    v.hasAlpha = true;

    return v;
}

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

void
identity(float* /*rgb*/,
         int /*count*/)
{
}

void
setUpWheel(ColorWheelWidget& wheel,
           ColorKnobRoleEnum role,
           const KnobColorValue& value,
           double rim)
{
    wheel.setDisplayConverter(&identity);
    wheel.setBasis(acescg());
    wheel.setRole(role);
    wheel.setValue(value);
    wheel.setRim(rim);
    wheel.resize(kWidgetSize, kWidgetSize);
    wheel.show();
}

double
screenAngleDegrees(const QPointF& from,
                   const QPointF& to)
{
    double degrees = std::atan2(-(to.y() - from.y()), to.x() - from.x()) * 180.0 / kPi;

    return degrees < 0.0 ? degrees + 360.0 : degrees;
}

double
angleDifference(double a,
                double b)
{
    double d = std::fmod(std::fabs(a - b), 360.0);

    return std::min(d, 360.0 - d);
}

double
distance(const QPointF& a,
         const QPointF& b)
{
    return std::hypot(a.x() - b.x(), a.y() - b.y());
}

void
expectAchromatic(QRgb pixel)
{
    EXPECT_LT(std::abs(qRed(pixel) - qGreen(pixel)), 2);
    EXPECT_LT(std::abs(qGreen(pixel) - qBlue(pixel)), 2);
}

QString
outputDirectory()
{
#ifdef NATRON_TESTS_OUTPUT_DIR
    QString dir = QString::fromUtf8(NATRON_TESTS_OUTPUT_DIR);
#else
    QString dir = QDir::tempPath() + QString::fromUtf8("/NatronGuiTests");
#endif

    QDir().mkpath(dir);

    return dir;
}

void
saveGrab(ColorWheelWidget& wheel,
         const char* name)
{
    QString path = outputDirectory() + QString::fromUtf8("/ColorWheelWidget_") + QString::fromUtf8(name) + QString::fromUtf8(".png");

    EXPECT_TRUE(wheel.grab().save(path)) << path.toStdString();
}

struct GestureRecorder {
    int started = 0;
    int finished = 0;
    std::vector<bool> finishedMoved;
    std::vector<ColorEdit> edits;
    std::vector<QString> labels;

    explicit GestureRecorder(ColorWheelWidget& wheel)
    {
        QObject::connect(&wheel, &ColorWheelWidget::gestureStarted, [this]() { ++started; });
        QObject::connect(&wheel, &ColorWheelWidget::gestureMoved, [this](ColorEdit edit, QString label) {
            edits.push_back(edit);
            labels.push_back(label);
        });
        QObject::connect(&wheel, &ColorWheelWidget::gestureFinished, [this](bool moved) {
            ++finished;
            finishedMoved.push_back(moved);
        });
    }

    double sum1(ColorAxisEnum axis) const
    {
        double s = 0.0;

        for (const ColorEdit& e : edits) {
            if (e.axis == axis) {
                s += e.d1;
            }
        }

        return s;
    }

    double sum2(ColorAxisEnum axis) const
    {
        double s = 0.0;

        for (const ColorEdit& e : edits) {
            if (e.axis == axis) {
                s += e.d2;
            }
        }

        return s;
    }

    int count(ColorAxisEnum axis) const
    {
        int n = 0;

        for (const ColorEdit& e : edits) {
            if (e.axis == axis) {
                ++n;
            }
        }

        return n;
    }

    void clear()
    {
        started = 0;
        finished = 0;
        finishedMoved.clear();
        edits.clear();
        labels.clear();
    }
};

void
sendMouse(ColorWheelWidget& wheel,
          QEvent::Type type,
          const QPointF& pos,
          Qt::MouseButton button,
          Qt::MouseButtons buttons,
          Qt::KeyboardModifiers modifiers)
{
    QMouseEvent e(type, pos, wheel.mapToGlobal(pos), button, buttons, modifiers);

    QApplication::sendEvent(&wheel, &e);
}

void
pressAt(ColorWheelWidget& wheel,
        const QPointF& pos,
        Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    sendMouse(wheel, QEvent::MouseButtonPress, pos, Qt::LeftButton, Qt::LeftButton, modifiers);
}

void
moveTo(ColorWheelWidget& wheel,
       const QPointF& pos,
       Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    sendMouse(wheel, QEvent::MouseMove, pos, Qt::NoButton, Qt::LeftButton, modifiers);
}

void
releaseAt(ColorWheelWidget& wheel,
          const QPointF& pos,
          Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    sendMouse(wheel, QEvent::MouseButtonRelease, pos, Qt::LeftButton, Qt::NoButton, modifiers);
}

void
doubleClickAt(ColorWheelWidget& wheel,
              const QPointF& pos)
{
    sendMouse(wheel, QEvent::MouseButtonDblClick, pos, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
}

void
sendKey(QWidget& target,
        QEvent::Type type,
        int key,
        bool autoRepeat = false,
        Qt::KeyboardModifiers modifiers = Qt::NoModifier,
        const QString& text = QString())
{
    QKeyEvent e(type, key, modifiers, text, autoRepeat);

    QApplication::sendEvent(&target, &e);
}

bool
shortcutOverrideAccepted(QWidget& target,
                         int key,
                         Qt::KeyboardModifiers modifiers)
{
    QKeyEvent e(QEvent::ShortcutOverride, key, modifiers);

    e.ignore();
    QApplication::sendEvent(&target, &e);

    return e.isAccepted();
}

// The LockKeyTracker's "pointer is over a wheel" rule reads the real cursor position, so these
// two widgets need distinct, known screen rectangles rather than the default overlapping (0, 0).
void
placeApart(ColorWheelWidget& wheel,
           QLineEdit& lineEdit)
{
    wheel.move(0, 0);
    lineEdit.resize(100, 30);
    lineEdit.move(400, 400);
    lineEdit.show();
    QApplication::processEvents();
}

double
trackballScale(const ColorWheelWidget& wheel)
{
    return wheel.getRim() / wheel.getDiscRadius();
}
} // namespace

TEST(ColorWheelWidget, NeutralCentrePixelIsAchromatic)
{
    ColorWheelWidget wheel;

    setUpWheel(wheel, eColorKnobRoleGain, makeValue(1.0, 1.0, 1.0), TrackballRates().wheelRim);

    QImage disc = wheel.getDiscImage();

    ASSERT_FALSE(disc.isNull());
    QRgb centre = disc.pixel(disc.width() / 2, disc.height() / 2);

    EXPECT_EQ(255, qAlpha(centre));
    expectAchromatic(centre);

    QImage grabbed = wheel.grab().toImage();

    ASSERT_FALSE(grabbed.isNull());
    expectAchromatic(grabbed.pixel(grabbed.width() / 2, grabbed.height() / 2));
}

TEST(ColorWheelWidget, OffsetNeutralCentrePixelIsAchromatic)
{
    ColorWheelWidget wheel;

    setUpWheel(wheel, eColorKnobRoleOffset, makeValue(0.0, 0.0, 0.0), TrackballRates().wheelRim);

    QImage disc = wheel.getDiscImage();

    ASSERT_FALSE(disc.isNull());
    expectAchromatic(disc.pixel(disc.width() / 2, disc.height() / 2));
}

// The ring is a perceptual legend: every hue at the legend's lightness for that hue and at
// nearly the same chroma, whatever the working space and display.
TEST(ColorWheelWidget, RingHuesShareALightnessAndChroma)
{
    const double Ro = 80.0;
    const double Ri = 68.0;
    const double mid = 0.5 * (Ro + Ri);
    const DisplayLegendConverter converters[2] = {
        DisplayLegendConverter(), [](float* rgb, int count) {
            for (int i = 0; i < 3 * count; ++i) {
                rgb[i] = (float)srgbEncode(rgb[i]);
            }
        }
    };

    for (const DisplayLegendConverter& converter : converters) {
        QImage ring = renderColorWheelRing(acescg(), Ro, Ri, 1.0, converter);
        double minL = 1.0;
        double maxL = 0.0;

        ASSERT_FALSE(ring.isNull());
        for (int k = 0; k < 24; ++k) {
            const double angle = (k * 15.0 + 7.0) * kPi / 180.0;
            const QColor c = QColor::fromRgba(ring.pixel((int)std::lround(Ro + mid * std::cos(angle)), (int)std::lround(Ro - mid * std::sin(angle))));
            const double rgb[3] = { srgbDecode(c.redF()), srgbDecode(c.greenF()), srgbDecode(c.blueF()) };
            const OkLab lab = linearSrgbToOkLab(rgb);
            const double C = std::hypot(lab.a, lab.b);

            EXPECT_EQ(255, c.alpha()) << k;
            EXPECT_NEAR(displayLegendHueLightness(std::atan2(lab.b, lab.a)), lab.L, 0.02) << k;
            EXPECT_GT(C, 0.11) << k;
            EXPECT_LT(C, 0.155) << k;
            minL = std::min(minL, lab.L);
            maxL = std::max(maxL, lab.L);
        }
        EXPECT_LT(maxL - minL, 0.13);
    }
}

TEST(ColorWheelWidget, OffDiscWhiteMarkerCaretPointsAtWhite)
{
    ColorWheelWidget wheel;
    KnobColorValue value = makeValue(2.0, 1.0, 1.0);

    setUpWheel(wheel, eColorKnobRoleGain, value, 0.01);

    DiscClamp marker = wheel.getWhiteMarker();

    EXPECT_TRUE(marker.offDisc);

    QPolygonF caret = wheel.getWhiteMarkerCaret();

    ASSERT_EQ(3, caret.size());

    PlanePoint white = whitePoint(toPlane(eColorKnobRoleGain, acescg(), value));
    double expected = planeHue(white);
    QPointF centre = wheel.getDiscCentre();

    EXPECT_LT(angleDifference(screenAngleDegrees(centre, caret[0]), expected), 2.0);
    EXPECT_LT(angleDifference(screenAngleDegrees(centre, wheel.getWhiteMarkerPosition()), expected), 2.0);
    EXPECT_LE(distance(centre, caret[0]), wheel.getDiscRadius() + 0.5);

    saveGrab(wheel, "OffDiscWhite");
}

TEST(ColorWheelWidget, OnDiscWhiteMarkerHasNoCaret)
{
    ColorWheelWidget wheel;

    setUpWheel(wheel, eColorKnobRoleGain, makeValue(1.0, 1.0, 1.0), 0.05);

    EXPECT_FALSE(wheel.getWhiteMarker().offDisc);
    EXPECT_TRUE(wheel.getWhiteMarkerCaret().isEmpty());
    EXPECT_LT(distance(wheel.getWhiteMarkerPosition(), wheel.getDiscCentre()), 0.5);
}

TEST(ColorWheelWidget, ParkedPuckReportsOffDisc)
{
    ColorWheelWidget wheel;
    double rim = 0.05;

    setUpWheel(wheel, eColorKnobRoleGain, makeValue(1.0, 1.0, 1.0), rim);

    EXPECT_FALSE(wheel.getPuck().offDisc);
    EXPECT_LT(distance(wheel.getPuckPosition(), wheel.getDiscCentre()), 0.5);

    wheel.setDragOffset(planePoint(0.5 * rim, 0.0));
    EXPECT_FALSE(wheel.getPuck().offDisc);

    wheel.setDragOffset(planePoint(-3.0 * rim, 2.0 * rim));

    DiscClamp puck = wheel.getPuck();

    EXPECT_TRUE(puck.offDisc);
    EXPECT_NEAR(wheel.getDiscRadius(), distance(wheel.getPuckPosition(), wheel.getDiscCentre()), 0.5);
    EXPECT_LT(angleDifference(screenAngleDegrees(wheel.getDiscCentre(), wheel.getPuckPosition()), planeHue(planePoint(-3.0, 2.0))), 1.0);

    saveGrab(wheel, "ParkedPuck");

    wheel.setValue(makeValue(1.1, 1.0, 0.9));
    EXPECT_FALSE(wheel.hasDragOffset());
    EXPECT_FALSE(wheel.getPuck().offDisc);
}

TEST(ColorWheelWidget, GamutTriangleVerticesMatchMaths)
{
    const KnobColorValue values[2] = { makeValue(1.0, 1.0, 1.0), makeValue(1.2, 1.0, 0.8) };

    for (int k = 0; k < 2; ++k) {
        ColorWheelWidget wheel;
        double rim = 0.5;

        setUpWheel(wheel, eColorKnobRoleGain, values[k], rim);

        GamutTriangle triangle = gamutTriangle(acescg());

        ASSERT_TRUE(triangle.valid);

        PlanePoint anchor = toPlane(eColorKnobRoleGain, acescg(), values[k]);
        QPolygonF drawn = wheel.getGamutTrianglePolygon();

        ASSERT_EQ(3, drawn.size());

        QPointF centre = wheel.getDiscCentre();
        double scale = wheel.getDiscRadius() / rim;

        for (int i = 0; i < 3; ++i) {
            QPointF expected(centre.x() + (triangle.vertex[i].x - anchor.x) * scale,
                             centre.y() - (triangle.vertex[i].y - anchor.y) * scale);

            EXPECT_LT(distance(drawn[i], expected), 1.0) << "value " << k << " vertex " << i;
        }
    }
}

TEST(ColorWheelWidget, WritesGrabPngs)
{
    {
        ColorWheelWidget wheel;

        setUpWheel(wheel, eColorKnobRoleGain, makeValue(1.0, 1.0, 1.0), TrackballRates().wheelRim);
        saveGrab(wheel, "GainNeutral");
    }
    {
        ColorWheelWidget wheel;

        setUpWheel(wheel, eColorKnobRoleGain, makeValue(1.0, 1.0, 1.0), 0.5);
        saveGrab(wheel, "GainWideRim");
    }
    {
        ColorWheelWidget wheel;

        setUpWheel(wheel, eColorKnobRoleGain, makeValue(1.3, 1.0, 0.7), 0.2);
        saveGrab(wheel, "GainWarmAnchor");
    }
    {
        ColorWheelWidget wheel;

        setUpWheel(wheel, eColorKnobRoleOffset, makeValue(0.02, 0.0, -0.01), 0.1);
        saveGrab(wheel, "OffsetAxes");
    }
}

TEST(ColorWheelWidget, TrackballDragEmitsRimOverRadiusPerPixel)
{
    ColorWheelWidget wheel;
    double rim = 0.05;

    setUpWheel(wheel, eColorKnobRoleGain, makeValue(1.0, 1.0, 1.0), rim);

    GestureRecorder rec(wheel);
    QPointF c = wheel.getDiscCentre();

    pressAt(wheel, c);
    EXPECT_EQ(1, rec.started);
    EXPECT_TRUE(rec.edits.empty());
    for (int i = 1; i <= 5; ++i) {
        moveTo(wheel, c + QPointF(10.0 * i, 0.0));
    }

    double expected = 50.0 * trackballScale(wheel);

    EXPECT_EQ(5, rec.count(eColorAxisTrackball));
    EXPECT_EQ((int)rec.edits.size(), rec.count(eColorAxisTrackball));
    EXPECT_NEAR(expected, rec.sum1(eColorAxisTrackball), 1e-12);
    EXPECT_NEAR(0.0, rec.sum2(eColorAxisTrackball), 1e-12);
    EXPECT_NEAR(expected, wheel.getDragOffset().x, 1e-12);

    moveTo(wheel, c + QPointF(50.0, -20.0));
    EXPECT_NEAR(20.0 * trackballScale(wheel), rec.sum2(eColorAxisTrackball), 1e-12);

    releaseAt(wheel, c + QPointF(50.0, -20.0));
    EXPECT_EQ(1, rec.finished);
    ASSERT_EQ(1u, rec.finishedMoved.size());
    EXPECT_TRUE(rec.finishedMoved[0]);
    EXPECT_FALSE(wheel.isDragging());
    EXPECT_FALSE(wheel.hasDragOffset());
}

TEST(ColorWheelWidget, TrackballGearing)
{
    const Qt::KeyboardModifiers modifiers[3] = { Qt::ControlModifier, Qt::ShiftModifier, Qt::ControlModifier | Qt::ShiftModifier };
    const double factors[3] = { 0.1, 4.0, 0.01 };

    for (int k = 0; k < 3; ++k) {
        ColorWheelWidget wheel;

        setUpWheel(wheel, eColorKnobRoleGain, makeValue(1.0, 1.0, 1.0), 0.05);

        GestureRecorder rec(wheel);
        QPointF start = wheel.getDiscCentre() - QPointF(25.0, 0.0);

        pressAt(wheel, start, modifiers[k]);
        moveTo(wheel, start + QPointF(50.0, 0.0), modifiers[k]);
        releaseAt(wheel, start + QPointF(50.0, 0.0), modifiers[k]);

        EXPECT_NEAR(50.0 * trackballScale(wheel) * factors[k], rec.sum1(eColorAxisTrackball), 1e-12) << "gear " << k;
    }
}

TEST(ColorWheelWidget, CtrlPressedMidDragDoesNotJump)
{
    ColorWheelWidget wheel;

    setUpWheel(wheel, eColorKnobRoleGain, makeValue(1.0, 1.0, 1.0), 0.05);

    GestureRecorder rec(wheel);
    QPointF c = wheel.getDiscCentre();
    double k = trackballScale(wheel);

    pressAt(wheel, c);
    moveTo(wheel, c + QPointF(25.0, 0.0));
    moveTo(wheel, c + QPointF(25.0, 0.0), Qt::ControlModifier);
    moveTo(wheel, c + QPointF(50.0, 0.0), Qt::ControlModifier);
    releaseAt(wheel, c + QPointF(50.0, 0.0), Qt::ControlModifier);

    ASSERT_EQ(2u, rec.edits.size());
    EXPECT_NEAR(25.0 * k, rec.edits[0].d1, 1e-12);
    EXPECT_NEAR(2.5 * k, rec.edits[1].d1, 1e-12);
    EXPECT_NEAR(27.5 * k, rec.sum1(eColorAxisTrackball), 1e-12);
}

TEST(ColorWheelWidget, RingClickSetsAbsoluteHueAndDragFollows)
{
    ColorWheelWidget wheel;

    setUpWheel(wheel, eColorKnobRoleGain, makeValue(1.2, 1.0, 0.8), 0.05);

    GestureRecorder rec(wheel);
    QPointF c = wheel.getDiscCentre();
    double r = 0.5 * (wheel.getRingInnerRadius() + wheel.getRingOuterRadius());

    pressAt(wheel, c + QPointF(0.0, -r));
    EXPECT_EQ(1, rec.started);
    ASSERT_EQ(1u, rec.edits.size());
    EXPECT_EQ(eColorAxisHue, rec.edits[0].axis);
    EXPECT_TRUE(rec.edits[0].absolute);
    EXPECT_LT(angleDifference(90.0, rec.edits[0].d1), 1.0);

    moveTo(wheel, c + QPointF(-r, 0.0));
    ASSERT_EQ(2u, rec.edits.size());
    EXPECT_EQ(eColorAxisHue, rec.edits[1].axis);
    EXPECT_TRUE(rec.edits[1].absolute);
    EXPECT_LT(angleDifference(180.0, rec.edits[1].d1), 1.0);

    releaseAt(wheel, c + QPointF(-r, 0.0));
    EXPECT_EQ(1, rec.finished);
    EXPECT_FALSE(wheel.hasDragOffset());
}

TEST(ColorWheelWidget, PressOutsideWheelIsIgnored)
{
    ColorWheelWidget wheel;

    setUpWheel(wheel, eColorKnobRoleGain, makeValue(1.0, 1.0, 1.0), 0.05);

    GestureRecorder rec(wheel);

    pressAt(wheel, QPointF(1.0, 1.0));
    moveTo(wheel, QPointF(20.0, 1.0));
    releaseAt(wheel, QPointF(20.0, 1.0));
    EXPECT_EQ(0, rec.started);
    EXPECT_TRUE(rec.edits.empty());
}

TEST(ColorWheelWidget, VBeforePressChangesOnlyValueWithFocusElsewhere)
{
    ColorWheelWidget wheel;
    QLineEdit other;

    setUpWheel(wheel, eColorKnobRoleGain, makeValue(1.0, 1.0, 1.0), 0.05);
    other.show();
    other.setFocus();

    GestureRecorder rec(wheel);
    QPointF c = wheel.getDiscCentre();

    sendKey(other, QEvent::KeyPress, Qt::Key_V);
    EXPECT_EQ(eColorAxisValue, wheel.getActiveLock());

    pressAt(wheel, c);
    moveTo(wheel, c + QPointF(10.0, 5.0));
    moveTo(wheel, c + QPointF(25.0, -15.0));
    releaseAt(wheel, c + QPointF(25.0, -15.0));
    sendKey(other, QEvent::KeyRelease, Qt::Key_V);

    ASSERT_FALSE(rec.edits.empty());
    EXPECT_EQ((int)rec.edits.size(), rec.count(eColorAxisValue));
    EXPECT_EQ(0, rec.count(eColorAxisTrackball));
    EXPECT_EQ(eColorAxisTrackball, wheel.getActiveLock());
}

TEST(ColorWheelWidget, VMidDragSwitchesAxisAndReleaseSwitchesBack)
{
    ColorWheelWidget wheel;

    setUpWheel(wheel, eColorKnobRoleGain, makeValue(1.0, 1.0, 1.0), 0.05);

    GestureRecorder rec(wheel);
    QPointF c = wheel.getDiscCentre();

    pressAt(wheel, c);
    moveTo(wheel, c + QPointF(10.0, 0.0));
    sendKey(wheel, QEvent::KeyPress, Qt::Key_V);
    moveTo(wheel, c + QPointF(10.0, -10.0));
    sendKey(wheel, QEvent::KeyRelease, Qt::Key_V);
    moveTo(wheel, c + QPointF(20.0, -10.0));
    releaseAt(wheel, c + QPointF(20.0, -10.0));

    ASSERT_EQ(3u, rec.edits.size());
    EXPECT_EQ(eColorAxisTrackball, rec.edits[0].axis);
    EXPECT_EQ(eColorAxisValue, rec.edits[1].axis);
    EXPECT_EQ(eColorAxisTrackball, rec.edits[2].axis);
    EXPECT_EQ(1, rec.started);
    EXPECT_EQ(1, rec.finished);
}

TEST(ColorWheelWidget, VTakesEitherAxisByTheSameAmount)
{
    TrackballRates rates;
    double rate = valueRatePerPixel(rates, eColorKnobRoleGain);

    {
        ColorWheelWidget wheel;

        setUpWheel(wheel, eColorKnobRoleGain, makeValue(1.0, 1.0, 1.0), 0.05);

        GestureRecorder rec(wheel);
        QPointF c = wheel.getDiscCentre();

        sendKey(wheel, QEvent::KeyPress, Qt::Key_V);
        pressAt(wheel, c);
        moveTo(wheel, c + QPointF(40.0, 0.0));
        releaseAt(wheel, c + QPointF(40.0, 0.0));
        sendKey(wheel, QEvent::KeyRelease, Qt::Key_V);

        EXPECT_NEAR(40.0 * rate, rec.sum1(eColorAxisValue), 1e-12);
    }
    {
        ColorWheelWidget wheel;

        setUpWheel(wheel, eColorKnobRoleGain, makeValue(1.0, 1.0, 1.0), 0.05);

        GestureRecorder rec(wheel);
        QPointF c = wheel.getDiscCentre();

        sendKey(wheel, QEvent::KeyPress, Qt::Key_V);
        pressAt(wheel, c);
        moveTo(wheel, c + QPointF(0.0, -40.0));
        releaseAt(wheel, c + QPointF(0.0, -40.0));
        sendKey(wheel, QEvent::KeyRelease, Qt::Key_V);

        EXPECT_NEAR(40.0 * rate, rec.sum1(eColorAxisValue), 1e-12);
    }
}

TEST(ColorWheelWidget, TBeforePressGivesTempTintWithFocusElsewhere)
{
    ColorWheelWidget wheel;
    QLineEdit other;

    setUpWheel(wheel, eColorKnobRoleGain, makeValue(1.0, 1.0, 1.0), 0.05);
    other.show();
    other.setFocus();

    GestureRecorder rec(wheel);
    QPointF c = wheel.getDiscCentre();
    TrackballRates rates;

    sendKey(other, QEvent::KeyPress, Qt::Key_T);
    pressAt(wheel, c);
    moveTo(wheel, c + QPointF(10.0, -6.0));
    releaseAt(wheel, c + QPointF(10.0, -6.0));
    sendKey(other, QEvent::KeyRelease, Qt::Key_T);

    ASSERT_EQ(1u, rec.edits.size());
    EXPECT_EQ(eColorAxisTempTint, rec.edits[0].axis);
    EXPECT_FALSE(rec.edits[0].absolute);
    EXPECT_NEAR(10.0 * tempRatePerPixel(rates, eColorKnobRoleGain), rec.edits[0].d1, 1e-12);
    EXPECT_NEAR(6.0 * tintRatePerPixel(rates, eColorKnobRoleGain), rec.edits[0].d2, 1e-12);
}

TEST(ColorWheelWidget, LastPressedLockKeyWins)
{
    ColorWheelWidget wheel;

    setUpWheel(wheel, eColorKnobRoleGain, makeValue(1.0, 1.0, 1.0), 0.05);

    sendKey(wheel, QEvent::KeyPress, Qt::Key_T);
    EXPECT_EQ(eColorAxisTempTint, wheel.getActiveLock());
    sendKey(wheel, QEvent::KeyPress, Qt::Key_V);
    EXPECT_EQ(eColorAxisValue, wheel.getActiveLock());
    sendKey(wheel, QEvent::KeyRelease, Qt::Key_V);
    EXPECT_EQ(eColorAxisTempTint, wheel.getActiveLock());
    sendKey(wheel, QEvent::KeyRelease, Qt::Key_T);
    EXPECT_EQ(eColorAxisTrackball, wheel.getActiveLock());
}

TEST(ColorWheelWidget, AutoRepeatLockKeyIsIgnored)
{
    ColorWheelWidget wheel;

    setUpWheel(wheel, eColorKnobRoleGain, makeValue(1.0, 1.0, 1.0), 0.05);

    GestureRecorder rec(wheel);
    QPointF c = wheel.getDiscCentre();

    sendKey(wheel, QEvent::KeyPress, Qt::Key_V, true);
    EXPECT_EQ(eColorAxisTrackball, wheel.getActiveLock());

    pressAt(wheel, c);
    moveTo(wheel, c + QPointF(0.0, -10.0));
    releaseAt(wheel, c + QPointF(0.0, -10.0));
    ASSERT_EQ(1u, rec.edits.size());
    EXPECT_EQ(eColorAxisTrackball, rec.edits[0].axis);

    sendKey(wheel, QEvent::KeyPress, Qt::Key_V);
    sendKey(wheel, QEvent::KeyRelease, Qt::Key_V, true);
    sendKey(wheel, QEvent::KeyPress, Qt::Key_V, true);
    EXPECT_EQ(eColorAxisValue, wheel.getActiveLock());
    sendKey(wheel, QEvent::KeyRelease, Qt::Key_V);
    EXPECT_EQ(eColorAxisTrackball, wheel.getActiveLock());
}

TEST(ColorWheelWidget, CtrlLockKeysAreNeverOverriddenWhileIdle)
{
    ColorWheelWidget wheel;
    // A plain widget rather than a QLineEdit, which accepts the override for Ctrl+V (paste) on
    // its own and would hide whether the tracker claimed it.
    QWidget other;

    setUpWheel(wheel, eColorKnobRoleGain, makeValue(1.0, 1.0, 1.0), 0.05);
    other.show();

    EXPECT_FALSE(shortcutOverrideAccepted(other, Qt::Key_V, Qt::ControlModifier));
    EXPECT_FALSE(shortcutOverrideAccepted(other, Qt::Key_T, Qt::ControlModifier));
    EXPECT_FALSE(shortcutOverrideAccepted(wheel, Qt::Key_V, Qt::ControlModifier));
    EXPECT_FALSE(shortcutOverrideAccepted(wheel, Qt::Key_X, Qt::NoModifier));
}

TEST(ColorWheelWidget, ShortcutOverrideMidDragAcceptsGearAndEscape)
{
    ColorWheelWidget wheel;

    setUpWheel(wheel, eColorKnobRoleGain, makeValue(1.0, 1.0, 1.0), 0.05);

    QPointF c = wheel.getDiscCentre();

    EXPECT_FALSE(shortcutOverrideAccepted(wheel, Qt::Key_X, Qt::NoModifier));

    pressAt(wheel, c);
    EXPECT_TRUE(shortcutOverrideAccepted(wheel, Qt::Key_V, Qt::NoModifier));
    EXPECT_TRUE(shortcutOverrideAccepted(wheel, Qt::Key_V, Qt::ControlModifier));
    EXPECT_TRUE(shortcutOverrideAccepted(wheel, Qt::Key_Escape, Qt::NoModifier));
    EXPECT_FALSE(shortcutOverrideAccepted(wheel, Qt::Key_X, Qt::NoModifier));
    releaseAt(wheel, c);
}

TEST(ColorWheelWidget, PointerOverWheelLetsVReachAFocusedTextField)
{
    ColorWheelWidget wheel;
    QLineEdit other;

    setUpWheel(wheel, eColorKnobRoleGain, makeValue(1.0, 1.0, 1.0), 0.05);
    placeApart(wheel, other);
    other.setFocus();

    QCursor::setPos(wheel.mapToGlobal(wheel.getDiscCentre().toPoint()));

    sendKey(other, QEvent::KeyPress, Qt::Key_V, false, Qt::NoModifier, QString::fromUtf8("v"));
    sendKey(other, QEvent::KeyRelease, Qt::Key_V);

    EXPECT_EQ(QString::fromUtf8("v"), other.text());
}

TEST(ColorWheelWidget, PointerOverWheelClaimsVFromAWidgetThatIsNotATextField)
{
    ColorWheelWidget wheel;
    QLineEdit away;

    setUpWheel(wheel, eColorKnobRoleGain, makeValue(1.0, 1.0, 1.0), 0.05);
    placeApart(wheel, away);

    QCursor::setPos(wheel.mapToGlobal(wheel.getDiscCentre().toPoint()));

    QWidget bystander;

    EXPECT_TRUE(shortcutOverrideAccepted(bystander, Qt::Key_V, Qt::NoModifier));
}

TEST(ColorWheelWidget, HidingOrDeletingAWheelMidDragEndsTheDrag)
{
    QLineEdit away;
    QWidget bystander;

    {
        ColorWheelWidget wheel;

        setUpWheel(wheel, eColorKnobRoleGain, makeValue(1.0, 1.0, 1.0), 0.05);
        placeApart(wheel, away);
        QCursor::setPos(away.mapToGlobal(QPoint(10, 10)));

        GestureRecorder rec(wheel);
        QPointF c = wheel.getDiscCentre();

        pressAt(wheel, c);
        moveTo(wheel, c + QPointF(10.0, 0.0));
        EXPECT_TRUE(shortcutOverrideAccepted(bystander, Qt::Key_V, Qt::NoModifier));
        wheel.hide();

        EXPECT_FALSE(wheel.isDragging());
        EXPECT_EQ(1, rec.finished);
        EXPECT_FALSE(shortcutOverrideAccepted(bystander, Qt::Key_V, Qt::NoModifier));
    }

    ColorWheelWidget* doomed = new ColorWheelWidget();

    setUpWheel(*doomed, eColorKnobRoleGain, makeValue(1.0, 1.0, 1.0), 0.05);
    placeApart(*doomed, away);
    QCursor::setPos(away.mapToGlobal(QPoint(10, 10)));
    pressAt(*doomed, doomed->getDiscCentre());
    EXPECT_TRUE(shortcutOverrideAccepted(bystander, Qt::Key_V, Qt::NoModifier));
    delete doomed;
    EXPECT_FALSE(shortcutOverrideAccepted(bystander, Qt::Key_V, Qt::NoModifier));
}

TEST(ColorWheelWidget, PointerAwayFromWheelLetsVReachTheFocusedLineEdit)
{
    ColorWheelWidget wheel;
    QLineEdit other;

    setUpWheel(wheel, eColorKnobRoleGain, makeValue(1.0, 1.0, 1.0), 0.05);
    placeApart(wheel, other);
    other.setFocus();

    QCursor::setPos(other.mapToGlobal(QPoint(10, 10)));

    // A QLineEdit accepts the override for any printable key on its own, so the tracker's
    // part is read on a plain widget.
    QWidget bystander;

    EXPECT_FALSE(shortcutOverrideAccepted(bystander, Qt::Key_V, Qt::NoModifier));
    sendKey(other, QEvent::KeyPress, Qt::Key_V, false, Qt::NoModifier, QString::fromUtf8("v"));
    sendKey(other, QEvent::KeyRelease, Qt::Key_V);

    EXPECT_EQ(QString::fromUtf8("v"), other.text());
}

TEST(ColorWheelWidget, CtrlScrollZoomsRimOnly)
{
    ColorWheelWidget wheel;
    KnobColorValue value = makeValue(1.2, 1.0, 0.8);
    double rim = 0.05;

    setUpWheel(wheel, eColorKnobRoleGain, value, rim);

    GestureRecorder rec(wheel);
    QPointF c = wheel.getDiscCentre();
    QWheelEvent e(c, wheel.mapToGlobal(c), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);

    QApplication::sendEvent(&wheel, &e);

    EXPECT_TRUE(e.isAccepted());
    EXPECT_NEAR(rim * TrackballRates().zoomFactorPerNotch, wheel.getRim(), 1e-15);
    EXPECT_EQ(0, rec.started);
    EXPECT_TRUE(rec.edits.empty());
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(value.rgb[i], wheel.getValue().rgb[i]);
    }
}

TEST(ColorWheelWidget, BareScrollOverAnUnfocusedWheelIsLeftForTheScrollArea)
{
    ColorWheelWidget wheel;
    double rim = 0.05;

    setUpWheel(wheel, eColorKnobRoleGain, makeValue(1.2, 1.0, 0.8), rim);
    wheel.clearFocus();
    ASSERT_FALSE(wheel.hasFocus());

    QPointF c = wheel.getDiscCentre();
    QWheelEvent e(c, wheel.mapToGlobal(c), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);

    QApplication::sendEvent(&wheel, &e);

    EXPECT_FALSE(e.isAccepted());
    EXPECT_EQ(rim, wheel.getRim());
}

TEST(ColorWheelWidget, DoubleClickEmitsChromaReset)
{
    ColorWheelWidget wheel;

    setUpWheel(wheel, eColorKnobRoleGain, makeValue(1.2, 1.0, 0.8), 0.05);

    GestureRecorder rec(wheel);
    QPointF p = wheel.getDiscCentre() + QPointF(5.0, 5.0);

    pressAt(wheel, p);
    releaseAt(wheel, p);
    doubleClickAt(wheel, p);
    releaseAt(wheel, p);

    ASSERT_EQ(1u, rec.edits.size());
    EXPECT_EQ(eColorAxisSat, rec.edits[0].axis);
    EXPECT_TRUE(rec.edits[0].absolute);
    EXPECT_EQ(0.0, rec.edits[0].d1);
    EXPECT_EQ(2, rec.started);
    EXPECT_EQ(2, rec.finished);
    ASSERT_EQ(2u, rec.finishedMoved.size());
    EXPECT_FALSE(rec.finishedMoved[0]);
    EXPECT_TRUE(rec.finishedMoved[1]);
}

TEST(ColorWheelWidget, EscapeEndsDragWithoutCancel)
{
    ColorWheelWidget wheel;

    setUpWheel(wheel, eColorKnobRoleGain, makeValue(1.0, 1.0, 1.0), 0.05);

    GestureRecorder rec(wheel);
    QPointF c = wheel.getDiscCentre();

    pressAt(wheel, c);
    moveTo(wheel, c + QPointF(10.0, 0.0));
    sendKey(wheel, QEvent::KeyPress, Qt::Key_Escape);
    EXPECT_FALSE(wheel.isDragging());
    EXPECT_EQ(1, rec.finished);
    ASSERT_EQ(1u, rec.finishedMoved.size());
    EXPECT_TRUE(rec.finishedMoved[0]);

    moveTo(wheel, c + QPointF(20.0, 0.0));
    releaseAt(wheel, c + QPointF(20.0, 0.0));
    EXPECT_EQ(1u, rec.edits.size());
    EXPECT_EQ(1, rec.finished);
}

TEST(ColorWheelWidget, IsSquareBetweenItsFloorAndCap)
{
    ColorWheelWidget wheel;

    EXPECT_EQ((int)TO_DPIX(160.), ColorWheelWidget::minimumDiameter());
    EXPECT_EQ((int)TO_DPIX(320.), ColorWheelWidget::maximumDiameter());
    EXPECT_TRUE(wheel.hasHeightForWidth());
    EXPECT_TRUE(wheel.sizePolicy().hasHeightForWidth());
    EXPECT_EQ(250, wheel.heightForWidth(250));
    EXPECT_EQ(QSize(ColorWheelWidget::minimumDiameter(), ColorWheelWidget::minimumDiameter()), wheel.minimumSizeHint());

    wheel.setPreferredDiameter(10000);
    EXPECT_EQ(ColorWheelWidget::maximumDiameter(), wheel.getPreferredDiameter());
    EXPECT_EQ(QSize(ColorWheelWidget::maximumDiameter(), ColorWheelWidget::maximumDiameter()), wheel.sizeHint());
    wheel.setPreferredDiameter(10);
    EXPECT_EQ(ColorWheelWidget::minimumDiameter(), wheel.getPreferredDiameter());
}

TEST(ColorWheelWidget, DiscFollowsTheShortSideAndStaysCentred)
{
    ColorWheelWidget square;

    setUpWheel(square, eColorKnobRoleGain, makeValue(1.0, 1.0, 1.0), 0.2);
    QApplication::processEvents();

    // The gap between the widget's edge and the outer ring, whatever the DPI.
    double margin = kWidgetSize / 2.0 - square.getRingOuterRadius();

    ASSERT_GE(margin, 0.0);

    ColorWheelWidget wide;

    setUpWheel(wide, eColorKnobRoleGain, makeValue(1.0, 1.0, 1.0), 0.2);
    wide.resize(300, 220);
    QApplication::processEvents();
    ASSERT_EQ(QSize(300, 220), wide.size());

    EXPECT_NEAR(220 / 2.0 - margin, wide.getRingOuterRadius(), 1.0);
    EXPECT_NEAR(150.0, wide.getDiscCentre().x(), 1.0);
    EXPECT_NEAR(110.0, wide.getDiscCentre().y(), 1.0);

    // The disc image is rendered at the new size on the next read, not left at the old one.
    QImage disc = wide.getDiscImage();

    EXPECT_NEAR(2.0 * wide.getDiscRadius() * disc.devicePixelRatio(), disc.width(), 2.0);
}

TEST(ColorWheelWidget, RingWidthScalesWithTheWheel)
{
    const int sides[4] = { 120, 160, 240, 320 };
    double ratio = 0.0;

    for (int i = 0; i < 4; ++i) {
        ColorWheelWidget wheel;

        setUpWheel(wheel, eColorKnobRoleGain, makeValue(1.0, 1.0, 1.0), 0.2);
        wheel.resize(sides[i], sides[i]);
        QApplication::processEvents();
        ASSERT_EQ(QSize(sides[i], sides[i]), wheel.size());

        const double outer = wheel.getRingOuterRadius();
        const double width = outer - wheel.getRingInnerRadius();

        ASSERT_GT(outer, 0.0);
        if (i == 0) {
            ratio = width / outer;
            EXPECT_GT(width, 0.0);
            continue;
        }
        EXPECT_NEAR(ratio * outer, width, 1.0) << "side " << sides[i];
    }

    // The ring at the largest size is visibly wider than at the smallest.
    ColorWheelWidget small, large;

    setUpWheel(small, eColorKnobRoleGain, makeValue(1.0, 1.0, 1.0), 0.2);
    setUpWheel(large, eColorKnobRoleGain, makeValue(1.0, 1.0, 1.0), 0.2);
    small.resize(120, 120);
    large.resize(320, 320);
    QApplication::processEvents();
    EXPECT_GT(large.getRingOuterRadius() - large.getRingInnerRadius(), 1.5 * (small.getRingOuterRadius() - small.getRingInnerRadius()));

    // A press near the ring's inner edge on the large wheel lands on the ring, not the disc.
    GestureRecorder rec(large);
    const QPointF c = large.getDiscCentre();
    const double r = large.getRingInnerRadius() + 2.0;

    ASSERT_GT(r, large.getDiscRadius());
    pressAt(large, c + QPointF(0.0, -r));
    ASSERT_EQ(1u, rec.edits.size());
    EXPECT_EQ(eColorAxisHue, rec.edits[0].axis);
    EXPECT_TRUE(rec.edits[0].absolute);
    releaseAt(large, c + QPointF(0.0, -r));
}
