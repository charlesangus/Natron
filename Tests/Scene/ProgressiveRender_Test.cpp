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

#include "Engine/Nodes/Scene/UsdIncludes.h"

#include "Global/Macros.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <sys/syscall.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QEventLoop>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>

#include "BaseTest.h"
#include "CountingTestEffect.h"
#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/CyclesAovs.h"
#include "Engine/Nodes/Scene/HydraRenderImpl.h"
#include "Engine/Nodes/Scene/LiveTextureStore.h"
#include "Engine/Nodes/Scene/MoonRayAovs.h"
#include "Engine/Nodes/Scene/MoonRayRuntime.h"
#include "Engine/Nodes/Scene/MoonRaySettings.h"
#include "Engine/Nodes/Scene/RenderDelegates.h"
#include "Engine/Nodes/Scene/SceneDrawFilter.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneStage.h"
#include "Engine/Nodes/Scene/SceneWriteBarrier.h"
#include "Tests/Scene/CyclesTestUtils.h"
#include "Tests/Scene/MoonRayTestProcesses.h"
#include "Tests/TestWatchdog.h"

NATRON_NAMESPACE_USING

using CyclesTestUtils::cyclesOrSkip;
using CyclesTestUtils::fixedSampling;
using MoonRayTestProcesses::RenderProcessStopper;
using MoonRayTestProcesses::ScopedEnv;

// Drives HydraRender::ProgressiveRender, the viewer's background refinement of a delegate other
// than Storm, on Cycles over Tests/fixtures/scene-cycles.usda seen through /World/Cam: /World/Near,
// a 2-unit cube at the origin, fills the centre of the view nine units from the camera.

namespace {
const char* const kFixture = NATRON_TESTS_FIXTURES_DIR "/scene-cycles.usda";
// /World/Glow, a 2-unit emissive card facing the camera ten units away, over nothing.
const char* const kEmissionFixture = NATRON_TESTS_FIXTURES_DIR "/scene-cycles-emission.usda";
const char* const kCamera = "/World/Cam";
const int kSize = 32;
const int kSamples = 256;
const float kNearDistance = 9.f;
const std::chrono::seconds kTimeout(120);
// How long a destroyed render's worker may take to stop its delegate and end.
const int kEndMs = 60000;

typedef HydraRender::ProgressiveRender ProgressiveRender;

// The fixture as a USDRead node hands it on: a node layer sublayering the file and carrying its
// stage metadata.
ScenePayload
fixtureScene(const char* fixture)
{
    PXR_NS::SdfLayerRefPtr file = PXR_NS::SdfLayer::FindOrOpen(fixture);
    if (!file) {
        ADD_FAILURE() << "cannot open " << fixture;

        return ScenePayload();
    }
    PXR_NS::SdfLayerRefPtr layer = SceneOps::createNodeLayer("ProgressiveRenderTest");
    const PXR_NS::SdfPath& root = PXR_NS::SdfPath::AbsoluteRootPath();
    PXR_NS::TfToken upAxis;
    if (file->HasField(root, PXR_NS::UsdGeomTokens->upAxis, &upAxis)) {
        layer->SetField(root, PXR_NS::UsdGeomTokens->upAxis, upAxis);
    }
    double metersPerUnit = 0.;
    if (file->HasField(root, PXR_NS::UsdGeomTokens->metersPerUnit, &metersPerUnit)) {
        layer->SetField(root, PXR_NS::UsdGeomTokens->metersPerUnit, metersPerUnit);
    }
    layer->SetTimeCodesPerSecond(file->GetTimeCodesPerSecond());
    layer->InsertSubLayerPath(file->GetIdentifier());

    return SceneOps::appendLayer(ScenePayload(), layer, 0x9e0a11ULL);
}

// base with /World/Near moved back along the view axis by back units, as a node downstream would.
ScenePayload
movedNear(const ScenePayload& base,
          double back,
          U64 hash)
{
    PXR_NS::SdfLayerRefPtr layer = SceneOps::createNodeLayer("ProgressiveRenderTestEdit");
    PXR_NS::SdfPrimSpecHandle prim = PXR_NS::SdfCreatePrimInLayer(layer, PXR_NS::SdfPath("/World/Near"));
    PXR_NS::SdfAttributeSpecHandle translate = PXR_NS::SdfAttributeSpec::New(prim, "xformOp:translate", PXR_NS::SdfValueTypeNames->Double3);
    translate->SetDefaultValue(PXR_NS::VtValue(PXR_NS::GfVec3d(0., 0., -back)));
    PXR_NS::SdfAttributeSpecHandle order = PXR_NS::SdfAttributeSpec::New(prim, "xformOpOrder", PXR_NS::SdfValueTypeNames->TokenArray, PXR_NS::SdfVariabilityUniform);
    order->SetDefaultValue(PXR_NS::VtValue(PXR_NS::VtTokenArray(1, PXR_NS::TfToken("xformOp:translate"))));

    return SceneOps::appendLayer(base, layer, hash);
}

// base, of the emission fixture, with its card's emission set to grey, as a node downstream would.
ScenePayload
emittingGrey(const ScenePayload& base,
             float grey,
             U64 hash)
{
    PXR_NS::SdfLayerRefPtr layer = SceneOps::createNodeLayer("ProgressiveRenderTestEmission");
    PXR_NS::SdfPrimSpecHandle prim = PXR_NS::SdfCreatePrimInLayer(layer, PXR_NS::SdfPath("/World/Looks/Glow/Surface"));
    PXR_NS::SdfAttributeSpecHandle emission = PXR_NS::SdfAttributeSpec::New(prim, "inputs:emissiveColor", PXR_NS::SdfValueTypeNames->Color3f);
    emission->SetDefaultValue(PXR_NS::VtValue(PXR_NS::GfVec3f(grey, grey, grey)));

    return SceneOps::appendLayer(base, layer, hash);
}

// The colour and alpha of frame's pixel (x, y), counted from its bottom-left pixel.
PXR_NS::GfVec4f
pixelOf(const ProgressiveRender::Frame& frame,
        int x,
        int y)
{
    const std::size_t at = ((std::size_t)y * (std::size_t)frame.width + (std::size_t)x) * 4;

    return PXR_NS::GfVec4f((float)frame.rgba[at], (float)frame.rgba[at + 1], (float)frame.rgba[at + 2], (float)frame.rgba[at + 3]);
}

// The mean red of frame's 4 by 4 pixels about its centre, all on the near cube.
float
centreRed(const ProgressiveRender::Frame& frame)
{
    float sum = 0.f;
    for (int y = frame.height / 2 - 2; y < frame.height / 2 + 2; ++y) {
        for (int x = frame.width / 2 - 2; x < frame.width / 2 + 2; ++x) {
            sum += pixelOf(frame, x, y)[0];
        }
    }

    return sum / 16.f;
}

// The window depth, in frustum's projection, of a point distance units along the view axis.
float
windowDepth(const PXR_NS::GfFrustum& frustum,
            double distance)
{
    const PXR_NS::GfMatrix4d projection = frustum.ComputeProjectionMatrix();
    const double z = -distance;
    const double clipZ = z * projection[2][2] + projection[3][2];
    const double clipW = z * projection[2][3] + projection[3][3];

    return (float)((clipZ / clipW + 1.) * 0.5);
}

float
centreDepth(const ProgressiveRender::Frame& frame)
{
    return frame.depth[(std::size_t)(frame.height / 2) * (std::size_t)frame.width + (std::size_t)(frame.width / 2)];
}

bool
anyUnder(const std::set<std::string>& paths,
         const std::string& root)
{
    for (const std::string& path : paths) {
        if ((path == root) || (path.compare(0, root.size() + 1, root + "/") == 0)) {
            return true;
        }
    }

    return false;
}

SceneDrawDelta
hiding(const std::string& path)
{
    SceneDrawDelta delta;
    delta.hidden.push_back(path);

    return delta;
}

const char* const kKey = "/World/Key";

// base with a red distant light, /World/Key, shining down the view axis: its colour tells its
// light apart from the white headlight and from a renderer's own default light.
ScenePayload
withKey(const ScenePayload& base)
{
    PXR_NS::SdfLayerRefPtr layer = SceneOps::createNodeLayer("ProgressiveRenderTestKey");
    PXR_NS::SdfPrimSpecHandle prim = PXR_NS::SdfCreatePrimInLayer(layer, PXR_NS::SdfPath(kKey));
    prim->SetSpecifier(PXR_NS::SdfSpecifierDef);
    prim->SetTypeName("DistantLight");
    PXR_NS::SdfAttributeSpecHandle intensity = PXR_NS::SdfAttributeSpec::New(prim, "inputs:intensity", PXR_NS::SdfValueTypeNames->Float);
    intensity->SetDefaultValue(PXR_NS::VtValue(3.f));
    PXR_NS::SdfAttributeSpecHandle color = PXR_NS::SdfAttributeSpec::New(prim, "inputs:color", PXR_NS::SdfValueTypeNames->Color3f);
    color->SetDefaultValue(PXR_NS::VtValue(PXR_NS::GfVec3f(1.f, 0.f, 0.f)));
    // hdMoonray reads an unnormalized distant light's intensity as the radiance of its half-degree
    // disk, which leaves the cube all but black; hdCycles reads it as irradiance either way.
    PXR_NS::SdfAttributeSpecHandle normalize = PXR_NS::SdfAttributeSpec::New(prim, "inputs:normalize", PXR_NS::SdfValueTypeNames->Bool);
    normalize->SetDefaultValue(PXR_NS::VtValue(true));

    return SceneOps::appendLayer(base, layer, 0x6b65790aULL);
}

// base with a grey dome light, /World/Dome, which the camera sees wherever nothing covers it.
ScenePayload
withDome(const ScenePayload& base)
{
    PXR_NS::SdfLayerRefPtr layer = SceneOps::createNodeLayer("ProgressiveRenderTestDome");
    PXR_NS::SdfPrimSpecHandle prim = PXR_NS::SdfCreatePrimInLayer(layer, PXR_NS::SdfPath("/World/Dome"));
    prim->SetSpecifier(PXR_NS::SdfSpecifierDef);
    prim->SetTypeName("DomeLight");
    PXR_NS::SdfAttributeSpecHandle intensity = PXR_NS::SdfAttributeSpec::New(prim, "inputs:intensity", PXR_NS::SdfValueTypeNames->Float);
    intensity->SetDefaultValue(PXR_NS::VtValue(1.f));
    PXR_NS::SdfAttributeSpecHandle color = PXR_NS::SdfAttributeSpec::New(prim, "inputs:color", PXR_NS::SdfValueTypeNames->Color3f);
    color->SetDefaultValue(PXR_NS::VtValue(PXR_NS::GfVec3f(0.5f, 0.5f, 0.5f)));

    return SceneOps::appendLayer(base, layer, 0x646f6d65ULL);
}

// A linear value as an 8-bit sRGB level, clamped to [0, 255].
double
levelOf(float linear)
{
    const double v = std::min(1., std::max(0., (double)linear));

    return 255. * ((v <= 0.0031308) ? (v * 12.92) : (1.055 * std::pow(v, 1. / 2.4) - 0.055));
}

// The mean linear value of channel, or of all three if channel is -1, over the pixels of frame
// in columns [x0, x1) and rows [y0, y1) that keep(x, y) accepts; -1 if it accepts none.
float
meanOver(const ProgressiveRender::Frame& frame,
         int x0,
         int x1,
         int y0,
         int y1,
         int channel,
         const std::function<bool(int, int)>& keep)
{
    double sum = 0.;
    int count = 0;
    for (int y = std::max(0, y0); y < std::min(frame.height, y1); ++y) {
        for (int x = std::max(0, x0); x < std::min(frame.width, x1); ++x) {
            if (keep && !keep(x, y)) {
                continue;
            }
            const PXR_NS::GfVec4f px = pixelOf(frame, x, y);
            sum += (channel < 0) ? (px[0] + px[1] + px[2]) / 3.f : px[channel];
            ++count;
        }
    }

    return count ? (float)(sum / count) : -1.f;
}

// The mean difference, in 8-bit sRGB levels, between two frames' colour channels.
double
meanLevelDifference(const ProgressiveRender::Frame& a,
                    const ProgressiveRender::Frame& b)
{
    if ((a.width != b.width) || (a.height != b.height) || a.rgba.empty() || (a.rgba.size() != b.rgba.size())) {
        return 255.;
    }
    double sum = 0.;
    std::size_t count = 0;
    for (std::size_t i = 0; i < a.rgba.size(); ++i) {
        if ((i % 4) != 3) {
            sum += std::abs(levelOf((float)a.rgba[i]) - levelOf((float)b.rgba[i]));
            ++count;
        }
    }

    return sum / count;
}

// A layer of grids meshes of size by size quads, children of parent, at most half a unit from
// its origin.
PXR_NS::SdfLayerRefPtr
heavyLayer(int grids,
           int size,
           const std::string& parent)
{
    PXR_NS::SdfLayerRefPtr layer = SceneOps::createNodeLayer("ProgressiveRenderTestHeavy");
    PXR_NS::VtIntArray counts((std::size_t)size * (std::size_t)size, 4);
    PXR_NS::VtIntArray indices;
    indices.reserve(counts.size() * 4);
    const int row = size + 1;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const int corner = y * row + x;
            indices.push_back(corner);
            indices.push_back(corner + 1);
            indices.push_back(corner + row + 1);
            indices.push_back(corner + row);
        }
    }
    for (int g = 0; g < grids; ++g) {
        const float z = (grids > 1) ? -0.5f + (float)g / (float)(grids - 1) : 0.f;
        PXR_NS::VtVec3fArray points;
        points.reserve((std::size_t)row * (std::size_t)row);
        for (int y = 0; y < row; ++y) {
            for (int x = 0; x < row; ++x) {
                points.push_back(PXR_NS::GfVec3f((float)x / (float)size - 0.5f, (float)y / (float)size - 0.5f, z));
            }
        }
        PXR_NS::SdfPrimSpecHandle prim = PXR_NS::SdfCreatePrimInLayer(layer, PXR_NS::SdfPath(parent + "/HeavyGrid_" + std::to_string(g)));
        prim->SetSpecifier(PXR_NS::SdfSpecifierDef);
        prim->SetTypeName("Mesh");
        PXR_NS::SdfAttributeSpec::New(prim, "points", PXR_NS::SdfValueTypeNames->Point3fArray)->SetDefaultValue(PXR_NS::VtValue(points));
        PXR_NS::SdfAttributeSpec::New(prim, "faceVertexCounts", PXR_NS::SdfValueTypeNames->IntArray)->SetDefaultValue(PXR_NS::VtValue(counts));
        PXR_NS::SdfAttributeSpec::New(prim, "faceVertexIndices", PXR_NS::SdfValueTypeNames->IntArray)->SetDefaultValue(PXR_NS::VtValue(indices));
        PXR_NS::SdfAttributeSpec::New(prim, "subdivisionScheme", PXR_NS::SdfValueTypeNames->Token, PXR_NS::SdfVariabilityUniform)->SetDefaultValue(PXR_NS::VtValue(PXR_NS::TfToken("none")));
    }

    return layer;
}

