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

#ifndef Tests_Native_GlHostWrapTestEffect_h
#define Tests_Native_GlHostWrapTestEffect_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <atomic>
#include <list>
#include <utility>

#include "Engine/EngineFwd.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Node.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/OSGLContext.h"

#define kTestPluginIDGlHostWrap "test.natron.built-in.GlHostWrap"

NATRON_NAMESPACE_ENTER

// The constant its render writes, whichever input it has.
const float kGlHostWrapOutput[4] = { 0.75f, -0.25f, 1.5f, 0.5f };

/**
 * @brief An effect that claims OpenGL support and asks the host for its mix, mask and channel
 * selectors, the way an OpenFX plug-in does. The host applies those around the render, so the
 * constant it writes only reaches the output through them.
 **/
class GlHostWrapTestEffect
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new GlHostWrapTestEffect(n);
    }

    explicit GlHostWrapTestEffect(NodePtr n)
        : NativeEffectBase(n)
    {
    }

    static std::atomic<int>& glRenders()
    {
        static std::atomic<int> n(0);

        return n;
    }

    static std::atomic<int>& cpuRenders()
    {
        static std::atomic<int> n(0);

        return n;
    }

    virtual PluginOpenGLRenderSupport supportsOpenGLRender() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return ePluginOpenGLRenderSupportYes;
    }

    virtual bool isHostMaskingEnabled() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual bool isHostMixingEnabled() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual bool supportsTiles() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual RenderSafetyEnum renderThreadSafety() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return eRenderSafetyFullySafe;
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

private:
    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Source", .kind = eDataKindImage, .optional = false },
        InputSpec { .label = "Mask", .kind = eDataKindImage, .role = InputRole::Mask, .optional = true },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = kTestPluginIDGlHostWrap,
        .label = "Test GL Host Wrap",
        .outputKind = eDataKindImage,
        .inputs = kInputs,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    virtual void initializeKnobs() OVERRIDE FINAL
    {
    }

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        if (args.useOpenGL) {
            ++glRenders();
            glClearColor(kGlHostWrapOutput[0], kGlHostWrapOutput[1], kGlHostWrapOutput[2], kGlHostWrapOutput[3]);
            glClear(GL_COLOR_BUFFER_BIT);

            return eStatusOK;
        }

        ++cpuRenders();
        for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
            const ImagePtr& image = it->second;
            if (!image || (image->getBitDepth() != eImageBitDepthFloat)) {
                return eStatusFailed;
            }
            const int nComps = static_cast<int>(image->getComponentsCount());
            Image::WriteAccess access(image.get());
            for (int y = args.roi.y1; y < args.roi.y2; ++y) {
                for (int x = args.roi.x1; x < args.roi.x2; ++x) {
                    float* pixel = reinterpret_cast<float*>(access.pixelAt(x, y));
                    if (!pixel) {
                        return eStatusFailed;
                    }
                    for (int c = 0; c < nComps; ++c) {
                        pixel[c] = kGlHostWrapOutput[c < 4 ? c : 3];
                    }
                }
            }
        }

        return eStatusOK;
    }
};

NATRON_NAMESPACE_EXIT

#endif // Tests_Native_GlHostWrapTestEffect_h
