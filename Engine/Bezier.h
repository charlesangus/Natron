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

#ifndef Engine_Bezier_h
#define Engine_Bezier_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <set>
#include <string>
#include <utility>
#include <vector>

// clang-format off
CLANG_DIAG_OFF(deprecated-declarations)
#include <QObject>
CLANG_DIAG_ON(deprecated-declarations)
// clang-format on

#include "Global/GlobalDefines.h"

#include "Engine/RotoDrawableItem.h"
#include "Engine/ViewIdx.h"
#include "Engine/EngineFwd.h"


#define ROTO_BEZIER_EVAL_ITERATIVE

NATRON_NAMESPACE_ENTER

/**
 * @brief What the viewer overlay draws of a Bezier at one time: the curve and the feather as x,y
 * float pairs, with their bounding boxes.
 **/
struct BezierOverlayPolygons {
    std::vector<float> curve;
    std::vector<float> feather;
    RectD curveBBox;
    RectD featherBBox;
    bool hasFeather;
};

typedef std::shared_ptr<const BezierOverlayPolygons> BezierOverlayPolygonsConstPtr;

/**
 * @class A base class for all items made by the roto context
 **/


/**
 * @class This class represents a bezier curve.
 * Note that the bezier also supports feather points.
 * This class is MT-safe
 *
 * The curve supports animation, and by default once the first control point is added the curve
 * has at least a minimum of 1 keyframe.
 **/

struct ParametricPoint
{
    double x,y,t;
};

struct BezierPrivate;
class Bezier
    : public RotoDrawableItem
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    Bezier(const RotoCurveContextPtr& context,
           const std::string& name,
           const RotoLayerPtr& parent,
           bool isOpenBezier);

    Bezier(const Bezier& other) = delete;
    Bezier& operator=(const Bezier& other) = delete;

    virtual ~Bezier();

    static
    double bezierEval(double p0,
                      double p1,
                      double p2,
                      double p3,
                      double t);
    static void bezierFullPoint(const Point & p0,
                                const Point & p1,
                                const Point & p2,
                                const Point & p3,
                                double t,
                                Point *p0p1,
                                Point *p1p2,
                                Point *p2p3,
                                Point *p0p1_p1p2,
                                Point *p1p2_p2p3,
                                Point *dest);
    static void bezierPoint(const Point & p0,
                            const Point & p1,
                            const Point & p2,
                            const Point & p3,
                            double t,
                            Point *dest);
    static void bezierPointBboxUpdate(const Point & p0,
                                      const Point & p1,
                                      const Point & p2,
                                      const Point & p3,
                                      RectD *bbox);

    bool isOpenBezier() const;

    /**
     * @brief Used to differentiate real shapes with feather of paint strokes which does not have a feather
     **/
    virtual bool useFeatherPoints() const { return true; }


    virtual void clone(const RotoItem* other) OVERRIDE;


    void clearAllPoints();

    /**
     * @brief Adds a new control point to the curve. A feather point will be added, at the same position.
     * If auto keying is enabled and this is the first point and there's no keyframe a new keyframe will be set at the current time.
     *
     * This function can only be called when the curved is not yet "finished". Once setCurveFinished has
     * been called you can no longer call this function either. The only way to make it finished again
     * is to call setCurveFinished. You can then add points normally with addControlPointAfterIndex
     * This function is used to build-up the curve as opposed to addControlPointAfterIndex which is there to
     * edit an already fully shaped spline.
     **/
    BezierCPPtr addControlPoint(double x, double y, double time);

    /**
     * @brief Adds a new control point to the curve after the control point at the given index.
     * A feather point will be added, at the same position.
     * If auto keying is enabled, and there's no keyframe a new keyframe will be set at the current time.
     *
     * If index is -1 then the point will be added as the first point of the curve.
     * If index is invalid an invalid argument exception will be thrown.
     **/
    BezierCPPtr addControlPointAfterIndex(int index, double t);

    /**
     * @brief Returns the number of control points of the bezier
     **/
    int getControlPointsCount() const;

    /**
     * @brief Given the (x,y) coordinates of a point, this function returns whether a point lies on
     * the cubic bezier curve of the feather points or of the control points or not.
     * @returns The index of the starting control point (P0) of the bezier segment on which the given
     * point lies. If the point doesn't belong to any bezier segment of this curve then it will return -1.
     **/
    int isPointOnCurve(double x, double y, double acceptance, double *t, bool* feather) const;

    /**
     * @brief Set whether the curve is finished or not. A finished curve will have an arc between the first
     * and the last control point.
     **/
    void setCurveFinished(bool finished);
    bool isCurveFinished() const;


    /**
     * @brief Removes the control point at the given index if any. The feather point will also be removed , at the same position.
     * If auto keying is enabled, and there's no keyframe a new keyframe will be set at the current time.
     **/
    void removeControlPointByIndex(int index);

    /**
     * @brief Move the control point at the given index by the given dx and dy.
     * If feather link is enabled, the feather point will also be moved by the same deltas.
     * If  there's no keyframe at the current time, a new keyframe will be added.
     * This function asserts that  auto-keying is  enabled.
     * If ripple edit is enabled, the point will be moved at the same location at all keyframes.
     **/
    void movePointByIndex(int index, double time, double dx, double dy);

    /**
     * @brief Set the control point at the given index to x,y.
     * If feather link is enabled, the feather point will also be moved by the same deltas.
     * If  there's no keyframe at the current time, a new keyframe will be added.
     * This function asserts that  auto-keying is  enabled.
     * If ripple edit is enabled, the point will be moved at the same location at all keyframes.
     **/
    void setPointByIndex(int index, double time, double x, double y);

    /**
     * @brief Moves the feather point at the given index if any by the given dx and dy.
     * If auto keying is enabled and there's no keyframe at the current time, a new keyframe will be added.
     * If ripple edit is enabled, the point will be moved at the same location at all keyframes.
     **/
    void moveFeatherByIndex(int index, double time, double dx, double dy);

