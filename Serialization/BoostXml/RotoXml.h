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

#ifndef Serialization_BoostXml_RotoXml_h
#define Serialization_BoostXml_RotoXml_h

#include "Global/Macros.h"

#include <cmath>
#include <list>
#include <map>
#include <memory>
#include <string>

#include "Serialization/BoostXml/CurveXml.h"
#include "Serialization/BoostXml/KnobXml.h"
#include "Serialization/RotoSerialization.h"

#if !defined(Q_MOC_RUN) && !defined(SBK_RUN)
GCC_DIAG_UNUSED_LOCAL_TYPEDEFS_OFF
// clang-format off
GCC_DIAG_OFF(unused-parameter)
// /opt/local/include/boost/serialization/smart_cast.hpp:254:25: warning: unused parameter 'u' [-Wunused-parameter]
#include <boost/archive/xml_iarchive.hpp>
#include <boost/archive/xml_oarchive.hpp>
#include <boost/serialization/list.hpp>
#include <boost/serialization/map.hpp>
#include <boost/serialization/nvp.hpp>
#include <boost/serialization/shared_ptr.hpp>
#include <boost/serialization/split_free.hpp>
#include <boost/serialization/string.hpp>
#include <boost/serialization/version.hpp>
GCC_DIAG_UNUSED_LOCAL_TYPEDEFS_ON
GCC_DIAG_ON(unused-parameter)
// clang-format on
#endif

#define NATRON_SERIALIZATION_BEZIER_CP_INTRODUCES_OFFSET 2
#define NATRON_SERIALIZATION_BEZIER_CP_FIX_BUG_CURVE_POINTER 3
#define NATRON_SERIALIZATION_BEZIER_CP_REMOVE_OFFSET 4
#define NATRON_SERIALIZATION_BEZIER_CP_ADD_BROKEN 5
#define NATRON_SERIALIZATION_BEZIER_CP_VERSION NATRON_SERIALIZATION_BEZIER_CP_ADD_BROKEN

#define NATRON_SERIALIZATION_ROTO_ITEM_INTRODUCES_LABEL 2
#define NATRON_SERIALIZATION_ROTO_ITEM_VERSION NATRON_SERIALIZATION_ROTO_ITEM_INTRODUCES_LABEL

#define NATRON_SERIALIZATION_ROTO_DRAWABLE_ITEM_INTRODUCES_COMPOSITING 2
#define NATRON_SERIALIZATION_ROTO_DRAWABLE_ITEM_REMOVES_INVERTED 3
#define NATRON_SERIALIZATION_ROTO_DRAWABLE_ITEM_CHANGES_TO_LIST 4
#define NATRON_SERIALIZATION_ROTO_DRAWABLE_ITEM_VERSION NATRON_SERIALIZATION_ROTO_DRAWABLE_ITEM_CHANGES_TO_LIST

#define NATRON_SERIALIZATION_BEZIER_INTRODUCES_ROTO_STROKE 2
#define NATRON_SERIALIZATION_BEZIER_REMOVES_IS_ROTO_STROKE 3
#define NATRON_SERIALIZATION_BEZIER_INTRODUCES_OPEN_BEZIER 4
#define NATRON_SERIALIZATION_BEZIER_VERSION NATRON_SERIALIZATION_BEZIER_INTRODUCES_OPEN_BEZIER

#define NATRON_SERIALIZATION_ROTO_STROKE_INTRODUCES_MULTIPLE_STROKES 2
#define NATRON_SERIALIZATION_ROTO_STROKE_VERSION NATRON_SERIALIZATION_ROTO_STROKE_INTRODUCES_MULTIPLE_STROKES

#define NATRON_SERIALIZATION_ROTO_LAYER_REMOVES_IS_BEZIER 2
#define NATRON_SERIALIZATION_ROTO_LAYER_VERSION NATRON_SERIALIZATION_ROTO_LAYER_REMOVES_IS_BEZIER

#define NATRON_SERIALIZATION_ROTO_CONTEXT_REMOVE_COUNTERS 2
#define NATRON_SERIALIZATION_ROTO_CONTEXT_VERSION NATRON_SERIALIZATION_ROTO_CONTEXT_REMOVE_COUNTERS