// base with grids meshes of size by size quads inside /World/Near, which hides them: a scene a
// delegate's first sync takes long enough on for an edit to come while it runs.
ScenePayload
heavyScene(const ScenePayload& base,
           int grids,
           int size)
{
    return SceneOps::appendLayer(base, heavyLayer(grids, size, "/World"), 0x4ea7700ULL);
}

// Writes to path, in place of what it held and in one step, a file of heavyLayer()'s grids under
// /World/Near with /World/Near moved back by back units: a reader of the file it replaces keeps
// reading that one.
bool
writeHeavyFile(const std::string& path,
               int grids,
               int size,
               double back)
{
    PXR_NS::SdfLayerRefPtr layer = heavyLayer(grids, size, "/World/Near");
    PXR_NS::SdfPrimSpecHandle prim = layer->GetPrimAtPath(PXR_NS::SdfPath("/World/Near"));
    PXR_NS::SdfAttributeSpecHandle translate = PXR_NS::SdfAttributeSpec::New(prim, "xformOp:translate", PXR_NS::SdfValueTypeNames->Double3);
    translate->SetDefaultValue(PXR_NS::VtValue(PXR_NS::GfVec3d(0., 0., -back)));
    PXR_NS::SdfAttributeSpecHandle order = PXR_NS::SdfAttributeSpec::New(prim, "xformOpOrder", PXR_NS::SdfValueTypeNames->TokenArray, PXR_NS::SdfVariabilityUniform);
    order->SetDefaultValue(PXR_NS::VtValue(PXR_NS::VtTokenArray(1, PXR_NS::TfToken("xformOp:translate"))));
    const std::string next = path.substr(0, path.size() - 5) + "-next.usdc";
    if (!layer->Export(next)) {
        ADD_FAILURE() << "cannot write " << next;

        return false;
    }
    if (std::rename(next.c_str(), path.c_str()) != 0) {
        ADD_FAILURE() << "cannot move " << next << " to " << path;

        return false;
    }

    return true;
}

bool
moonRayAvailable()
{
    return bool(PXR_NS::PlugRegistry::GetInstance().GetPluginWithName("hd_moonray"));
}

// The bundled gtest has no GTEST_SKIP, so the test returns early on false.
bool
moonRayOrSkip()
{
    if (moonRayAvailable()) {
        return true;
    }
    const char* reason = "OpenUSD has no hd_moonray plug-in registered; this build image predates the "
                         "MoonRay render delegate (tools/ci/local/build-moonray.sh), or the CPU lacks an instruction set MoonRay needs (MoonRayRuntime::cpuRunsMoonRay)";
    const char* require = std::getenv("NATRON_TEST_REQUIRE_MOONRAY");
    if (require && (std::strcmp(require, "1") == 0) && MoonRayRuntime::cpuRunsMoonRay()) {
        ADD_FAILURE() << "NATRON_TEST_REQUIRE_MOONRAY=1 but " << reason;
    } else {
        std::printf("[  SKIPPED ] %s\n", reason);
    }

    return false;
}

long long
millisecondsSince(const std::chrono::steady_clock::time_point& from)
{
    return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - from).count();
}
} // anon namespace