private:

    void movePointByIndexInternal(bool useGuiCurve, int index, double time, double dx, double dy, bool onlyFeather);
    void setPointByIndexInternal(bool useGuiCurve, int index, double time, double dx, double dy);

public:


    /**
     * @brief Moves the left bezier point of the control point at the given index by the given deltas
     * and at the given time.
     * If auto keying is enabled and there's no keyframe at the current time, a new keyframe will be added.
     * If ripple edit is enabled, the point will be moved at the same location at all keyframes.
     **/
    void moveLeftBezierPoint(int index, double time, double dx, double dy);

    /**
     * @brief Moves the right bezier point of the control point at the given index by the given deltas
     * and at the given time.
     * If auto keying is enabled and there's no keyframe at the current time, a new keyframe will be added.
     * If ripple edit is enabled, the point will be moved at the same location at all keyframes.
     **/
    void moveRightBezierPoint(int index, double time, double dx, double dy);

private:

    void moveBezierPointInternal(BezierCP* cpParam,
                                 BezierCP* fpParam,
                                 int index,
                                 double time,
                                 double lx, double ly, double rx, double ry,
                                 double flx, double fly, double frx, double fry,
                                 bool isLeft,
                                 bool moveBoth,
                                 bool breakTangents,
                                 bool onlyFeather);

public:


    /**
     * @brief Transforms the given point at the given time by the given matrix.
     **/
    void transformPoint(const BezierCPPtr & point, double time, Transform::Matrix3x3* matrix);

    /**
     * @brief Provided for convenience. It set the left bezier point of the control point at the given index to
     * the position given by (x,y) at the given time.
     * If auto keying is enabled and there's no keyframe at the current time, a new keyframe will be added.
     * If ripple edit is enabled, the point will be moved at the same location at all keyframes.
     * This function will also set the point of the feather point at the given x,y.
     **/
    void setLeftBezierPoint(int index, double time, double x, double y);

    /**
     * @brief Provided for convenience. It set the right bezier point of the control point at the given index to
     * the position given by (x,y) at the given time.
     * If auto keying is enabled and there's no keyframe at the current time, a new keyframe will be added.
     * If ripple edit is enabled, the point will be moved at the same location at all keyframes.
     * This function will also set the point of the feather point at the given x,y.
     **/
    void setRightBezierPoint(int index, double time, double x, double y);


    /**
     * @brief This function is a combinaison of setPosition + setLeftBezierPoint / setRightBeziePoint
     **/
    void setPointAtIndex(bool feather, int index, double time, double x, double y, double lx, double ly, double rx, double ry);

