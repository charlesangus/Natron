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

#ifndef Serialization_SplineWarpSerialization_h
#define Serialization_SplineWarpSerialization_h

#include "Global/Macros.h"

#include <list>
#include <string>

#include "Serialization/CurveSerialization.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

/**
 * @brief A value that is static until its curve holds a keyframe.
 **/
struct SplineWarpAnimatedValueSerialization {
    double value;
    CurveSerialization curve;

    SplineWarpAnimatedValueSerialization()
        : value(0.)
        , curve()
    {
    }

    bool operator==(const SplineWarpAnimatedValueSerialization&) const = default;
};

/**
 * @brief A source-curve parameter paired with a destination-curve parameter.
 **/
struct SplineWarpCorrespondenceSerialization {
    SplineWarpAnimatedValueSerialization source;
    SplineWarpAnimatedValueSerialization destination;

    bool operator==(const SplineWarpCorrespondenceSerialization&) const = default;
};

/**
 * @brief A link between two curves of the context, named by their script names.
 **/
struct SplineWarpLinkSerialization {
    std::string source;
    std::string destination;
    double startOffset;
    bool reversed;
    SplineWarpAnimatedValueSerialization amount;
    std::list<SplineWarpCorrespondenceSerialization> correspondences;

    SplineWarpLinkSerialization()
        : source()
        , destination()
        , startOffset(0.)
        , reversed(false)
        , amount()
        , correspondences()
    {
    }

    bool operator==(const SplineWarpLinkSerialization&) const = default;
};

/**
 * @brief The amount of a layer of the context, named by its script name.
 **/
struct SplineWarpLayerAmountSerialization {
    std::string layer;
    SplineWarpAnimatedValueSerialization amount;

    bool operator==(const SplineWarpLayerAmountSerialization&) const = default;
};

/**
 * @brief What a SplineWarp curve context holds beside its curves: the boundary, soft-boundary and
 * cookie-cutter curves, the layer amounts and the links, saved with the curves and resolved after them.
 **/
struct SplineWarpTransformValueSerialization {
    std::string knob;
    int dimension;
    SplineWarpAnimatedValueSerialization value;

    SplineWarpTransformValueSerialization()
        : knob()
        , dimension(0)
        , value()
    {
    }

    bool operator==(const SplineWarpTransformValueSerialization&) const = default;
};

struct SplineWarpLayerTransformSerialization {
    std::string layer;
    std::list<SplineWarpTransformValueSerialization> values;

    bool operator==(const SplineWarpLayerTransformSerialization&) const = default;
};

struct SplineWarpTransformLinkSerialization {
    std::string item;
    std::string target;

    bool operator==(const SplineWarpTransformLinkSerialization&) const = default;
};

struct SplineWarpSerialization {
    std::list<std::string> boundaries;
    std::list<std::string> softBoundaries;
    std::list<std::string> cookieCutters;
    std::list<SplineWarpLayerAmountSerialization> layerAmounts;
    std::list<SplineWarpLinkSerialization> links;
    std::list<SplineWarpLayerTransformSerialization> layerTransforms;
    std::list<SplineWarpTransformLinkSerialization> transformLinks;

    bool operator==(const SplineWarpSerialization&) const = default;
};

} // namespace Serialization
NATRON_NAMESPACE_EXIT

#endif // Serialization_SplineWarpSerialization_h