class ProgressiveRenderTest
    : public BaseTest {
protected:
    ProgressiveRenderTest()
        : _stage(new SceneStage)
    {
    }

    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        _watchdog.start();
        HydraRender::setRendererSettings(kRenderDelegateCyclesId, fixedSampling(kSamples));
    }

    virtual void TearDown() OVERRIDE
    {
        _watchdog.setPhase("teardown");
        ProgressiveRender::setPassHook(std::function<void()>());
        hold(false);
        _render.reset();
        // A worker still ending would hold a Cycles session of its thread count into the next test.
        EXPECT_TRUE(ProgressiveRender::waitForDetachedWorkers(kEndMs));
        _stage.reset();
        HydraRender::setRendererSettings(kRenderDelegateCyclesId, std::map<std::string, PXR_NS::VtValue>());
        BaseTest::TearDown();
        _watchdog.stop();
    }

    // Syncs payload into the stage under the stage lock, and restarts the render if that edited
    // it, as the viewer's sync does.
    void sync(const ScenePayload& payload)
    {
        _watchdog.setPhase("syncScene");
        bool edited = false;
        {
            std::lock_guard<std::mutex> lock(_stageLock);
            edited = _stage->sync(payload);
        }
        if (edited && _render) {
            _render->invalidate();
        }
    }

    // What a render takes the stage it renders from, as the viewer's does.
    std::function<bool(SceneStageSnapshot*)> snapshotOfStage()
    {
        SceneStage* stage = _stage.get();

        return [stage](SceneStageSnapshot* into) {
            *into = stage->getSnapshot();

            return true;
        };
    }

    // A render of the stage, with nothing set yet, whose listener is heard().
    void newRender()
    {
        hold(false);
        _listened = nullptr;
        _render.reset();
        // The worker of the one before could otherwise meet the fault hook a test sets next.
        EXPECT_TRUE(ProgressiveRender::waitForDetachedWorkers(kEndMs));
        {
            std::lock_guard<std::mutex> lock(_signalMutex);
            _published.clear();
            _calls = 0;
        }
        _render.reset(new ProgressiveRender(&_stageLock, snapshotOfStage(), [this]() { heard(); }));
        _listened = _render.get();
    }

    // Starts a render of fixture by renderer, Cycles unless given, through its camera, kSize
    // pixels square, with a render settings prim's settings.
    bool start(const std::map<std::string, PXR_NS::VtValue>& settings = std::map<std::string, PXR_NS::VtValue>(),
               const std::string& renderer = kRenderDelegateCyclesId,
               const char* fixture = kFixture)
    {
        if (!_stage->isValid()) {
            ADD_FAILURE() << "OpenUSD could not open a stage";

            return false;
        }
        _base = fixtureScene(fixture);
        sync(_base);
        PXR_NS::UsdGeomCamera camera(_stage->getStage()->GetPrimAtPath(PXR_NS::SdfPath(kCamera)));
        if (!camera) {
            ADD_FAILURE() << "the fixture has no " << kCamera;

            return false;
        }
        _view.frustum = camera.GetCamera(PXR_NS::UsdTimeCode::Default()).GetFrustum();
        _view.width = kSize;
        _view.height = kSize;
        _view.time = PXR_NS::UsdTimeCode::Default();
        newRender();
        _render->setSettings(settings);
        _render->setRenderer(renderer);
        _render->setThreads([]() { return 2; });
        _render->setView(_view);

        return true;
    }

    // The listener: records the frame just published, and waits while the render is held.
    void heard()
    {
        ProgressiveRender* render = _listened.load();
        const std::shared_ptr<const ProgressiveRender::Frame> frame = render ? render->getFrame() : std::shared_ptr<const ProgressiveRender::Frame>();
        std::unique_lock<std::mutex> lock(_signalMutex);

        if (frame && (_published.empty() || (_published.back() != frame))) {
            _published.push_back(frame);
        }
        ++_calls;
        if (_hold) {
            _held = true;
            _signal.notify_all();
            _signal.wait(lock, [this]() { return !_hold; });
            _held = false;
        }
        _signal.notify_all();
    }

    // While on, the worker stops in the listener after its next frame, without a lock of the
    // render's or the stage's, until it is off again.
    void hold(bool on)
    {
        std::lock_guard<std::mutex> lock(_signalMutex);

        _hold = on;
        _signal.notify_all();
    }

    std::size_t publishedCount()
    {
        std::lock_guard<std::mutex> lock(_signalMutex);

        return _published.size();
    }

    int calls()
    {
        std::lock_guard<std::mutex> lock(_signalMutex);

        return _calls;
    }

    // Waits, under _signalMutex, until reached() holds, reading it again at each call of the
    // listener and at least every 10 ms for what the listener is not told of. False, with a
    // failure, if it did not hold within kTimeout.
    bool waitUntil(const std::function<bool()>& reached,
                   const char* what)
    {
        _watchdog.setPhase("render");
        const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now() + kTimeout;
        std::unique_lock<std::mutex> lock(_signalMutex);

        while (!reached()) {
            if (std::chrono::steady_clock::now() >= end) {
                ADD_FAILURE() << what << " did not happen in " << kTimeout.count() << " s";

                return false;
            }
            _signal.wait_for(lock, std::chrono::milliseconds(10));
        }

        return true;
    }

    // Waits until the render is done or fails, and gives the frames published from the from-th
    // on. True if it finished.
    bool collectUntilDone(std::size_t from,
                          std::vector<std::shared_ptr<const ProgressiveRender::Frame>>* frames)
    {
        std::string error;
        const bool finished = waitUntil([this, &error]() {
            const ProgressiveRender::Status status = _render->getStatus();
            if (!status.error.empty()) {
                error = status.error;

                return true;
            }
            const std::shared_ptr<const ProgressiveRender::Frame> frame = _render->getFrame();

            return status.converged && frame && frame->converged && (frame->restart == status.restarts) && !_published.empty() && (_published.back() == frame);
        },
                                        "the end of the render");
        {
            std::lock_guard<std::mutex> lock(_signalMutex);
            frames->assign(_published.begin() + (std::ptrdiff_t)std::min(from, _published.size()), _published.end());
        }
        if (!error.empty()) {
            ADD_FAILURE() << error;

            return false;
        }

        return finished;
    }

    // The prims frame's pixels show, from the engine's primId AOV.
    std::set<std::string> pickedPrims(const ProgressiveRender::Frame& frame)
    {
        std::set<std::string> paths;
        for (int y = 0; y < frame.height; ++y) {
            for (int x = 0; x < frame.width; ++x) {
                const std::string path = _render->pick(frame, x, y);
                if (!path.empty()) {
                    paths.insert(path);
                }
            }
        }

        return paths;
    }

    // Waits for the render to finish from the mark-th frame on, and gives its last frame.
    std::shared_ptr<const ProgressiveRender::Frame> lastWhenDone(std::size_t mark)
    {
        std::vector<std::shared_ptr<const ProgressiveRender::Frame>> frames;
        if (!collectUntilDone(mark, &frames) || frames.empty()) {
            return std::shared_ptr<const ProgressiveRender::Frame>();
        }

        return frames.back();
    }

    /**
     * With the key light the only one and its row Hidden, renderer lights the near cube from the
     * camera as Storm's viewer would: lit, white, and brightest where it faces the camera however
     * the camera turns, which a renderer's own uniform default light is not. Showing the key again,
     * or Render Everything, takes the headlight away without rebuilding the engine.
     **/
    void checkHeadlight(const char* renderer)
    {
        ASSERT_TRUE(start(std::map<std::string, PXR_NS::VtValue>(), renderer));
        if (std::string(renderer) != kRenderDelegateCyclesId) {
            // MoonRay's cap is its pixel_samples, the square root of the samples per pixel: 8 takes 64.
            _render->setMaxSamples(8);
        }
        SceneDrawFilter::Rules rules;
        rules[PXR_NS::SdfPath("/World/Far")] = SceneDrawFilter::eRuleHidden;
        _render->setDrawRules(rules);
        sync(withKey(_base));
        const std::shared_ptr<const ProgressiveRender::Frame> keyOnly = lastWhenDone(0);
        ASSERT_TRUE(bool(keyOnly));
        const int c = kSize / 2;
        const auto onNear = [this](const ProgressiveRender::Frame& frame) {
            return [this, &frame](int x, int y) { return _render->pick(frame, x, y).compare(0, 11, "/World/Near") == 0; };
        };
        const float keyRed = meanOver(*keyOnly, c - 3, c + 3, c - 3, c + 3, 0, onNear(*keyOnly));
        const float keyGreen = meanOver(*keyOnly, c - 3, c + 3, c - 3, c + 3, 1, onNear(*keyOnly));
        ASSERT_GT(keyRed, 0.02f) << "the key light does not light the near cube";
        ASSERT_LT(keyGreen, 0.1f * keyRed) << "something other than the red key lights the near cube";
        const U64 builds = HydraRender::getEngineBuildCount();
        const U64 remakesBefore = _render->getStatus().unseenRemakes;

        rules[PXR_NS::SdfPath(kKey)] = SceneDrawFilter::eRuleHidden;
        _render->setDrawRules(rules);
        const std::shared_ptr<const ProgressiveRender::Frame> facing = lastWhenDone(publishedCount());
        ASSERT_TRUE(bool(facing));
        const float front = meanOver(*facing, c - 3, c + 3, c - 3, c + 3, -1, onNear(*facing));
        EXPECT_GT(front, 0.02f) << "nothing lights the near cube with every light hidden";
        EXPECT_GT(meanOver(*facing, c - 3, c + 3, c - 3, c + 3, 1, onNear(*facing)), 0.01f) << "the light on the near cube is not white";
        // A renderer's own default light, such as hdMoonray's dome, would show in the background.
        EXPECT_NEAR(meanOver(*keyOnly, 0, 4, 0, 4, -1, std::function<bool(int, int)>()),
                    meanOver(*facing, 0, 4, 0, 4, -1, std::function<bool(int, int)>()), 0.01f);

        // Turned 45 degrees about the cube, the camera sees two faces at the same angle to it.
        ProgressiveRender::View turned = _view;
        const double radius = _view.frustum.GetPosition().GetLength();
        const double s45 = std::sqrt(0.5);
        PXR_NS::GfMatrix4d cameraToWorld;
        cameraToWorld.SetTransform(PXR_NS::GfRotation(PXR_NS::GfVec3d(0., 1., 0.), 45.), PXR_NS::GfVec3d(radius * s45, 0., radius * s45));
        turned.frustum.SetPositionAndRotationFromMatrix(cameraToWorld);
        _render->setView(turned);
        const std::shared_ptr<const ProgressiveRender::Frame> corner = lastWhenDone(publishedCount());
        ASSERT_TRUE(bool(corner));
        const float left = meanOver(*corner, 0, c - 1, c - 3, c + 3, -1, onNear(*corner));
        const float right = meanOver(*corner, c + 1, kSize, c - 3, c + 3, -1, onNear(*corner));
        ASSERT_GT(left, 0.f) << "the turned view shows one face of the near cube";
        ASSERT_GT(right, 0.f) << "the turned view shows one face of the near cube";
        EXPECT_NEAR(left, right, 0.15f * front) << "the headlight does not turn with the camera";
        EXPECT_GT(front, 1.15f * std::max(left, right)) << "the near cube is lit evenly, not from the camera";

        _render->setView(_view);
        rules.erase(PXR_NS::SdfPath(kKey));
        _render->setDrawRules(rules);
        const std::shared_ptr<const ProgressiveRender::Frame> keyAgain = lastWhenDone(publishedCount());
        ASSERT_TRUE(bool(keyAgain));
        EXPECT_LT(meanLevelDifference(*keyOnly, *keyAgain), 1.) << "the headlight stays with the key light shown";
        EXPECT_NEAR(keyGreen, meanOver(*keyAgain, c - 3, c + 3, c - 3, c + 3, 1, onNear(*keyAgain)), 0.01f);

        rules[PXR_NS::SdfPath(kKey)] = SceneDrawFilter::eRuleHidden;
        _render->setDrawRules(rules);
        ASSERT_TRUE(bool(lastWhenDone(publishedCount())));
        _render->setRenderEverything(true);
        const std::shared_ptr<const ProgressiveRender::Frame> everything = lastWhenDone(publishedCount());
        ASSERT_TRUE(bool(everything));
        const float everythingRed = meanOver(*everything, c - 3, c + 3, c - 3, c + 3, 0, onNear(*everything));
        EXPECT_GT(everythingRed, 0.02f);
        EXPECT_LT(meanOver(*everything, c - 3, c + 3, c - 3, c + 3, 1, onNear(*everything)), 0.1f * everythingRed)
            << "the headlight stays with Render Everything on";
        // Less the engines the worker makes again when a delegate is slow to show a restart.
        EXPECT_EQ(builds + (_render->getStatus().unseenRemakes - remakesBefore), HydraRender::getEngineBuildCount());
    }

    // Waits for the render to finish from the mark-th frame on, and gives its last frame's prims.
    bool pickedWhenDone(std::size_t mark,
                        std::set<std::string>* paths)
    {
        std::vector<std::shared_ptr<const ProgressiveRender::Frame>> frames;
        if (!collectUntilDone(mark, &frames) || frames.empty()) {
            return false;
        }
        *paths = pickedPrims(*frames.back());

        return true;
    }

    // Sets settings, which makes the engine again, and returns once its first pass over the heavy
    // scene has been under way for a while and has published nothing yet. False, with a failure,
    // if no such pass began or it ended first.
    bool heavyPass(const std::map<std::string, PXR_NS::VtValue>& settings)
    {
        std::shared_ptr<std::atomic<std::uint64_t>> seen = std::make_shared<std::atomic<std::uint64_t>>(0);
        const std::uint64_t builds = HydraRender::getEngineBuildCount();
        ProgressiveRender::setPassHook([seen]() { *seen = HydraRender::getEngineBuildCount(); });
        _render->setSettings(settings);
        const bool began = waitUntil([seen, builds]() { return seen->load() > builds; }, "a pass of a new engine");
        ProgressiveRender::setPassHook(std::function<void()>());
        if (!began) {
            return false;
        }
        // The new engine publishes only once its Render() returns, which the hook precedes.
        const std::size_t mark = publishedCount();
        // Into the delegate's sync of the heavy scene, past the hook.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (publishedCount() != mark) {
            ADD_FAILURE() << "the new engine's pass over the heavy scene ended before the change it was meant to interrupt";

            return false;
        }

        return true;
    }

    // A settings change, a change of renderer to other and the render's destruction, each made
    // while renderer is in its first pass over a heavy scene, return on this thread within 50 ms,
    // and the frames that follow the first two are of the change.
    void interruptsMidPass(const std::string& renderer,
                           const std::string& samplesKey,
                           const std::string& other)
    {
        const long long kReturnMs = 50;
        const int liveBefore = ProgressiveRender::getLiveCount();
        std::map<std::string, PXR_NS::VtValue> settings;
        settings[samplesKey] = PXR_NS::VtValue(2);
        ASSERT_TRUE(start(settings, renderer));
        sync(heavyScene(_base, 24, 160));

        settings[samplesKey] = PXR_NS::VtValue(64);
        ASSERT_TRUE(heavyPass(settings));
        settings[samplesKey] = PXR_NS::VtValue(3);
        std::chrono::steady_clock::time_point asked = std::chrono::steady_clock::now();
        std::size_t mark = publishedCount();
        _render->setSettings(settings);
        EXPECT_LT(millisecondsSince(asked), kReturnMs) << "a settings change waited for the pass";
        std::vector<std::shared_ptr<const ProgressiveRender::Frame>> frames;
        ASSERT_TRUE(collectUntilDone(mark, &frames));
        ASSERT_FALSE(frames.empty());
        EXPECT_EQ(3, _render->getMaxSamples());
        EXPECT_EQ(3, frames.back()->maxSamples);

        settings[samplesKey] = PXR_NS::VtValue(4);
        ASSERT_TRUE(heavyPass(settings));
        asked = std::chrono::steady_clock::now();
        _render->setRenderer(other);
        EXPECT_LT(millisecondsSince(asked), kReturnMs) << "a renderer change waited for the pass";
        std::printf("[          ] the renderer change mid-pass went to %s\n",
                    other.empty() ? "no renderer: NOT TESTED, no second delegate is installed, so this only checked going idle" : other.c_str());
        if (other.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            const ProgressiveRender::Status status = _render->getStatus();
            EXPECT_FALSE(status.active);
            EXPECT_TRUE(status.error.empty()) << status.error;
            EXPECT_TRUE(_render->getFrame() == nullptr) << "a frame of the renderer left was shown";
        } else {
            std::string error;
            EXPECT_TRUE(waitUntil([this, &error]() {
                const ProgressiveRender::Status status = _render->getStatus();
                error = status.error;
                const std::shared_ptr<const ProgressiveRender::Frame> frame = _render->getFrame();

                return !error.empty() || (frame && (frame->restart == status.restarts));
            },
                                  "a frame of the other renderer"));
            EXPECT_TRUE(error.empty()) << error;
        }

        _render->setRenderer(renderer);
        settings[samplesKey] = PXR_NS::VtValue(5);
        ASSERT_TRUE(heavyPass(settings));
        asked = std::chrono::steady_clock::now();
        _listened = nullptr;
        _render.reset();
        EXPECT_LT(millisecondsSince(asked), kReturnMs) << "destroying the render waited for the pass";
        EXPECT_TRUE(ProgressiveRender::waitForDetachedWorkers(kEndMs));
        EXPECT_EQ(liveBefore, ProgressiveRender::getLiveCount());
    }

    std::mutex _stageLock;
    std::unique_ptr<SceneStage> _stage;
    TestWatchdog _watchdog;
    std::unique_ptr<ProgressiveRender> _render;
    std::atomic<ProgressiveRender*> _listened { nullptr };
    ProgressiveRender::View _view;
    ScenePayload _base;

    // Guards what the listener records, and the hold.
    std::mutex _signalMutex;
    std::condition_variable _signal;
    std::vector<std::shared_ptr<const ProgressiveRender::Frame>> _published;
    int _calls = 0;
    bool _hold = false;
    bool _held = false;
};

// The render refines in the background: each frame published has at least the samples of the one
// before, the last has the cap and is converged, and nothing is published once it is done.
TEST_F(ProgressiveRenderTest, RefinesToTheCapAndStops)
{
    if (!cyclesOrSkip()) {
        return;
    }
    const int liveBefore = ProgressiveRender::getLiveCount();
    ASSERT_TRUE(start());
    std::vector<std::shared_ptr<const ProgressiveRender::Frame>> frames;
    ASSERT_TRUE(collectUntilDone(0, &frames));
    ASSERT_FALSE(frames.empty());

    EXPECT_EQ(kSamples, _render->getMaxSamples());
    std::cout << "[   NOTE   ] samples per frame:";
    for (std::size_t i = 0; i < frames.size(); ++i) {
        std::cout << " " << frames[i]->samples;
        EXPECT_GT(frames[i]->samples, 0) << "frame " << i;
        EXPECT_EQ(kSize, frames[i]->width);
        EXPECT_EQ(kSize, frames[i]->height);
        EXPECT_EQ((std::size_t)kSize * kSize * 4, frames[i]->rgba.size());
        EXPECT_EQ((std::size_t)kSize * kSize, frames[i]->depth.size());
        EXPECT_EQ(U64(1), frames[i]->restart);
        if (i > 0) {
            EXPECT_GE(frames[i]->samples, frames[i - 1]->samples) << "frame " << i;
            EXPECT_GT(frames[i]->serial, frames[i - 1]->serial);
        }
        if (i + 1 < frames.size()) {
            EXPECT_FALSE(frames[i]->converged) << "frame " << i;
        }
    }
    std::cout << std::endl;
    const ProgressiveRender::Frame& last = *frames.back();
    EXPECT_EQ(kSamples, last.samples);
    EXPECT_EQ(kSamples, last.maxSamples);
    EXPECT_TRUE(last.converged);
    // The centre shows the near cube, in front of the far plane, and is lit.
    EXPECT_NEAR(windowDepth(_view.frustum, kNearDistance), centreDepth(last), 1e-4f);
    const PXR_NS::GfVec4f centre = pixelOf(last, kSize / 2, kSize / 2);
    EXPECT_GT(centre[0] + centre[1] + centre[2], 0.02f);
    EXPECT_NEAR(1.f, centre[3], 1e-3f);

    const ProgressiveRender::Status status = _render->getStatus();
    EXPECT_TRUE(status.active);
    EXPECT_TRUE(status.converged);
    EXPECT_EQ(kSamples, status.samples);
    EXPECT_EQ(U64(1), status.restarts);
    // Nothing comes later: a frame would, were the render still under way.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    EXPECT_EQ(status.frames, _render->getStatus().frames) << "frames were published after the render was done";
    EXPECT_EQ(last.serial, _render->getFrame()->serial);

    EXPECT_EQ(liveBefore + 1, ProgressiveRender::getLiveCount());
    _render.reset();
    EXPECT_TRUE(ProgressiveRender::waitForDetachedWorkers(kEndMs));
    EXPECT_EQ(liveBefore, ProgressiveRender::getLiveCount());
}

