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

#ifndef NATRON_ENGINE_PYSPLINEWARP_H
#define NATRON_ENGINE_PYSPLINEWARP_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <memory>

#include "Engine/EngineFwd.h"

NATRON_NAMESPACE_ENTER

class SplineWarpContext;
class SplineWarpLink;

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_ENTER
NATRON_PYTHON_NAMESPACE_ENTER

/**
 * @brief A link between a source and a destination curve of a SplineWarp node. Edits raise
 * ValueError when a value is out of range or the link has been removed.
 **/
class SplineWarpCurveLink {
public:
    SplineWarpCurveLink(const std::shared_ptr<SplineWarpContext>& ctx,
                        const std::shared_ptr<SplineWarpLink>& link);

    ~SplineWarpCurveLink();

    BezierCurve* getSource() const;
    BezierCurve* getDestination() const;

    bool isValid() const;

    double getAmount(double time) const;
    void setAmount(double value, double time);
    void setAmountAtTime(double time, double value);

    double getStartOffset() const;
    bool isReversed() const;
    void setAlignment(double startOffset, bool reversed);

    int getCorrespondenceCount() const;

    /**
     * @brief Adds a static correspondence between a source and a destination curve parameter and
     * returns its index.
     **/
    int addCorrespondence(double sourceParam, double destinationParam);
    void setCorrespondence(int index, double sourceParam, double destinationParam, double time);
    void setCorrespondenceAtTime(int index, double time, double sourceParam, double destinationParam);
    void removeCorrespondence(int index);

    /**
     * @brief The (source, destination) parameters of every correspondence at time, flattened.
     **/
    void getCorrespondences(double time, std::list<double>* params) const;

    void remove();

private:
    bool refuses() const;

    std::shared_ptr<SplineWarpContext> _ctx;
    std::shared_ptr<SplineWarpLink> _link;
};

/**
 * @brief The links, boundaries and layer amounts of a SplineWarp node's curves. The curves themselves are made and
 * edited through the node's Roto context.
 **/
class SplineWarpCurves {
public:
    SplineWarpCurves(const std::shared_ptr<SplineWarpContext>& ctx);

    ~SplineWarpCurves();

    SplineWarpCurveLink* link(BezierCurve* source, BezierCurve* destination, bool duplicate = false);
    void unlink(BezierCurve* curve);
    SplineWarpCurveLink* getLink(BezierCurve* curve) const;
    std::list<SplineWarpCurveLink*> getLinks() const;

    void setBoundary(BezierCurve* curve, bool boundary);
    bool isBoundary(BezierCurve* curve) const;

    // A soft boundary pins the warp along the curve, open or closed, without masking.
    void setSoftBoundary(BezierCurve* curve, bool boundary);
    bool isSoftBoundary(BezierCurve* curve) const;

    // A layer's amount scales every link whose destination curve is inside it.
    double getLayerAmount(Layer* layer, double time) const;
    void setLayerAmount(Layer* layer, double value, double time);
    void setLayerAmountAtTime(Layer* layer, double time, double value);

private:
    bool refuses() const;

    std::shared_ptr<SplineWarpContext> _ctx;
};

NATRON_PYTHON_NAMESPACE_EXIT
NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_PYSPLINEWARP_H
