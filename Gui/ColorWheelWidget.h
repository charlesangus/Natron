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

#ifndef NATRON_GUI_COLORWHEELWIDGET_H
#define NATRON_GUI_COLORWHEELWIDGET_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <vector>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QColor>
#include <QImage>
#include <QPointF>
#include <QPolygonF>
#include <QSize>
#include <QString>
#include <QWidget>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/ColorWheelMath.h"

class QEvent;
class QHideEvent;
class QKeyEvent;
class QMouseEvent;
class QPainter;
class QPaintEvent;
class QWheelEvent;

NATRON_NAMESPACE_ENTER

/**
 * @brief The hue ring ColorWheelWidget draws around its disc, shared with every other disc that
 * wears the same ring. For a widget of size size, the ring's outer edge sits a small margin in
 * from the shorter side's half, the ring's width is a fixed fraction of that outer radius (never
 * below a small legibility floor), and the disc starts a small gap inside it; all three are
 * DPI-scaled widget pixels.
 **/
double colorWheelRingOuterRadius(const QSize& size);
double colorWheelRingInnerRadius(const QSize& size);
double colorWheelDiscRadius(const QSize& size);

/**
 * @brief The ring's hues, one entry per degree of plane hue (0 = +x, counter-clockwise): rgb
 * holds the working-space colour a quarter of the way from mid grey to the gamut edge, which
 * only gives the hue its direction, and limit the gamut edge's plane radius itself. Not
 * display-converted.
 **/
struct ColorWheelHueTable {
    std::vector<float> rgb;
    std::vector<double> limit;
};

constexpr int kColorWheelHueTableSize = 360;

ColorWheelHueTable buildColorWheelHueTable(const LumaBasis& basis);

/**
 * @brief The table entry nearest to degrees, any angle accepted.
 **/
int colorWheelHueIndex(double degrees);

/**
 * @brief The anti-aliased ring between innerRadius and outerRadius (widget pixels), rendered at
 * dpr into a square image 2 * outerRadius wide whose centre is the ring's. Each hue is the
 * table's direction as converter displays it (see displayLegendHues), drawn at the legend's
 * lightness and chroma (DisplayLegendColour.h).
 **/
QImage renderColorWheelRing(const LumaBasis& basis, double outerRadius, double innerRadius, double dpr, const std::function<void(float* rgb, int count)>& converter);

/**
 * @brief The dark hairlines at the disc's edge and both edges of the ring.
 **/
void drawColorWheelRingOutlines(QPainter& p, const QPointF& centre, double discRadius, double innerRadius, double outerRadius);

/**
 * @brief The trackball colour wheel: a disc of the chroma plane re-centred on the knob's current
 * plane point (the anchor), an absolute hue ring about white around it, the gamut triangle, the
 * Planckian locus (ratio roles) or linear temp/tint axes (Offset), the neutral white marker and
 * the puck. It knows nothing about knobs: the owner feeds it a basis, a role, a value and a rim,
 * and every overlay is drawn relative to the anchor so a re-centre is just another setValue.
 *
 * Plane coordinates passed in or out (drag offset, overlay points) are wheel-relative, i.e.
 * already minus the anchor, matching whitePoint and drawLocus.
 *
 * Input: a left press in the disc starts a relative trackball drag (Ctrl/Shift gearing read on
 * every move), a press on the hue ring sets the hue to the angle under the cursor and keeps
 * doing so while dragged, holding V or T, whether pressed before the drag starts or during it,
 * turns a disc drag into a value (either axis) or a temp/tint drag, the scroll wheel zooms the
 * rim while the wheel has focus, is being dragged or Ctrl is held, and a double-click resets
 * the chroma. Hiding the wheel mid-drag finishes the drag. Each gesture is bracketed by gestureStarted and
 * gestureFinished, with one gestureMoved per step carrying a delta (absolute for the ring and
 * the reset); the owner applies the edits, makes the gesture one undo step, and calls setValue
 * on finish to re-centre the wheel.
 **/
