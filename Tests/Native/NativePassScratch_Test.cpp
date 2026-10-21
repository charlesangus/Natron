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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "NativeParity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/Format.h"
#include "Engine/HostImageMemory.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/EdgeDetect.h"
#include "Engine/Nodes/Filter/ErodeDilate.h"
#include "Engine/Nodes/Generator/CheckerBoard.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

// Large enough that each scratch plane set of an RGBA render is well past the 32 MiB at which the
// allocator switches to huge pages (1800 x 1500 x 4 floats = 41 MiB).
const int kWidth = 1800;
const int kHeight = 1500;
const double kTime = 1.;

// Pins the pool for the duration of a test: it may not trim or shrink under the test's feet.
class PoolGuard
{
public:
    PoolGuard()
        : _maxPoolBytes(HostImageMemory::maxPoolBytes())
    {
        HostImageMemory::trim();
        HostImageMemory::setMaxPoolBytes((std::size_t)8 << 30);
        HostImageMemory::setIdleTrim((std::size_t)8 << 30, 10 * 60 * 1000, 1000);
    }

    ~PoolGuard()
    {
        HostImageMemory::resetIdleTrim();
        HostImageMemory::setMaxPoolBytes(_maxPoolBytes);
        HostImageMemory::trim();
    }

private:
    std::size_t _maxPoolBytes;
};

bool
sameBits(const std::vector<float>& a,
         const std::vector<float>& b,
         std::size_t* firstDifference)
{
    if (a.size() != b.size()) {
        *firstDifference = 0;

        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::memcmp(&a[i], &b[i], sizeof(float)) != 0) {
            *firstDifference = i;

            return false;
        }
    }

    return true;
}
} // namespace

class NativePassScratchTest
    : public BaseTest {
protected:
    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kWidth, kHeight, "nativePassScratch", 1.));
    }

    NodePtr makeSource()
    {
        NodePtr checker = createNode(QString::fromUtf8(PLUGINID_NATRON_CHECKERBOARD), PLUGIN_MAJOR_NATRON_CHECKERBOARD);

        EXPECT_TRUE(bool(checker));
        if (checker) {
            EXPECT_TRUE(setKnobValue(checker, kNativeGeneratorParamExtent, std::string(kNativeGeneratorExtentProject)));
            EXPECT_TRUE(setKnobValues(checker, kCheckerBoardParamBoxSize, { 37., 37. }));
        }

        return checker;
    }

    // Renders `node` over the whole frame once with an empty pool, so that its scratch is fresh
    // pages, then again with every pooled buffer full of NaN: the two must match bit for bit,
    // which they cannot if any scratch element is read before it is written.
    void expectScratchIsFullyWritten(const NodePtr& node,
                                     std::size_t minPooledBuffers,
                                     const std::string& caseName)
    {
        PoolGuard guard;
        const RectI window(0, 0, kWidth, kHeight);
        const std::list<ImageLayerDesc> rgba(1, ImageLayerDesc::getRGBAComponents());
        std::vector<RenderedPlane> clean;
        std::vector<RenderedPlane> poisoned;
        std::string error;

        ASSERT_TRUE(renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, window, rgba, &clean, &error)) << caseName << ": " << error;
        ASSERT_EQ(1u, clean.size());
        // The scratch of the first render is back in the pool, so that the second one is served from it.
        ASSERT_GE(HostImageMemory::pooledBufferCount(), minPooledBuffers) << caseName;
        // NaN is the worst value to be read by mistake.
        HostImageMemory::fillPooled(std::numeric_limits<float>::quiet_NaN());

        ASSERT_TRUE(renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, window, rgba, &poisoned, &error)) << caseName << ": " << error;
        ASSERT_EQ(1u, poisoned.size());
        std::size_t at = 0;
        EXPECT_TRUE(sameBits(clean[0].pixels, poisoned[0].pixels, &at)) << caseName << ": first difference at float " << at;
        // Something was drawn: an all-zero result would pass trivially.
        float peak = 0.f;
        for (const float v : clean[0].pixels) {
            peak = std::max(peak, std::abs(v));
        }
        EXPECT_GT(peak, 0.f) << caseName;
        EXPECT_FALSE(std::isnan(peak)) << caseName;
    }
};

TEST_F(NativePassScratchTest, EdgeDetectScratchIsFullyWrittenBeforeRead)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr edge = createNode(QString::fromUtf8(PLUGINID_NATRON_EDGEDETECT), PLUGIN_MAJOR_NATRON_EDGEDETECT);
    ASSERT_TRUE(bool(source) && bool(edge));
    connectNodes(source, edge, 0, true);
    ASSERT_TRUE(setKnobValues(edge, kEdgeDetectParamBlurSize, { 2. }));

    // Four slots: the buffer, both gradients and the suppression's output.
    ASSERT_TRUE(setKnobValues(edge, kEdgeDetectParamNMS, { 1. }));
    ASSERT_TRUE(setKnobValues(edge, kEdgeDetectParamErodeSize, { 1. }));
    ASSERT_TRUE(setKnobValue(edge, kEdgeDetectParamMultiChannel, std::string(kEdgeDetectParamMultiChannelRMS)));
    expectScratchIsFullyWritten(edge, 4, "gaussian-rms-nms");

    ASSERT_TRUE(setKnobValue(edge, kEdgeDetectParamFilter, std::string(kEdgeDetectParamFilterSobel)));
    ASSERT_TRUE(setKnobValue(edge, kEdgeDetectParamMultiChannel, std::string(kEdgeDetectParamMultiChannelSeparate)));
    expectScratchIsFullyWritten(edge, 4, "sobel-separate-nms");

    // Three slots, no suppression.
    ASSERT_TRUE(setKnobValues(edge, kEdgeDetectParamNMS, { 0. }));
    ASSERT_TRUE(setKnobValue(edge, kEdgeDetectParamFilter, std::string(kEdgeDetectParamFilterGaussian)));
    ASSERT_TRUE(setKnobValue(edge, kEdgeDetectParamMultiChannel, std::string(kEdgeDetectParamMultiChannelTensor)));
    expectScratchIsFullyWritten(edge, 3, "gaussian-tensor");
}

TEST_F(NativePassScratchTest, ErodeDilateScratchIsFullyWrittenBeforeRead)
{
    resetProject();
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    for (const char* id : { PLUGINID_NATRON_ERODE, PLUGINID_NATRON_DILATE }) {
        const int major = (std::string(id) == PLUGINID_NATRON_ERODE) ? PLUGIN_MAJOR_NATRON_ERODE : PLUGIN_MAJOR_NATRON_DILATE;
        NodePtr node = createNode(QString::fromUtf8(id), major);
        ASSERT_TRUE(bool(node)) << id;
        connectNodes(source, node, 0, true);
        ASSERT_TRUE(setKnobValues(node, kErodeDilateParamSize, { 5., 3. }));
        expectScratchIsFullyWritten(node, 1, id);
    }
}