private:

    void setPointAtIndexInternal(bool setLeft, bool setRight, bool setPoint, bool feather, bool featherAndCp, int index, double time, double x, double y, double lx, double ly, double rx, double ry);

public:


    /**
     * @brief Set the left and right bezier point of the control point.
     **/
    void movePointLeftAndRightIndex(BezierCP & cp,
                                    BezierCP & fp,
                                    double time,
                                    double lx, double ly, double rx, double ry,
                                    double flx, double fly, double frx, double fry,
                                    bool breakTangents,
                                    bool onlyFeather);


    /**
     * @brief Removes the feather point at the given index by making it equal the "true" control point.
     **/
    void removeFeatherAtIndex(int index);

    /**
     * @brief Expand the feather point in the direction of the feather distance by the given distance.
     **/
    //void expandFeatherAtIndex(int index,double distance);

    /**
     * @brief Smooth the curvature of the bezier at the given index by expanding the tangents.
     * This is also applied to the feather points.
     * If auto keying is enabled and there's no keyframe at the current time, a new keyframe will be added.
     * If ripple edit is enabled, the point will be moved at the same location at all keyframes.
     **/
    void smoothPointAtIndex(int index, double time, const std::pair<double, double>& pixelScale);

    /**
     * @brief Cusps the curvature of the bezier at the given index by reducing the tangents.
     * This is also applied to the feather points.
     * If auto keying is enabled and there's no keyframe at the current time, a new keyframe will be added.
     * If ripple edit is enabled, the point will be moved at the same location at all keyframes.
     **/
    void cuspPointAtIndex(int index, double time, const std::pair<double, double>& pixelScale);

    void getMotionBlurSettings(const double time,
                               double* startTime,
                               double* endTime,
                               double* timeStep) const;

private:

    void smoothOrCuspPointAtIndex(bool isSmooth, int index, double time, const std::pair<double, double>& pixelScale);

public:


    /**
     * @brief Set a new keyframe at the given time. If a keyframe already exists this function does nothing.
     **/
    void setKeyframe(double time);

    /**
     * @brief Removes a keyframe at the given time if any.
     **/
    void removeKeyframe(double time);

    /**
     * @brief Removes all animation
     **/
    void removeAnimation();

    /**
     * @brief Moves a keyframe
     **/
    void moveKeyframe(double oldTime, double newTime);


    /**
     * @brief Returns the number of keyframes for this spline.
     **/
    int getKeyframesCount() const;

    static void deCastelJau(bool isOpenBezier,
                            bool useGuiCurves,
                            const std::list<BezierCPPtr>& cps, double time, unsigned int mipmapLevel,
                            bool finished,
                            int nBPointsPerSegment,
                            const Transform::Matrix3x3& transform,
                            std::list<std::list<ParametricPoint> >* points,
                            std::list<ParametricPoint >* pointsSingleList,
                            RectD* bbox);
    static void point_line_intersection(const Point &p1,
                                        const Point &p2,
                                        const Point &pos,
                                        int *winding);

    /**
     * @brief Evaluates the spline at the given time and returns the list of all the points on the curve.
     * @param nbPointsPerSegment controls how many points are used to draw one Bezier segment
     **/
    void evaluateAtTime_DeCasteljau(bool useGuiCurves,
                                    double time,
                                    unsigned int mipmapLevel,
#ifdef ROTO_BEZIER_EVAL_ITERATIVE
                                    int nbPointsPerSegment,
#else
                                    double errorScale,
#endif
                                    std::list<std::list<ParametricPoint> >* points,
                                    RectD* bbox) const;

    void evaluateAtTime_DeCasteljau(bool useGuiCurves,
                                    double time,
                                    unsigned int mipmapLevel,
#ifdef ROTO_BEZIER_EVAL_ITERATIVE
                                    int nbPointsPerSegment,
#else
                                    double errorScale,
#endif
                                    std::list<ParametricPoint >* points,
                                    RectD* bbox) const;

private:

    void evaluateAtTime_DeCasteljau_internal(bool useGuiCurves,
                                    double time,
                                    unsigned int mipmapLevel,
#ifdef ROTO_BEZIER_EVAL_ITERATIVE
                                    int nbPointsPerSegment,
#else
                                    double errorScale,
#endif
                                    std::list<std::list<ParametricPoint> >* points,
                                    std::list<ParametricPoint >* pointsSingleList,
                                    RectD* bbox) const;

