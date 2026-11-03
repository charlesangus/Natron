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

#ifndef Engine_Nodes_Time_TimeSampledEffect_h
#define Engine_Nodes_Time_TimeSampledEffect_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"

#define kTimeSampleParamFilter "filter"
#define kTimeSampleParamShutter "shutter"

#define kTimeSampleFilterNearest "nearest"
#define kTimeSampleFilterBlend "blend"
#define kTimeSampleFilterFractional "fractional"
#define kTimeSampleFilterBox "box"

NATRON_NAMESPACE_ENTER

namespace TimeSample {
enum Filter {
    eFilterNearest = 0,
    eFilterBlend,
    eFilterFractional,
    eFilterBox,
};

/**
 * @brief What the output at a time shows: nothing (black), the input at times[0], or the
 * weighted mean of the input at each of times, ascending whole frames, weighted by weights.
 **/
struct Sample {
    enum Kind {
        eBlack,
        eInput,
        eBlend,
    };

    Kind kind = eBlack;
    std::vector<double> times;
    std::vector<double> weights;
};

/**
 * @brief The source time s through filter. Except with Box, a source time within 1e-6 of a whole
 * frame counts as that frame. Box averages the input frames [low, high] covers, each weighted by
 * its overlap; while s lies in [keepLow, keepHigh] the span stays within half a frame of that
 * range, and it never reaches further than 127.5 frames from s.
 **/
Sample filter(Filter filter, double s, double low, double high, double keepLow, double keepHigh);
} // namespace TimeSample

/**
 * @brief A node whose output at a time is a TimeSample::Sample of its only input: a route to the
 * input at one time, black, or a weighted mean of whole input frames that renders on the CPU or
 * the GPU through TimeBlend. Owns the Filter and Shutter knobs.
 **/
class TimeSampledEffect
    : public StagedImageEffect {
public:
    using Sample = TimeSample::Sample;

    explicit TimeSampledEffect(NodePtr node);

    virtual ~TimeSampledEffect();

    virtual Sample sample(double time, ViewIdx view) const WARN_UNUSED_RETURN = 0;

    virtual Route route(double time,
                        ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual FramesNeededMap getFramesNeeded(double time,
                                            ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum getRegionOfDefinition(U64 hash,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;

protected:
    // Adds Filter, then Shutter, to page.
    void createFilterKnobs(const KnobPagePtr& page);

    TimeSample::Filter filterValue() const WARN_UNUSED_RETURN;

    // The shutter at time, at least 0.
    double shutterAt(double time, ViewIdx view) const WARN_UNUSED_RETURN;

private:
    virtual bool isOpIdentity(double time,
                              const RenderScale& scale,
                              const RectI& roi,
                              ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum renderOp(const RenderActionArgs& args,
                                const StagePlan& plan) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual bool canRenderOpOnGpu(const GpuQuery& query) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual bool getOpGpuInputLayer(int inputNb,
                                    const ImageLayerDesc& plane,
                                    double time,
                                    ViewIdx view,
                                    ImageLayerDesc* layer) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum renderOpGpu(const GpuRenderArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    KnobChoiceWPtr _filter;
    KnobDoubleWPtr _shutter;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Time_TimeSampledEffect_h
