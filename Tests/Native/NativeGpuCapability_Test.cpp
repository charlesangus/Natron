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

#include <set>
#include <string>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Channel/AddLayers.h"
#include "Engine/Nodes/Channel/RemoveLayers.h"
#include "Engine/Nodes/Channel/Shuffle.h"
#include "Engine/Nodes/Color/Clamp.h"
#include "Engine/Nodes/Color/ColorCorrect.h"
#include "Engine/Nodes/Color/ColorLookup.h"
#include "Engine/Nodes/Color/ColorMathNode.h"
#include "Engine/Nodes/Color/ColorMatrix.h"
#include "Engine/Nodes/Color/Colorspace.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Color/HSVTool.h"
#include "Engine/Nodes/Color/HueCorrect.h"
#include "Engine/Nodes/Color/Invert.h"
#include "Engine/Nodes/Color/Posterize.h"
#include "Engine/Nodes/Color/Premult.h"
#include "Engine/Nodes/Color/Saturation.h"
#include "Engine/Nodes/Color/Threshold.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Filter/EdgeDetect.h"
#include "Engine/Nodes/Filter/ErodeDilate.h"
#include "Engine/Nodes/Filter/IBlur.h"
#include "Engine/Nodes/Filter/IErodeDilate.h"
#include "Engine/Nodes/Generator/CheckerBoard.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Generator/Noise.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Keyer/ChromaKeyer.h"
#include "Engine/Nodes/Keyer/Despill.h"
#include "Engine/Nodes/Keyer/HSVKeyer.h"
#include "Engine/Nodes/Keyer/Keyer.h"
#include "Engine/Nodes/Merge/Dissolve.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Raw/RawColorCalibration.h"
#include "Engine/Nodes/Raw/RawHotPixels.h"
#include "Engine/Nodes/Raw/RawInputColor.h"
#include "Engine/Nodes/Transform/CornerPinKnobs.h"
#include "Engine/Nodes/Transform/Crop.h"
#include "Engine/Nodes/Transform/ICornerPin.h"
#include "Engine/Nodes/Transform/IDistort.h"
#include "Engine/Nodes/Transform/ITransform.h"
#include "Engine/Nodes/Transform/LensDistortion.h"
#include "Engine/Nodes/Transform/Mirror.h"
#include "Engine/Nodes/Transform/Position.h"
#include "Engine/Nodes/Transform/Reformat.h"
#include "Engine/Nodes/Transform/STMap.h"
#include "Engine/Nodes/Transform/Transform.h"
#include "Engine/Plugin.h"

NATRON_NAMESPACE_USING

namespace {

// The nodes that may answer true to canRenderOnGpu(); every other native node must stay on the CPU.
const std::set<std::string>&
gpuCapableIds()
{
    static const std::set<std::string> ids = {
        PLUGINID_NATRON_GRADE,
        PLUGINID_NATRON_BLUR,
        PLUGINID_NATRON_CLAMP,
        PLUGINID_NATRON_INVERT,
        PLUGINID_NATRON_COLORLOOKUP,
        PLUGINID_NATRON_COLORCORRECT,
        PLUGINID_NATRON_CONSTANT,
        PLUGINID_NATRON_SOLID,
        PLUGINID_NATRON_CHECKERBOARD,
        PLUGINID_NATRON_NOISE,
        PLUGINID_NATRON_SATURATION,
        PLUGINID_NATRON_ADD,
        PLUGINID_NATRON_MULTIPLY,
        PLUGINID_NATRON_GAMMA,
        PLUGINID_NATRON_ERODE,
        PLUGINID_NATRON_DILATE,
        PLUGINID_NATRON_TRANSFORM,
        PLUGINID_NATRON_TRANSFORMMASKED,
        PLUGINID_NATRON_REFORMAT,
        PLUGINID_NATRON_POSITION,
        PLUGINID_NATRON_CROP,
        PLUGINID_NATRON_CORNERPIN,
        PLUGINID_NATRON_MIRROR,
        PLUGINID_NATRON_STMAP,
        PLUGINID_NATRON_IDISTORT,
        PLUGINID_NATRON_LENSDISTORTION,
        PLUGINID_NATRON_KEYER,
        PLUGINID_NATRON_CHROMAKEYER,
        PLUGINID_NATRON_RAWINPUTCOLOR,
        PLUGINID_NATRON_RAWCOLORCALIBRATION,
        PLUGINID_NATRON_RAWHOTPIXELS,
        PLUGINID_NATRON_DISSOLVE,
        PLUGINID_NATRON_EDGEDETECT,
        PLUGINID_NATRON_IBLUR,
        PLUGINID_NATRON_IERODE,
        PLUGINID_NATRON_IDILATE,
        PLUGINID_NATRON_ITRANSFORM,
        PLUGINID_NATRON_ICORNERPIN,
        PLUGINID_NATRON_SHUFFLE,
        PLUGINID_NATRON_SHUFFLECOPY,
        PLUGINID_NATRON_ADDLAYERS,
        PLUGINID_NATRON_REMOVELAYERS,
        PLUGINID_NATRON_PREMULT,
        PLUGINID_NATRON_UNPREMULT,
        PLUGINID_NATRON_COLORMATRIX,
        PLUGINID_NATRON_THRESHOLD,
        PLUGINID_NATRON_COLORSPACE,
        PLUGINID_NATRON_HSVTOOL,
        PLUGINID_NATRON_HUECORRECT,
        PLUGINID_NATRON_HSVKEYER,
        PLUGINID_NATRON_DESPILL,
        PLUGINID_NATRON_POSTERIZE,
    };

    return ids;
}

} // namespace

class NativeGpuCapabilityTest
    : public BaseTest {
};

TEST_F(NativeGpuCapabilityTest, OnlyAllowlistedNativeNodesRenderOnGpu)
{
    GpuQuery query;
    query.nComps = 4;
    query.bitDepth = eImageBitDepthFloat;

    int nChecked = 0;
    for (const auto& versions : appPTR->getPluginsList()) {
        for (const Plugin* plugin : versions.second) {
            // The OFX descriptor is only loaded on demand, so the plugin handle is the reliable OFX marker.
            if (plugin->getOfxPlugin() || !plugin->getIsUserCreatable()) {
                continue;
            }
            NodePtr node = createNode(plugin->getPluginID(), plugin->getMajorVersion());
            if (!node) {
                continue;
            }
            NativeEffectBase* effect = dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());
            if (!effect) {
                continue;
            }
            ++nChecked;
            const std::string id = versions.first;
            if (gpuCapableIds().count(id) == 0) {
                EXPECT_FALSE(effect->canRenderOnGpu(query)) << id;
            }
        }
    }
    EXPECT_GT(nChecked, 0);
}

TEST_F(NativeGpuCapabilityTest, DefaultsDeclineAndPassTheRegionThrough)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    ASSERT_TRUE(bool(node));
    NativeEffectBase* effect = dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());
    ASSERT_TRUE(effect != nullptr);

    const RectI roi(10, 20, 110, 220);
    EXPECT_EQ(roi, effect->getGpuInputRoI(0, effect->getCurrentTime(), effect->getCurrentView(), roi, RenderScale()));
}