NATRON_NAMESPACE_ENTER
namespace Serialization {

/**
 * @brief Whether the archive being read was written by Natron 1.0.0 RC2 or RC3. Those builds
 * saved the animation curves of bezier points through pointers, and the only way to tell such
 * an archive apart is the Natron version a project file names, which the archive itself does
 * not see: whoever opens the project sets this before reading it.
 **/
void setReadingRC2OrRC3Archive(bool isRC2OrRC3);
bool isReadingRC2OrRC3Archive();

} // namespace Serialization
NATRON_NAMESPACE_EXIT

// Non-intrusive adapters reproducing the NVP sequence, nesting and class versions of the old
// intrusive BezierCP and RotoItem/RotoDrawableItem/Bezier/RotoStrokeItem/RotoLayer/RotoContext
// serializations, including every branch that reads files written by older versions. Every
// item is archived by reference, never through a pointer, with the layer saving each child's
// kind as an int "Type" ahead of it, so the format carries no exported class names: the class
// IDs only depend on each distinct type being first archived in the same order as before.
namespace boost {
namespace serialization {

    template <class Archive>
    void
    save(Archive& ar,
         const NATRON_NAMESPACE::Serialization::BezierCPSerialization& point,
         const unsigned int /*version*/)
    {
        ar& ::boost::serialization::make_nvp("X", point.x);
        ar& ::boost::serialization::make_nvp("X_animation", point.xCurve);
        ar& ::boost::serialization::make_nvp("Y", point.y);
        ar& ::boost::serialization::make_nvp("Y_animation", point.yCurve);
        ar& ::boost::serialization::make_nvp("Left_X", point.leftX);
        ar& ::boost::serialization::make_nvp("Left_X_animation", point.leftXCurve);
        ar& ::boost::serialization::make_nvp("Left_Y", point.leftY);
        ar& ::boost::serialization::make_nvp("Left_Y_animation", point.leftYCurve);
        ar& ::boost::serialization::make_nvp("Right_X", point.rightX);
        ar& ::boost::serialization::make_nvp("Right_X_animation", point.rightXCurve);
        ar& ::boost::serialization::make_nvp("Right_Y", point.rightY);
        ar& ::boost::serialization::make_nvp("Right_Y_animation", point.rightYCurve);
        ar& ::boost::serialization::make_nvp("Broken", point.broken);
    }