// A frame carries the colour AOV as the delegate rendered it: an emissive card at 4 reads about 4
// at the centre with alpha 1, unclamped and with no background under it, and a pixel nothing was
// rendered to is transparent black.
TEST_F(ProgressiveRenderTest, FramesAreSceneLinearWithTheirAlpha)
{
    if (!cyclesOrSkip()) {
        return;
    }
    ASSERT_TRUE(start(std::map<std::string, PXR_NS::VtValue>(), kRenderDelegateCyclesId, kEmissionFixture));
    sync(emittingGrey(_base, 4.f, 0x4e3a11ULL));
    std::vector<std::shared_ptr<const ProgressiveRender::Frame>> frames;
    ASSERT_TRUE(collectUntilDone(0, &frames));
    ASSERT_FALSE(frames.empty());
    const ProgressiveRender::Frame& last = *frames.back();
    ASSERT_EQ((std::size_t)kSize * kSize * 4, last.rgba.size());

    const PXR_NS::GfVec4f centre = pixelOf(last, kSize / 2, kSize / 2);
    for (int c = 0; c < 3; ++c) {
        EXPECT_NEAR(4.f, centre[c], 0.4f) << "channel " << c;
    }
    EXPECT_NEAR(1.f, centre[3], 1e-3f);
    const PXR_NS::GfVec4f corner = pixelOf(last, 0, 0);
    EXPECT_NEAR(0.f, corner[3], 1e-3f);
    for (int c = 0; c < 3; ++c) {
        EXPECT_NEAR(0.f, corner[c], 1e-3f) << "channel " << c;
    }
}

// A dome the camera sees where nothing was rendered reaches the frame as light, whatever alpha the
// delegate gives it there: the viewer's grade adds light at alpha 0 to the background.
TEST_F(ProgressiveRenderTest, ADomeSeenWhereNothingCoversReachesTheFrame)
{
    if (!cyclesOrSkip()) {
        return;
    }
    ASSERT_TRUE(start(std::map<std::string, PXR_NS::VtValue>(), kRenderDelegateCyclesId, kEmissionFixture));
    sync(withDome(emittingGrey(_base, 4.f, 0x4e3a12ULL)));
    std::vector<std::shared_ptr<const ProgressiveRender::Frame>> frames;
    ASSERT_TRUE(collectUntilDone(0, &frames));
    ASSERT_FALSE(frames.empty());
    const PXR_NS::GfVec4f corner = pixelOf(*frames.back(), 0, 0);
    std::printf("[          ] the corner under the dome: %g %g %g, alpha %g\n", corner[0], corner[1], corner[2], corner[3]);
    for (int c = 0; c < 3; ++c) {
        EXPECT_GT(corner[c], 0.1f) << "channel " << c << ": the dome does not reach the frame";
    }
}

// The headlight lights the stage from the camera in place of its lights: a view asking for it
// makes the engine again, changes the lit cube's colour and the status says the engine has it,
// and a view without it makes the engine again and brings the colour back.
TEST_F(ProgressiveRenderTest, TheHeadlightLightsFromTheCameraAndTheStatusSaysSo)
{
    if (!cyclesOrSkip()) {
        return;
    }
    ASSERT_TRUE(start());
    std::vector<std::shared_ptr<const ProgressiveRender::Frame>> frames;
    ASSERT_TRUE(collectUntilDone(0, &frames));
    ASSERT_FALSE(frames.empty());
    const float sceneLit = centreRed(*frames.back());
    EXPECT_FALSE(_render->getStatus().headlightAsked);
    EXPECT_FALSE(_render->getStatus().headlight);

    std::uint64_t builds = HydraRender::getEngineBuildCount();
    std::size_t mark = publishedCount();
    ProgressiveRender::View lit = _view;
    lit.headlight = true;
    _render->setView(lit);
    frames.clear();
    ASSERT_TRUE(collectUntilDone(mark, &frames));
    ASSERT_FALSE(frames.empty());
    const float headlit = centreRed(*frames.back());
    std::printf("[          ] the cube's red: %g by the stage's lights, %g by the headlight\n", sceneLit, headlit);
    EXPECT_GT(std::fabs(headlit - sceneLit), 0.1f * std::max(sceneLit, headlit)) << "the headlight changed nothing";
    EXPECT_TRUE(_render->getStatus().headlightAsked);
    EXPECT_TRUE(_render->getStatus().headlight);
    EXPECT_GT(HydraRender::getEngineBuildCount(), builds) << "the headlight did not make the engine again";

    builds = HydraRender::getEngineBuildCount();
    mark = publishedCount();
    _render->setView(_view);
    frames.clear();
    ASSERT_TRUE(collectUntilDone(mark, &frames));
    ASSERT_FALSE(frames.empty());
    EXPECT_GT(HydraRender::getEngineBuildCount(), builds) << "turning the headlight off did not make the engine again";
    const float back = centreRed(*frames.back());
    EXPECT_NEAR(sceneLit, back, 0.1f * std::max(sceneLit, 0.01f));
    EXPECT_FALSE(_render->getStatus().headlightAsked);
}

