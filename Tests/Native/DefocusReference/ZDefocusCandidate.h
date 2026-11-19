/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
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

#ifndef NATRON_TESTS_DEFOCUSREFERENCE_ZDEFOCUSCANDIDATE_H
#define NATRON_TESTS_DEFOCUSREFERENCE_ZDEFOCUSCANDIDATE_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/RectD.h"
#include "Engine/RenderScale.h"

#include "AnalyticScene.h"

#define kTestPluginIDDefocusHarnessSource "test.natron.built-in.DefocusHarnessSource"
#define kDefocusHarnessSourceParamGeneration "generation"
#define kDefocusHarnessDepthLayerID "harnessDepth"

NATRON_NAMESPACE_ENTER

/**
 * @brief A source of the pinhole image last given to setDefocusHarnessImage(): its RGBA, and its
 * planar depth in the red of the harnessDepth layer, over a format of the image's size, under
 * Stretch edges. The Generation knob changes with every image, so nothing cached outlives one.
 **/
class DefocusHarnessSourceTestEffect
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new DefocusHarnessSourceTestEffect(n);
    }

    explicit DefocusHarnessSourceTestEffect(NodePtr n);

    virtual bool getMakeSettingsPanel() const OVERRIDE FINAL
    {
        return false;
    }

    virtual bool isMultiPlanar() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual bool supportsTiles() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual void addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const OVERRIDE FINAL;

    virtual StatusEnum getRegionOfDefinition(U64 hash,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;

private:
    static constexpr NodeSpec kNodeSpec = {
        .id = kTestPluginIDDefocusHarnessSource,
        .label = "Test Defocus Harness Source",
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual StatusEnum getPreferredMetadataAt(double time,
                                              ViewIdx view,
                                              NodeMetadata& metadata) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void getComponentsNeededAndProduced(double time,
                                                ViewIdx view,
                                                EffectInstance::ComponentsNeededMap* comps,
                                                double* passThroughTime,
                                                int* passThroughView,
                                                int* passThroughInputNb) OVERRIDE FINAL;

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    KnobIntWPtr _generation;
};

ImageLayerDesc defocusHarnessDepthLayer();

/**
 * @brief Today's ZDefocus as a harness candidate: a harness source into a ZDefocus in Lens mode
 * at the scene camera's lens, depth in metres, Stretch edges and a Max Size that covers the
 * image's largest circle of confusion. Its nodes are made in `app`'s project on the first render
 * and reused.
 **/
class ZDefocusCandidate {
public:
    explicit ZDefocusCandidate(const AppInstancePtr& app);

    // Defocuses `pinhole` with `layers` depth slices into premultiplied RGBA of its size.
    bool render(const DefocusReference::Camera& camera,
                const DefocusReference::PinholeImage& pinhole,
                int layers,
                std::vector<float>* rgba,
                std::string* error);

private:
    AppInstancePtr _app;
    NodePtr _source;
    NodePtr _zdefocus;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_TESTS_DEFOCUSREFERENCE_ZDEFOCUSCANDIDATE_H
