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

#ifndef NATRON_TESTS_NATIVE_RAWGPUTESTSUPPORT_H
#define NATRON_TESTS_NATIVE_RAWGPUTESTSUPPORT_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include <ofxImageEffect.h>

#include "BaseTest.h"
#include "NativeGpuParity.h"
#include "RawFixtures.h"
#include "RawTestSource.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/Node.h"
#include "Engine/Nodes/IO/NativeRead.h"
#include "Engine/Nodes/IO/RawDecodeSupport.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief What the raw develop nodes' GPU parity tests share: Reads of the raw fixtures, CPU
 * renders of a node and of its source over the windows the GPU path reads, and the comparison of
 * the node's renderGpu() with its own CPU render.
 **/
class RawGpuTest
    : public BaseTest {
protected:
    static constexpr double kTime = 1.;

    virtual void SetUp() OVERRIDE
    {
        registerRawTestSourceOnce();
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
        RawDecodeSupport::clearCaches();
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        RawDecodeSupport::clearCaches();
        BaseTest::TearDown();
    }

    NodePtr createRead(const RawFixture& fixture)
    {
        NodePtr read = createNode(QString::fromUtf8(PLUGINID_NATRON_READ), PLUGIN_MAJOR_NATRON_READ);

        EXPECT_TRUE(bool(read));
        if (!read) {
            return NodePtr();
        }
        KnobFile* file = dynamic_cast<KnobFile*>(read->getKnobByName(kOfxImageEffectFileParamName).get());
        EXPECT_TRUE(file != NULL);
        if (file) {
            file->setValue(fixture.path());
        }

        return read;
    }

    NodePtr createAfter(const char* pluginID,
                        const NodePtr& input)
    {
        NodePtr node = createNode(QString::fromUtf8(pluginID));

        EXPECT_TRUE(bool(node));
        if (node && input) {
            connectNodes(input, node, 0, true);
        }

        return node;
    }

    static StagedImageEffect* stagedOf(const NodePtr& node)
    {
        return node ? dynamic_cast<StagedImageEffect*>(node->getEffectInstance().get()) : NULL;
    }

    // The colour layer of node over window on the CPU, bypassing the cache.
    static bool renderCpu(const NodePtr& node,
                          const RectI& window,
                          RenderedPlane* plane)
    {
        std::string error;
        const bool ok = renderRawTestPlane(node, window, plane, &error);

        EXPECT_TRUE(ok) << error;

        return ok;
    }

    static GpuParityImage imageOf(const RenderedPlane& plane)
    {
        GpuParityImage image;

        image.bounds = plane.window;
        image.nComps = (int)plane.channels.size();
        image.pixels = plane.pixels;

        return image;
    }

    static GpuQuery queryFor(int nComps)
    {
        GpuQuery query;

        query.time = kTime;
        query.view = ViewIdx(0);
        query.nComps = nComps;
        query.bitDepth = eImageBitDepthFloat;

        return query;
    }

#ifdef NATRON_HAVE_GPU
    /**
     * @brief Renders node over window on the CPU and through renderGpu(), fed the CPU render of
     * source over the region the GPU path reads, and expects every float within tolerance.
     * Returns the CPU render.
     **/
    RenderedPlane expectGpuMatchesCpu(GpuParityRuntime& runtime,
                                      const NodePtr& node,
                                      const NodePtr& source,
                                      const RectI& window,
                                      const ParityTolerance& tolerance,
                                      const std::string& what,
                                      const GpuQuery* customQuery = NULL)
    {
        RenderedPlane cpu;
        StagedImageEffect* effect = stagedOf(node);

        EXPECT_TRUE(effect != NULL) << what;
        if (!effect || !renderCpu(node, window, &cpu)) {
            return cpu;
        }
        const RectI sourceWindow = effect->getGpuInputRoI(0, window, RenderScale::identity);
        RenderedPlane input;
        if (!renderCpu(source, sourceWindow, &input)) {
            return cpu;
        }
        const GpuQuery query = customQuery ? *customQuery : queryFor((int)cpu.channels.size());
        EXPECT_TRUE(effect->canRenderOnGpu(query)) << what;
        if (!effect->canRenderOnGpu(query)) {
            return cpu;
        }
        const GpuRunResult run = runRenderGpu(runtime, *effect, query, std::vector<GpuParityImage>(1, imageOf(input)), nullptr, window);
        EXPECT_TRUE(run.ok) << what;
        if (!run.ok) {
            return cpu;
        }
        EXPECT_EQ(0u, run.writtenOutside) << what;
        EXPECT_EQ(cpu.pixels.size(), run.pixels.size()) << what;
        if (cpu.pixels.size() != run.pixels.size()) {
            return cpu;
        }
        const ImageDiff diff = compareImages(cpu.pixels, run.pixels, tolerance);
        const std::size_t first = diff.firstMismatchIndex;
        EXPECT_EQ(0u, diff.mismatches) << what << ": " << diff.mismatches << " of " << diff.compared << " floats differ, the first at float " << first
                                       << " (cpu " << cpu.pixels[first] << ", gpu " << run.pixels[first] << "), the worst by " << diff.worstAbsolute;
        double worstRatio = 0.;
        for (std::size_t i = 0; i < cpu.pixels.size(); ++i) {
            const double reference = cpu.pixels[i];
            const double bound = tolerance.absolute + tolerance.relative * std::max(1., std::fabs(reference));
            const double error = std::fabs(reference - (double)run.pixels[i]);
            if (std::isfinite(error) && error > 0.) {
                worstRatio = std::max(worstRatio, bound > 0. ? error / bound : std::numeric_limits<double>::infinity());
            }
        }
        std::printf("[          ] %s: %zu floats, worst difference %g, worst error/bound %.3f\n", what.c_str(), diff.compared, diff.worstAbsolute, worstRatio);

        return cpu;
    }
#endif // NATRON_HAVE_GPU
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_TESTS_NATIVE_RAWGPUTESTSUPPORT_H