public:

    /**
     * @brief Same as evaluateAtTime_DeCasteljau but nbPointsPerSegment is approximated automatically
     **/
    void evaluateAtTime_DeCasteljau_autoNbPoints(bool useGuiCurves,
                                                 double time,
                                                 unsigned int mipmapLevel,
                                                 std::list<std::list<ParametricPoint> >* points,
                                                 RectD* bbox) const;

    /**
     * @brief Evaluates the bezier formed by the feather points. Segments which are equal to the control points of the bezier
     * will not be drawn.
     **/
    void evaluateFeatherPointsAtTime_DeCasteljau(bool useGuiCurves,
                                                 double time,
                                                 unsigned int mipmapLevel,
#ifdef ROTO_BEZIER_EVAL_ITERATIVE
                                                 int nbPointsPerSegment,
#else
                                                 double errorScale,
#endif
                                                 bool evaluateIfEqual,
                                                 std::list<std::list<ParametricPoint>  >* points,
                                                 RectD* bbox) const;

    void evaluateFeatherPointsAtTime_DeCasteljau(bool useGuiCurves,
                                                 double time,
                                                 unsigned int mipmapLevel,
#ifdef ROTO_BEZIER_EVAL_ITERATIVE
                                                 int nbPointsPerSegment,
#else
                                                 double errorScale,
#endif
                                                 bool evaluateIfEqual,
                                                 std::list<ParametricPoint >* points,
                                                 RectD* bbox) const;

private:

    void evaluateFeatherPointsAtTime_DeCasteljau_internal(bool useGuiCurves,
                                                          double time,
                                                          unsigned int mipmapLevel,
#ifdef ROTO_BEZIER_EVAL_ITERATIVE
                                                          int nbPointsPerSegment,
#else
                                                          double errorScale,
#endif
                                                          bool evaluateIfEqual,
                                                          std::list<std::list<ParametricPoint>  >* points,
                                                          std::list<ParametricPoint >* pointsSingleList,
                                                          RectD* bbox) const;

public:

    /**
     * @brief Returns the bounding box of the bezier. The last value computed by evaluateAtTime_DeCasteljau will be returned,
     * otherwise if it has never been called, evaluateAtTime_DeCasteljau will be called to compute the bounding box.
     **/
    virtual RectD getBoundingBox(double time) const OVERRIDE;

    /**
     * @brief The polygons the viewer overlay draws at time, evaluated from the GUI copy, with the
     * feather when withFeather is true. The last result is kept until the shape, its GUI copy or
     * its transform changes, or another time is asked for. Main thread only.
     **/
    BezierOverlayPolygonsConstPtr getOverlayPolygons(double time, bool withFeather) const;
    static void bezierSegmentListBboxUpdate(bool useGuiCurves,
                                            const std::list<BezierCPPtr> & points,
                                            bool finished,
                                            bool isOpenBezier,
                                            double time,
                                            ViewIdx view,
                                            unsigned int mipmapLevel,
                                            const Transform::Matrix3x3& transform,
                                            RectD* bbox);

    /**
     * @brief Returns a const ref to the control points of the bezier curve. This can only ever be called on the main thread.
     **/
    const std::list<BezierCPPtr> & getControlPoints() const;

protected:

    std::list<BezierCPPtr> & getControlPoints_internal();

