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

#ifndef NATRON_TESTS_SCENE_LIVEVIEWERRENDER_H
#define NATRON_TESTS_SCENE_LIVEVIEWERRENDER_H

// Include after Python.h and UsdIncludes.h, as the tests that use it do.

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/camera.h>

#include "Engine/Nodes/Scene/CyclesAovs.h"
#include "Engine/Nodes/Scene/HydraRenderImpl.h"
#include "Engine/Nodes/Scene/SceneDrawFilter.h"
#include "Engine/Nodes/Scene/SceneStage.h"
#include "Engine/ScenePayload.h"

/**
 * A viewer's Cycles render in the background, as the viewer runs it: a stage of its own, the lock
 * that guards it, and a HydraRender::ProgressiveRender drawing by draw rules. Tests of the offline
 * renders keep one alive beside them to show it changes nothing they make.
 **/
class LiveViewerRender {
public:
    typedef NATRON_NAMESPACE::HydraRender::ProgressiveRender ProgressiveRender;

    explicit LiveViewerRender(PXR_NS::UsdStage::InitialLoadSet load = PXR_NS::UsdStage::LoadAll)
        : _stage(new NATRON_NAMESPACE::SceneStage(load))
    {
    }

    ~LiveViewerRender()
    {
        _render.reset();
        _stage.reset();
    }

    NATRON_NAMESPACE::SceneStage& stage() { return *_stage; }
    std::mutex& stageLock() { return _stageLock; }
    ProgressiveRender& render() { return *_render; }

    // Syncs payload into the stage under its lock, and restarts the render if that edited it.
    void sync(const NATRON_NAMESPACE::ScenePayload& payload)
    {
        bool edited = false;
        {
            std::lock_guard<std::mutex> lock(_stageLock);
            edited = _stage->sync(payload);
        }
        if (edited && _render) {
            _render->invalidate();
        }
    }

    /**
     * @brief Starts rendering payload with Cycles through camera, size pixels square, drawn by
     * rules. False after a gtest failure.
     **/
    bool start(const NATRON_NAMESPACE::ScenePayload& payload,
               const char* camera,
               int size,
               const NATRON_NAMESPACE::SceneDrawFilter::Rules& rules)
    {
        if (!_stage->isValid()) {
            ADD_FAILURE() << "OpenUSD could not open a stage";

            return false;
        }
        sync(payload);
        PXR_NS::UsdGeomCamera cam(_stage->getStage()->GetPrimAtPath(PXR_NS::SdfPath(camera)));
        if (!cam) {
            ADD_FAILURE() << "the scene has no " << camera;

            return false;
        }
        ProgressiveRender::View view;
        view.frustum = cam.GetCamera(PXR_NS::UsdTimeCode::Default()).GetFrustum();
        view.width = size;
        view.height = size;
        view.time = PXR_NS::UsdTimeCode::Default();
        NATRON_NAMESPACE::SceneStage* stage = _stage.get();
        _render.reset(new ProgressiveRender(&_stageLock, [stage]() { return stage->getStage(); }, std::function<void()>(), 0.f));
        _render->setDrawRules(rules);
        _render->setThreads([]() { return 2; });
        _render->setRenderer(kRenderDelegateCyclesId);
        _render->setView(view);

        return true;
    }

    /**
     * @brief Waits until a restart after the after-th is done and its last frame published. False,
     * with a failure, if the render failed or did not finish in two minutes.
     **/
    bool waitUntilDone(std::uint64_t after = 0)
    {
        const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now() + std::chrono::seconds(120);
        while (std::chrono::steady_clock::now() < end) {
            const ProgressiveRender::Status status = _render->getStatus();
            if (!status.error.empty()) {
                ADD_FAILURE() << status.error;

                return false;
            }
            const std::shared_ptr<const ProgressiveRender::Frame> frame = _render->getFrame();
            if (status.converged && frame && frame->converged && (frame->restart == status.restarts) && (frame->restart > after)) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ADD_FAILURE() << "the viewer's render did not finish in 120 s";

        return false;
    }

    // The prims the last frame shows, from the engine's primId AOV.
    std::set<std::string> pickedPrims()
    {
        std::set<std::string> paths;
        const std::shared_ptr<const ProgressiveRender::Frame> frame = _render->getFrame();
        if (!frame) {
            return paths;
        }
        for (int y = 0; y < frame->height; ++y) {
            for (int x = 0; x < frame->width; ++x) {
                const std::string path = _render->pick(*frame, x, y);
                if (!path.empty()) {
                    paths.insert(path);
                }
            }
        }

        return paths;
    }

    // Every prim path the stage composes, under its lock.
    std::set<std::string> stagePrims()
    {
        std::lock_guard<std::mutex> lock(_stageLock);

        return primsOf(_stage->getStage());
    }

    static std::set<std::string> primsOf(const PXR_NS::UsdStageRefPtr& stage)
    {
        std::set<std::string> paths;
        if (stage) {
            for (const PXR_NS::UsdPrim& prim : stage->Traverse()) {
                paths.insert(prim.GetPath().GetString());
            }
        }

        return paths;
    }

private:
    std::mutex _stageLock;
    std::unique_ptr<NATRON_NAMESPACE::SceneStage> _stage;
    std::unique_ptr<ProgressiveRender> _render;
};

#endif // NATRON_TESTS_SCENE_LIVEVIEWERRENDER_H