// A finished render's worker woken asks for its Cycles thread count without the render's lock. A
// render destroyed while it asks still ends its worker: the destructor's wake-up comes before the
// worker waits again.
TEST_F(ProgressiveRenderTest, ARenderEndedWhileItsWorkerAsksForThreadsEndsItsWorker)
{
    if (!cyclesOrSkip()) {
        return;
    }
    ASSERT_TRUE(start());
    std::vector<std::shared_ptr<const ProgressiveRender::Frame>> frames;
    ASSERT_TRUE(collectUntilDone(0, &frames));

    // Shared, as the worker holds the threads function past the test's end if it does not end.
    struct Gate {
        std::mutex mutex;
        std::condition_variable changed;
        bool armed = true;
        bool asking = false;
        bool released = false;
    };
    const std::shared_ptr<Gate> gate = std::make_shared<Gate>();
    const std::function<int()> threads = [gate]() {
        std::unique_lock<std::mutex> lock(gate->mutex);
        if (gate->armed) {
            gate->armed = false;
            gate->asking = true;
            gate->changed.notify_all();
            gate->changed.wait(lock, [&gate]() { return gate->released; });
        }

        return 2;
    };
    // setThreads wakes the worker; one woken before it waits again would miss a single call.
    const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now() + kTimeout;
    bool asking = false;
    while (!asking && (std::chrono::steady_clock::now() < end)) {
        _render->setThreads(threads);
        std::unique_lock<std::mutex> lock(gate->mutex);
        asking = gate->changed.wait_for(lock, std::chrono::milliseconds(50), [&gate]() { return gate->asking; });
    }
    ASSERT_TRUE(asking) << "the worker never asked for its threads";

    _listened = nullptr;
    ProgressiveRender* ending = _render.release();
    std::atomic<long> enderTid(0);
    std::thread ender([ending, &enderTid]() {
        enderTid.store(static_cast<long>(syscall(SYS_gettid)));
        delete ending;
    });
    // The destructor sets quit at once, then waits for the threads function to return. Released
    // early, the worker is waiting again when quit comes and the test passes without the race.
    // Nothing else holds a lock the ender takes, so once it sleeps it waits on the threads
    // function, past setting quit.
    const std::chrono::steady_clock::time_point waitEnd = std::chrono::steady_clock::now() + kTimeout;
    bool enderWaits = false;
    while (!enderWaits && (std::chrono::steady_clock::now() < waitEnd)) {
        const long tid = enderTid.load();
        if (tid != 0) {
            std::ifstream stat("/proc/self/task/" + std::to_string(tid) + "/stat");
            std::string line;
            if (std::getline(stat, line)) {
                const std::string::size_type close = line.rfind(')');
                enderWaits = (close != std::string::npos) && (close + 2 < line.size()) && (line[close + 2] == 'S');
            }
        }
        if (!enderWaits) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    EXPECT_TRUE(enderWaits) << "the destructor never waited for the threads function";
    {
        std::lock_guard<std::mutex> lock(gate->mutex);
        gate->released = true;
    }
    gate->changed.notify_all();
    ender.join();
    EXPECT_TRUE(ProgressiveRender::waitForDetachedWorkers(kEndMs));
}

// Ten edits while the render refines restart it once, and no frame of the scene before them is
// published after them: every later frame shows the near cube where the last edit put it. The
// render is held in its listener while the edits come, so that it cannot restart between them.
TEST_F(ProgressiveRenderTest, CoalescesEditsIntoOneRestart)
{
    if (!cyclesOrSkip()) {
        return;
    }
    ASSERT_TRUE(start());
    std::vector<std::shared_ptr<const ProgressiveRender::Frame>> frames;
    ASSERT_TRUE(collectUntilDone(0, &frames));
    const U64 restartsBefore = _render->getRestartCount();
    // A render under way, as when edits come while it refines: held at its first frame.
    hold(true);
    const std::size_t mark = publishedCount();
    _render->setMaxSamples(kSamples * 2);
    ASSERT_TRUE(waitUntil([this]() { return _held; }, "a frame of the raised cap"));
    const std::shared_ptr<const ProgressiveRender::Frame> held = _render->getFrame();
    ASSERT_TRUE(held != nullptr);
    const ProgressiveRender::Status refining = _render->getStatus();
    const U64 restartsRefining = refining.restarts;
    EXPECT_GT(restartsRefining, restartsBefore);
    EXPECT_EQ(restartsRefining, held->restart);

    const int kEdits = 10;
    const double kLastBack = 5.;
    for (int i = 1; i <= kEdits; ++i) {
        sync(movedNear(_base, kLastBack * i / kEdits, 0xed17000ULL + (U64)i));
    }
    hold(false);
    std::vector<std::shared_ptr<const ProgressiveRender::Frame>> after;
    ASSERT_TRUE(collectUntilDone(mark, &after));
    ASSERT_FALSE(after.empty());
    EXPECT_EQ(held, after.front());

    const ProgressiveRender::Status end = _render->getStatus();
    const U64 restartsAfter = end.restarts;
    // The worker makes its engine again when Cycles is slow to show the edits' restart, a
    // restart of its own that no edit caused.
    const U64 remade = end.engines - refining.engines;
    EXPECT_EQ(restartsRefining + 1 + remade, restartsAfter) << remade << " engine(s) made after the edits";
    const float moved = windowDepth(_view.frustum, kNearDistance + kLastBack);
    const float before = windowDepth(_view.frustum, kNearDistance);
    for (const std::shared_ptr<const ProgressiveRender::Frame>& frame : frames) {
        EXPECT_NEAR(before, centreDepth(*frame), 1e-4f) << "frame " << frame->serial;
    }
    EXPECT_NEAR(before, centreDepth(*held), 1e-4f) << "frame " << held->serial;
    U64 lastRestart = restartsRefining + 1;
    for (const std::shared_ptr<const ProgressiveRender::Frame>& frame : after) {
        if (frame->serial <= held->serial) {
            continue;
        }
        EXPECT_LE(lastRestart, frame->restart) << "frame " << frame->serial;
        EXPECT_GE(restartsAfter, frame->restart) << "frame " << frame->serial;
        lastRestart = frame->restart;
        EXPECT_NEAR(moved, centreDepth(*frame), 1e-4f) << "frame " << frame->serial << " (restart " << frame->restart << ") is stale";
    }
    EXPECT_EQ(kSamples * 2, after.back()->samples);
}

// Stopping pauses the delegate where it is; resuming continues from its samples, without a
// restart.
TEST_F(ProgressiveRenderTest, StopKeepsTheSamplesAndResumeContinues)
{
    if (!cyclesOrSkip()) {
        return;
    }
    HydraRender::setRendererSettings(kRenderDelegateCyclesId, fixedSampling(1000000));
    ASSERT_TRUE(start());
    ASSERT_TRUE(waitUntil([this]() {
        const ProgressiveRender::Status status = _render->getStatus();

        return !status.error.empty() || (status.samples >= 2);
    },
                          "a second sample"));
    ASSERT_TRUE(_render->getStatus().error.empty()) << _render->getStatus().error;
    _render->stop();
    ASSERT_TRUE(waitUntil([this]() { return _render->getStatus().paused; }, "the pause"));
    const ProgressiveRender::Status stopped = _render->getStatus();
    EXPECT_TRUE(stopped.stopped);
    // Nothing moves while it is paused.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const ProgressiveRender::Status still = _render->getStatus();
    EXPECT_TRUE(still.paused);
    EXPECT_EQ(stopped.samples, still.samples);
    EXPECT_EQ(stopped.frames, still.frames);

    _render->resume();
    ASSERT_TRUE(waitUntil([this, &still]() {
        const ProgressiveRender::Status status = _render->getStatus();

        return !status.error.empty() || (!status.paused && (status.samples > still.samples));
    },
                          "a sample after the resume"));
    const ProgressiveRender::Status resumed = _render->getStatus();
    EXPECT_TRUE(resumed.error.empty()) << resumed.error;
    EXPECT_FALSE(resumed.stopped);
    EXPECT_GT(resumed.samples, still.samples);
    EXPECT_EQ(still.restarts, resumed.restarts);
}

// A delegate that cannot start fails the render: the status says why, and the listener is called,
// so that a viewer waiting for a frame repaints to show the error.
TEST_F(ProgressiveRenderTest, AFailureReachesTheListener)
{
    if (!cyclesOrSkip()) {
        return;
    }
    ASSERT_TRUE(start());
    newRender();
    _render->setRenderer("NatronNoSuchRenderDelegate");
    _render->setView(_view);
    EXPECT_TRUE(waitUntil([this]() { return _calls > 0; }, "a call of the listener"));
    const ProgressiveRender::Status status = _render->getStatus();
    EXPECT_FALSE(status.error.empty());
    EXPECT_FALSE(status.active);
    EXPECT_TRUE(_render->getFrame() == nullptr);
    // It does not retry: no further call comes.
    const int after = calls();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(after, calls());
    _render.reset();
}

// A delegate can throw from any call on the worker, not only Render(): the render fails, the process
// lives, nothing retries in a loop, and an edit starts it again. threads is the one thing a test can
// make throw there, both before there is an engine and with one rendering.
TEST_F(ProgressiveRenderTest, AThrowOutsideTheRenderFailsItAndAnEditRecovers)
{
    if (!cyclesOrSkip()) {
        return;
    }
    ASSERT_TRUE(start());
    std::atomic<bool> throwing(true);
    newRender();
    _render->setThreads([&throwing]() -> int {
        if (throwing.load()) {
            throw std::runtime_error("delegate broke");
        }

        return 2;
    });
    _render->setRenderer(kRenderDelegateCyclesId);
    _render->setView(_view);
    ASSERT_TRUE(waitUntil([this]() { return _calls > 0; }, "a call of the listener"));
    EXPECT_NE(std::string::npos, _render->getStatus().error.find("delegate broke")) << _render->getStatus().error;
    EXPECT_FALSE(_render->getStatus().active);
    const int after = calls();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(after, calls());

    throwing = false;
    const std::size_t mark = publishedCount();
    _render->invalidate();
    EXPECT_TRUE(waitUntil([this]() { return _render->getStatus().error.empty(); }, "the recovery"));
    std::vector<std::shared_ptr<const ProgressiveRender::Frame>> frames;
    EXPECT_TRUE(collectUntilDone(mark, &frames));

    throwing = true;
    _render->invalidate();
    EXPECT_TRUE(waitUntil([this]() { return !_render->getStatus().error.empty(); }, "the second failure"));
    EXPECT_NE(std::string::npos, _render->getStatus().error.find("delegate broke")) << _render->getStatus().error;
    EXPECT_FALSE(_render->getStatus().active);
    throwing = false;
    _render.reset();
}

// The pass that fails is edited while it runs: its failure still counts, though what it rendered
// would be stale. The edit is a move of the view, after which the failure stays.
TEST_F(ProgressiveRenderTest, AnEditDuringAFailingPassDoesNotHideTheFailure)
{
    if (!cyclesOrSkip()) {
        return;
    }
    ASSERT_TRUE(start());
    std::atomic<bool> armed(true);
    newRender();
    ProgressiveRender::View moved = _view;
    moved.width += 2;
    RenderDelegates::setRenderFaultHook([this, &armed, moved]() {
        if (armed.exchange(false)) {
            _render->setView(moved);
            throw std::runtime_error("texture commit failed");
        }
    });
    _render->setRenderer(kRenderDelegateCyclesId);
    _render->setThreads([]() { return 2; });
    _render->setView(_view);
    EXPECT_TRUE(waitUntil([this]() { return _calls > 0; }, "a call of the listener"));
    EXPECT_FALSE(armed.load());
    EXPECT_NE(std::string::npos, _render->getStatus().error.find("texture commit failed")) << _render->getStatus().error;
    _render.reset();
    // The hook reads this test's locals, which a worker still ending could call it on.
    EXPECT_TRUE(ProgressiveRender::waitForDetachedWorkers(kEndMs));
    RenderDelegates::setRenderFaultHook(nullptr);
}

// One throw does not stop the rendering for good: the next scene or settings edit builds an engine
// that renders, while moving the view, which cannot be what the delegate failed on, does not.
TEST_F(ProgressiveRenderTest, AFailureRetriesAtTheNextSceneEditAndNotAtAMove)
{
    if (!cyclesOrSkip()) {
        return;
    }
    ASSERT_TRUE(start());
    std::atomic<bool> armed(true);
    RenderDelegates::setRenderFaultHook([&armed]() {
        if (armed.exchange(false)) {
            throw std::runtime_error("texture commit failed");
        }
    });
    newRender();
    _render->setRenderer(kRenderDelegateCyclesId);
    _render->setThreads([]() { return 2; });
    _render->setView(_view);
    ASSERT_TRUE(waitUntil([this]() { return _calls > 0; }, "a call of the listener"));
    EXPECT_FALSE(_render->getStatus().error.empty());
    const std::uint64_t builds = HydraRender::getEngineBuildCount();

    ProgressiveRender::View moved = _view;
    moved.width += 2;
    _render->setView(moved);
    // No engine is built for the move.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(builds, HydraRender::getEngineBuildCount());
    EXPECT_FALSE(_render->getStatus().error.empty());

    _render->setView(_view);
    const std::size_t mark = publishedCount();
    _render->invalidate();
    EXPECT_TRUE(waitUntil([this]() { return _render->getStatus().error.empty(); }, "the retry"));
    std::vector<std::shared_ptr<const ProgressiveRender::Frame>> frames;
    EXPECT_TRUE(collectUntilDone(mark, &frames));
    EXPECT_FALSE(frames.empty());
    EXPECT_GT(HydraRender::getEngineBuildCount(), builds);
    _render.reset();
    // The hook reads this test's locals, which a worker still ending could call it on.
    EXPECT_TRUE(ProgressiveRender::waitForDetachedWorkers(kEndMs));
    RenderDelegates::setRenderFaultHook(nullptr);
}

// An exception out of Hydra's Render() on the worker fails the render as one that cannot start,
// and choosing the delegate again builds a fresh engine that renders.
TEST_F(ProgressiveRenderTest, AnExceptionFromHydraFailsTheRenderAndALaterRenderRecovers)
{
    if (!cyclesOrSkip()) {
        return;
    }
    ASSERT_TRUE(start());
    RenderDelegates::setRenderFaultHook([]() { throw std::runtime_error("texture commit failed"); });
    newRender();
    _render->setRenderer(kRenderDelegateCyclesId);
    _render->setThreads([]() { return 2; });
    _render->setView(_view);
    EXPECT_TRUE(waitUntil([this]() { return _calls > 0; }, "a call of the listener"));
    RenderDelegates::setRenderFaultHook(nullptr);
    const ProgressiveRender::Status failed = _render->getStatus();
    EXPECT_NE(std::string::npos, failed.error.find("texture commit failed")) << failed.error;
    EXPECT_FALSE(failed.active);

    const std::size_t mark = publishedCount();
    _render->setRenderer(std::string());
    _render->setRenderer(kRenderDelegateCyclesId);
    std::vector<std::shared_ptr<const ProgressiveRender::Frame>> frames;
    EXPECT_TRUE(collectUntilDone(mark, &frames));
    EXPECT_FALSE(frames.empty());
    _render.reset();
}

// Every Cycles session of the process shares one thread count, the one asked for last: a second
// viewer asking for another has the first one's engine made again under it, and both refine.
TEST_F(ProgressiveRenderTest, CyclesSessionsShareTheLastCountAskedFor)
{
    if (!cyclesOrSkip()) {
        return;
    }
    ASSERT_TRUE(start());
    std::vector<std::shared_ptr<const ProgressiveRender::Frame>> frames;
    ASSERT_TRUE(collectUntilDone(0, &frames));
    EXPECT_EQ(2, HydraRender::getCyclesThreads());
    const U64 restartsBefore = _render->getRestartCount();
    const std::uint64_t buildsBefore = HydraRender::getEngineBuildCount();

    std::unique_ptr<ProgressiveRender> second(new ProgressiveRender(&_stageLock, snapshotOfStage(), std::function<void()>()));
    second->setRenderer(kRenderDelegateCyclesId);
    second->setThreads([]() { return 3; });
    second->setView(_view);
    std::string error;
    ASSERT_TRUE(waitUntil([this, &second, &error, restartsBefore]() {
        const ProgressiveRender::Status a = _render->getStatus();
        const ProgressiveRender::Status b = second->getStatus();
        if (!a.error.empty() || !b.error.empty()) {
            error = a.error + b.error;

            return true;
        }
        const std::shared_ptr<const ProgressiveRender::Frame> frameA = _render->getFrame();
        const std::shared_ptr<const ProgressiveRender::Frame> frameB = second->getFrame();
        const bool doneA = a.converged && frameA && frameA->converged && (frameA->restart == a.restarts) && (a.restarts > restartsBefore);
        const bool doneB = b.converged && frameB && frameB->converged && (frameB->restart == b.restarts);

        return doneA && doneB;
    },
                          "the end of both renders"));
    ASSERT_TRUE(error.empty()) << error;
    EXPECT_EQ(3, HydraRender::getCyclesThreads());
    EXPECT_GE(HydraRender::getEngineBuildCount(), buildsBefore + 2) << "the first viewer's engine was not made again";
    EXPECT_EQ(kSamples, _render->getFrame()->samples);
    EXPECT_EQ(kSamples, second->getFrame()->samples);
    second.reset();
}

// A render settings prim's settings are set on the engine: its cycles:samples is the cap the
// render refines to, a sample cap set applies over it, the same settings again restart nothing,
// and others make the engine again under them.
TEST_F(ProgressiveRenderTest, ThePrimsSettingsAreTheEnginesAndTheCapAppliesOverThem)
{
    if (!cyclesOrSkip()) {
        return;
    }
    // The fixed sampling without its samples, which would override the prim's.
    std::map<std::string, PXR_NS::VtValue> fixed = fixedSampling(kSamples);
    fixed.erase("cycles:samples");
    HydraRender::setRendererSettings(kRenderDelegateCyclesId, fixed);
    std::map<std::string, PXR_NS::VtValue> prim;
    prim["cycles:samples"] = PXR_NS::VtValue(12);
    ASSERT_TRUE(start(prim));
    std::vector<std::shared_ptr<const ProgressiveRender::Frame>> frames;
    ASSERT_TRUE(collectUntilDone(0, &frames));
    EXPECT_EQ(12, _render->getMaxSamples());
    EXPECT_EQ(12, frames.back()->samples);

    std::size_t mark = publishedCount();
    _render->setMaxSamples(5);
    ASSERT_TRUE(collectUntilDone(mark, &frames));
    EXPECT_EQ(5, _render->getMaxSamples());
    EXPECT_EQ(5, frames.back()->samples);

    const U64 restarts = _render->getRestartCount();
    const U64 builds = HydraRender::getEngineBuildCount();
    _render->setSettings(prim);
    // The same settings: nothing restarts, now or a little later.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(restarts, _render->getRestartCount());
    EXPECT_EQ(builds, HydraRender::getEngineBuildCount());

    mark = publishedCount();
    _render->setMaxSamples(0);
    prim["cycles:samples"] = PXR_NS::VtValue(7);
    _render->setSettings(prim);
    ASSERT_TRUE(collectUntilDone(mark, &frames));
    EXPECT_EQ(7, _render->getMaxSamples());
    EXPECT_EQ(7, frames.back()->samples);
    EXPECT_GT(HydraRender::getEngineBuildCount(), builds);
}

// A cap lowered mid-render to samples Cycles already has still ends the render at the cap, with a
// frame whose prims can be picked.
TEST_F(ProgressiveRenderTest, ACapLoweredBelowTheSamplesRenderedEndsAtIt)
{
    if (!cyclesOrSkip()) {
        return;
    }
    ASSERT_TRUE(start());
    // Held at its first frame while Cycles samples on, so that it has more than the cap below.
    hold(true);
    ASSERT_TRUE(waitUntil([this]() { return _held; }, "the first frame"));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const std::size_t mark = publishedCount();
    _render->setMaxSamples(1);
    hold(false);
    std::vector<std::shared_ptr<const ProgressiveRender::Frame>> frames;
    ASSERT_TRUE(collectUntilDone(mark, &frames));
    ASSERT_FALSE(frames.empty());
    EXPECT_EQ(1, frames.back()->samples);
    const std::set<std::string> picked = pickedPrims(*frames.back());
    EXPECT_TRUE(anyUnder(picked, "/World/Near"));
}

// A cap above the samples of the frame last read, but below those Cycles has rendered since, ends the
// render at the cap all the same: Cycles samples on while the worker waits between passes.
TEST_F(ProgressiveRenderTest, ACapLoweredAboveTheSamplesLastReadEndsAtIt)
{
    if (!cyclesOrSkip()) {
        return;
    }
    // A cap no pass reaches, so the first frame is partial however fast the runner is.
    const int kHighCap = 1000000;
    HydraRender::setRendererSettings(kRenderDelegateCyclesId, fixedSampling(kHighCap));
    ASSERT_TRUE(start());
    hold(true);
    ASSERT_TRUE(waitUntil([this]() { return _held; }, "the first frame"));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const std::shared_ptr<const ProgressiveRender::Frame> first = _render->getFrame();
    ASSERT_TRUE(first != nullptr);
    const int cap = std::max(1, first->samples) + 1;
    ASSERT_LT(cap, kHighCap);
    const std::size_t mark = publishedCount();
    _render->setMaxSamples(cap);
    hold(false);
    std::vector<std::shared_ptr<const ProgressiveRender::Frame>> frames;
    ASSERT_TRUE(collectUntilDone(mark, &frames));
    ASSERT_FALSE(frames.empty());
    EXPECT_EQ(cap, frames.back()->samples);
    const std::set<std::string> picked = pickedPrims(*frames.back());
    EXPECT_TRUE(anyUnder(picked, "/World/Near"));
}

// A prim the draw rules hide leaves the frame, and what they leave stays.
TEST_F(ProgressiveRenderTest, ThePrimsTheDrawRulesHideLeaveTheFrame)
{
    if (!cyclesOrSkip()) {
        return;
    }
    ASSERT_TRUE(start());
    std::set<std::string> shown;
    ASSERT_TRUE(pickedWhenDone(0, &shown));
    ASSERT_TRUE(anyUnder(shown, "/World/Near"));
    ASSERT_TRUE(anyUnder(shown, "/World/Far"));

    const std::size_t mark = publishedCount();
    SceneDrawFilter::Rules rules;
    rules[PXR_NS::SdfPath("/World/Near")] = SceneDrawFilter::eRuleHidden;
    _render->setDrawRules(rules);
    std::set<std::string> hidden;
    ASSERT_TRUE(pickedWhenDone(mark, &hidden));
    EXPECT_FALSE(anyUnder(hidden, "/World/Near"));
    EXPECT_TRUE(anyUnder(hidden, "/World/Far"));
    // Nothing else is behind the near cube at the centre.
    EXPECT_NEAR(1.f, centreDepth(*_render->getFrame()), 1e-4f);
}

// Five draw changes in a burst restart the render once, and its frames follow the last of them.
// The render is held in its listener while they come, so that it cannot restart between them.
TEST_F(ProgressiveRenderTest, CoalescesDrawChangesIntoOneRestart)
{
    if (!cyclesOrSkip()) {
        return;
    }
    ASSERT_TRUE(start());
    std::vector<std::shared_ptr<const ProgressiveRender::Frame>> frames;
    ASSERT_TRUE(collectUntilDone(0, &frames));
    const U64 builds = HydraRender::getEngineBuildCount();
    const U64 remakesBefore = _render->getStatus().unseenRemakes;
    hold(true);
    const std::size_t mark = publishedCount();
    SceneDrawDelta boxFar;
    boxFar.boxed.push_back("/World/Far");
    _render->applyDrawDeltas(std::vector<SceneDrawDelta> { boxFar });
    ASSERT_TRUE(waitUntil([this]() { return _held; }, "a frame with the far cube boxed"));
    const ProgressiveRender::Status refining = _render->getStatus();

    SceneDrawDelta clearFar;
    clearFar.cleared.push_back("/World/Far");
    SceneDrawDelta boxNear;
    boxNear.boxed.push_back("/World/Near");
    SceneDrawDelta clearNear;
    clearNear.cleared.push_back("/World/Near");
    const std::vector<SceneDrawDelta> burst { clearFar, boxNear, clearNear, hiding("/World/Far"), hiding("/World/Near") };
    const std::chrono::steady_clock::time_point burstStart = std::chrono::steady_clock::now();
    for (const SceneDrawDelta& delta : burst) {
        _render->applyDrawDeltas(std::vector<SceneDrawDelta> { delta });
    }
    EXPECT_LT(std::chrono::steady_clock::now() - burstStart, std::chrono::milliseconds(20));
    hold(false);
    std::set<std::string> after;
    ASSERT_TRUE(pickedWhenDone(mark, &after));
    const ProgressiveRender::Status end = _render->getStatus();
    // The worker makes its engine again when Cycles is slow to show a restart, a restart and an
    // engine of its own that no draw change caused.
    const U64 remade = end.unseenRemakes - refining.unseenRemakes;
    EXPECT_EQ(refining.restarts + 1 + remade, end.restarts) << remade << " engine(s) made after the draw changes";
    EXPECT_FALSE(anyUnder(after, "/World/Near"));
    EXPECT_FALSE(anyUnder(after, "/World/Far"));
    EXPECT_EQ(builds + (end.unseenRemakes - remakesBefore), HydraRender::getEngineBuildCount());
}

// Render Everything draws what the rules hide, and off again draws by them as before, all with
// the one engine.
TEST_F(ProgressiveRenderTest, RenderEverythingIgnoresTheDrawRulesUntilOff)
{
    if (!cyclesOrSkip()) {
        return;
    }
    ASSERT_TRUE(start());
    std::vector<std::shared_ptr<const ProgressiveRender::Frame>> frames;
    ASSERT_TRUE(collectUntilDone(0, &frames));
    std::size_t mark = publishedCount();
    _render->applyDrawDeltas(std::vector<SceneDrawDelta> { hiding("/World/Near") });
    std::set<std::string> ruled;
    ASSERT_TRUE(pickedWhenDone(mark, &ruled));
    ASSERT_FALSE(anyUnder(ruled, "/World/Near"));
    ASSERT_FALSE(ruled.empty());
    const U64 builds = HydraRender::getEngineBuildCount();
    const U64 remakesBefore = _render->getStatus().unseenRemakes;

    mark = publishedCount();
    _render->setRenderEverything(true);
    std::set<std::string> everything;
    ASSERT_TRUE(pickedWhenDone(mark, &everything));
    EXPECT_TRUE(anyUnder(everything, "/World/Near"));
    EXPECT_TRUE(anyUnder(everything, "/World/Far"));

    mark = publishedCount();
    _render->setRenderEverything(false);
    std::set<std::string> restored;
    ASSERT_TRUE(pickedWhenDone(mark, &restored));
    EXPECT_EQ(ruled, restored);
    // Less the engines the worker makes again when Cycles is slow to show a restart.
    EXPECT_EQ(builds + (_render->getStatus().unseenRemakes - remakesBefore), HydraRender::getEngineBuildCount());
}

TEST_F(ProgressiveRenderTest, AHeadlightStandsInForHiddenLightsOnCycles)
{
    if (!cyclesOrSkip()) {
        return;
    }
    checkHeadlight(kRenderDelegateCyclesId);
}

// hdMoonray lights a scene with no light by a white dome of its own, which the headlight keeps off.
TEST_F(ProgressiveRenderTest, AHeadlightStandsInForHiddenLightsOnMoonRay)
{
    if (!moonRayOrSkip()) {
        return;
    }
    checkHeadlight(kMoonRayRendererId);
}

namespace {
// The next worker pass, once armed, held up for sleepMs inside the worker's engine lock as a slow
// delegate call would be, and whether it still is.
struct SlowSync {
    std::atomic<bool> armed { false };
    std::atomic<bool> entered { false };
    std::atomic<bool> inside { false };
    std::atomic<int> sleepMs { 0 };
};

// Takes and drops the stage lock every couple of milliseconds until stopped, recording the
// longest wait for it; joined when it goes, however the test ends.
class LockProbe {
public:
    explicit LockProbe(std::mutex* lock)
        : _thread([this, lock]() {
            while (!_stop) {
                const std::chrono::steady_clock::time_point asked = std::chrono::steady_clock::now();
                {
                    std::lock_guard<std::mutex> l(*lock);
                }
                const long long waited = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - asked).count();
                if (waited > _longest.load()) {
                    _longest = waited;
                }
                ++_taken;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        })
    {
    }

    ~LockProbe()
    {
        stop();
    }

    void stop()
    {
        _stop = true;
        if (_thread.joinable()) {
            _thread.join();
        }
    }

    // In milliseconds.
    double longest() const
    {
        return _longest.load() / 1000.;
    }

    int taken() const
    {
        return _taken.load();
    }

private:
    std::atomic<bool> _stop { false };
    // In microseconds.
    std::atomic<long long> _longest { 0 };
    std::atomic<int> _taken { 0 };
    std::thread _thread;
};
} // anon namespace

// The worker holds the stage lock only to take its snapshot. Through a new engine's first sync and
// render of a heavy scene the stage lock is free within 50 ms whenever it is asked for. A pass held
// up inside the worker's engine lock, as a slow delegate call would be, ends while the test thread
// holds the stage lock. The edit still reaches the frames.
TEST_F(ProgressiveRenderTest, TheStageLockIsFreeWhileTheWorkerSyncsAndRenders)
{
    if (!cyclesOrSkip()) {
        return;
    }
    const double kPromptMs = 50.;
    // The fixed sampling without its samples, which would override the test's.
    std::map<std::string, PXR_NS::VtValue> fixed = fixedSampling(kSamples);
    fixed.erase("cycles:samples");
    HydraRender::setRendererSettings(kRenderDelegateCyclesId, fixed);
    std::map<std::string, PXR_NS::VtValue> settings;
    settings["cycles:samples"] = PXR_NS::VtValue(2);
    ASSERT_TRUE(start(settings));
    std::vector<std::shared_ptr<const ProgressiveRender::Frame>> frames;
    ASSERT_TRUE(collectUntilDone(0, &frames));

    std::shared_ptr<SlowSync> slow = std::make_shared<SlowSync>();
    ProgressiveRender::setPassHook([slow]() {
        if (!slow->armed.exchange(false)) {
            return;
        }
        slow->inside = true;
        slow->entered = true;
        const std::chrono::milliseconds wait(slow->sleepMs.load());
        if (wait.count() > 0) {
            std::this_thread::sleep_for(wait);
        }
        slow->inside = false;
    });
    // payload and new settings in one edit, so the pass that brings them is a new engine's first
    // sync of the whole scene. Returns the published count from before it.
    const auto edit = [this, &settings, &slow](const ScenePayload& payload,
                                               int samples,
                                               int sleepMs) {
        settings["cycles:samples"] = PXR_NS::VtValue(samples);
        slow->sleepMs = sleepMs;
        slow->entered = false;
        slow->armed = true;
        const std::size_t mark = publishedCount();
        std::lock_guard<std::mutex> lock(_stageLock);
        if (_stage->sync(payload)) {
            _render->invalidate();
        }
        _render->setSettings(settings);

        return mark;
    };

    const ScenePayload heavy = heavyScene(_base, 24, 160);
    std::size_t mark = edit(heavy, 3, 0);
    {
        const std::chrono::steady_clock::time_point entered = std::chrono::steady_clock::now();
        LockProbe probe(&_stageLock);
        ASSERT_TRUE(waitUntil([&slow]() { return slow->entered.load(); }, "the worker's pass over the heavy scene"));
        const bool framed = waitUntil([this, mark]() { return _published.size() > mark; }, "a frame of the heavy scene");
        const long long passMs = millisecondsSince(entered);
        probe.stop();
        ASSERT_TRUE(framed);
        std::printf("[          ] a new engine's first pass over the heavy scene took %lld ms; the stage lock was taken %d times meanwhile, after %.2f ms at most\n",
                    passMs, probe.taken(), probe.longest());
        EXPECT_GT(probe.taken(), 0);
        EXPECT_LT(probe.longest(), kPromptMs) << "the worker's sync or render of the heavy scene holds the stage lock";
        // A pass longer than the bound is enough: a stage lock held through it would be waited
        // on for longer than the bound. A fast runner's pass comes close to twice the bound.
        EXPECT_GT(passMs, (long long)kPromptMs) << "the heavy pass was too quick to show the stage lock free through it";
    }

    const double kBack = 4.;
    // Without the heavy layer: its grids sit inside /World/Near, and would show once it moved back.
    mark = edit(movedNear(_base, kBack, 0x5104ULL), 4, 800);
    ASSERT_TRUE(waitUntil([&slow]() { return slow->entered.load(); }, "the worker's held-up pass"));
    {
        const std::chrono::steady_clock::time_point asked = std::chrono::steady_clock::now();
        std::unique_lock<std::mutex> held(_stageLock);
        const double waited = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - asked).count();
        EXPECT_LT(waited, kPromptMs) << "the worker holds the stage lock inside its engine lock";
        // Held from here for the rest of the held-up call: it ends all the same.
        const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (slow->inside && (std::chrono::steady_clock::now() < end)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        EXPECT_FALSE(slow->inside.load()) << "the worker's held-up pass waits for the stage lock";
    }

    std::vector<std::shared_ptr<const ProgressiveRender::Frame>> after;
    ASSERT_TRUE(collectUntilDone(mark, &after));
    ASSERT_FALSE(after.empty());
    EXPECT_NEAR(windowDepth(_view.frustum, kNearDistance + kBack), centreDepth(*after.back()), 1e-4f);
}

// A reload of a file layer while the worker is held up inside its engine lock, as a slow delegate
// call holds it, keeps nobody else waiting: asked for on the main thread it returns at once, and
// while another thread's reload waits for the worker, readers and the stage lock stay free. Both
// end once the call does, and the frames after them show the file's new content.
TEST_F(ProgressiveRenderTest, AFileReloadDuringAPassKeepsTheMainThreadAndReadersFree)
{
    if (!cyclesOrSkip()) {
        return;
    }
    const double kPromptMs = 100.;
    const double kBack = 4.;
    const int kHoldMs = 2000;
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = dir.filePath(QString::fromUtf8("heavy.usdc")).toStdString();
    ASSERT_TRUE(writeHeavyFile(path, 24, 160, 0.));

    std::map<std::string, PXR_NS::VtValue> fixed = fixedSampling(kSamples);
    fixed.erase("cycles:samples");
    HydraRender::setRendererSettings(kRenderDelegateCyclesId, fixed);
    std::map<std::string, PXR_NS::VtValue> settings;
    settings["cycles:samples"] = PXR_NS::VtValue(2);
    ASSERT_TRUE(start(settings));
    PXR_NS::SdfLayerRefPtr fileScene = SceneOps::createNodeLayer("ProgressiveRenderTestFile");
    fileScene->InsertSubLayerPath(path);
    sync(SceneOps::appendLayer(_base, fileScene, 0xf11e5ULL));
    std::vector<std::shared_ptr<const ProgressiveRender::Frame>> frames;
    ASSERT_TRUE(collectUntilDone(0, &frames));
    ASSERT_FALSE(frames.empty());
    EXPECT_NEAR(windowDepth(_view.frustum, kNearDistance), centreDepth(*frames.back()), 1e-4f);
    PXR_NS::SdfLayerRefPtr file = PXR_NS::SdfLayer::Find(path);
    ASSERT_TRUE(bool(file)) << "the stage did not open " << path;
    const std::vector<PXR_NS::SdfLayerRefPtr> layers = SceneOps::getFileLayerClosure(file);
    // Before the hold, which writing this much could use up on a busy machine: the stage keeps
    // the file it opened until the reload.
    ASSERT_TRUE(writeHeavyFile(path, 24, 160, kBack));

    // As the viewer's stage lock is: a write's last participant, since quiescing it waits for
    // a worker taking its snapshot.
    struct StageParticipant {
        explicit StageParticipant(std::mutex* lock)
            : id(SceneWriteBarrier::registerParticipant([lock]() { lock->lock(); },
                                                        [lock]() { lock->unlock(); },
                                                        SceneWriteBarrier::eQuiesceOrderLast))
        {
        }

        ~StageParticipant()
        {
            SceneWriteBarrier::unregisterParticipant(id);
        }

        SceneWriteBarrier::ParticipantId id;
    } stageParticipant(&_stageLock);

    std::shared_ptr<SlowSync> slow = std::make_shared<SlowSync>();
    ProgressiveRender::setPassHook([slow]() {
        if (!slow->armed.exchange(false)) {
            return;
        }
        slow->inside = true;
        slow->entered = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(slow->sleepMs.load()));
        slow->inside = false;
    });
    slow->sleepMs = kHoldMs;
    slow->armed = true;
    settings["cycles:samples"] = PXR_NS::VtValue(3);
    _render->setSettings(settings);
    ASSERT_TRUE(waitUntil([&slow]() { return slow->entered.load(); }, "the worker's held-up pass"));
    const std::uint64_t reloads = SceneOps::getFileLayerReloadCount();
    const std::size_t mark = publishedCount();

    // Shared, as the completion may come after a failed assertion has left the test.
    struct Outcome {
        std::atomic<bool> done { false };
        std::atomic<bool> ok { false };
        std::mutex errorMutex;
        std::string error;
    };
    const std::shared_ptr<Outcome> asked = std::make_shared<Outcome>();
    const std::chrono::steady_clock::time_point request = std::chrono::steady_clock::now();
    SceneOps::requestFileLayerReload(layers, [asked](bool ok, const std::string& error) {
        {
            std::lock_guard<std::mutex> l(asked->errorMutex);
            asked->error = error;
        }
        asked->ok = ok;
        asked->done = true;
    });
    const double requestMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - request).count();
    EXPECT_LT(requestMs, kPromptMs) << "requestFileLayerReload on the main thread waits for the worker's held-up pass";
    EXPECT_TRUE(slow->inside.load()) << "the held-up pass ended before the reload it was meant to hold up";

    const std::shared_ptr<Outcome> blocking = std::make_shared<Outcome>();
    struct Joined {
        ~Joined()
        {
            if (thread.joinable()) {
                thread.join();
            }
        }

        std::thread thread;
    } other;
    other.thread = std::thread([blocking, layers]() {
        std::string error;
        const bool ok = SceneOps::reloadFileLayers(layers, &error);
        {
            std::lock_guard<std::mutex> l(blocking->errorMutex);
            blocking->error = error;
        }
        blocking->ok = ok;
        blocking->done = true;
    });

    _watchdog.setPhase("reload");
    double longest = 0.;
    int taken = 0;
    const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now() + kTimeout;
    while (!(asked->done && blocking->done) && (std::chrono::steady_clock::now() < end)) {
        const std::chrono::steady_clock::time_point probe = std::chrono::steady_clock::now();
        {
            SceneWriteBarrier::ReadScope read;
            std::lock_guard<std::mutex> stage(_stageLock);
        }
        longest = std::max(longest, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - probe).count());
        ++taken;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const long long reloadMs = millisecondsSince(request);
    if (other.thread.joinable()) {
        other.thread.join();
    }
    ASSERT_TRUE(asked->done.load()) << "the main thread's reload did not end in " << kTimeout.count() << " s";
    ASSERT_TRUE(blocking->done.load()) << "the other thread's reload did not end in " << kTimeout.count() << " s";
    std::printf("[          ] the reloads ended %lld ms after they were asked for; a reader and the stage lock were taken %d times meanwhile, after %.2f ms at most\n",
                reloadMs, taken, longest);
    EXPECT_TRUE(asked->ok.load()) << asked->error;
    EXPECT_TRUE(blocking->ok.load()) << blocking->error;
    EXPECT_GT(reloadMs, (long long)(2 * kPromptMs)) << "the reloads did not wait for the held-up pass, which they cannot cut short";
    EXPECT_GT(taken, 0);
    EXPECT_LT(longest, kPromptMs) << "a reload waiting for the worker keeps readers or the stage lock waiting";
    EXPECT_GT(SceneOps::getFileLayerReloadCount(), reloads);

    std::vector<std::shared_ptr<const ProgressiveRender::Frame>> after;
    ASSERT_TRUE(collectUntilDone(mark, &after));
    ASSERT_FALSE(after.empty());
    EXPECT_NEAR(windowDepth(_view.frustum, kNearDistance + kBack), centreDepth(*after.front()), 1e-4f) << "a frame of the file's old content came after the reload";
    EXPECT_NEAR(windowDepth(_view.frustum, kNearDistance + kBack), centreDepth(*after.back()), 1e-4f);
}

