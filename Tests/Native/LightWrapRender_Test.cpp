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
#include <cstddef>
#include <iostream>
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QtCore/QString>

#include "BaseTest.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Transform/Crop.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

// The LightWrap PyPlug rendered headlessly: a grey foreground cropped to a box over an orange
// background must come out with a wrap band just inside the box's edge. Each inner node is also
// rendered, so a failure names the node the signal dies at.

namespace {

const char* const kLightWrapID = "fr.inria.LightWrap";
const double kTime = 1.;
const int kWidth = 64;
const int kHeight = 48;
// The foreground box: 32x24 at (16,12).
const int kBoxX = 16;
const int kBoxY = 12;
const int kBoxW = 32;
const int kBoxH = 24;
const RectI kWindow(0, 0, kWidth, kHeight);

// A pixel one inside the box's left edge, on its middle row, and the box's centre.
const int kBandX = kBoxX + 1;
const int kMidY = kBoxY + kBoxH / 2;
const int kCentreX = kBoxX + kBoxW / 2;

enum Channel { eR = 0,
               eG = 1,
               eB = 2,
               eA = 3 };

bool
isPluginLoaded(const char* pluginID)
{
    try {
        return appPTR->getPluginBinary(QString::fromUtf8(pluginID), -1, -1, false) != NULL;
    } catch (const std::exception&) {
        return false;
    }
}

bool
renderRGBA(const NodePtr& node,
           RenderedPlane* plane,
           std::string* error)
{
    std::list<ImageLayerDesc> layers;
    layers.push_back(ImageLayerDesc::getRGBAComponents());
    std::vector<RenderedPlane> planes;
    if (!renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, kWindow, layers, &planes, error)) {
        return false;
    }
    if ((planes.size() != 1) || (planes[0].channels.size() != 4)) {
        *error = "expected one RGBA plane";

        return false;
    }
    *plane = planes[0];

    return true;
}

float
sample(const RenderedPlane& plane,
       int x,
       int y,
       Channel c)
{
    const RectI& w = plane.window;
    const std::size_t i = ((std::size_t)(y - w.y1) * w.width() + (x - w.x1)) * plane.channels.size() + (std::size_t)c;

    return (i < plane.pixels.size()) ? plane.pixels[i] : 0.f;
}

float
maxOf(const RenderedPlane& plane,
      Channel c)
{
    float result = 0.f;
    const std::size_t n = plane.channels.size();
    for (std::size_t i = (std::size_t)c; i < plane.pixels.size(); i += n) {
        result = std::max(result, plane.pixels[i]);
    }

    return result;
}

} // namespace

class LightWrapRenderTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        ProjectPtr project = getApp()->getProject();
        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kWidth, kHeight, "lightWrap64x48", 1.));
    }

    virtual void TearDown() OVERRIDE
    {
        _wrap.reset();
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    template <typename K>
    std::shared_ptr<K> knob(const NodePtr& node,
                            const char* name)
    {
        std::shared_ptr<K> result = std::dynamic_pointer_cast<K>(node->getKnobByName(name));

        EXPECT_TRUE(bool(result)) << node->getScriptName_mt_safe() << "." << name;

        return result;
    }

    void setDoubles(const NodePtr& node,
                    const char* name,
                    const std::vector<double>& values)
    {
        KnobDoubleBasePtr k = knob<KnobDoubleBase>(node, name);
        if (!k) {
            return;
        }
        for (std::size_t i = 0; i < values.size(); ++i) {
            k->setValue(values[i], ViewSpec::all(), (int)i);
        }
    }

    void setBool(const char* name,
                 bool value)
    {
        KnobBoolPtr k = knob<KnobBool>(_wrap, name);
        if (k) {
            k->setValue(value);
        }
    }

    // A grey 0.5 constant cropped to the box goes into input 0 (A, the foreground), an orange
    // constant into input 1 (B, the background). The blurs and intensity are large enough that
    // the band sits well clear of float noise at this size.
    bool build()
    {
        if (!isPluginLoaded(kLightWrapID)) {
            return false;
        }
        NodePtr fg = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
        NodePtr crop = createNode(QString::fromUtf8(PLUGINID_NATRON_CROP));
        NodePtr bg = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
        _wrap = createNode(QString::fromUtf8(kLightWrapID));
        EXPECT_TRUE(fg && crop && bg && _wrap);
        if (!fg || !crop || !bg || !_wrap) {
            return false;
        }
        setDoubles(fg, kConstantParamColor, { 0.5, 0.5, 0.5, 1. });
        setDoubles(bg, kConstantParamColor, { 0.9, 0.5, 0.1, 1. });
        setDoubles(crop, kNativeGeneratorParamBottomLeft, { (double)kBoxX, (double)kBoxY });
        setDoubles(crop, kNativeGeneratorParamSize, { (double)kBoxW, (double)kBoxH });
        KnobBoolPtr reformat = knob<KnobBool>(crop, kCropParamReformat);
        if (reformat) {
            reformat->setValue(false);
        }
        connectNodes(fg, crop, 0, true);
        connectNodes(crop, _wrap, 0, true);
        connectNodes(bg, _wrap, 1, true);

        setDoubles(_wrap, "intensity", { 10. });
        setDoubles(_wrap, "fgblur", { 4., 4. });
        setDoubles(_wrap, "diffuse", { 4., 4. });
        setBool("onlyWrap", true);
        setBool("useConstant", false);
        setBool("disableLuma", false);

        return true;
    }

    NodePtr inner(const char* name)
    {
        NodeCollectionPtr collection = std::dynamic_pointer_cast<NodeCollection>(_wrap->getEffectInstance());
        NodePtr result = collection ? collection->getNodeByName(name) : NodePtr();

        EXPECT_TRUE(bool(result)) << "LightWrap has no inner node " << name;

        return result;
    }

    bool renderInner(const char* name,
                     RenderedPlane* plane)
    {
        NodePtr node = inner(name);
        if (!node) {
            return false;
        }
        std::string error;
        const bool ok = renderRGBA(node, plane, &error);
        EXPECT_TRUE(ok) << name << ": " << error;

        return ok;
    }

    NodePtr _wrap;
};

// The signal path from the inputs to the output, in order, with the channel that carries the wrap
// at that point. The first entry that is black is where the wrap is lost.
TEST_F(LightWrapRenderTest, EveryInnerNodeOnTheWrapPathCarriesSignal)
{
    if (!build()) {
        std::cerr << "Skipping: " << kLightWrapID << " is not registered." << std::endl;

        return;
    }
    struct Stage {
        const char* name;
        Channel channel;
    };
    const Stage stages[] = {
        { "Keyer1", eA },
        { "Premult1", eR },
        { "BGBlur", eR },
        { "DisableLuma1", eR },
        { "ConstantColor", eR },
        { "FGBlur", eA },
        { "EdgeMerge", eA },
        { "MaskMerge", eR },
        { "Diffuse", eR },
        { "Intensity", eR },
        { "Saturation1", eR },
        { "ColorCorrect1", eR },
        { "MaskMerge2", eR },
        { "Shuffle1", eR },
        { "HighlightMerge", eR },
    };
    for (const Stage& stage : stages) {
        RenderedPlane plane;
        if (!renderInner(stage.name, &plane)) {
            continue;
        }
        EXPECT_GT(maxOf(plane, stage.channel), 1e-3f) << stage.name << " is black in channel " << (int)stage.channel;
    }

    // EdgeMerge (A out blurred A) is the edge band: high just inside the box edge, gone at its centre.
    RenderedPlane edge;
    if (renderInner("EdgeMerge", &edge)) {
        EXPECT_GT(sample(edge, kBandX, kMidY, eA), 0.1f);
        EXPECT_LT(sample(edge, kCentreX, kMidY, eA), 0.01f);
    }
}