public:


    std::list<BezierCPPtr> getControlPoints_mt_safe() const;

    /**
     * @brief Returns a const ref to the feather points of the bezier curve. This can only ever be called on the main thread.
     **/
    const std::list<BezierCPPtr> & getFeatherPoints() const;
    std::list<BezierCPPtr> getFeatherPoints_mt_safe() const;
    enum ControlPointSelectionPrefEnum
    {
        eControlPointSelectionPrefFeatherFirst = 0,
        eControlPointSelectionPrefControlPointFirst,
        eControlPointSelectionPrefWhateverFirst
    };

    /**
     * @brief Returns a pointer to a nearby control point if any. This function  also returns the feather point
     * The first member is the actual point nearby, and the second the counter part (i.e: either the feather point
     * if the first is a control point, or the other way around).
     **/
    std::pair<BezierCPPtr, BezierCPPtr>isNearbyControlPoint(double x, double y, double acceptance, ControlPointSelectionPrefEnum pref, int* index) const;

    /**
     * @brief Given the control point in parameter, return its index in the curve's control points list.
     * If no such control point could be found, -1 is returned.
     **/
    int getControlPointIndex(const BezierCPPtr & cp) const;
    int getControlPointIndex(const BezierCP* cp) const;

    /**
     * @brief Given the feather point in parameter, return its index in the curve's feather points list.
     * If no such feather point could be found, -1 is returned.
     **/
    int getFeatherPointIndex(const BezierCPPtr & fp) const;

    /**
     * @brief Returns the control point at the given index if any, NULL otherwise.
     **/
    BezierCPPtr getControlPointAtIndex(int index) const;

    /**
     * @brief Returns the feather point at the given index if any, NULL otherwise.
     **/
    BezierCPPtr getFeatherPointAtIndex(int index) const;
    BezierCPPtr getFeatherPointForControlPoint(const BezierCPPtr & cp) const;
    BezierCPPtr getControlPointForFeatherPoint(const BezierCPPtr & fp) const;

    /**
     * @brief Returns all the control points/feather points within the rectangle
     * given by (l,r,b,t). Each control point is paired with its counter part.
     * The first member is always "the selected point" and the second the counter part.
     * @param mode An integer representing what the function should do.
     * mode == 0: Add all the cp/fp within the rect and their counter parts in the return value
     * mode == 1: Add only the cp within the rect and their respective feather points counter parts
     * mode == 2: Add only the fp within the rect and their repsective control points counter parts
     **/
    std::list<std::pair<BezierCPPtr, BezierCPPtr> >controlPointsWithinRect(double l, double r, double b, double t, double acceptance, int mode) const;
    static void leftDerivativeAtPoint(bool useGuiCurves, double time, const BezierCP & p, const BezierCP & prev, const Transform::Matrix3x3& transform, double *dx, double *dy);
    static void rightDerivativeAtPoint(bool useGuiCurves, double time, const BezierCP & p, const BezierCP & next, const Transform::Matrix3x3& transform, double *dx, double *dy);

    /**
     * @brief Computes the location of the feather extent relative to the current feather point position and
     * the given feather distance.
     * In the case the control point and the feather point of the bezier are distinct, this function just makes use
     * of Thales theorem.
       >     * If the feather point and the control point are equal then this function computes the left and right derivative
     * of the bezier at that point to determine the direction in which the extent is.
     * @returns The delta from the given feather point to apply to find out the extent position.
     *
     * Note that the delta will be applied to fp.
     **/
    static Point expandToFeatherDistance(bool useGuiCurve,
                                         const Point & cp,         //< the point
                                         Point* fp,         //< the feather point
                                         double featherDistance,         //< feather distance
                                         //const std::list<Point> & featherPolygon, //< the polygon of the bezier
                                         double time,         //< time
                                         bool clockWise,         //< is the bezier  clockwise oriented or not
                                         const Transform::Matrix3x3& transform,
                                         std::list<BezierCPPtr>::const_iterator prevFp,         //< iterator pointing to the feather before curFp
                                         std::list<BezierCPPtr>::const_iterator curFp,         //< iterator pointing to fp
                                         std::list<BezierCPPtr>::const_iterator nextFp);         //< iterator pointing after curFp
    enum FillRuleEnum
    {
        eFillRuleOddEven,
        eFillRuleWinding
    };


    bool isFeatherPolygonClockwiseOriented(bool useGuiCurve, double time) const;

    /**
     * @brief Forgets the orientation of the GUI (useGuiCurve) or internal polygon wherever an edit
     * at time can have reshaped it, so the next read computes it again.
     * Does nothing while setAutoOrientationComputation(false) is in effect.
     **/
    void invalidatePolygonOrientation(bool useGuiCurve, double time);

    /**
     * @brief Forgets the orientation of both polygons at every time, after an edit that changed
     * the points or whether the shape is closed.
     * Does nothing while setAutoOrientationComputation(false) is in effect.
     **/
    void invalidatePolygonOrientation();

    void setAutoOrientationComputation(bool autoCompute);

    bool dequeueGuiActions();

    /**
     * @brief Sets point, a control or feather point of this Bezier, to the GUI state of state
     * at every time. While the node renders only the GUI copy changes, and the render picks it
     * up when the queued GUI edits are dequeued, as with any other edit.
     **/
    void restorePointState(const BezierCPPtr& point, const BezierCP& state);