    template <class Archive>
    void
    load(Archive& ar,
         NATRON_NAMESPACE::Serialization::BezierCPSerialization& point,
         const unsigned int version)
    {
        point = NATRON_NAMESPACE::Serialization::BezierCPSerialization();

        if ((version >= NATRON_SERIALIZATION_BEZIER_CP_FIX_BUG_CURVE_POINTER) || !NATRON_NAMESPACE::Serialization::isReadingRC2OrRC3Archive()) {
            ar& ::boost::serialization::make_nvp("X", point.x);
            ar& ::boost::serialization::make_nvp("X_animation", point.xCurve);

            if (version < NATRON_SERIALIZATION_BEZIER_CP_FIX_BUG_CURVE_POINTER) {
                // These versions saved a curve, not the static y, under the "Y" tag.
                NATRON_NAMESPACE::Serialization::CurveSerialization curveBugData;
                ar& ::boost::serialization::make_nvp("Y", curveBugData);
            } else {
                ar& ::boost::serialization::make_nvp("Y", point.y);
            }

            ar& ::boost::serialization::make_nvp("Y_animation", point.yCurve);
            ar& ::boost::serialization::make_nvp("Left_X", point.leftX);
            ar& ::boost::serialization::make_nvp("Left_X_animation", point.leftXCurve);
            ar& ::boost::serialization::make_nvp("Left_Y", point.leftY);
            ar& ::boost::serialization::make_nvp("Left_Y_animation", point.leftYCurve);
            ar& ::boost::serialization::make_nvp("Right_X", point.rightX);
            ar& ::boost::serialization::make_nvp("Right_X_animation", point.rightXCurve);
            ar& ::boost::serialization::make_nvp("Right_Y", point.rightY);
            ar& ::boost::serialization::make_nvp("Right_Y_animation", point.rightYCurve);

            if (version >= NATRON_SERIALIZATION_BEZIER_CP_ADD_BROKEN) {
                ar& ::boost::serialization::make_nvp("Broken", point.broken);
            } else {
                // Older files did not save the flag, so it is inferred from the signed distance
                // between the point and the line through its tangents.
                double tangentVectorX = point.rightX - point.leftX;
                double tangentVectorY = point.rightY - point.leftY;
                double tangentLength = std::sqrt(tangentVectorX * tangentVectorX + tangentVectorY * tangentVectorY);
                double normalX = tangentVectorY / tangentLength;
                double normalY = -tangentVectorX / tangentLength;
                double signedDistance = normalX * (point.x - point.leftX) + normalY * (point.y - point.leftY);
                point.broken = (std::abs(signedDistance) < 1e-4);
            }
        } else {
            // The wire shape boost emits for a shared_ptr<T> does not depend on T, so reading the
            // pointee as plain curve data reads these archives as the shared_ptr<Curve> they hold.
            std::shared_ptr<NATRON_NAMESPACE::Serialization::CurveSerialization> xCurveData, curveBugData, yCurveData, leftXCurveData, leftYCurveData, rightXCurveData, rightYCurveData;

            ar& ::boost::serialization::make_nvp("X", point.x);
            ar& ::boost::serialization::make_nvp("X_animation", xCurveData);
            ar& ::boost::serialization::make_nvp("Y", curveBugData);
            ar& ::boost::serialization::make_nvp("Y_animation", yCurveData);
            ar& ::boost::serialization::make_nvp("Left_X", point.leftX);
            ar& ::boost::serialization::make_nvp("Left_X_animation", leftXCurveData);
            ar& ::boost::serialization::make_nvp("Left_Y", point.leftY);
            ar& ::boost::serialization::make_nvp("Left_Y_animation", leftYCurveData);
            ar& ::boost::serialization::make_nvp("Right_X", point.rightX);
            ar& ::boost::serialization::make_nvp("Right_X_animation", rightXCurveData);
            ar& ::boost::serialization::make_nvp("Right_Y", point.rightY);
            ar& ::boost::serialization::make_nvp("Right_Y_animation", rightYCurveData);

            if (xCurveData) {
                point.xCurve = *xCurveData;
            }
            if (yCurveData) {
                point.yCurve = *yCurveData;
            }
            if (leftXCurveData) {
                point.leftXCurve = *leftXCurveData;
            }
            if (leftYCurveData) {
                point.leftYCurve = *leftYCurveData;
            }
            if (rightXCurveData) {
                point.rightXCurve = *rightXCurveData;
            }
            if (rightYCurveData) {
                point.rightYCurve = *rightYCurveData;
            }
        }

        if ((version >= NATRON_SERIALIZATION_BEZIER_CP_INTRODUCES_OFFSET) && (version < NATRON_SERIALIZATION_BEZIER_CP_REMOVE_OFFSET)) {
            int offsetTime;
            ar& ::boost::serialization::make_nvp("OffsetTime", offsetTime);
        }
    }

    template <class Archive>
    void
    serialize(Archive& ar,
              NATRON_NAMESPACE::Serialization::BezierCPSerialization& point,
              const unsigned int version)
    {
        ::boost::serialization::split_free(ar, point, version);
    }

    template <class Archive>
    void
    save(Archive& ar,
         const NATRON_NAMESPACE::Serialization::RotoItemSerialization& item,
         const unsigned int /*version*/)
    {
        ar& ::boost::serialization::make_nvp("Name", item.name);
        ar& ::boost::serialization::make_nvp("Label", item.label);
        ar& ::boost::serialization::make_nvp("Activated", item.activated);
        ar& ::boost::serialization::make_nvp("ParentLayer", item.parentLayerName);
        ar& ::boost::serialization::make_nvp("Locked", item.locked);
    }

    template <class Archive>
    void
    load(Archive& ar,
         NATRON_NAMESPACE::Serialization::RotoItemSerialization& item,
         const unsigned int version)
    {
        item = NATRON_NAMESPACE::Serialization::RotoItemSerialization();

        ar& ::boost::serialization::make_nvp("Name", item.name);
        if (version >= NATRON_SERIALIZATION_ROTO_ITEM_INTRODUCES_LABEL) {
            ar& ::boost::serialization::make_nvp("Label", item.label);
        }
        ar& ::boost::serialization::make_nvp("Activated", item.activated);
        ar& ::boost::serialization::make_nvp("ParentLayer", item.parentLayerName);
        ar& ::boost::serialization::make_nvp("Locked", item.locked);
    }

    template <class Archive>
    void
    serialize(Archive& ar,
              NATRON_NAMESPACE::Serialization::RotoItemSerialization& item,
              const unsigned int version)
    {
        ::boost::serialization::split_free(ar, item, version);
    }

