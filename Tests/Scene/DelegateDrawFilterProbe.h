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
#ifndef NATRON_TESTS_SCENE_DELEGATEDRAWFILTERPROBE_H
#define NATRON_TESTS_SCENE_DELEGATEDRAWFILTERPROBE_H

// Include after Python.h and UsdIncludes.h, as the tests that use it do.

#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <thread>

#include "Engine/Nodes/Scene/CyclesAovs.h"
#include "Engine/Nodes/Scene/MoonRayAovs.h"
#include "Engine/Nodes/Scene/SceneDrawFilter.h"

#ifdef emit
#define NATRON_DELEGATEDRAWFILTERPROBE_RESTORE_EMIT
#undef emit
#endif
#include <pxr/base/gf/vec3f.h>
#include <pxr/imaging/hd/renderBuffer.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hd/types.h>
#include <pxr/imaging/hdx/tokens.h>
#include <pxr/usd/usdGeom/cube.h>
#include <pxr/usd/usdGeom/xform.h>
#include <pxr/usd/usdGeom/xformCommonAPI.h>
#include <pxr/usd/usdLux/distantLight.h>
#include <pxr/usdImaging/usdImagingGL/engine.h>
#include <pxr/usdImaging/usdImagingGL/renderParams.h>
#ifdef NATRON_DELEGATEDRAWFILTERPROBE_RESTORE_EMIT
#undef NATRON_DELEGATEDRAWFILTERPROBE_RESTORE_EMIT
#define emit
#endif

namespace DelegateDrawFilterProbe {

// What one render of the probe's scene drew: the prim paths its primId AOV names, and the means of
// its colour AOV's colour channels, together and for red and green alone.
struct Frame {
    std::set<std::string> paths;
    double mean = 0.;
    double red = 0.;
    double green = 0.;
    std::string error;
};

inline const char*
meshA()
{
    return "/World/a";
}
inline const char*
meshB()
{
    return "/World/b";
}
inline const char*
meshC()
{
    return "/World/c";
}
inline const char*
light()
{
    return "/World/Key";
}

/**
 * @brief Three cubes in a row and a pure red distant light, rendered by one delegate engine that was built
 * under a SceneDrawFilter::EngineScope, so the engine's render index has a filter this class
 * drives.
 */
class Probe {
public:
    enum KindEnum {
        eKindCycles,
        eKindMoonRay
    };

    Probe(KindEnum kind,
          const NATRON_NAMESPACE::SceneDrawFilter::Rules& seed)
        : _kind(kind)
    {
        _stage = PXR_NS::UsdStage::CreateInMemory();
        (void)PXR_NS::UsdGeomXform::Define(_stage, PXR_NS::SdfPath("/World"));
        const char* const cubes[3] = { meshA(), meshB(), meshC() };
        for (int i = 0; i < 3; ++i) {
            PXR_NS::UsdGeomCube cube = PXR_NS::UsdGeomCube::Define(_stage, PXR_NS::SdfPath(cubes[i]));
            cube.GetSizeAttr().Set(2.);
            PXR_NS::UsdGeomXformCommonAPI(cube.GetPrim()).SetTranslate(PXR_NS::GfVec3d(-3. + 3. * i, 0., 0.));
        }
        PXR_NS::UsdLuxDistantLight key = PXR_NS::UsdLuxDistantLight::Define(_stage, PXR_NS::SdfPath(light()));
        key.GetIntensityAttr().Set(3.f);
        // The key's colour tells its light apart from a renderer's own default light, such as the
        // white environment light hdMoonray turns on when its scene has no light left.
        key.GetColorAttr().Set(PXR_NS::GfVec3f(1.f, 0.f, 0.f));
        if (kind == eKindMoonRay) {
            // hdMoonray reads an unnormalized distant light's intensity as the radiance of its
            // half-degree disk, which leaves the cubes all but black; hdCycles reads it as
            // irradiance either way.
            key.GetNormalizeAttr().Set(true);
        }

        PXR_NS::UsdImagingGLEngine::Parameters parameters;
        parameters.rendererPluginId = PXR_NS::TfToken(kind == eKindCycles ? "HdCyclesPlugin" : "HdMoonrayRendererPlugin");
        parameters.gpuEnabled = false;
        if (kind == eKindCycles) {
            NATRON_NAMESPACE::CyclesAovs::registerSceneIndices();
        } else {
            NATRON_NAMESPACE::MoonRayAovs::registerSceneIndices();
        }
        NATRON_NAMESPACE::SceneDrawFilter::registerForAllRenderers();
        std::string instanceId;
        {
            const NATRON_NAMESPACE::SceneDrawFilter::EngineScope scope;
            const std::function<void()> make = [&]() { _engine.reset(new PXR_NS::UsdImagingGLEngine(parameters)); };
            if (kind == eKindCycles) {
                NATRON_NAMESPACE::CyclesAovs::withThreadBudget(2, make);
            } else {
                make();
            }
            instanceId = scope.getInstanceId();
        }
        _filter = NATRON_NAMESPACE::SceneDrawFilter::find(instanceId);
        if (!_engine || !PXR_NS::get_pointer(_filter)) {
            return;
        }
        _filter->seedRules(seed);

        if (kind == eKindCycles) {
            NATRON_NAMESPACE::CyclesAovs::configureEngine(_engine.get());
            _engine->SetRendererSetting(PXR_NS::TfToken("cycles:samples"), PXR_NS::VtValue(16));
            _engine->SetRendererSetting(PXR_NS::TfToken("cycles:integrator:seed"), PXR_NS::VtValue(7));
            _engine->SetRendererSetting(PXR_NS::TfToken("cycles:integrator:use_adaptive_sampling"), PXR_NS::VtValue(false));
            _aovs = { PXR_NS::HdAovTokens->color, PXR_NS::HdAovTokens->depth, PXR_NS::HdAovTokens->primId, PXR_NS::HdAovTokens->instanceId };
        } else {
            NATRON_NAMESPACE::MoonRayAovs::configureEngine(_engine.get(), 2);
            _aovs = NATRON_NAMESPACE::MoonRayAovs::aovs();
        }
        _aovsAccepted = _engine->SetRendererAovs(_aovs);
    }

