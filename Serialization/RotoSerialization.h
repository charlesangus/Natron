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

#ifndef Serialization_RotoSerialization_h
#define Serialization_RotoSerialization_h

#include "Global/Macros.h"

#include <list>
#include <string>

#include "Serialization/CurveSerialization.h"
#include "Serialization/KnobSerialization.h"
#include "Serialization/SerializationFwd.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

/**
 * @brief The saved state of one control or feather point of a bezier: its static position and
 * tangents, used when they are not animated, and their animation curves.
 **/
struct BezierCPSerialization {
    double x;
    double y;
    CurveSerialization xCurve;
    CurveSerialization yCurve;

    double leftX;
    double leftY;
    CurveSerialization leftXCurve;
    CurveSerialization leftYCurve;

    double rightX;
    double rightY;
    CurveSerialization rightXCurve;
    CurveSerialization rightYCurve;

    // Whether the two tangents move independently.
    bool broken;

    BezierCPSerialization()
        : x(0.)
        , y(0.)
        , xCurve()
        , yCurve()
        , leftX(0.)
        , leftY(0.)
        , leftXCurve()
        , leftYCurve()
        , rightX(0.)
        , rightY(0.)
        , rightXCurve()
        , rightYCurve()
        , broken(false)
    {
    }
};

/**
 * @brief What every roto item saves: layers, beziers and strokes alike.
 **/
struct RotoItemSerialization {
    std::string name;
    std::string label;
    bool activated;

    // The script name of the layer holding the item, empty for the base layer.
    std::string parentLayerName;
    bool locked;

    RotoItemSerialization()
        : name()
        , label()
        , activated(false)
        , parentLayerName()
        , locked(false)
    {
    }
};

/**
 * @brief What beziers and strokes save on top of RotoItemSerialization: their knobs, in the
 * order the item holds them, and the color their overlay is drawn with.
 **/
struct RotoDrawableItemSerialization
    : public RotoItemSerialization {
    std::list<KnobSerialization> knobs;

    // Red, green, blue and alpha.
    double overlayColor[4];

    RotoDrawableItemSerialization()
        : RotoItemSerialization()
        , knobs()
        , overlayColor()
    {
    }
};

/**
 * @brief The saved state of a bezier shape. featherPoints holds one point per control point.
 **/
struct BezierSerialization
    : public RotoDrawableItemSerialization {
    std::list<BezierCPSerialization> controlPoints;
    std::list<BezierCPSerialization> featherPoints;
    bool closed;
    bool isOpenBezier;

    BezierSerialization()
        : RotoDrawableItemSerialization()
        , controlPoints()
        , featherPoints()
        , closed(false)
        , isOpenBezier(false)
    {
    }
};

/**
 * @brief The curves of one pen-down to pen-up movement of a paint stroke.
 **/
struct RotoStrokeCurvesSerialization {
    CurveSerialization xCurve;
    CurveSerialization yCurve;
    CurveSerialization pressureCurve;
};

/**
 * @brief The saved state of a paint stroke.
 **/
struct RotoStrokeItemSerialization
    : public RotoDrawableItemSerialization {
    // The Engine RotoStrokeType of the stroke.
    int brushType;
    std::list<RotoStrokeCurvesSerialization> strokes;

    RotoStrokeItemSerialization()
        : RotoDrawableItemSerialization()
        , brushType(0)
        , strokes()
    {
    }
};

/**
 * @brief The kind of a layer child. The values are those the project format saves.
 **/
enum RotoLayerChildTypeEnum {
    eRotoLayerChildTypeBezier = 0,
    eRotoLayerChildTypeStroke = 1,
    eRotoLayerChildTypeLayer = 2
};

struct RotoLayerChildSerialization;

/**
 * @brief The saved state of a layer and, in order, of the items it holds.
 **/
struct RotoLayerSerialization
    : public RotoItemSerialization {
    std::list<RotoLayerChildSerialization> children;

    RotoLayerSerialization();
};

/**
 * @brief One item of a layer: of bezier, stroke and layer, only the member type selects is
 * meaningful.
 **/
struct RotoLayerChildSerialization {
    RotoLayerChildTypeEnum type;
    BezierSerialization bezier;
    RotoStrokeItemSerialization stroke;
    RotoLayerSerialization layer;

    RotoLayerChildSerialization()
        : type(eRotoLayerChildTypeBezier)
        , bezier()
        , stroke()
        , layer()
    {
    }
};

// Defined once RotoLayerChildSerialization is complete, which the children list needs.
inline RotoLayerSerialization::RotoLayerSerialization()
    : RotoItemSerialization()
    , children()
{
}

/**
 * @brief The saved state of the roto context of a Roto or RotoPaint node: the base layer, which
 * holds every other item, the script names of the selected items, and the editing toggles.
 **/
struct RotoContextSerialization {
    RotoLayerSerialization baseLayer;
    std::list<std::string> selectedItems;
    bool autoKeying;
    bool rippleEdit;
    // 0 all keys, 1 from start, 2 to end, 3 the range below
    int rippleEditMode;
    double rippleEditRangeFirst;
    double rippleEditRangeLast;
    bool featherLink;

    RotoContextSerialization()
        : baseLayer()
        , selectedItems()
        , autoKeying(false)
        , rippleEdit(false)
        , rippleEditMode(0)
        , rippleEditRangeFirst(1.)
        , rippleEditRangeLast(100.)
        , featherLink(false)
    {
    }
};

} // namespace Serialization
NATRON_NAMESPACE_EXIT

#endif // Serialization_RotoSerialization_h