    // The base parts below are archived as the objects the old classes archived with
    // BOOST_SERIALIZATION_BASE_OBJECT_NVP, which tags them with the base class name.

    template <class Archive>
    void
    save(Archive& ar,
         const NATRON_NAMESPACE::Serialization::RotoDrawableItemSerialization& drawable,
         const unsigned int /*version*/)
    {
        ar& ::boost::serialization::make_nvp("RotoItemSerialization", static_cast<const NATRON_NAMESPACE::Serialization::RotoItemSerialization&>(drawable));
        int knobCount = (int)drawable.knobs.size();
        ar& ::boost::serialization::make_nvp("NbItems", knobCount);
        for (std::list<NATRON_NAMESPACE::Serialization::KnobSerialization>::const_iterator it = drawable.knobs.begin(); it != drawable.knobs.end(); ++it) {
            ar& ::boost::serialization::make_nvp("Item", *it);
        }
        ar& ::boost::serialization::make_nvp("OC.r", drawable.overlayColor[0]);
        ar& ::boost::serialization::make_nvp("OC.g", drawable.overlayColor[1]);
        ar& ::boost::serialization::make_nvp("OC.b", drawable.overlayColor[2]);
        ar& ::boost::serialization::make_nvp("OC.a", drawable.overlayColor[3]);
    }

    template <class Archive>
    void
    loadRotoDrawableItemKnob(Archive& ar,
                             NATRON_NAMESPACE::Serialization::RotoDrawableItemSerialization& drawable,
                             const char* tagName)
    {
        drawable.knobs.push_back(NATRON_NAMESPACE::Serialization::KnobSerialization());
        ar& ::boost::serialization::make_nvp(tagName, drawable.knobs.back());
    }

    template <class Archive>
    void
    load(Archive& ar,
         NATRON_NAMESPACE::Serialization::RotoDrawableItemSerialization& drawable,
         const unsigned int version)
    {
        ar& ::boost::serialization::make_nvp("RotoItemSerialization", static_cast<NATRON_NAMESPACE::Serialization::RotoItemSerialization&>(drawable));

        drawable.knobs.clear();
        if (version < NATRON_SERIALIZATION_ROTO_DRAWABLE_ITEM_CHANGES_TO_LIST) {
            loadRotoDrawableItemKnob(ar, drawable, "Activated");
            loadRotoDrawableItemKnob(ar, drawable, "Opacity");
            loadRotoDrawableItemKnob(ar, drawable, "Feather");
            loadRotoDrawableItemKnob(ar, drawable, "FallOff");
            if (version < NATRON_SERIALIZATION_ROTO_DRAWABLE_ITEM_REMOVES_INVERTED) {
                loadRotoDrawableItemKnob(ar, drawable, "Inverted");
            }
            if (version >= NATRON_SERIALIZATION_ROTO_DRAWABLE_ITEM_INTRODUCES_COMPOSITING) {
                loadRotoDrawableItemKnob(ar, drawable, "Color");
                loadRotoDrawableItemKnob(ar, drawable, "CompOP");
            }
        } else {
            int knobCount = 0;
            ar& ::boost::serialization::make_nvp("NbItems", knobCount);
            for (int i = 0; i < knobCount; ++i) {
                loadRotoDrawableItemKnob(ar, drawable, "Item");
            }
        }
        ar& ::boost::serialization::make_nvp("OC.r", drawable.overlayColor[0]);
        ar& ::boost::serialization::make_nvp("OC.g", drawable.overlayColor[1]);
        ar& ::boost::serialization::make_nvp("OC.b", drawable.overlayColor[2]);
        ar& ::boost::serialization::make_nvp("OC.a", drawable.overlayColor[3]);
    }

    template <class Archive>
    void
    serialize(Archive& ar,
              NATRON_NAMESPACE::Serialization::RotoDrawableItemSerialization& drawable,
              const unsigned int version)
    {
        ::boost::serialization::split_free(ar, drawable, version);
    }