// Shufflecopy1 takes RGB from Constant1 (white) and alpha from BGBlur, so that Premult2 is the
// constant highlight scaled by the background key. With its inputs swapped it took RGB from BGBlur
// and alpha from Constant1's zero alpha, and Premult2 was black.
TEST_F(LightWrapRenderTest, ConstantHighlightTakesColourFromTheConstantAndAlphaFromTheKey)
{
    if (!build()) {
        std::cerr << "Skipping: " << kLightWrapID << " is not registered." << std::endl;

        return;
    }
    RenderedPlane key;
    RenderedPlane shuffled;
    RenderedPlane premult;
    ASSERT_TRUE(renderInner("BGBlur", &key));
    ASSERT_TRUE(renderInner("Shufflecopy1", &shuffled));
    ASSERT_TRUE(renderInner("Premult2", &premult));

    const int x = kCentreX;
    const int y = kMidY;
    const float keyAlpha = sample(key, x, y, eA);
    ASSERT_GT(keyAlpha, 0.1f) << "BGBlur carries no key";
    EXPECT_NEAR(1.f, sample(shuffled, x, y, eR), 1e-4f);
    EXPECT_NEAR(1.f, sample(shuffled, x, y, eG), 1e-4f);
    EXPECT_NEAR(1.f, sample(shuffled, x, y, eB), 1e-4f);
    EXPECT_NEAR(keyAlpha, sample(shuffled, x, y, eA), 1e-4f);
    EXPECT_NEAR(keyAlpha, sample(premult, x, y, eR), 1e-4f);
}

// The output, with Generate Wrap Only on: the background colour wrapped just inside the foreground
// edge, nothing at the foreground's centre.
TEST_F(LightWrapRenderTest, OutputHasAWrapBandInsideTheForegroundEdge)
{
    if (!build()) {
        std::cerr << "Skipping: " << kLightWrapID << " is not registered." << std::endl;

        return;
    }
    RenderedPlane out;
    std::string error;
    ASSERT_TRUE(renderRGBA(_wrap, &out, &error)) << error;

    const float band = sample(out, kBandX, kMidY, eR);
    EXPECT_GT(band, 0.05f) << "no wrap band inside the foreground edge";
    EXPECT_GT(sample(out, kBandX, kMidY, eG), 0.01f);
    EXPECT_LT(sample(out, kCentreX, kMidY, eR), band * 0.1f) << "the wrap reaches the foreground's centre";
    // Orange background: the band keeps its hue, red over green.
    EXPECT_GT(band, sample(out, kBandX, kMidY, eG));
}

// The toggles reach the render, not only the Switch knobs: Use Constant Highlight turns the band
// from the background's orange to the (white) constant colour, Disable Luma-Based Wrap feeds the
// unkeyed background through DisableLuma1.
TEST_F(LightWrapRenderTest, TogglesChangeWhatTheInnerSwitchesRender)
{
    if (!build()) {
        std::cerr << "Skipping: " << kLightWrapID << " is not registered." << std::endl;

        return;
    }
    const int x = kCentreX;
    const int y = kMidY;
    RenderedPlane before;
    ASSERT_TRUE(renderInner("DisableLuma1", &before));
    RenderedPlane premult1;
    ASSERT_TRUE(renderInner("Premult1", &premult1));
    EXPECT_NEAR(sample(premult1, x, y, eR), sample(before, x, y, eR), 1e-3f) << "DisableLuma1 with disableLuma off";

    setBool("disableLuma", true);
    RenderedPlane keyer;
    ASSERT_TRUE(renderInner("Keyer1", &keyer));
    RenderedPlane after;
    ASSERT_TRUE(renderInner("DisableLuma1", &after));
    EXPECT_NEAR(sample(keyer, x, y, eR), sample(after, x, y, eR), 1e-3f) << "DisableLuma1 with disableLuma on";
    setBool("disableLuma", false);

    setBool("useConstant", true);
    RenderedPlane constantPath;
    ASSERT_TRUE(renderInner("DisableLuma", &constantPath));
    RenderedPlane chosen;
    ASSERT_TRUE(renderInner("ConstantColor", &chosen));
    EXPECT_NEAR(sample(constantPath, x, y, eR), sample(chosen, x, y, eR), 1e-3f) << "ConstantColor with useConstant on";
    EXPECT_NEAR(sample(constantPath, x, y, eG), sample(chosen, x, y, eG), 1e-3f) << "ConstantColor with useConstant on";

    RenderedPlane out;
    std::string error;
    ASSERT_TRUE(renderRGBA(_wrap, &out, &error)) << error;
    const float r = sample(out, kBandX, kMidY, eR);
    EXPECT_GT(r, 0.05f) << "no wrap band with useConstant on";
    // The white constant wraps grey: green matches red, where the orange background gave less.
    EXPECT_NEAR(r, sample(out, kBandX, kMidY, eG), r * 0.05f);
}
