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

#ifndef Tests_Native_FilterKnobRenderChecks_h
#define Tests_Native_FilterKnobRenderChecks_h

#include <functional>
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "NativeParity.h"
#include "RenderBothWays.h"

#include "Engine/ImageLayerDesc.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Whether node's RGBA render over window differs after each setter runs. The node has to
 * sample between pixel centres, or every interpolating filter returns the source.
 **/
inline ::testing::AssertionResult
settingChangesTheRender(const NodePtr& node,
                        const RectI& window,
                        double time,
                        const std::function<bool()>& first,
                        const std::function<bool()>& second)
{
    std::vector<float> pixels[2];
    const std::function<bool()>* setters[2] = { &first, &second };

    for (int i = 0; i < 2; ++i) {
        if (!(*setters[i])()) {
            return ::testing::AssertionFailure() << "setter " << i << " failed";
        }
        std::list<ImageLayerDesc> layers;
        std::vector<RenderedPlane> planes;
        std::string error;
        layers.push_back(ImageLayerDesc::getRGBAComponents());
        if (!renderNodePlanesDirect(node, time, ViewIdx(0), 0, window, layers, &planes, &error) || (planes.size() != 1)) {
            return ::testing::AssertionFailure() << "render " << i << " failed: " << error;
        }
        pixels[i] = planes[0].pixels;
    }
    if (pixels[0] == pixels[1]) {
        return ::testing::AssertionFailure() << "the two renders are identical";
    }

    return ::testing::AssertionSuccess();
}

inline bool
selectFilter(const NodePtr& node,
             Resampler::FilterEnum filter)
{
    return setKnobValue(node, kResamplerParamFilterType, std::string(Resampler::filterOptions()[(std::size_t)filter].id));
}

inline ::testing::AssertionResult
filterChoiceChangesTheRender(const NodePtr& node,
                             const RectI& window,
                             double time,
                             Resampler::FilterEnum first,
                             Resampler::FilterEnum second)
{
    return settingChangesTheRender(node, window, time, [&] { return selectFilter(node, first); }, [&] { return selectFilter(node, second); });
}

inline ::testing::AssertionResult
sincLobesChangeTheRender(const NodePtr& node,
                         const RectI& window,
                         double time)
{
    return settingChangesTheRender(node, window, time, [&] { return selectFilter(node, Resampler::eFilterSinc) && setKnobValues(node, kResamplerParamLobes, { 2. }); }, [&] { return selectFilter(node, Resampler::eFilterSinc) && setKnobValues(node, kResamplerParamLobes, { 8. }); });
}

inline ::testing::AssertionResult
bicubicParametersChangeTheRender(const NodePtr& node,
                                 const RectI& window,
                                 double time)
{
    const auto setBicubic = [&](double b,
                                double c) {
        return selectFilter(node, Resampler::eFilterBicubic) && setKnobValues(node, kResamplerParamBicubicB, { b }) && setKnobValues(node, kResamplerParamBicubicC, { c });
    };

    return settingChangesTheRender(node, window, time, [&] { return setBicubic(1., 0.); }, [&] { return setBicubic(0., 0.5); });
}

NATRON_NAMESPACE_EXIT

#endif // Tests_Native_FilterKnobRenderChecks_h