    template <class Archive>
    void
    save(Archive& ar,
         const NATRON_NAMESPACE::Serialization::BezierSerialization& bezier,
         const unsigned int /*version*/)
    {
        ar& ::boost::serialization::make_nvp("RotoDrawableItemSerialization", static_cast<const NATRON_NAMESPACE::Serialization::RotoDrawableItemSerialization&>(bezier));
        int pointCount = (int)bezier.controlPoints.size();
        ar& ::boost::serialization::make_nvp("NumPoints", pointCount);

        // Every bezier has one feather point per control point; the placeholder only guards a
        // feather list that is shorter than it should be.
        const NATRON_NAMESPACE::Serialization::BezierCPSerialization missingFeatherPoint;
        std::list<NATRON_NAMESPACE::Serialization::BezierCPSerialization>::const_iterator featherIt = bezier.featherPoints.begin();
        for (std::list<NATRON_NAMESPACE::Serialization::BezierCPSerialization>::const_iterator it = bezier.controlPoints.begin(); it != bezier.controlPoints.end(); ++it) {
            ar& ::boost::serialization::make_nvp("CP", *it);
            if (featherIt != bezier.featherPoints.end()) {
                ar& ::boost::serialization::make_nvp("FP", *featherIt);
                ++featherIt;
            } else {
                ar& ::boost::serialization::make_nvp("FP", missingFeatherPoint);
            }
        }
        ar& ::boost::serialization::make_nvp("Closed", bezier.closed);
        ar& ::boost::serialization::make_nvp("OpenBezier", bezier.isOpenBezier);
    }

    template <class Archive>
    void
    load(Archive& ar,
         NATRON_NAMESPACE::Serialization::BezierSerialization& bezier,
         const unsigned int version)
    {
        bezier = NATRON_NAMESPACE::Serialization::BezierSerialization();

        ar& ::boost::serialization::make_nvp("RotoDrawableItemSerialization", static_cast<NATRON_NAMESPACE::Serialization::RotoDrawableItemSerialization&>(bezier));
        int pointCount = 0;
        ar& ::boost::serialization::make_nvp("NumPoints", pointCount);
        if ((version >= NATRON_SERIALIZATION_BEZIER_INTRODUCES_ROTO_STROKE) && (version < NATRON_SERIALIZATION_BEZIER_REMOVES_IS_ROTO_STROKE)) {
            bool isStroke;
            ar& ::boost::serialization::make_nvp("IsStroke", isStroke);
        }
        for (int i = 0; i < pointCount; ++i) {
            bezier.controlPoints.push_back(NATRON_NAMESPACE::Serialization::BezierCPSerialization());
            ar& ::boost::serialization::make_nvp("CP", bezier.controlPoints.back());
            bezier.featherPoints.push_back(NATRON_NAMESPACE::Serialization::BezierCPSerialization());
            ar& ::boost::serialization::make_nvp("FP", bezier.featherPoints.back());
        }
        ar& ::boost::serialization::make_nvp("Closed", bezier.closed);
        if (version >= NATRON_SERIALIZATION_BEZIER_INTRODUCES_OPEN_BEZIER) {
            ar& ::boost::serialization::make_nvp("OpenBezier", bezier.isOpenBezier);
        } else {
            bezier.isOpenBezier = false;
        }
    }

    template <class Archive>
    void
    serialize(Archive& ar,
              NATRON_NAMESPACE::Serialization::BezierSerialization& bezier,
              const unsigned int version)
    {
        ::boost::serialization::split_free(ar, bezier, version);
    }

    template <class Archive>
    void
    save(Archive& ar,
         const NATRON_NAMESPACE::Serialization::RotoStrokeItemSerialization& stroke,
         const unsigned int /*version*/)
    {
        ar& ::boost::serialization::make_nvp("RotoDrawableItemSerialization", static_cast<const NATRON_NAMESPACE::Serialization::RotoDrawableItemSerialization&>(stroke));
        ar& ::boost::serialization::make_nvp("BrushType", stroke.brushType);
        int strokeCount = (int)stroke.strokes.size();
        ar& ::boost::serialization::make_nvp("NbItems", strokeCount);
        for (std::list<NATRON_NAMESPACE::Serialization::RotoStrokeCurvesSerialization>::const_iterator it = stroke.strokes.begin(); it != stroke.strokes.end(); ++it) {
            ar& ::boost::serialization::make_nvp("CurveX", it->xCurve);
            ar& ::boost::serialization::make_nvp("CurveY", it->yCurve);
            ar& ::boost::serialization::make_nvp("CurveP", it->pressureCurve);
        }
    }

