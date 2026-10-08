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

#include <functional>
#include <iostream>
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "NativeParity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/Invert.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Project.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kInvertID = PLUGINID_NATRON_INVERT;
const int kOfxInvertMajor = 2;
const int kNativeInvertMajor = PLUGIN_MAJOR_NATRON_INVERT;

const ParityTolerance kInvertTolerance = ParityTolerance::exact();

bool
setColorChannels(const NodePtr& node,
                 const std::vector<std::string>& names)
{
    KnobChannelSetPtr channels = node ? std::dynamic_pointer_cast<KnobChannelSet>(node->getKnobByName(kNodeParamChannelSet)) : KnobChannelSetPtr();

    EXPECT_TRUE(bool(channels));
    if (!channels) {
        return false;
    }
    channels->setChannels(0, names);

    return true;
}

bool
setChannelSelect(const NodePtr& node,
                 const std::string& name,
                 const std::string& value)
{
    KnobChannelSelect* select = node ? dynamic_cast<KnobChannelSelect*>(node->getKnobByName(name).get()) : NULL;

    EXPECT_TRUE(select != NULL) << name;
    if (!select) {
        return false;
    }
    select->set(value);

    return true;
}

bool
isNative(const NodePtr& node)
{
    return node && dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());
}

} // namespace

class NativeInvertTest
    : public BaseTest {
protected:
    ParityPair makePair(bool withMask = false)
    {
        ParityPair pair = makeParityPair(getApp(), kInvertID, kOfxInvertMajor, kNativeInvertMajor, withMask ? std::string("Mask") : std::string());

        EXPECT_TRUE(bool(pair.native));
        EXPECT_FALSE(pair.live()) << "the OFX Invert is retired, so parity replays the recorded references";
        EXPECT_TRUE(isNative(pair.native));

        return pair;
    }

    void expectParity(const ParityPair& pair,
                      const std::string& caseName,
                      bool record)
    {
        for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
            const ParityResult r = compareParity(pair, caseName, RectI(), mipmapLevel, kInvertTolerance, record);
            EXPECT_TRUE(r.ok) << caseName << ", mipmap " << mipmapLevel << ": " << describe(r);
            EXPECT_FALSE(r.live);
            EXPECT_GE(r.planesCompared, 1) << caseName;
            std::cout << "[ parity ] Invert " << caseName << " mipmap " << mipmapLevel << ": planes " << r.planesCompared
                      << ", max abs diff " << r.maxAbsDiff << std::endl;
        }
    }
};

TEST_F(NativeInvertTest, DefaultIncludingValuesOutsideZeroToOne)
{
    ParityPair pair = makePair();
    expectParity(pair, "default", true);
}

TEST_F(NativeInvertTest, RedAndBlueOnly)
{
    ParityPair pair = makePair();
    std::vector<std::string> names;
    names.push_back("R");
    names.push_back("B");
    ASSERT_TRUE(setColorChannels(pair.native, names));
    expectParity(pair, "red-blue-only", true);
}

TEST_F(NativeInvertTest, HostUnPremultBy)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(setChannelSelect(pair.native, kUnPremultByKnobName, "rgba.A"));
    expectParity(pair, "unpremult", true);
}

TEST_F(NativeInvertTest, MixZeroIsIdentity)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(setKnobOnBoth(pair, "mix", { 0. }));

    const RectI roi(0, 0, 32, 32);
    const RenderScale scale = RenderScale::identity;
    double inputTime = 0.;
    ViewIdx inputView(0);
    int inputNb = -1;
    EffectInstancePtr effect = pair.native->getEffectInstance();
    EXPECT_TRUE(effect->isIdentity_public(false, effect->getRenderHash(), 1., scale, roi, ViewIdx(0), &inputTime, &inputView, &inputNb));
    EXPECT_EQ(0, inputNb);
}

TEST_F(NativeInvertTest, InvertingTwiceRestoresTheSource)
{
    getApp()->getProject()->reset(false, true);
    Format format(0, 0, 128, 96, "nativeInvertTwiceFormat", 1.);
    getApp()->getProject()->setOrAddProjectFormat(format);

    NodePtr checker = createNode(QString::fromUtf8("net.sf.openfx.CheckerBoardPlugin"));
    ASSERT_TRUE(bool(checker));
    NodePtr first = createNode(QString::fromUtf8(kInvertID));
    NodePtr second = createNode(QString::fromUtf8(kInvertID));
    ASSERT_TRUE(isNative(first));
    ASSERT_TRUE(isNative(second));
    connectNodes(checker, first, 0, true);
    connectNodes(first, second, 0, true);

    const RectI roi(0, 0, 128, 96);
    std::vector<RenderedPlane> before;
    std::vector<RenderedPlane> after;
    std::list<ImageLayerDesc> layers;
    layers.push_back(ImageLayerDesc::getRGBAComponents());
    std::string error;
    ASSERT_TRUE(renderNodePlanesDirect(checker, 1., ViewIdx(0), 0, roi, layers, &before, &error)) << error;
    ASSERT_TRUE(renderNodePlanesDirect(second, 1., ViewIdx(0), 0, roi, layers, &after, &error)) << error;
    ASSERT_EQ(1u, before.size());
    ASSERT_EQ(1u, after.size());
    ASSERT_EQ(before[0].pixels.size(), after[0].pixels.size());
    for (std::size_t i = 0; i < before[0].pixels.size(); ++i) {
        ASSERT_NEAR(before[0].pixels[i], after[0].pixels[i], 1e-6f) << "float index " << i;
    }
}

TEST_F(NativeInvertTest, ChainRendersTheSameInBothSchedulerModesWithNoUnplannedPulls)
{
    getApp()->getProject()->reset(false, true);
    Format format(0, 0, 128, 96, "nativeInvertChainFormat", 1.);
    getApp()->getProject()->setOrAddProjectFormat(format);

    NodePtr previous = createNode(QString::fromUtf8("net.sf.openfx.CheckerBoardPlugin"));
    ASSERT_TRUE(bool(previous));
    NodePtr last;
    for (int i = 0; i < 3; ++i) {
        NodePtr invert = createNode(QString::fromUtf8(kInvertID));
        ASSERT_TRUE(isNative(invert));
        connectNodes(previous, invert, 0, true);
        previous = invert;
        last = invert;
    }
    KnobDouble* mix = dynamic_cast<KnobDouble*>(last->getKnobByName("mix").get());
    ASSERT_TRUE(mix != NULL);
    mix->setValue(0.6);

    const RectI roi(0, 0, 128, 96);
    for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
        std::vector<int> unplanned;
        const RenderMismatch m = renderBothWaysDirect(last, 1., ViewIdx(0), mipmapLevel, mipmapLevel ? RectI(0, 0, 64, 48) : roi, std::vector<int> { 1, 4 }, std::function<void()>(), 0.f, &unplanned);
        EXPECT_FALSE(m.any) << "mipmap " << mipmapLevel << ": " << describe(m);
        for (std::size_t i = 0; i < unplanned.size(); ++i) {
            EXPECT_EQ(0, unplanned[i]) << "mipmap " << mipmapLevel << ", pool " << i;
        }
    }
}

TEST_F(NativeInvertTest, UnversionedRequestsGetTheNativeInvert)
{
    NodePtr unversioned = createNode(QString::fromUtf8(kInvertID));
    ASSERT_TRUE(bool(unversioned));
    EXPECT_TRUE(isNative(unversioned));
    EXPECT_EQ(kNativeInvertMajor, unversioned->getMajorVersion());

    EXPECT_FALSE(isPluginMajorRegistered(kInvertID, kOfxInvertMajor));
}
