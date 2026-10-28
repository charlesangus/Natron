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

#ifndef Tests_Native_NativeShapeTestSupport_h
#define Tests_Native_NativeShapeTestSupport_h

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
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

// What the shape generator tests share: rendering a node's RGBA plane and reading it back.
namespace NativeShapeTest {

NATRON_NAMESPACE_USING

const double kTime = 1.;

inline bool
isNative(const NodePtr& node)
{
    return node && dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());
}

// A fresh project whose default format is width by height at pixel aspect ratio 1.
inline void
resetProject(const AppInstancePtr& app,
             int width,
             int height)
{
    ProjectPtr project = app->getProject();

    project->reset(false, true);
    project->setAutoSetProjectFormatEnabled(true);
    project->setOrAddProjectFormat(Format(0, 0, width, height, "nativeShape", 1.));
}

// The RGBA plane of `node` over `window` at `mipmapLevel`.
inline bool
renderRgba(const NodePtr& node,
           const RectI& window,
           RenderedPlane* plane,
           unsigned mipmapLevel = 0)
{
    std::list<ImageLayerDesc> layers(1, ImageLayerDesc::mapNCompsToColorLayer(4));
    std::vector<RenderedPlane> planes;
    std::string error;
    const bool ok = renderNodePlanesDirect(node, kTime, ViewIdx(0), mipmapLevel, window, layers, &planes, &error);

    EXPECT_TRUE(ok) << error;
    if (!ok || (planes.size() != 1) || (planes[0].channels.size() != 4)) {
        return false;
    }
    *plane = planes[0];

    return true;
}

// Channel c of pixel (x, y) of a plane rendered with renderRgba().
inline float
at(const RenderedPlane& plane,
   int x,
   int y,
   int c)
{
    const RectI& w = plane.window;

    return plane.pixels[((std::size_t)(y - w.y1) * w.width() + (std::size_t)(x - w.x1)) * 4 + c];
}

inline bool
sameBits(float a,
         float b)
{
    return std::memcmp(&a, &b, sizeof(float)) == 0;
}

// Whether every pixel of part is bit for bit the same pixel of whole.
inline bool
sameAsWhole(const RenderedPlane& part,
            const RenderedPlane& whole)
{
    const RectI& w = part.window;
    for (int y = w.y1; y < w.y2; ++y) {
        for (int x = w.x1; x < w.x2; ++x) {
            for (int c = 0; c < 4; ++c) {
                if (!sameBits(at(part, x, y, c), at(whole, x, y, c))) {
                    ADD_FAILURE() << "pixel (" << x << ", " << y << ") channel " << c << ": " << at(part, x, y, c) << " != " << at(whole, x, y, c);

                    return false;
                }
            }
        }
    }

    return true;
}

// The largest difference in channel c between pixel (x, y) of half, rendered at scale 1/2, and the
// mean of the 2x2 block of full it covers, over the pixels for which include(x, y) holds. Pixel
// coverage is an area, so it averages over the block exactly where the shape is locally straight.
template <typename INCLUDE>
float
worstHalfScaleDifference(const RenderedPlane& half,
                         const RenderedPlane& full,
                         int c,
                         INCLUDE include)
{
    float worst = 0.f;
    const RectI& w = half.window;
    for (int y = w.y1; y < w.y2; ++y) {
        for (int x = w.x1; x < w.x2; ++x) {
            if (!include(x, y)) {
                continue;
            }
            const float mean = 0.25f * (at(full, 2 * x, 2 * y, c) + at(full, 2 * x + 1, 2 * y, c) + at(full, 2 * x, 2 * y + 1, c) + at(full, 2 * x + 1, 2 * y + 1, c));
            worst = (std::max)(worst, std::fabs(at(half, x, y, c) - mean));
        }
    }

    return worst;
}

inline int
countNonFinite(const RenderedPlane& plane)
{
    int n = 0;
    for (float v : plane.pixels) {
        n += std::isfinite(v) ? 0 : 1;
    }

    return n;
}

// Sets choice knob `name` to option optionID as a user would, so the node reacts to it.
inline bool
setChoiceAsUser(const NodePtr& node,
                const std::string& name,
                const std::string& optionID)
{
    KnobChoice* choice = node ? dynamic_cast<KnobChoice*>(node->getKnobByName(name).get()) : NULL;

    EXPECT_TRUE(choice != NULL) << name;
    if (!choice) {
        return false;
    }
    const std::vector<ChoiceOption> entries = choice->getEntries_mt_safe();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].id == optionID) {
            choice->setValue((int)i, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);

            return true;
        }
    }
    ADD_FAILURE() << name << " has no option " << optionID;

    return false;
}

} // namespace NativeShapeTest

#endif // Tests_Native_NativeShapeTestSupport_h