    template <class Archive>
    void
    loadRotoStrokeCurves(Archive& ar,
                         NATRON_NAMESPACE::Serialization::RotoStrokeItemSerialization& stroke)
    {
        stroke.strokes.push_back(NATRON_NAMESPACE::Serialization::RotoStrokeCurvesSerialization());
        NATRON_NAMESPACE::Serialization::RotoStrokeCurvesSerialization& curves = stroke.strokes.back();
        ar& ::boost::serialization::make_nvp("CurveX", curves.xCurve);
        ar& ::boost::serialization::make_nvp("CurveY", curves.yCurve);
        ar& ::boost::serialization::make_nvp("CurveP", curves.pressureCurve);
    }

    template <class Archive>
    void
    load(Archive& ar,
         NATRON_NAMESPACE::Serialization::RotoStrokeItemSerialization& stroke,
         const unsigned int version)
    {
        stroke = NATRON_NAMESPACE::Serialization::RotoStrokeItemSerialization();

        ar& ::boost::serialization::make_nvp("RotoDrawableItemSerialization", static_cast<NATRON_NAMESPACE::Serialization::RotoDrawableItemSerialization&>(stroke));
        ar& ::boost::serialization::make_nvp("BrushType", stroke.brushType);
        if (version < NATRON_SERIALIZATION_ROTO_STROKE_INTRODUCES_MULTIPLE_STROKES) {
            loadRotoStrokeCurves(ar, stroke);
        } else {
            int strokeCount = 0;
            ar& ::boost::serialization::make_nvp("NbItems", strokeCount);
            for (int i = 0; i < strokeCount; ++i) {
                loadRotoStrokeCurves(ar, stroke);
            }
        }
    }

    template <class Archive>
    void
    serialize(Archive& ar,
              NATRON_NAMESPACE::Serialization::RotoStrokeItemSerialization& stroke,
              const unsigned int version)
    {
        ::boost::serialization::split_free(ar, stroke, version);
    }

    template <class Archive>
    void
    save(Archive& ar,
         const NATRON_NAMESPACE::Serialization::RotoLayerSerialization& layer,
         const unsigned int /*version*/)
    {
        ar& ::boost::serialization::make_nvp("RotoItemSerialization", static_cast<const NATRON_NAMESPACE::Serialization::RotoItemSerialization&>(layer));
        int childCount = (int)layer.children.size();
        ar& ::boost::serialization::make_nvp("NumChildren", childCount);
        for (std::list<NATRON_NAMESPACE::Serialization::RotoLayerChildSerialization>::const_iterator it = layer.children.begin(); it != layer.children.end(); ++it) {
            int type = (int)it->type;
            ar& ::boost::serialization::make_nvp("Type", type);
            switch (it->type) {
            case NATRON_NAMESPACE::Serialization::eRotoLayerChildTypeBezier:
                ar& ::boost::serialization::make_nvp("Item", it->bezier);
                break;
            case NATRON_NAMESPACE::Serialization::eRotoLayerChildTypeStroke:
                ar& ::boost::serialization::make_nvp("Item", it->stroke);
                break;
            case NATRON_NAMESPACE::Serialization::eRotoLayerChildTypeLayer:
                ar& ::boost::serialization::make_nvp("Item", it->layer);
                break;
            }
        }
    }

    template <class Archive>
    void
    load(Archive& ar,
         NATRON_NAMESPACE::Serialization::RotoLayerSerialization& layer,
         const unsigned int version)
    {
        layer = NATRON_NAMESPACE::Serialization::RotoLayerSerialization();

        ar& ::boost::serialization::make_nvp("RotoItemSerialization", static_cast<NATRON_NAMESPACE::Serialization::RotoItemSerialization&>(layer));
        int childCount = 0;
        ar& ::boost::serialization::make_nvp("NumChildren", childCount);
        for (int i = 0; i < childCount; ++i) {
            int type;
            if (version < NATRON_SERIALIZATION_ROTO_LAYER_REMOVES_IS_BEZIER) {
                bool isBezier;
                ar& ::boost::serialization::make_nvp("IsBezier", isBezier);
                type = isBezier ? NATRON_NAMESPACE::Serialization::eRotoLayerChildTypeBezier : NATRON_NAMESPACE::Serialization::eRotoLayerChildTypeLayer;
            } else {
                ar& ::boost::serialization::make_nvp("Type", type);
            }

            // Each child is read in place: an archive may record where it loaded an object.
            layer.children.push_back(NATRON_NAMESPACE::Serialization::RotoLayerChildSerialization());
            NATRON_NAMESPACE::Serialization::RotoLayerChildSerialization& child = layer.children.back();
            if (type == NATRON_NAMESPACE::Serialization::eRotoLayerChildTypeBezier) {
                child.type = NATRON_NAMESPACE::Serialization::eRotoLayerChildTypeBezier;
                ar& ::boost::serialization::make_nvp("Item", child.bezier);
            } else if (type == NATRON_NAMESPACE::Serialization::eRotoLayerChildTypeStroke) {
                child.type = NATRON_NAMESPACE::Serialization::eRotoLayerChildTypeStroke;
                ar& ::boost::serialization::make_nvp("Item", child.stroke);
            } else {
                child.type = NATRON_NAMESPACE::Serialization::eRotoLayerChildTypeLayer;
                ar& ::boost::serialization::make_nvp("Item", child.layer);
            }
        }
    }