// A reader that holds the GIL and waits for a writer releases the GIL while it waits, and takes it
// back only once it has let go of the barrier's lock, so another thread that took the GIL
// meanwhile can still open a ReadScope. The threads of a deadlock cannot be joined, so a hang
// ends the process.
TEST_F(ProgressiveRenderTest, AReaderWaitingForAWriterTakesTheGilBackOutsideTheBarrierLock)
{
    ASSERT_TRUE(Py_IsInitialized() != 0);
    _watchdog.setPhase("gil lock order");

    std::mutex m;
    std::condition_variable changed;
    bool writing = false;
    bool releaseWriter = false;
    bool writerDone = false;
    bool readerHasGil = false;
    bool otherHasGil = false;
    int finished = 0;
    bool wrote = false;
    std::string writeError;

    auto setFlag = [&](bool& flag) {
        {
            std::lock_guard<std::mutex> l(m);
            flag = true;
        }
        changed.notify_all();
    };
    auto finish = [&]() {
        {
            std::lock_guard<std::mutex> l(m);
            ++finished;
        }
        changed.notify_all();
    };
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    auto waitFor = [&](const char* what, const std::function<bool()>& done) {
        std::unique_lock<std::mutex> l(m);
        if (!changed.wait_until(l, deadline, done)) {
            ADD_FAILURE() << "the threads deadlocked waiting for " << what;
            std::fprintf(stderr, "[  FAILED  ] the GIL and the barrier lock deadlocked waiting for %s\n", what);
            std::fflush(stdout);
            std::fflush(stderr);
            std::_Exit(1);
        }
    };

    std::thread writer([&]() {
        wrote = SceneWriteBarrier::runExclusive([&]() {
            std::unique_lock<std::mutex> l(m);
            writing = true;
            changed.notify_all();
            changed.wait(l, [&]() { return releaseWriter; });
        },
                                                &writeError);
        setFlag(writerDone);
        finish();
    });
    waitFor("the write to start", [&]() { return writing; });

    std::thread reader([&]() {
        const PyGILState_STATE gil = PyGILState_Ensure();
        setFlag(readerHasGil);
        {
            SceneWriteBarrier::ReadScope read;
        }
        PyGILState_Release(gil);
        finish();
    });
    waitFor("the reader to take the GIL", [&]() { return readerHasGil; });

    // Takes the GIL once the waiting reader has released it.
    std::thread other([&]() {
        const PyGILState_STATE gil = PyGILState_Ensure();
        setFlag(otherHasGil);
        {
            std::unique_lock<std::mutex> l(m);
            changed.wait(l, [&]() { return writerDone; });
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        {
            SceneWriteBarrier::ReadScope read;
        }
        PyGILState_Release(gil);
        finish();
    });
    waitFor("the other thread to take the GIL", [&]() { return otherHasGil; });

    setFlag(releaseWriter);
    waitFor("the writer and both readers to end", [&]() { return finished == 3; });
    writer.join();
    reader.join();
    other.join();
    EXPECT_TRUE(wrote) << writeError;
}

// A thread that holds the GIL and registers or unregisters a participant while a writer is inside
// runExclusive releases the GIL while it waits for the writer lock, because the writer takes the
// GIL back while it holds that lock. The writer is held, once its readers have drained and before
// it takes the GIL back, until the registrant sleeps on the writer lock: a registrant that kept
// the GIL then would block the writer for good. A hang ends the process.
TEST_F(ProgressiveRenderTest, RegisteringAParticipantDuringAWriteReleasesTheGilWhileWaiting)
{
    ASSERT_TRUE(Py_IsInitialized() != 0);
    _watchdog.setPhase("gil writer lock order");

    std::mutex m;
    std::condition_variable changed;
    bool readerIn = false;
    bool releaseReader = false;
    bool writerHasGil = false;
    bool registrantHasGil = false;
    int finished = 0;
    bool wrote = false;
    std::string writeError;
    std::atomic<long> registrantTid(0);

    auto setFlag = [&](bool& flag) {
        {
            std::lock_guard<std::mutex> l(m);
            flag = true;
        }
        changed.notify_all();
    };
    auto finish = [&]() {
        {
            std::lock_guard<std::mutex> l(m);
            ++finished;
        }
        changed.notify_all();
    };
    auto deadlocked = [&](const char* what) {
        ADD_FAILURE() << "the threads deadlocked waiting for " << what;
        std::fprintf(stderr, "[  FAILED  ] the GIL and the writer lock deadlocked waiting for %s\n", what);
        std::fflush(stdout);
        std::fflush(stderr);
        std::_Exit(1);
    };
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    auto waitFor = [&](const char* what, const std::function<bool()>& done) {
        std::unique_lock<std::mutex> l(m);
        if (!changed.wait_until(l, deadline, done)) {
            deadlocked(what);
        }
    };
    auto threadIsSleeping = [](long tid) {
        std::ifstream stat("/proc/self/task/" + std::to_string(tid) + "/stat");
        std::string line;
        std::getline(stat, line);
        const std::size_t close = line.rfind(')');
        return close != std::string::npos && close + 2 < line.size() && (line[close + 2] == 'S' || line[close + 2] == 'D');
    };

    struct Hold {
        std::function<void()> run;
    } hold;
    hold.run = [&]() {
        while (!threadIsSleeping(registrantTid.load())) {
            if (std::chrono::steady_clock::now() > deadline) {
                deadlocked("the registrant to wait for the writer lock");
            }
            std::this_thread::yield();
        }
    };
    SceneWriteBarrier::setTestHook([](void* context) { static_cast<Hold*>(context)->run(); }, &hold);

    // Keeps the writer waiting for readers, with the writer lock held and the GIL released.
    std::thread reader([&]() {
        {
            SceneWriteBarrier::ReadScope read;
            setFlag(readerIn);
            std::unique_lock<std::mutex> l(m);
            changed.wait(l, [&]() { return releaseReader; });
        }
        finish();
    });
    waitFor("the reader to enter", [&]() { return readerIn; });

    std::thread writer([&]() {
        const PyGILState_STATE gil = PyGILState_Ensure();
        setFlag(writerHasGil);
        wrote = SceneWriteBarrier::runExclusive([]() { }, &writeError);
        PyGILState_Release(gil);
        finish();
    });
    waitFor("the writer to take the GIL", [&]() { return writerHasGil; });

    std::thread registrant([&]() {
        const PyGILState_STATE gil = PyGILState_Ensure();
        registrantTid.store(static_cast<long>(syscall(SYS_gettid)));
        setFlag(registrantHasGil);
        const SceneWriteBarrier::ParticipantId id = SceneWriteBarrier::registerParticipant([]() { }, []() { });
        SceneWriteBarrier::unregisterParticipant(id);
        PyGILState_Release(gil);
        finish();
    });
    waitFor("the registrant to take the GIL", [&]() { return registrantHasGil; });

    setFlag(releaseReader);
    waitFor("the reader, writer and registrant to end", [&]() { return finished == 3; });
    reader.join();
    writer.join();
    registrant.join();
    SceneWriteBarrier::setTestHook(nullptr, nullptr);
    EXPECT_TRUE(wrote) << writeError;
}

// A drag preview set on the caller's stage reaches the worker's stage with the snapshot, and
// clearing it brings the prim back.
TEST_F(ProgressiveRenderTest, ThePreviewTransformsReachTheRender)
{
    if (!cyclesOrSkip()) {
        return;
    }
    ASSERT_TRUE(start());
    std::vector<std::shared_ptr<const ProgressiveRender::Frame>> frames;
    ASSERT_TRUE(collectUntilDone(0, &frames));
    const float rest = windowDepth(_view.frustum, kNearDistance);
    EXPECT_NEAR(rest, centreDepth(*frames.back()), 1e-4f);

    const double kBack = 3.;
    PXR_NS::GfMatrix4d delta(1.);
    delta.SetTranslate(PXR_NS::GfVec3d(0., 0., -kBack));
    std::size_t mark = publishedCount();
    {
        std::lock_guard<std::mutex> lock(_stageLock);
        ASSERT_TRUE(_stage->setPreviewTransforms(std::vector<std::pair<std::string, PXR_NS::GfMatrix4d>>(1, std::make_pair(std::string("/World/Near"), delta)), 0.));
        _render->invalidate();
    }
    ASSERT_TRUE(collectUntilDone(mark, &frames));
    EXPECT_NEAR(windowDepth(_view.frustum, kNearDistance + kBack), centreDepth(*frames.back()), 1e-4f);

    mark = publishedCount();
    {
        std::lock_guard<std::mutex> lock(_stageLock);
        _stage->clearPreviewTransforms();
        _render->invalidate();
    }
    ASSERT_TRUE(collectUntilDone(mark, &frames));
    EXPECT_NEAR(rest, centreDepth(*frames.back()), 1e-4f);
}

TEST_F(ProgressiveRenderTest, CyclesReturnsAtOnceFromChangesMidPass)
{
    if (!cyclesOrSkip()) {
        return;
    }
    // The fixed sampling without its samples, which would override the test's.
    std::map<std::string, PXR_NS::VtValue> fixed = fixedSampling(kSamples);
    fixed.erase("cycles:samples");
    HydraRender::setRendererSettings(kRenderDelegateCyclesId, fixed);
    interruptsMidPass(kRenderDelegateCyclesId, "cycles:samples", moonRayAvailable() ? std::string(kMoonRayRendererId) : std::string());
}

TEST_F(ProgressiveRenderTest, CyclesSurvivesRepeatedChangesMidPass)
{
    if (!cyclesOrSkip()) {
        return;
    }
    std::map<std::string, PXR_NS::VtValue> fixed = fixedSampling(kSamples);
    fixed.erase("cycles:samples");
    HydraRender::setRendererSettings(kRenderDelegateCyclesId, fixed);
    const int liveBefore = ProgressiveRender::getLiveCount();
    std::map<std::string, PXR_NS::VtValue> settings;
    settings["cycles:samples"] = PXR_NS::VtValue(2);
    ASSERT_TRUE(start(settings, kRenderDelegateCyclesId));
    sync(heavyScene(_base, 24, 160));

    // Each change tears down an engine whose session thread may be writing tiles while the next
    // engine binds its outputs; varying the delay moves the teardown across the pass, so a race
    // between the two has many chances to corrupt the heap.
    for (int i = 0; i < 8; ++i) {
        settings["cycles:samples"] = PXR_NS::VtValue(16 + i);
        ASSERT_TRUE(heavyPass(settings));
        std::this_thread::sleep_for(std::chrono::milliseconds(10 * (i % 4)));
    }
    settings["cycles:samples"] = PXR_NS::VtValue(3);
    const std::size_t mark = publishedCount();
    _render->setSettings(settings);
    std::vector<std::shared_ptr<const ProgressiveRender::Frame>> frames;
    ASSERT_TRUE(collectUntilDone(mark, &frames));
    ASSERT_FALSE(frames.empty());
    EXPECT_EQ(3, frames.back()->maxSamples);

    settings["cycles:samples"] = PXR_NS::VtValue(5);
    ASSERT_TRUE(heavyPass(settings));
    _listened = nullptr;
    _render.reset();
    EXPECT_TRUE(ProgressiveRender::waitForDetachedWorkers(kEndMs));
    EXPECT_EQ(liveBefore, ProgressiveRender::getLiveCount());
}

TEST_F(ProgressiveRenderTest, MoonRayReturnsAtOnceFromChangesMidPass)
{
    if (!moonRayOrSkip()) {
        return;
    }
    const bool cycles = bool(PXR_NS::PlugRegistry::GetInstance().GetPluginWithName("hdCycles"));
    interruptsMidPass(kMoonRayRendererId, MoonRaySettings::samplesKey(), cycles ? std::string(kRenderDelegateCyclesId) : std::string());
}

// MoonRay reports no reset counts: an edit restarts its render in place, and its frames are
// published without waiting out kRestartUnseen and making the engine again.
TEST_F(ProgressiveRenderTest, AMoonRayEditRestartsInPlace)
{
    if (!moonRayOrSkip()) {
        return;
    }
    std::map<std::string, PXR_NS::VtValue> settings;
    settings[MoonRaySettings::samplesKey()] = PXR_NS::VtValue(2);
    ASSERT_TRUE(start(settings, kMoonRayRendererId));
    std::vector<std::shared_ptr<const ProgressiveRender::Frame>> frames;
    ASSERT_TRUE(collectUntilDone(0, &frames));
    const ProgressiveRender::Status before = _render->getStatus();
    const std::uint64_t builds = HydraRender::getEngineBuildCount();

    const double kBack = 5.;
    const std::size_t mark = publishedCount();
    const std::chrono::steady_clock::time_point edited = std::chrono::steady_clock::now();
    sync(movedNear(_base, kBack, 0x300e1ULL));
    std::vector<std::shared_ptr<const ProgressiveRender::Frame>> after;
    ASSERT_TRUE(collectUntilDone(mark, &after));
    ASSERT_FALSE(after.empty());
    std::printf("[          ] the edit's render converged in %lld ms\n", millisecondsSince(edited));

    const ProgressiveRender::Status end = _render->getStatus();
    EXPECT_EQ(before.restarts + 1, end.restarts);
    EXPECT_EQ(before.engines, end.engines) << "the engine was made again after the edit";
    EXPECT_EQ(builds, HydraRender::getEngineBuildCount());
    const float moved = windowDepth(_view.frustum, kNearDistance + kBack);
    const float still = windowDepth(_view.frustum, kNearDistance);
    const float depth = centreDepth(*after.back());
    EXPECT_LT(std::fabs(moved - depth), std::fabs(still - depth)) << "the last frame is of the scene before the edit";
}

// A MoonRay session whose render process stops answering mid-render is made again once, and when
// the next one stops answering too the render fails and says why. A scene edit gives the render
// back its one remaking: the session the edit's retry makes, stopped in turn, is made again
// rather than failing at once. Each execComp is stopped once its session is rendering, and
// continued at the end so that its session's shutdown can end it. The startup bound stays long:
// a session slow to start on a loaded runner must not meet it before its process is stopped.
TEST_F(ProgressiveRenderTest, AMoonRaySessionThatStopsAnsweringIsMadeAgainOnceThenFails)
{
    if (!moonRayOrSkip()) {
        return;
    }
    const ScopedEnv stallBound("NATRON_DELEGATE_STALL_S", "3");
    const ScopedEnv startupStallBound("NATRON_DELEGATE_STARTUP_STALL_S", "120");
    const std::chrono::seconds kReapBound(15);
    RenderProcessStopper stopper(kReapBound);
    std::map<std::string, PXR_NS::VtValue> settings;
    // Enough samples that the frame is still rendering when its process is stopped.
    settings[MoonRaySettings::samplesKey()] = PXR_NS::VtValue(4096);
    ASSERT_TRUE(start(settings, kMoonRayRendererId));

    ASSERT_TRUE(waitUntil([this]() { return _render->getStatus().stallRemakes >= 1; }, "the first session's remaking"));
    {
        const ProgressiveRender::Status status = _render->getStatus();
        EXPECT_EQ(1u, status.stallRemakes);
        EXPECT_TRUE(status.error.empty()) << status.error;
    }
    ASSERT_TRUE(waitUntil([this]() { return !_render->getStatus().error.empty(); }, "the render's failure"));
    {
        const ProgressiveRender::Status status = _render->getStatus();
        EXPECT_NE(std::string::npos, status.error.find("stopped answering")) << status.error;
        EXPECT_EQ(1u, status.stallRemakes);
    }

    _render->invalidate();
    ASSERT_TRUE(waitUntil([this]() { return _render->getStatus().stallRemakes >= 2; }, "the remaking after the edit"));
    {
        const ProgressiveRender::Status status = _render->getStatus();
        EXPECT_EQ(2u, status.stallRemakes);
        EXPECT_TRUE(status.error.empty()) << "the edit's session failed without being made again: " << status.error;
    }
    ASSERT_TRUE(waitUntil([this]() { return !_render->getStatus().error.empty(); }, "the render's failure after the edit"));
    {
        const ProgressiveRender::Status status = _render->getStatus();
        EXPECT_NE(std::string::npos, status.error.find("stopped answering")) << status.error;
        EXPECT_EQ(2u, status.stallRemakes);
    }

    // The first two sessions, then the edit's two.
    EXPECT_EQ(4u, stopper.stopNoMore().size());
    _watchdog.setPhase("stopped render processes exit");
    EXPECT_TRUE(stopper.continueAndReap()) << "a stopped render process outlived its session";
}

// A pass waiting on a texture export stops waiting when a change makes its engine again, and the
// render goes on with a new engine, which asks for the texture afresh.
TEST_F(ProgressiveRenderTest, AnEngineChangeCutsTheTextureExportThePassWaitsOn)
{
    if (!cyclesOrSkip()) {
        return;
    }
    const long long kCutMs = 1000;
    std::map<std::string, PXR_NS::VtValue> fixed = fixedSampling(kSamples);
    fixed.erase("cycles:samples");
    HydraRender::setRendererSettings(kRenderDelegateCyclesId, fixed);
    std::map<std::string, PXR_NS::VtValue> settings;
    settings["cycles:samples"] = PXR_NS::VtValue(2);
    ASSERT_TRUE(start(settings));
    std::vector<std::shared_ptr<const ProgressiveRender::Frame>> frames;
    ASSERT_TRUE(collectUntilDone(0, &frames));

    // A source whose texture is rendering until the test ends it: an export of it waits.
    CountingTestRegistry::reset();
    std::atomic<bool> rendering(false);
    CountingTestRegistry::setRenderHook([&rendering](const Node* node) {
        rendering = true;
        QElapsedTimer timer;
        timer.start();
        while (!node->aborted() && (timer.elapsed() < 20000)) {
            QThread::msleep(5);
        }
    });
    NodePtr slow = createNode(QString::fromUtf8(kTestPluginIDCounting));
    ASSERT_TRUE(bool(slow));
    LiveTextureSource source;
    source.node = slow;
    source.nodeName = slow->getFullyQualifiedName();
    source.layer = "rgba";
    source.contentHash = slow->getEffectInstance()->getHash();
    const std::string uri = source.getUri(1.);
    std::atomic<bool> sourceAborted(false);
    std::thread preparing([&source, &sourceAborted]() {
        LiveTextureStore::instance().prepare(LiveTextureSourceList(1, source), 1., [&sourceAborted]() { return sourceAborted.load(); });
    });
    const bool started = waitUntil([&rendering]() { return rendering.load(); }, "the source's render");

    struct Export {
        std::atomic<bool> armed { false };
        std::atomic<bool> entered { false };
        std::atomic<bool> returned { false };
    };
    std::shared_ptr<Export> waiting = std::make_shared<Export>();
    ProgressiveRender::setPassHook([waiting, uri]() {
        if (!waiting->armed.exchange(false)) {
            return;
        }
        waiting->entered = true;
        (void)LiveTextureStore::instance().exportToSharedMemory(uri);
        waiting->returned = true;
    });
    if (started) {
        const std::uint64_t builds = HydraRender::getEngineBuildCount();
        waiting->armed = true;
        _render->invalidate();
        EXPECT_TRUE(waitUntil([&waiting]() { return waiting->entered.load(); }, "the pass's export"));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        EXPECT_FALSE(waiting->returned.load()) << "the export did not wait for the texture being rendered";

        const std::chrono::steady_clock::time_point asked = std::chrono::steady_clock::now();
        const std::size_t mark = publishedCount();
        settings["cycles:samples"] = PXR_NS::VtValue(3);
        _render->setSettings(settings);
        EXPECT_TRUE(waitUntil([&waiting]() { return waiting->returned.load(); }, "the cut export"));
        EXPECT_LT(millisecondsSince(asked), kCutMs) << "the export waited on through the change";
        EXPECT_TRUE(LiveTextureStore::instance().getFailure(uri).empty()) << "a cut export is not a failure of the texture";

        std::vector<std::shared_ptr<const ProgressiveRender::Frame>> after;
        EXPECT_TRUE(collectUntilDone(mark, &after));
        EXPECT_GT(HydraRender::getEngineBuildCount(), builds);
    }

    ProgressiveRender::setPassHook(std::function<void()>());
    sourceAborted = true;
    preparing.join();
    CountingTestRegistry::reset();
    EXPECT_TRUE(started) << "the source never started rendering";
}