    bool valid() const
    {
        return _engine && PXR_NS::get_pointer(_filter) && _aovsAccepted;
    }

    NATRON_NAMESPACE::SceneDrawFilter& filter() { return *PXR_NS::get_pointer(_filter); }

    Frame render()
    {
        Frame frame;
        const int size = kSize;
        PXR_NS::GfFrustum frustum;
        frustum.SetPerspective(45., 1., 1., 100.);
        frustum.SetPosition(PXR_NS::GfVec3d(0., 0., 12.));
        _engine->SetRenderBufferSize(PXR_NS::GfVec2i(size, size));
        _engine->SetFraming(PXR_NS::CameraUtilFraming(PXR_NS::GfRect2i(PXR_NS::GfVec2i(0, 0), size, size)));
        _engine->SetCameraState(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix());

        PXR_NS::UsdImagingGLRenderParams params;
        params.frame = PXR_NS::UsdTimeCode::Default();
        params.drawMode = PXR_NS::UsdImagingGLDrawMode::DRAW_SHADED_SMOOTH;
        params.enableLighting = true;
        params.enableSceneLights = true;
        params.enableSceneMaterials = true;
        params.clearColor = PXR_NS::GfVec4f(0.f, 0.f, 0.f, 0.f);
        params.colorCorrectionMode = PXR_NS::HdxColorCorrectionTokens->disabled;
        params.highlight = false;

        std::unique_ptr<NATRON_NAMESPACE::MoonRayAovs::LogCapture> log;
        if (_kind == eKindMoonRay) {
            log.reset(new NATRON_NAMESPACE::MoonRayAovs::LogCapture);
        }
        const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
        for (;;) {
            try {
                _engine->Render(_stage->GetPseudoRoot(), params);
            } catch (const std::exception& e) {
                frame.error = e.what();

                return frame;
            }
            if (_engine->IsConverged()) {
                break;
            }
            if (log) {
                frame.error = NATRON_NAMESPACE::MoonRayAovs::sessionFailure(_engine->GetRenderStats(), log->notices());
                if (!frame.error.empty()) {
                    return frame;
                }
            }
            if (std::chrono::steady_clock::now() - started > std::chrono::seconds(180)) {
                frame.error = "The render did not converge within 180 seconds.";

                return frame;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        readFrame(&frame);

        return frame;
    }

private:
    static const int kSize = 96;

    class Mapped {
    public:
        Mapped(PXR_NS::UsdImagingGLEngine* engine,
               const PXR_NS::TfToken& name)
        {
            _buffer = engine->GetAovRenderBuffer(name);
            if (!_buffer) {
                return;
            }
            _buffer->Resolve();
            if (((int)_buffer->GetWidth() != kSize) || ((int)_buffer->GetHeight() != kSize)) {
                _buffer = nullptr;

                return;
            }
            _data = static_cast<const unsigned char*>(_buffer->Map());
            _format = _buffer->GetFormat();
            _stride = PXR_NS::HdDataSizeOfFormat(_format);
        }

        ~Mapped()
        {
            if (_buffer && _data) {
                _buffer->Unmap();
            }
        }

        bool valid() const { return _data != nullptr; }
        PXR_NS::HdFormat format() const { return _format; }
        const unsigned char* pixel(std::size_t index) const { return _data + index * _stride; }

    private:
        PXR_NS::HdRenderBuffer* _buffer = nullptr;
        const unsigned char* _data = nullptr;
        PXR_NS::HdFormat _format = PXR_NS::HdFormatInvalid;
        std::size_t _stride = 0;
    };

    static float component(PXR_NS::HdFormat format,
                           const unsigned char* at)
    {
        switch (PXR_NS::HdGetComponentFormat(format)) {
        case PXR_NS::HdFormatUNorm8:
            return *at / 255.f;
        case PXR_NS::HdFormatFloat16: {
            PXR_NS::GfHalf value;
            std::memcpy(&value, at, sizeof(value));

            return float(value);
        }
        case PXR_NS::HdFormatFloat32: {
            float value;
            std::memcpy(&value, at, sizeof(value));

            return value;
        }
        default:
            return 0.f;
        }
    }

    void readFrame(Frame* frame)
    {
        const std::size_t pixels = (std::size_t)kSize * kSize;
        {
            Mapped primIds(_engine.get(), PXR_NS::HdAovTokens->primId);
            Mapped instanceIds(_engine.get(), PXR_NS::HdAovTokens->instanceId);
            if (!primIds.valid() || !instanceIds.valid()) {
                frame->error = "The delegate rendered no primId or instanceId AOV.";

                return;
            }
            const auto readInt = [](const unsigned char* at) {
                std::int32_t value;
                std::memcpy(&value, at, sizeof(value));

                return value;
            };
            std::set<std::pair<std::int32_t, std::int32_t>> seen;
            for (std::size_t i = 0; i < pixels; ++i) {
                const std::int32_t prim = readInt(primIds.pixel(i));
                if (prim < 0) {
                    continue;
                }
                const std::int32_t instance = readInt(instanceIds.pixel(i));
                const std::pair<std::int32_t, std::int32_t> key(prim, (_kind == eKindCycles) ? NATRON_NAMESPACE::CyclesAovs::instanceIndex(instance) : instance);
                if (!seen.insert(key).second) {
                    continue;
                }
                PXR_NS::SdfPath path;
                if (_engine->DecodeIntersection(key.first, key.second, &path) && !path.IsEmpty()) {
                    frame->paths.insert(path.GetString());
                }
            }
        }
        Mapped color(_engine.get(), PXR_NS::HdAovTokens->color);
        if (!color.valid()) {
            frame->error = "The delegate rendered no color AOV.";

            return;
        }
        const std::size_t channels = std::min<std::size_t>(3, PXR_NS::HdGetComponentCount(color.format()));
        const std::size_t componentSize = PXR_NS::HdDataSizeOfFormat(PXR_NS::HdGetComponentFormat(color.format()));
        double sums[3] = { 0., 0., 0. };
        for (std::size_t i = 0; i < pixels; ++i) {
            for (std::size_t c = 0; c < channels; ++c) {
                sums[c] += component(color.format(), color.pixel(i) + c * componentSize);
            }
        }
        frame->mean = (sums[0] + sums[1] + sums[2]) / (double)(pixels * std::max<std::size_t>(1, channels));
        frame->red = sums[0] / (double)pixels;
        frame->green = (channels > 1) ? sums[1] / (double)pixels : 0.;
    }

    KindEnum _kind;
    PXR_NS::UsdStageRefPtr _stage;
    std::unique_ptr<PXR_NS::UsdImagingGLEngine> _engine;
    NATRON_NAMESPACE::SceneDrawFilter::WeakPtr _filter;
    PXR_NS::TfTokenVector _aovs;
    bool _aovsAccepted = false;
};

// The delegate's frames for the scene the filter's rules change in turn.
struct Scenario {
    Frame seeded;
    Frame aBoxed;
    Frame bHidden;
    Frame lightHidden;
    std::size_t heldInvisibleAfterBox = 0;
    bool engineMade = false;
};

inline Scenario
runScenario(Probe::KindEnum kind)
{
    using NATRON_NAMESPACE::SceneDrawFilter;
    Scenario result;
    SceneDrawFilter::Rules rules;
    rules[PXR_NS::SdfPath("/World")] = SceneDrawFilter::eRuleShow;
    Probe probe(kind, rules);
    if (!probe.valid()) {
        return result;
    }
    result.engineMade = true;
    result.seeded = probe.render();
    if (!result.seeded.error.empty()) {
        return result;
    }
    rules[PXR_NS::SdfPath(meshA())] = SceneDrawFilter::eRuleHide;
    probe.filter().replaceRules(rules);
    result.heldInvisibleAfterBox = probe.filter().getHeldInvisibleCount();
    result.aBoxed = probe.render();
    if (!result.aBoxed.error.empty()) {
        return result;
    }
    rules[PXR_NS::SdfPath(meshB())] = SceneDrawFilter::eRuleHidden;
    probe.filter().replaceRules(rules);
    result.bHidden = probe.render();
    if (!result.bHidden.error.empty()) {
        return result;
    }
    rules[PXR_NS::SdfPath(light())] = SceneDrawFilter::eRuleHidden;
    probe.filter().replaceRules(rules);
    result.lightHidden = probe.render();

    return result;
}

} // namespace DelegateDrawFilterProbe

#endif // NATRON_TESTS_SCENE_DELEGATEDRAWFILTERPROBE_H