class ColorWheelWidget
    : public QWidget {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    /**
     * @brief Converts count packed working-space RGB triplets in place to the display-referred
     * colour picking space; the result is clamped to [0, 1] and quantised to 8 bits.
     **/
    typedef std::function<void(float* rgb, int count)> DisplayConverter;

    explicit ColorWheelWidget(QWidget* parent = nullptr);
    virtual ~ColorWheelWidget() OVERRIDE;

    void setBasis(const LumaBasis& basis);
    void setRole(ColorKnobRoleEnum role);

    /**
     * @brief Re-anchors the wheel on value's plane point and returns the puck to the centre.
     **/
    void setValue(const KnobColorValue& value);
    void setRim(double rim);

    /**
     * @brief An empty converter means identity.
     **/
    void setDisplayConverter(const DisplayConverter& converter);
    const DisplayConverter& getDisplayConverter() const;

    /**
     * @brief Moves the puck to dq away from the anchor, keeping the frame fixed; past the rim the
     * puck parks on the edge. clearDragOffset returns it to the centre at rest.
     **/
    void setDragOffset(const PlanePoint& dq);
    void clearDragOffset();

    const LumaBasis& getBasis() const;
    ColorKnobRoleEnum getRole() const;
    const KnobColorValue& getValue() const;
    double getRim() const;

    /**
     * @brief The value's absolute plane point; when the value has none (a frozen ratio role) the
     * wheel is drawn about white and this reports valid false.
     **/
    PlanePoint getAnchor() const;
    PlanePoint getDragOffset() const;
    bool hasDragOffset() const;

    QPointF getDiscCentre() const;
    double getDiscRadius() const;
    double getRingInnerRadius() const;
    double getRingOuterRadius() const;

    QPointF planeToWidget(const PlanePoint& p) const;
    PlanePoint widgetToPlane(const QPointF& pos) const;

    DiscClamp getWhiteMarker() const;
    QPointF getWhiteMarkerPosition() const;

    /**
     * @brief The caret drawn when white is off the disc, tip first; empty otherwise.
     **/
    QPolygonF getWhiteMarkerCaret() const;

    DiscClamp getPuck() const;
    QPointF getPuckPosition() const;

    /**
     * @brief The gamut triangle as drawn, in widget coordinates; empty on a matrix-less basis.
     **/
    QPolygonF getGamutTrianglePolygon() const;

    /**
     * @brief The rendered disc (alpha 0 outside it), one pixel per device pixel, its top-left
     * corner at getDiscCentre() minus getDiscRadius() on both axes.
     **/
    QImage getDiscImage() const;

    /**
     * @brief The held-key lock a disc drag would use right now: eColorAxisValue (V),
     * eColorAxisTempTint (T), or eColorAxisTrackball when neither is held.
     **/
    ColorAxisEnum getActiveLock() const;
    bool isDragging() const;

    /**
     * @brief The wheel is square: its floor and its cap (both DPI-scaled) bound the preferred
     * diameter the owner sets, and the disc is centred in whatever rect it ends up with.
     * maximumDiameter() is the default cap; an instance can be given its own with
     * setMaximumDiameter().
     **/
    static int minimumDiameter();
    static int maximumDiameter();

    /**
     * @brief This wheel's cap, never below minimumDiameter(). The preferred diameter is
     * re-clamped to it.
     **/
    void setMaximumDiameter(int pixels);
    int getMaximumDiameter() const;

    /**
     * @brief The width the owner can spare, clamped to [minimumDiameter, getMaximumDiameter];
     * it becomes the sizeHint. The disc images re-render on the next paint at the new size.
     **/
    void setPreferredDiameter(int pixels);
    int getPreferredDiameter() const;

    virtual QSize sizeHint() const OVERRIDE;
    virtual QSize minimumSizeHint() const OVERRIDE;
    virtual bool hasHeightForWidth() const OVERRIDE;
    virtual int heightForWidth(int width) const OVERRIDE;

Q_SIGNALS:
    void gestureStarted();

    /**
     * @brief One step of the current gesture, ready for applyEdit: a relative Trackball (d1, d2 =
     * plane delta), Value or TempTint step from a disc drag, an absolute Hue from the ring, or an
     * absolute Sat of 0 for a double-click reset. axisLabel names the step for the undo text.
     **/
    void gestureMoved(ColorEdit edit, QString axisLabel);

    /**
     * @brief moved is false when no gestureMoved was emitted since gestureStarted. Esc during a
     * drag finishes it like a release does: there is no cancel, undo covers that.
     **/
    void gestureFinished(bool moved);

protected:
    virtual void paintEvent(QPaintEvent* e) OVERRIDE;
    virtual bool event(QEvent* e) OVERRIDE;
    virtual void mousePressEvent(QMouseEvent* e) OVERRIDE;
    virtual void mouseMoveEvent(QMouseEvent* e) OVERRIDE;
    virtual void mouseReleaseEvent(QMouseEvent* e) OVERRIDE;
    virtual void mouseDoubleClickEvent(QMouseEvent* e) OVERRIDE;
    virtual void wheelEvent(QWheelEvent* e) OVERRIDE;
    virtual void keyPressEvent(QKeyEvent* e) OVERRIDE;
    virtual void hideEvent(QHideEvent* e) OVERRIDE;

private:
    enum HitZoneEnum {
        eHitZoneNone,
        eHitZoneDisc,
        eHitZoneRing
    };

    struct DiscCacheKey {
        QSize size;
        double rim = 0.0;
        ColorKnobRoleEnum role = eColorKnobRoleAuto;
        double anchorX = 0.0;
        double anchorY = 0.0;
        bool anchorLinear = false;
        unsigned basisGeneration = 0;
        unsigned converterGeneration = 0;
        bool valid = false;

        bool operator==(const DiscCacheKey& other) const;
    };

    struct RingCacheKey {
        QSize size;
        unsigned basisGeneration = 0;
        unsigned converterGeneration = 0;
        bool valid = false;

        bool operator==(const RingCacheKey& other) const;
    };

    void updateAnchor();
    double devicePixelRatioForCache() const;
    void convertToDisplay(std::vector<float>& rgb) const;
    bool discColour(const PlanePoint& q, double rgb[3]) const;
    const QImage& discImage() const;
    const QImage& ringImage() const;
    QColor activeColour() const;

    HitZoneEnum hitTest(const QPointF& pos) const;
    double ringAngleAt(const QPointF& pos) const;
    void emitStep(const ColorEdit& edit, const QString& axisLabel);
    void emitRingHue(const QPointF& pos);
    void dragTo(const QPointF& pos, Qt::KeyboardModifiers modifiers);
    void finishGesture();

    void drawOverlays(QPainter& p) const;
    void drawWhiteMarker(QPainter& p) const;
    void drawPuck(QPainter& p) const;

    LumaBasis _basis;
    ColorKnobRoleEnum _role;
    KnobColorValue _value;
    PlanePoint _anchor;
    double _rim;
    DisplayConverter _converter;
    PlanePoint _dragOffset;
    bool _hasDragOffset;
    unsigned _basisGeneration;
    unsigned _converterGeneration;
    int _preferredDiameter;
    int _maxDiameter;

    HitZoneEnum _dragZone;
    QPointF _lastDragPos;
    PlanePoint _gestureOffset;
    bool _gestureMoved;

    mutable DiscCacheKey _discKey;
    mutable QImage _disc;
    mutable RingCacheKey _ringKey;
    mutable QImage _ring;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_COLORWHEELWIDGET_H
