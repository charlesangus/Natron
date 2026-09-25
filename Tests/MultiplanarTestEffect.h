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

#ifndef Tests_MultiplanarTestEffect_h
#define Tests_MultiplanarTestEffect_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Node.h"
#include "Engine/Nodes/NativeEffectBase.h"

#define kTestPluginIDMultiplanarDiffuseOnly "test.natron.built-in.MultiplanarDiffuseOnly"

NATRON_NAMESPACE_ENTER

// Stands in for a Shuffle-shaped multiplanar effect that declares only one produced layer
// ("diffuse"), so a test can drive getComponentsNeededAndProduced_public() against a plug-in that
// never touches Color and check what the metadata-layer merge does around it, with
// producesMetadataLayerImplicitly() switchable at runtime instead of fixed by an override.
class MultiplanarDiffuseOnlyTestEffect
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new MultiplanarDiffuseOnlyTestEffect(n);
    }

    explicit MultiplanarDiffuseOnlyTestEffect(NodePtr n)
        : NativeEffectBase(n)
        , _producesMetadataLayerImplicitly(true)
    {
    }

    virtual bool getMakeSettingsPanel() const OVERRIDE FINAL
    {
        return false;
    }

    virtual bool isMultiPlanar() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual bool producesMetadataLayerImplicitly() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return _producesMetadataLayerImplicitly;
    }

    void setProducesMetadataLayerImplicitly(bool produces)
    {
        _producesMetadataLayerImplicitly = produces;
    }

private:
    virtual NativePluginDescription getNativePluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        NativePluginDescription desc;

        desc.id = kTestPluginIDMultiplanarDiffuseOnly;
        desc.label = "Test Multiplanar Diffuse Only";
        desc.description = "";
        desc.inputs.push_back(NativeInputDescription("Source", true, eDataKindImage));
        desc.outputKind = eDataKindImage;

        return desc;
    }

    virtual void getComponentsNeededAndProduced(double time,
                                                ViewIdx view,
                                                EffectInstance::ComponentsNeededMap* comps,
                                                double* passThroughTime,
                                                int* passThroughView,
                                                int* passThroughInputNb) OVERRIDE FINAL
    {
        std::vector<std::string> rgb;
        rgb.push_back("R");
        rgb.push_back("G");
        rgb.push_back("B");
        (*comps)[-1].push_back(ImageLayerDesc("diffuse", "diffuse", "RGB", rgb));

        *passThroughTime = time;
        *passThroughView = view;
        NodePtr node = getNode();
        *passThroughInputNb = node ? node->getPreferredInput() : -1;
    }

    bool _producesMetadataLayerImplicitly;
};

NATRON_NAMESPACE_EXIT

#endif // Tests_MultiplanarTestEffect_h
