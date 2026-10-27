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

#ifndef NATRON_TESTS_NATIVETIMENODESTEST_H
#define NATRON_TESTS_NATIVETIMENODESTEST_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "Tests/BaseTest.h"
#include "Tests/RenderBothWays.h"

#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

namespace NativeTimeNodes {
inline const RectI kWindow(0, 0, 8, 8);

// The colour a source made by createSource() shows at a whole frame: every frame differs.
inline float
colourAt(int frame,
         int channel)
{
    return 0.01f * (float)frame + 0.1f * (float)channel + 0.05f;
}
} // namespace NativeTimeNodes

/**
 * @brief Helpers for the tests of the native time nodes, over a native Constant with a set frame
 * range.
 **/
class NativeTimeNodeTest
    : public BaseTest {
protected:
    // A native Constant over the frames [first, last], its colour keyed to colourAt() at each of keyed.
    NodePtr createSource(int first,
                         int last,
                         const std::vector<int>& keyed = std::vector<int>())
    {
        NodePtr constant = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT), PLUGIN_MAJOR_NATRON_CONSTANT);
        EXPECT_TRUE(bool(constant));
        if (!constant) {
            return NodePtr();
        }
        KnobIntPtr range = std::dynamic_pointer_cast<KnobInt>(constant->getKnobByName(kNativeGeneratorParamFrameRange));
        EXPECT_TRUE(bool(range));
        if (range) {
            range->setValue(first, ViewSpec::all(), 0);
            range->setValue(last, ViewSpec::all(), 1);
        }
        KnobDoubleBase* colour = dynamic_cast<KnobDoubleBase*>(constant->getKnobByName(kConstantParamColor).get());
        EXPECT_TRUE(colour != NULL);
        if (colour) {
            for (int frame : keyed) {
                for (int d = 0; d < colour->getDimension(); ++d) {
                    colour->setValueAtTime(frame, NativeTimeNodes::colourAt(frame, d), ViewSpec::all(), d);
                }
            }
        }

        return constant;
    }

    NodePtr createOver(const char* pluginID,
                       int major,
                       const NodePtr& input)
    {
        NodePtr node = createNode(QString::fromUtf8(pluginID), major);
        EXPECT_TRUE(bool(node)) << pluginID;
        if (node && input) {
            connectNodes(input, node, 0, true);
        }

        return node;
    }

    template <typename K, typename V>
    void setKnob(const NodePtr& node,
                 const char* name,
                 const V& value,
                 int dimension = 0)
    {
        std::shared_ptr<K> knob = std::dynamic_pointer_cast<K>(node->getKnobByName(name));
        ASSERT_TRUE(bool(knob)) << name;
        knob->setValue(value, ViewSpec::all(), dimension);
    }

    static Route routeAt(const NodePtr& node,
                         double time)
    {
        NativeEffectBase* effect = node ? dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get()) : NULL;
        EXPECT_TRUE(effect != NULL);

        return effect ? effect->route(time, ViewIdx(0)) : Route::none();
    }

    // The node at time is an identity of its Source at inputTime.
    static void expectInputAt(const NodePtr& node,
                              double time,
                              double inputTime)
    {
        const Route route = routeAt(node, time);
        EXPECT_EQ(Route::Kind::Input, route.kind) << "at " << time;
        EXPECT_EQ(0, route.inputNb) << "at " << time;
        EXPECT_DOUBLE_EQ(inputTime, route.time) << "at " << time;

        const EffectInstancePtr effect = node->getEffectInstance();
        double identityTime = 0.;
        ViewIdx identityView(0);
        int identityInput = -1;
        EXPECT_TRUE(effect->isIdentity_public(false, 0, time, RenderScale::identity, NativeTimeNodes::kWindow, ViewIdx(0), &identityTime, &identityView, &identityInput)) << "at " << time;
        EXPECT_EQ(0, identityInput) << "at " << time;
        EXPECT_DOUBLE_EQ(inputTime, identityTime) << "at " << time;
    }

    static void expectBlack(const NodePtr& node,
                            double time)
    {
        EXPECT_EQ(Route::Kind::Black, routeAt(node, time).kind) << "at " << time;
        const EffectInstancePtr effect = node->getEffectInstance();
        RectD rod;
        EXPECT_NE(eStatusFailed, effect->getRegionOfDefinition(effect->getRenderHash(), time, RenderScale::identity, ViewIdx(0), &rod)) << "at " << time;
        EXPECT_TRUE(rod.isNull()) << "at " << time;
    }

    static void expectFrameRange(const NodePtr& node,
                                 double first,
                                 double last)
    {
        const EffectInstancePtr effect = node->getEffectInstance();
        double f = 0.;
        double l = 0.;
        effect->getFrameRange_public(effect->getRenderHash(), &f, &l);
        EXPECT_DOUBLE_EQ(first, f);
        EXPECT_DOUBLE_EQ(last, l);
    }

    // The first pixel of node's RGBA at time, rendered by one direct renderRoI call.
    static std::vector<float> firstPixel(const NodePtr& node,
                                         double time)
    {
        std::vector<RenderedPlane> planes;
        std::string error;
        const bool ok = renderNodePlanesDirect(node, time, ViewIdx(0), 0, NativeTimeNodes::kWindow, std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()), &planes, &error);
        EXPECT_TRUE(ok) << error;
        if (!ok || planes.empty()) {
            return std::vector<float>();
        }
        const std::size_t nComps = planes.front().channels.size();
        if (planes.front().pixels.size() < nComps) {
            return std::vector<float>();
        }

        return std::vector<float>(planes.front().pixels.begin(), planes.front().pixels.begin() + nComps);
    }
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_TESTS_NATIVETIMENODESTEST_H
