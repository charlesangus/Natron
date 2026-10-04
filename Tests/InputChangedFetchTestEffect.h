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

#ifndef Tests_InputChangedFetchTestEffect_h
#define Tests_InputChangedFetchTestEffect_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Node.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/RectD.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

#define kTestPluginIDInputChangedFetch "test.natron.built-in.InputChangedFetch"

NATRON_NAMESPACE_ENTER

// Pulls its input from inside onInputChanged, the way an OpenFX plug-in may call clipGetImage
// from kOfxActionInstanceChanged, so a test can check what that leaves behind on the upstream graph.
class InputChangedFetchTestEffect
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new InputChangedFetchTestEffect(n);
    }

    explicit InputChangedFetchTestEffect(NodePtr n)
        : NativeEffectBase(n)
        , _fetchedImage()
    {
    }

    virtual bool getMakeSettingsPanel() const OVERRIDE FINAL
    {
        return false;
    }

    virtual void onInputChanged(int inputNb) OVERRIDE FINAL
    {
        if ((inputNb != 0) || !getInput(0)) {
            return;
        }
        // Explicit bounds and layer: this node's own clip metadata is only refreshed after onInputChanged returns.
        const RectD bounds(0., 0., 64., 64.);
        _fetchedImage = getImage(0,
                                 getCurrentTime(),
                                 RenderScale::fromMipmapLevel(0),
                                 ViewIdx(0),
                                 &bounds,
                                 &ImageLayerDesc::getRGBAComponents(),
                                 false, // mapToClipPrefs
                                 false, // dontUpscale
                                 eStorageModeRAM,
                                 0, // textureDepth
                                 0); // roiPixel
    }

    const ImagePtr& getFetchedImage() const
    {
        return _fetchedImage;
    }

private:
    virtual NativePluginDescription getNativePluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        NativePluginDescription desc;

        desc.id = kTestPluginIDInputChangedFetch;
        desc.label = "Test Input Changed Fetch";
        desc.description = "";
        desc.inputs.push_back(NativeInputDescription("Source", true, eDataKindImage));
        desc.outputKind = eDataKindImage;

        return desc;
    }

    ImagePtr _fetchedImage;
};

NATRON_NAMESPACE_EXIT

#endif // Tests_InputChangedFetchTestEffect_h
