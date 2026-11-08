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

#ifndef ROTOWRAPPER_H
#define ROTOWRAPPER_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>

#include "Engine/MergingEnum.h"
#include "Engine/PyParameter.h"
#include "Engine/EngineFwd.h"

NATRON_NAMESPACE_ENTER;
NATRON_PYTHON_NAMESPACE_ENTER;

class Layer; // defined below
class Track;

class ItemBase
{
public:

    ItemBase(const RotoItemPtr& item);

    virtual ~ItemBase();

    RotoItemPtr getInternalItem() const
    {
        return _item;
    }

    void setLabel(const QString & name);
    QString getLabel() const;

    bool setScriptName(const QString& name);
    QString getScriptName() const;

    void setLocked(bool locked);
    bool getLocked() const;
    bool getLockedRecursive() const;

    void setVisible(bool activated);
    bool getVisible() const;

    Layer* getParentLayer() const;
    Param* getParam(const QString& name) const;

private:

    RotoItemPtr _item;
};

class Layer
    : public ItemBase
{
public:

    Layer(const RotoItemPtr& item);

    virtual ~Layer();

    void addItem(ItemBase* item);

    void insertItem(int pos, ItemBase* item);

    void removeItem(ItemBase* item);

    std::list<ItemBase*> getChildren() const;

private:

    RotoLayerPtr _layer;
};

class BezierCurve
    : public ItemBase
{
public:


    BezierCurve(const RotoItemPtr& item);

    virtual ~BezierCurve();


    void setCurveFinished(bool finished);
    bool isCurveFinished() const;

    void addControlPoint(double x, double y);

    void addControlPointOnSegment(int index, double t);

    void removeControlPointByIndex(int index);

    void movePointByIndex(int index, double time, double dx, double dy);

    void moveFeatherByIndex(int index, double time, double dx, double dy);

    void moveLeftBezierPoint(int index, double time, double dx, double dy);

    void moveRightBezierPoint(int index, double time, double dx, double dy);

    void setPointAtIndex(int index, double time, double x, double y, double lx, double ly, double rx, double ry);

    void setFeatherPointAtIndex(int index, double time, double x, double y, double lx, double ly, double rx, double ry);

    int getNumControlPoints() const;

    /**
     * @brief Makes the control point at index, its tangents and its feather point follow track,
     * keeping their offsets from the track's centre at the current frame. Raises ValueError for an
     * index out of range or no track.
     **/
    void linkPointToTrack(int index, Track* track);

    /**
     * @brief Stops the control point at index following its track. With bake, the point is
     * keyed where the track took it, at every frame of the track's keyed range; otherwise its
     * own animation applies again. Raises ValueError for an index out of range.
     **/
    void unlinkPoint(int index, bool bake);

    void getKeyframes(std::list<double>* keys) const;

    void getControlPointPosition(int index, double time, double* x, double *y, double *lx, double *ly, double *rx, double *ry) const;

    void getFeatherPointPosition(int index, double time, double* x, double *y, double *lx, double *ly, double *rx, double *ry) const;

    void setActivated(double time, bool activated);
    bool getIsActivated(double time);

    void setOpacity(double opacity, double time);
    double getOpacity(double time) const;

    ColorTuple getOverlayColor() const;
    void setOverlayColor(double r, double g, double b);

    double getFeatherDistance(double time) const;
    void setFeatherDistance(double dist, double time);

    double getFeatherFallOff(double time) const;
    void setFeatherFallOff(double falloff, double time);

    ColorTuple getColor(double time);
    void setColor(double time, double r, double g, double b);

    void setCompositingOperator(Natron::MergingFunctionEnum op);
    Natron::MergingFunctionEnum getCompositingOperator() const;
    BooleanParam* getActivatedParam() const;
    DoubleParam* getOpacityParam() const;
    DoubleParam* getFeatherDistanceParam() const;
    DoubleParam* getFeatherFallOffParam() const;
    ColorParam* getColorParam() const;
    ChoiceParam* getCompositingOperatorParam() const;

private:

    BezierPtr _bezier;
};

class Roto
{
public:
    Roto(const RotoCurveContextPtr& ctx);

    ~Roto();

    Layer* getBaseLayer() const;
    ItemBase* getItemByName(const QString& name) const;
    Layer* createLayer();
    BezierCurve* createBezier(double x, double y, double time);
    BezierCurve* createEllipse(double x, double y, double diameter, bool fromCenter, double time);
    BezierCurve* createRectangle(double x, double y, double size, double time);

    /**
     * @brief Moves each item with the motion vectors of the node source, a fully qualified name,
     * over first..last, and returns once done, as one undo step. direction is "forward" or
     * "backward", model is "perPoint", "translate", "affine" or "perspective". Between two keys, it
     * tracks both ways and blends the two so it meets both exactly. The frame a follow starts from,
     * first going forward and last going backward, counts as a key when it is unkeyed but the item
     * has a key before it, going forward, or after it, going backward; then it keeps what it shows
     * there. Otherwise it tracks back to that frame from the nearest key ahead. A key is one of the
     * points for "perPoint", and one of the transform knobs the model keys for a fit: translate,
     * or for "affine" and "perspective" any of translate, rotate, scale, skew, center and the extra
     * matrix. Returns a dict of the items' names to {"inferred": [(first, last), ...],
     * "stopFrame": frame or None, "passStops": [...], "unfollowed": [(first, last), ...]}. A
     * passStops entry is a pass that ended before the key it went to, or that the source has no
     * layer for, as {"direction": "forward" or "backward", "reason": "lowConfidence",
     * "unreadable" or "noLayer", "frame": the last it reached, "otherOnly": (first, last) or None,
     * the frames the other pass carried alone}; unfollowed lists the frames between two keys that
     * neither pass reached. Raises ValueError when source has no forward motion layer, has no
     * backward one for a backward follow, or depends on this node, and for a direction or model it
     * does not know.
     **/
    PyObject* followVectors(const std::list<ItemBase*>& items,
                            const QString& source,
                            int first,
                            int last,
                            const QString& direction,
                            const QString& model,
                            bool reduce = false,
                            double tolerance = 0.5,
                            double confidenceThreshold = 0.5);

    /**
     * @brief True while a follow started from the Follow Vectors buttons or the curve panel's
     * Follow Vectors... entry runs. Such a follow runs in the background and returns at once.
     **/
    bool isFollowingVectors() const;

    /**
     * @brief The report of the last follow started from the Follow Vectors buttons or the curve
     * panel's Follow Vectors... entry, or None while it runs, after a press that was refused (the
     * reason is then the node's warning message), and before any. A report is a dict:
     * {"status": "done", "aborted", "sourceLost" or "error", "error": the error or "", "message":
     * the text shown to the user, "shapes": the items' names to entries as followVectors() returns
     * them}. On "sourceLost" the follow was reverted. followVectors() does not change it.
     * The node also keeps the report's text as its message (getPersistentMessage()), without a
     * warning or error mark; a follow that stopped early, was aborted or lost its source opens a
     * dialog with it as well, a clean one does not.
     **/
    PyObject* getLastFollowReport() const;

private:
    RotoCurveContextPtr _ctx;
};

NATRON_PYTHON_NAMESPACE_EXIT;
NATRON_NAMESPACE_EXIT;

#endif // ROTOWRAPPER_H
