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

#ifndef Tests_Native_OcioNodeTestSupport_h
#define Tests_Native_OcioNodeTestSupport_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include <OpenColorIO/OpenColorIO.h>

#include "NativeParity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/ProjectColorManagement.h"
#include "Engine/ViewIdx.h"

// Shared by the native OCIO node tests: the project config they run on, a render of a node's
// colour plane over the parity source, and the comparison of that render with OpenColorIO applied
// to the source directly.
namespace OcioNodeTest {
NATRON_NAMESPACE_USING

const char* const kStudioURI = "ocio://studio-config-v4.0.0_aces-v2.0_ocio-v2.5";
const char* const kSRGB = "sRGB Encoded Rec.709 (sRGB)";
const double kTime = 1.;

/**
 * @brief Selects the built-in config \p uri on \p app's project.
 **/
inline void
setConfig(const AppInstancePtr& app,
          const char* uri)
{
    ProjectPtr project = app->getProject();
    KnobChoicePtr config = project->getKnobByNameAndType<KnobChoice>("ocioConfig");

    ASSERT_TRUE(bool(config));
    config->setValueFromID(uri, 0);
    ASSERT_EQ(std::string(uri), project->getColorManagement()->getConfigSource());
}

inline OCIO_NAMESPACE::ConstConfigRcPtr
config(const AppInstancePtr& app)
{
    return app->getProject()->getColorManagement()->getConfig();
}

/**
 * @brief The CPU processor OpenColorIO builds for \p transform in \p config, at the precision and
 * optimisation the nodes render with.
 **/
inline OCIO_NAMESPACE::ConstCPUProcessorRcPtr
referenceProcessor(const OCIO_NAMESPACE::ConstConfigRcPtr& config,
                   const OCIO_NAMESPACE::ConstTransformRcPtr& transform,
                   OCIO_NAMESPACE::TransformDirection direction = OCIO_NAMESPACE::TRANSFORM_DIR_FORWARD)
{
    return config->getProcessor(transform, direction)->getOptimizedCPUProcessor(OCIO_NAMESPACE::BIT_DEPTH_F32, OCIO_NAMESPACE::BIT_DEPTH_F32, OCIO_NAMESPACE::OPTIMIZATION_DEFAULT);
}

inline bool
renderColorPlane(const NodePtr& node,
                 const RectI& window,
                 RenderedPlane* plane)
{
    std::list<ImageLayerDesc> present;
    node->getEffectInstance()->getPresentLayers(kTime, ViewIdx(0), -1, &present);
    std::list<ImageLayerDesc> layers;
    for (std::list<ImageLayerDesc>::const_iterator it = present.begin(); it != present.end(); ++it) {
        if (it->isColorLayer()) {
            layers.push_back(*it);
            break;
        }
    }
    EXPECT_EQ(1u, layers.size());
    std::vector<RenderedPlane> planes;
    std::string error;
    const bool ok = renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, window, layers, &planes, &error);
    EXPECT_TRUE(ok) << error;
    if (!ok || (planes.size() != 1)) {
        return false;
    }
    *plane = planes[0];

    return true;
}

inline float
planeValue(const RenderedPlane& plane,
           int x,
           int y,
           int c)
{
    const std::size_t nComps = plane.channels.size();

    return plane.pixels[(static_cast<std::size_t>(y - plane.window.y1) * plane.window.width() + (x - plane.window.x1)) * nComps + c];
}

/**
 * @brief Whether \p actual is \p expected within \p tolerance, relative above 1. NaNs match NaNs
 * and infinities match themselves.
 **/
inline bool
matches(float expected,
        float actual,
        double tolerance)
{
    if (std::isnan(expected) || std::isnan(actual)) {
        return std::isnan(expected) && std::isnan(actual);
    }
    if (std::isinf(expected) || std::isinf(actual)) {
        return expected == actual;
    }

    return std::fabs((double)expected - actual) <= tolerance * std::max(1., std::fabs((double)expected));
}

/**
 * @brief The RGB of the parity source at (x, y) after \p expected, which is given the pixel's RGB
 * and alpha in place.
 **/
typedef std::function<void(float rgb[3], float alpha)> ExpectedPixel;

/**
 * @brief Renders \p node, whose input is the parity source \p source, and checks every pixel:
 * RGB as \p expected makes it from the source, alpha as the source's.
 **/
inline void
expectRender(const NodePtr& node,
             const NodePtr& source,
             const ExpectedPixel& expected,
             const std::string& what,
             double tolerance = 1e-5)
{
    const RectI window = paritySourceWindow(source, kTime, 0);
    RenderedPlane plane;

    ASSERT_TRUE(renderColorPlane(node, window, &plane)) << what;
    ASSERT_EQ(4u, plane.channels.size()) << what;
    for (int y = window.y1; y < window.y2; ++y) {
        for (int x = window.x1; x < window.x2; ++x) {
            float rgb[3] = { paritySourceColorValue(0, x, y, kTime), paritySourceColorValue(1, x, y, kTime), paritySourceColorValue(2, x, y, kTime) };
            const float alpha = paritySourceColorValue(3, x, y, kTime);
            expected(rgb, alpha);
            for (int c = 0; c < 3; ++c) {
                ASSERT_TRUE(matches(rgb[c], planeValue(plane, x, y, c), tolerance))
                    << what << " at " << x << "," << y << " c=" << c << ": expected " << rgb[c] << ", got " << planeValue(plane, x, y, c);
            }
            ASSERT_EQ(alpha, planeValue(plane, x, y, 3)) << what << " at " << x << "," << y;
        }
    }
}

/**
 * @brief expectRender() with \p cpu applied to the source's RGB, or the source as it is when
 * \p cpu is null.
 **/
inline void
expectProcessed(const NodePtr& node,
                const NodePtr& source,
                const OCIO_NAMESPACE::ConstCPUProcessorRcPtr& cpu,
                const std::string& what)
{
    expectRender(node, source, [&cpu](float rgb[3], float /*alpha*/) {
        if (cpu) {
            cpu->applyRGB(rgb);
        } }, what);
}

inline std::string
stringValue(const NodePtr& node,
            const char* name)
{
    KnobStringBasePtr knob = std::dynamic_pointer_cast<KnobStringBase>(node->getKnobByName(name));

    EXPECT_TRUE(bool(knob)) << name;

    return knob ? knob->getValue() : std::string();
}

inline QString
persistentMessage(const NodePtr& node)
{
    QString message;
    int type = 0;

    node->getPersistentMessage(&message, &type, false);

    return message;
}
} // namespace OcioNodeTest

#endif // Tests_Native_OcioNodeTestSupport_h
