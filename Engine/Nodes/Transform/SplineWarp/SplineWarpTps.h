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

#ifndef Engine_Nodes_Transform_SplineWarp_SplineWarpTps_h
#define Engine_Nodes_Transform_SplineWarp_SplineWarpTps_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>
#include <vector>

#include "Global/GlobalDefines.h"

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpCache.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpSampling.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief A map from destination positions to source positions through constraint pairs, in
 * canonical coordinates: a thin-plate spline, or a simpler fit when the pairs cannot determine
 * one. Immutable once solved, so it is evaluated from any thread.
 **/
class SplineWarpTps {
public:
    enum ModelEnum {
        // No pair.
        eModelIdentity = 0,
        // One pair.
        eModelTranslation,
        // Two pairs.
        eModelSimilarity,
        // Collinear destinations, or a thin-plate system that could not be solved: the least-squares
        // affine map of minimum norm, so the direction across the line keeps its scale.
        eModelAffine,
        eModelThinPlate
    };

    SplineWarpTps();

    /**
     * @param smoothness The regularisation of the thin-plate fit, 0 for an exact fit. It is
     * relative to the pairs' spread, so the same value smooths alike at any size.
     **/
    static SplineWarpTps solve(const std::vector<SplineWarpSamplePair>& pairs, double smoothness);

    ModelEnum getModel() const
    {
        return _model;
    }

    bool isIdentity() const
    {
        return _model == eModelIdentity;
    }

    Point evaluate(double x, double y) const;

    /**
     * @brief The bending energy trace(W^T K W) of the thin-plate part, in the normalized
     * coordinates the fit is solved in; 0 for the other models.
     **/
    double getBendingEnergy() const
    {
        return _bendingEnergy;
    }

private:
    void setAffine(const double m[2][2], const double offset[2]);

    ModelEnum _model;
    // The non-thin-plate models: source = _matrix * destination + _offset.
    double _matrix[2][2];
    double _offset[2];
    // The thin-plate model, on destinations normalized as (p - _center) * _invScale:
    // source = _affine[0] + _affine[1] x + _affine[2] y + sum_i _weights[i] U(|p - _centers[i]|).
    Point _center;
    double _invScale;
    std::vector<double> _centersX;
    std::vector<double> _centersY;
    std::vector<double> _weightsX;
    std::vector<double> _weightsY;
    double _affineX[3];
    double _affineY[3];
    double _bendingEnergy;
};

typedef std::shared_ptr<const SplineWarpTps> SplineWarpTpsConstPtr;

/**
 * @brief Solved maps by sample-set hash and smoothness, shared by every render of a node.
 **/
class SplineWarpTpsCache {
public:
    explicit SplineWarpTpsCache(std::size_t capacity = 4)
        : _cache(capacity)
    {
    }

    SplineWarpTpsConstPtr get(U64 sampleHash,
                              double smoothness,
                              const std::vector<SplineWarpSamplePair>& pairs);

    void clear()
    {
        _cache.clear();
    }

private:
    struct Key {
        U64 hash;
        double smoothness;

        bool operator==(const Key& o) const
        {
            return hash == o.hash && smoothness == o.smoothness;
        }
    };

    SplineWarpLruCache<Key, SplineWarpTps> _cache;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Transform_SplineWarp_SplineWarpTps_h