protected:
    virtual void appendGeometryContentHash(ContentHasher& h) const OVERRIDE;

private:

    virtual void onTransformSet(double time) OVERRIDE FINAL;

    bool isFeatherPolygonClockwiseOrientedInternal(bool useGuiCurve, double time) const;

    void computePolygonOrientation(bool useGuiCurves, double time, bool isStatic) const;

    /**
     * @brief The times between which an edit at time reshapes the polygon: the second keyframe
     * either side of time, because smooth interpolation derives each key's tangents from its
     * neighbours. Infinite on a side with fewer keyframes.
     **/
    void getOrientationSpan(bool useGuiCurve, double time, double* from, double* to) const;

    /**
     * @brief Erases the cached orientation of one side within getOrientationSpan(), and the
     * static one. itemMutex must be held.
     **/
    void eraseOrientationAroundTime(bool useGuiCurve, double time);

    /**
     * @brief Erases every cached orientation of one side. itemMutex must be held.
     **/
    void eraseOrientation(bool useGuiCurve);

    /*
     * @brief If the node is currently involved in a render, returns false, otherwise returns true
     */
    bool canSetInternalPoints() const;

    /**
     * @brief setKeyframe() on the internal curves. copyToGui also copies the new key and its
     * neighbours' re-derived tangents to the GUI copy; edit paths pass false and sync the GUI
     * copy themselves, so pending GUI edits made during a render are not overwritten.
     **/
    void setKeyframeInternal(double time, bool copyToGui);

    void copyInternalPointsToGuiPoints();

    /**
     * @brief Copies every control and feather point at time, and the orientation around time,
     * to the GUI copy. The GUI copy must hold the internal state everywhere else.
     **/
    void copyInternalPointsToGuiAtTime(double time);

    /**
     * @brief Copies one control point and its feather point, either of which may be null, at
     * every time to the GUI copy.
     **/
    void copyInternalPointToGui(BezierCP* cp, BezierCP* fp);

    /**
     * @brief Copies one control point and its feather point, either of which may be null, at
     * time only to the GUI copy.
     **/
    void copyInternalPointToGuiAtTime(BezierCP* cp, BezierCP* fp, double time);

    /**
     * @brief Copies the orientation cached within getOrientationSpan() of time, and the
     * orientation of an unkeyed shape, from one side of the GUI copy to the other.
     **/
    void copyOrientationAroundTime(bool toGui, double time);

    /**
     * @brief After an edit at time of a control point and its feather point, either of which may
     * be null, brings the GUI copy up to date, or during a render records what dequeueGuiActions()
     * must write back. allTimes tells that the edit changed the points at every keyframe, and
     * keyedAllPoints that its auto-key keyed every point at time. Takes itemMutex.
     **/
    void syncGuiAfterEdit(bool useGuiCurve, const BezierCPPtr& cp, const BezierCPPtr& fp, double time, bool allTimes, bool keyedAllPoints);

public:
    void toSerialization(Serialization::BezierSerialization* serialization) const;
    void fromSerialization(const Serialization::BezierSerialization& serialization);

    void getKeyframeTimes(std::set<double> *times) const;

    void getKeyframeTimesAndInterpolation(std::list<std::pair<double, KeyframeTypeEnum> > *keys) const;

    /**
     * @brief Get the nearest previous keyframe from the given time.
     * If nothing was found INT_MIN is returned.
     **/
    int getPreviousKeyframeTime(double time) const;

    /**
     * @brief Get the nearest next keyframe from the given time.
     * If nothing was found INT_MAX is returned.
     **/
    int getNextKeyframeTime(double time) const;

    int getKeyFrameIndex(double time) const;

    void setKeyFrameInterpolation(KeyframeTypeEnum interp, int index);


Q_SIGNALS:

    void aboutToClone();

    void cloned();

    void keyframeSet(double time);

    void keyframeRemoved(double time);

    void animationRemoved();

    void controlPointRemoved();

private:

    std::unique_ptr<BezierPrivate> _imp;
};

NATRON_NAMESPACE_EXIT


#endif // Engine_Bezier_h
