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

#ifndef Engine_Nodes_Test_OverlayImageGuiTestNode_h
#define Engine_Nodes_Test_OverlayImageGuiTestNode_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <algorithm>
#include <list>
#include <string>
#include <utility>
#include <vector>

#include "Engine/EffectInstance.h"
#include "Engine/EngineFwd.h"
#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/OverlayDrawList.h"
#include "Engine/RectI.h"

#define kTestPluginIDOverlayImageGui "test.natron.built-in.OverlayImageGui"
#define kOverlayImageGuiTestParamShow "showMask"

NATRON_NAMESPACE_ENTER

/**
 * @brief A flat grey frame that offers two overlay image planes when its "showMask" knob is on and
 * its panel open: a red mask one canonical pixel wide, to see that thin masks survive zooming out,
 * and a half-transparent green block. For tests only, registered like OverlayTestNode.
 **/
class OverlayImageGuiTestNode
    : public NativeEffectBase {
public:
    static constexpr int kWidth = 512;
    static constexpr int kHeight = 256;
    static constexpr int kLineX = 200;
    static constexpr int kBlockX1 = 300;
    static constexpr int kBlockX2 = 364;
    static constexpr int kBlockY1 = 100;
    static constexpr int kBlockY2 = 164;

    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new OverlayImageGuiTestNode(n);
    }

    explicit OverlayImageGuiTestNode(NodePtr n)
        : NativeEffectBase(n)
    {
    }

    virtual bool supportsTiles() const OVERRIDE FINAL
    {
        return true;
    }

    virtual RenderSafetyEnum renderThreadSafety() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return eRenderSafetyFullySafe;
    }

    virtual StatusEnum getRegionOfDefinition(U64 /*hash*/,
                                             double /*time*/,
                                             const RenderScale& /*scale*/,
                                             ViewIdx /*view*/,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        rod->x1 = 0.;
        rod->y1 = 0.;
        rod->x2 = kWidth;
        rod->y2 = kHeight;

        return eStatusOK;
    }

    virtual void addAcceptedComponents(int /*inputNb*/,
                                       std::list<ImageLayerDesc>* comps) OVERRIDE FINAL
    {
        comps->push_back(ImageLayerDesc::getRGBAComponents());
    }

    virtual void addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const OVERRIDE FINAL
    {
        depths->push_back(eImageBitDepthFloat);
    }

    virtual bool isOverlayImageViewOn() const OVERRIDE FINAL
    {
        KnobBoolPtr knob = _show.lock();

        return knob && knob->getValue();
    }

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum /*reason*/,
                             ViewSpec /*view*/,
                             double /*time*/,
                             bool /*originatedFromMainThread*/) OVERRIDE FINAL
    {
        KnobBoolPtr show = _show.lock();
        if (!show || (k != show.get())) {
            return false;
        }
        refreshOverlayImages();

        return true;
    }

private:
    virtual void initializeKnobs() OVERRIDE FINAL
    {
        KnobPagePtr page = createKnob<KnobPage>(std::string("Controls"));
        KnobBoolPtr show = createKnob<KnobBool>(std::string("Show Mask"));

        show->setName(kOverlayImageGuiTestParamShow);
        show->setAnimationEnabled(false);
        show->setDefaultValue(false);
        show->setEvaluateOnChange(false);
        show->setHashExempt(true);
        page->addKnob(show);
        _show = show;
    }

    // Whether the pixel x at mipmap level level covers any canonical pixel of [lo, hi).
    static bool covers(int pixel,
                       unsigned int level,
                       int lo,
                       int hi)
    {
        return (pixel << level) < hi && ((pixel + 1) << level) > lo;
    }

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
            it->second->fill(args.roi, 0.15f, 0.15f, 0.15f, 1.f);
        }

        if (isOverlayImageRender()) {
            const unsigned int level = args.mappedScale.toMipmapLevel();
            const size_t w = (size_t)args.roi.width();
            const size_t h = (size_t)args.roi.height();
            std::vector<unsigned char> line(w * h, 0);
            std::vector<unsigned char> block(w * h, 0);
            for (size_t y = 0; y < h; ++y) {
                const int py = args.roi.y1 + (int)y;
                for (size_t x = 0; x < w; ++x) {
                    const int px = args.roi.x1 + (int)x;
                    if (covers(px, level, kLineX, kLineX + 1)) {
                        line[y * w + x] = 255;
                    }
                    if (covers(px, level, kBlockX1, kBlockX2) && covers(py, level, kBlockY1, kBlockY2)) {
                        block[y * w + x] = 255;
                    }
                }
            }
            addOverlayImageTile("line", OverlayColor::fromRGBA(1.f, 0.f, 0.f, 1.f), args.roi, level, std::move(line));
            addOverlayImageTile("block", OverlayColor::fromRGBA(0.f, 1.f, 0.f, 0.5f), args.roi, level, std::move(block));
        }

        return eStatusOK;
    }

    static constexpr NodeSpec kNodeSpec = {
        .id = kTestPluginIDOverlayImageGui,
        .label = "Test Overlay Image Gui",
        .outputKind = eDataKindImage,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    KnobBoolWPtr _show;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Test_OverlayImageGuiTestNode_h