    template <class Archive>
    void
    serialize(Archive& ar,
              NATRON_NAMESPACE::Serialization::RotoLayerSerialization& layer,
              const unsigned int version)
    {
        ::boost::serialization::split_free(ar, layer, version);
    }

    template <class Archive>
    void
    save(Archive& ar,
         const NATRON_NAMESPACE::Serialization::RotoContextSerialization& context,
         const unsigned int /*version*/)
    {
        ar& ::boost::serialization::make_nvp("BaseLayer", context.baseLayer);
        ar& ::boost::serialization::make_nvp("AutoKeying", context.autoKeying);
        ar& ::boost::serialization::make_nvp("RippleEdit", context.rippleEdit);
        ar& ::boost::serialization::make_nvp("FeatherLink", context.featherLink);
        ar& ::boost::serialization::make_nvp("Selection", context.selectedItems);
    }

    template <class Archive>
    void
    load(Archive& ar,
         NATRON_NAMESPACE::Serialization::RotoContextSerialization& context,
         const unsigned int version)
    {
        context = NATRON_NAMESPACE::Serialization::RotoContextSerialization();

        ar& ::boost::serialization::make_nvp("BaseLayer", context.baseLayer);
        ar& ::boost::serialization::make_nvp("AutoKeying", context.autoKeying);
        ar& ::boost::serialization::make_nvp("RippleEdit", context.rippleEdit);
        ar& ::boost::serialization::make_nvp("FeatherLink", context.featherLink);
        ar& ::boost::serialization::make_nvp("Selection", context.selectedItems);

        if (version < NATRON_SERIALIZATION_ROTO_CONTEXT_REMOVE_COUNTERS) {
            std::map<std::string, int> itemCounters;
            ar& ::boost::serialization::make_nvp("Counters", itemCounters);
        }
    }

    template <class Archive>
    void
    serialize(Archive& ar,
              NATRON_NAMESPACE::Serialization::RotoContextSerialization& context,
              const unsigned int version)
    {
        ::boost::serialization::split_free(ar, context, version);
    }

} // namespace serialization
} // namespace boost

BOOST_CLASS_VERSION(NATRON_NAMESPACE::Serialization::BezierCPSerialization, NATRON_SERIALIZATION_BEZIER_CP_VERSION)
BOOST_CLASS_VERSION(NATRON_NAMESPACE::Serialization::RotoItemSerialization, NATRON_SERIALIZATION_ROTO_ITEM_VERSION)
BOOST_CLASS_VERSION(NATRON_NAMESPACE::Serialization::RotoDrawableItemSerialization, NATRON_SERIALIZATION_ROTO_DRAWABLE_ITEM_VERSION)
BOOST_CLASS_VERSION(NATRON_NAMESPACE::Serialization::BezierSerialization, NATRON_SERIALIZATION_BEZIER_VERSION)
BOOST_CLASS_VERSION(NATRON_NAMESPACE::Serialization::RotoStrokeItemSerialization, NATRON_SERIALIZATION_ROTO_STROKE_VERSION)
BOOST_CLASS_VERSION(NATRON_NAMESPACE::Serialization::RotoLayerSerialization, NATRON_SERIALIZATION_ROTO_LAYER_VERSION)
BOOST_CLASS_VERSION(NATRON_NAMESPACE::Serialization::RotoContextSerialization, NATRON_SERIALIZATION_ROTO_CONTEXT_VERSION)

#endif // Serialization_BoostXml_RotoXml_h
