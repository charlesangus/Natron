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

#ifndef Tests_Ai_AiModelTestEffect_h
#define Tests_Ai_AiModelTestEffect_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <memory>
#include <utility>

#include "Engine/Ai/ModelKnob.h"
#include "Engine/EngineFwd.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/RectD.h"

#define kTestPluginIDAiModel "test.natron.built-in.AiModel"

NATRON_NAMESPACE_ENTER

const int kAiModelTestSize = 8;
const float kAiModelTestOutput = 0.5f;

/**
 * @brief A denoise-task generator with a ModelKnob: it renders a constant once the model resolves
 * and fails otherwise, so a test can watch the knob's errors come and go.
 **/
class AiModelTestEffect
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new AiModelTestEffect(n);
    }

    explicit AiModelTestEffect(NodePtr n)
        : NativeEffectBase(n)
    {
    }

    // The downloader the next node created uses, so a test can point it at a fixture manifest.
    static ModelDownloader*& downloader()
    {
        static ModelDownloader* d = nullptr;

        return d;
    }

    ModelKnob* modelKnob() const
    {
        return _modelKnob.get();
    }

    virtual bool supportsTiles() const OVERRIDE FINAL
    {
        return true;
    }

    virtual StatusEnum getRegionOfDefinition(U64 /*hash*/,
                                             double /*time*/,
                                             const RenderScale& /*scale*/,
                                             ViewIdx /*view*/,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        rod->x1 = 0.;
        rod->y1 = 0.;
        rod->x2 = kAiModelTestSize;
        rod->y2 = kAiModelTestSize;

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

    virtual void onEffectCreated(bool /*mayCreateFileDialog*/,
                                 const CreateNodeArgs& args) OVERRIDE FINAL
    {
        if (_modelKnob) {
            _modelKnob->onEffectCreated(args);
        }
    }

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum /*reason*/,
                             ViewSpec /*view*/,
                             double /*time*/,
                             bool /*originatedFromMainThread*/) OVERRIDE FINAL
    {
        return _modelKnob && _modelKnob->knobChanged(k);
    }

private:
    static constexpr NodeSpec kNodeSpec = {
        .id = kTestPluginIDAiModel,
        .label = "Test AI Model",
        .outputKind = eDataKindImage,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    virtual void initializeKnobs() OVERRIDE FINAL
    {
        KnobPagePtr page = createKnob<KnobPage>(std::string("Controls"));

        _modelKnob = ModelKnob::create(this, page, AiModelTask::eDenoise, downloader());
    }

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        if (!_modelKnob || !_modelKnob->resolve()) {
            return eStatusFailed;
        }
        for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
            const ImagePtr& image = it->second;
            if (!image || (image->getBitDepth() != eImageBitDepthFloat)) {
                return eStatusFailed;
            }
            const int numChannels = (int)image->getComponentsCount();
            Image::WriteAccess access(image.get());
            for (int y = args.roi.y1; y < args.roi.y2; ++y) {
                for (int x = args.roi.x1; x < args.roi.x2; ++x) {
                    float* pixel = (float*)access.pixelAt(x, y);
                    if (!pixel) {
                        return eStatusFailed;
                    }
                    for (int c = 0; c < numChannels; ++c) {
                        pixel[c] = kAiModelTestOutput;
                    }
                }
            }
        }

        return eStatusOK;
    }

    std::unique_ptr<ModelKnob> _modelKnob;
};

NATRON_NAMESPACE_EXIT

#endif // Tests_Ai_AiModelTestEffect_h
