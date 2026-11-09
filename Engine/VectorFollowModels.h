/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
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

#ifndef Engine_VectorFollowModels_h
#define Engine_VectorFollowModels_h

#include "Global/Macros.h"

#include <vector>

#include "Engine/VectorFollow.h"

NATRON_NAMESPACE_ENTER

struct VectorFollowFit {
    FollowMatrix matrix;
    // False when nothing could be fitted, and matrix is the identity.
    bool valid = false;
    // The model actually fitted: too few or degenerate samples fall back to a simpler one.
    VectorFollowModel model = VectorFollowModel::eTranslate;
};

/**
 * @brief Fits the motion from src to dst by IRLS with a Huber weight, on top of weights. A sample
 * of weight 0 is left out. ePerPoint fits eAffine.
 **/
VectorFollowFit fitFollowMotion(VectorFollowModel model,
                                const std::vector<FollowPoint>& src,
                                const std::vector<FollowPoint>& dst,
                                const std::vector<double>& weights);

/**
 * @brief The least-squares solution with no outlier handling, for correspondences known to be good.
 **/
VectorFollowFit solveFollowMotion(VectorFollowModel model,
                                  const std::vector<FollowPoint>& src,
                                  const std::vector<FollowPoint>& dst);

/**
 * @brief A grid inside polygon, its spacing taken from the polygon's bounding box so a shape gets
 * about as many samples whatever its size. A degenerate polygon gives none.
 **/
std::vector<FollowPoint> followGridSamplesInside(const std::vector<FollowPoint>& polygon);

NATRON_NAMESPACE_EXIT

#endif // Engine_VectorFollowModels_h
