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
#include <QWidget>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/ColorWheelMath.h"

class QPainter;
class QPaintEvent;

NATRON_NAMESPACE_ENTER

/**
 * @brief The trackball colour wheel: a disc of the chroma plane re-centred on the knob's current
 * plane point (the anchor), an absolute hue ring about white around it, the gamut triangle, the
 * Planckian locus (ratio roles) or linear temp/tint axes (Offset), the neutral white marker and
 * the puck. It knows nothing about knobs: the owner feeds it a basis, a role, a value and a rim,
 * and every overlay is drawn relative to the anchor so a re-centre is just another setValue.
 *
 * Plane coordinates passed in or out (drag offset, overlay points) are wheel-relative, i.e.
 * already minus the anchor, matching whitePoint and drawLocus.
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

    virtual QSize sizeHint() const OVERRIDE;
    virtual QSize minimumSizeHint() const OVERRIDE;

protected:
    virtual void paintEvent(QPaintEvent* e) OVERRIDE;

private:
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
    void buildHueTable(std::vector<float>& table) const;
    const QImage& discImage() const;
    const QImage& ringImage() const;
    QColor activeColour() const;

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

    mutable DiscCacheKey _discKey;
    mutable QImage _disc;
    mutable RingCacheKey _ringKey;
    mutable QImage _ring;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_COLORWHEELWIDGET_H
