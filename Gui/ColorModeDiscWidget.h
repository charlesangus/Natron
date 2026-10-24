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

#ifndef NATRON_GUI_COLORMODEDISCWIDGET_H
#define NATRON_GUI_COLORMODEDISCWIDGET_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QImage>
#include <QPointF>
#include <QSize>
#include <QWidget>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/ColorWheelMath.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The reveal graphic of the S, T and V global trackball modes: a disc the size of its
 * rect, laid out like ColorWheelWidget's, showing what a drag towards each point does, and a
 * small ring marker for the puck.
 *
 * Ring and fill are one function of the drag direction: the colour a drag that way produces,
 * with the ring holding the colour at the disc's rim in that direction. Directions follow the
 * gesture's sign in globalModeEdit. S runs from the centre hue -30 degrees on the left to +30
 * degrees on the right, with full saturation at the top and white at the bottom, and follows
 * the live value's hue. T runs cool blue on the left to warm orange on the right and green at
 * the bottom to magenta at the top. V runs black at the bottom and left to white at the top and
 * right. The colours come from ColorWheelMath and pass through the display converter. C has no
 * gradient: the C reveal is the real ColorWheelWidget, and C here draws the wheel's ring and an
 * empty disc.
 *
 * The puck is a position in [-1, 1]^2 (x right, y up) and is clamped to the disc's rim. The
 * images are cached per size, mode and, in S, centre hue to the degree, and rebuilt lazily on
 * the next paint.
 **/
class ColorModeDiscWidget
    : public QWidget {
public:
    /**
     * @brief Converts count packed working-space RGB triplets in place to display colour; the
     * result is clamped to [0, 1]. An empty converter means identity.
     **/
    typedef std::function<void(float* rgb, int count)> DisplayConverter;

    explicit ColorModeDiscWidget(QWidget* parent = nullptr);
    virtual ~ColorModeDiscWidget() OVERRIDE;

    void setMode(ColorGlobalModeEnum mode);
    ColorGlobalModeEnum getMode() const { return _mode; }

    void setBasis(const LumaBasis& basis);
    void setDisplayConverter(const DisplayConverter& converter);

    /**
     * @brief Centres the S disc on value's hue. A value without a hue (saturation about 0)
     * keeps the last hue a value had since forgetHue(), or 0 degrees if none had one.
     **/
    void setCentreValue(ColorKnobRoleEnum role, const KnobColorValue& value);
    void forgetHue();

    /**
     * @brief The S disc's centre hue in plane degrees, rounded to a whole degree in [0, 360).
     **/
    void setCentreHue(double degrees);
    int getCentreHue() const { return _hue; }

    /**
     * @brief Sets the puck, clamped to the unit disc.
     **/
    void setPuck(const QPointF& puck);
    QPointF getPuck() const { return _puck; }

    /**
     * @brief The puck in widget coordinates.
     **/
    QPointF getPuckWidgetPos() const;

    QPointF getDiscCentre() const;
    double getRingOuterRadius() const;
    double getRingInnerRadius() const;
    double getDiscRadius() const;

    virtual QSize sizeHint() const OVERRIDE FINAL;
    virtual QSize minimumSizeHint() const OVERRIDE FINAL;

protected:
    virtual void paintEvent(QPaintEvent* e) OVERRIDE FINAL;

private:
    struct CacheKey {
        int size = 0;
        double dpr = 0.0;
        ColorGlobalModeEnum mode = eColorGlobalModeS;
        int hue = 0;
        unsigned generation = 0;
        bool valid = false;

        bool operator==(const CacheKey& other) const;
    };

    double devicePixelRatioForCache() const;
    CacheKey cacheKey(int size, bool withMode) const;
    const QImage& ringImage() const;
    const QImage& gradientImage() const;

    ColorGlobalModeEnum _mode;
    LumaBasis _basis;
    DisplayConverter _converter;
    unsigned _generation;
    int _hue;
    bool _hasDefinedHue;
    double _lastDefinedHue;
    QPointF _puck;
    mutable QImage _ring;
    mutable CacheKey _ringKey;
    mutable QImage _gradient;
    mutable CacheKey _gradientKey;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_COLORMODEDISCWIDGET_H
