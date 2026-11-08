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

#include "SceneViewport.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <QActionGroup>
#include <QCheckBox>
#include <QContextMenuEvent>
#include <QCursor>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFocusEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMetaObject>
#include <QMouseEvent>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QPainter>
#include <QPointer>
#include <QSignalBlocker>
#include <QTimer>
#include <QUndoCommand>
#include <QWheelEvent>
#include <QWidgetAction>
#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/LiveTextureStore.h"
#include "Engine/Nodes/Scene/RenderDelegates.h"
#include "Engine/Nodes/Scene/SceneDrawStates.h"
#include "Engine/Nodes/Scene/SceneNodeAuthoring.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneStage.h"
#include "Engine/Nodes/Scene/SceneWriteBarrier.h"
#include "Engine/Nodes/Scene/StormGLSection.h"
#include "Engine/Nodes/Scene/USDCamera.h"
#include "Engine/Nodes/Scene/USDEditTransform.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"
#include "Engine/Project.h"
#include "Engine/Settings.h"
#include "Engine/UndoGesture.h"

// See UsdIncludes.h: oneTBB, which these include too, declares members named emit().
#ifdef emit
#define NATRON_SCENEVIEWPORT_RESTORE_EMIT
#undef emit
#endif
#include <pxr/base/gf/camera.h>
#include <pxr/base/gf/frustum.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/range1f.h>
#include <pxr/base/gf/range3d.h>
#include <pxr/base/gf/rect2i.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/vec2i.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/tf/errorMark.h>
#include <pxr/base/tf/getenv.h>
#include <pxr/imaging/cameraUtil/conformWindow.h>
#include <pxr/imaging/cameraUtil/framing.h>
#include <pxr/imaging/glf/simpleLight.h>
#include <pxr/imaging/glf/simpleMaterial.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hdx/pickTask.h>
#include <pxr/imaging/hdx/tokens.h>
#include <pxr/imaging/hgi/tokens.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/bboxCache.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/usdImaging/usdImagingGL/engine.h>
#include <pxr/usdImaging/usdImagingGL/renderParams.h>
#ifdef NATRON_SCENEVIEWPORT_RESTORE_EMIT
#undef NATRON_SCENEVIEWPORT_RESTORE_EMIT
#define emit
#endif

#include "Gui/ActionShortcuts.h"
#include "Gui/GLContextLoss.h"
#include "Gui/GuiApplicationManager.h"
#include "Gui/KnobGui.h"
#include "Gui/KnobUndoCommand.h"
#include "Gui/Menu.h"
#include "Gui/SceneDrawFilter.h"
#include "Gui/SceneGizmo.h"
#include "Gui/SceneGuides.h"
#include "Gui/SceneTransformTarget.h"
#include "Gui/SceneTransformWidget.h"
#include "Gui/UndoHistory.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {

// Storm clears to this, in scene-linear; it reaches the framebuffer through the display transform.
const float kBackgroundLinear = 0.05f;
// The same grey once encoded to sRGB, for the clears and checks done outside Storm.
const float kBackgroundSrgb = 0.2478f;

// Storm may need more than one pass before an image is final.
const int kMaxRenderPasses = 8;

// A stage change usually reaches the screen sooner than this, and a status flashing over the old
// image on every edit would be worse than the short wait.
const int kComposingStatusDelayMs = 250;

SdfPathVector
toSdfPaths(const std::vector<std::string>& paths)
{
    SdfPathVector result;

    for (const std::string& path : paths) {
        if (SdfPath::IsValidPathString(path)) {
            result.push_back(SdfPath(path));
        }
    }

    return result;
}

// Releases the Python GIL while the calling thread waits for the render thread, if it holds it: a
// script may have caused the paint, and an evaluation the render thread waits behind may need the GIL.
class PythonUnlockedWhileWaiting {
public:
    PythonUnlockedWhileWaiting()
        : _state(nullptr)
    {
        if (Py_IsInitialized() && PyGILState_Check()) {
            _state = PyEval_SaveThread();
        }
    }

    ~PythonUnlockedWhileWaiting()
    {
        if (_state) {
            PyEval_RestoreThread(_state);
        }
    }

    PythonUnlockedWhileWaiting(const PythonUnlockedWhileWaiting&) = delete;
    PythonUnlockedWhileWaiting& operator=(const PythonUnlockedWhileWaiting&) = delete;

private:
    PyThreadState* _state;
};

const double kMinElevation = -89.;
// The radius framed around a selection without extent, in scene units.
const double kDefaultFrameRadius = 1.;
const double kMaxElevation = 89.;

double
degreesToRadians(double d)
{
    return d * M_PI / 180.;
}

// A display transform Hydra cannot draw is baked into a log2 shaper and a 3D LUT, which it can.
// The shaper spans scene-linear 0 to 2^8, finest near 2^-10, so highlights still tone-map.
const float kBakeLog2Min = -10.f;
const float kBakeLog2Max = 8.f;
const unsigned long kBakeLutSize = 65;
const char* const kBakeInputSpace = "Natron 3D Viewport Input";
const char* const kBakeOutputSpace = "Natron 3D Viewport Output";
const char* const kBakeDisplay = "Natron 3D Viewport";
const char* const kBakeView = "Baked";

OCIO_NAMESPACE::ConstProcessorRcPtr
displayViewProcessor(const OCIO_NAMESPACE::ConstConfigRcPtr& config,
                     const std::string& colorSpace,
                     const std::string& display,
                     const std::string& view)
{
    OCIO_NAMESPACE::DisplayViewTransformRcPtr displayView = OCIO_NAMESPACE::DisplayViewTransform::Create();
    displayView->setSrc(colorSpace.c_str());
    displayView->setDisplay(display.c_str());
    displayView->setView(view.c_str());

    return config->getProcessor(displayView);
}

// Hydra's colour correction task (v26.08 hdx/colorCorrectionTask.cpp) loses the sampler name of
// every 1D and 2D LUT texture OpenColorIO asks for, and the draw then fails with a GL error,
// leaving only the clear colour on screen. ACES 2.0 output transforms need such textures;
// analytic transforms and 3D LUTs do not.
bool
needsLutTextures(const OCIO_NAMESPACE::ConstProcessorRcPtr& processor)
{
    OCIO_NAMESPACE::GpuShaderDescRcPtr shader = OCIO_NAMESPACE::GpuShaderDesc::CreateShaderDesc();
    shader->setLanguage(OCIO_NAMESPACE::GPU_LANGUAGE_GLSL_4_0);
    processor->getDefaultGPUProcessor()->extractGpuShaderInfo(shader);

    return shader->getNumTextures() > 0;
}

std::string
appendToList(const char* list,
             const std::string& item)
{
    const std::string existing = list ? list : "";

    return existing.empty() ? item : existing + ", " + item;
}

// Throws what OpenColorIO throws.
OCIO_NAMESPACE::GroupTransformRcPtr
bakeToLut(const OCIO_NAMESPACE::ConstProcessorRcPtr& processor)
{
    const unsigned long n = kBakeLutSize;
    // Offsetting by 2^min puts scene-linear 0 on the first grid point.
    const float offset = std::exp2(kBakeLog2Min);
    std::vector<float> grid(n);
    for (unsigned long i = 0; i < n; ++i) {
        const float t = float(i) / float(n - 1);
        grid[i] = std::exp2(kBakeLog2Min + t * (kBakeLog2Max - kBakeLog2Min)) - offset;
    }
    std::vector<float> rgb(n * n * n * 3);
    std::size_t p = 0;
    for (unsigned long r = 0; r < n; ++r) {
        for (unsigned long g = 0; g < n; ++g) {
            for (unsigned long b = 0; b < n; ++b) {
                rgb[p++] = grid[r];
                rgb[p++] = grid[g];
                rgb[p++] = grid[b];
            }
        }
    }
    OCIO_NAMESPACE::PackedImageDesc image(rgb.data(), long(n * n * n), 1, 3);
    processor->getDefaultCPUProcessor()->apply(image);

    OCIO_NAMESPACE::Lut3DTransformRcPtr lut = OCIO_NAMESPACE::Lut3DTransform::Create(n);
    lut->setInterpolation(OCIO_NAMESPACE::INTERP_TETRAHEDRAL);
    p = 0;
    for (unsigned long r = 0; r < n; ++r) {
        for (unsigned long g = 0; g < n; ++g) {
            for (unsigned long b = 0; b < n; ++b, p += 3) {
                lut->setValue(r, g, b, rgb[p], rgb[p + 1], rgb[p + 2]);
            }
        }
    }

    OCIO_NAMESPACE::AllocationTransformRcPtr shaper = OCIO_NAMESPACE::AllocationTransform::Create();
    shaper->setAllocation(OCIO_NAMESPACE::ALLOCATION_LG2);
    const float vars[3] = { kBakeLog2Min, kBakeLog2Max, offset };
    shaper->setVars(3, vars);
    OCIO_NAMESPACE::GroupTransformRcPtr baked = OCIO_NAMESPACE::GroupTransform::Create();
    baked->appendTransform(shaper);
    baked->appendTransform(lut);

    return baked;
}

/**
 * @brief A copy of config with transform as an extra display and view, kBakeDisplay and kBakeView,
 * reached from kBakeInputSpace. Hydra reads OCIO's process-wide current config, which OpenFX
 * plugins read too, so the copy keeps everything config has and hides what it adds from listings:
 * the display stays out of the active displays and the colour spaces are inactive.
 **/
OCIO_NAMESPACE::ConstConfigRcPtr
configWithViewportView(const OCIO_NAMESPACE::ConstConfigRcPtr& config,
                       const OCIO_NAMESPACE::ConstTransformRcPtr& transform)
{
    OCIO_NAMESPACE::ConfigRcPtr copy = config->createEditableCopy();
    std::string activeDisplays;
    for (int i = 0; i < config->getNumDisplays(); ++i) {
        activeDisplays = appendToList(activeDisplays.c_str(), config->getDisplay(i));
    }
    OCIO_NAMESPACE::ColorSpaceRcPtr input = OCIO_NAMESPACE::ColorSpace::Create(OCIO_NAMESPACE::REFERENCE_SPACE_SCENE);
    input->setName(kBakeInputSpace);
    copy->addColorSpace(input);
    OCIO_NAMESPACE::ColorSpaceRcPtr output = OCIO_NAMESPACE::ColorSpace::Create(OCIO_NAMESPACE::REFERENCE_SPACE_SCENE);
    output->setName(kBakeOutputSpace);
    output->setTransform(transform, OCIO_NAMESPACE::COLORSPACE_DIR_FROM_REFERENCE);
    copy->addColorSpace(output);
    const std::string inactive = appendToList(config->getInactiveColorSpaces(),
                                              std::string(kBakeInputSpace) + ", " + kBakeOutputSpace);
    copy->setInactiveColorSpaces(inactive.c_str());
    copy->addDisplayView(kBakeDisplay, kBakeView, kBakeOutputSpace, "");
    copy->setActiveDisplays(activeDisplays.c_str());

    return copy;
}

std::string
displayTransformKey(const OCIO_NAMESPACE::ConstConfigRcPtr& config,
                    const std::string& colorSpace,
                    const std::string& display,
                    const std::string& view)
{
    return std::string(config->getCacheID()) + '\n' + colorSpace + '\n' + display + '\n' + view;
}

// Main thread only. Baking evaluates the transform at every LUT grid point, so each one is kept.
// The viewer's gain and gamma stay out of the bake, so dragging them reuses it.
OCIO_NAMESPACE::ConstTransformRcPtr
cachedLut(const OCIO_NAMESPACE::ConstConfigRcPtr& config,
          const std::string& colorSpace,
          const std::string& display,
          const std::string& view,
          const OCIO_NAMESPACE::ConstProcessorRcPtr& processor)
{
    static std::map<std::string, OCIO_NAMESPACE::ConstTransformRcPtr> cache;
    const std::string key = displayTransformKey(config, colorSpace, display, view);
    std::map<std::string, OCIO_NAMESPACE::ConstTransformRcPtr>::const_iterator found = cache.find(key);
    if (found != cache.end()) {
        return found->second;
    }
    const std::size_t kMaxBakes = 8;
    if (cache.size() >= kMaxBakes) {
        cache.clear();
    }
    OCIO_NAMESPACE::ConstTransformRcPtr baked = bakeToLut(processor);
    cache[key] = baked;

    return baked;
}

// The 2D viewer multiplies the working-space image by gain before its display transform.
OCIO_NAMESPACE::TransformRcPtr
viewerGainTransform(double gain)
{
    const double m[16] = {
        gain, 0., 0., 0.,
        0., gain, 0., 0.,
        0., 0., gain, 0.,
        0., 0., 0., 1.
    };
    OCIO_NAMESPACE::MatrixTransformRcPtr matrix = OCIO_NAMESPACE::MatrixTransform::Create();
    matrix->setMatrix(m);

    return matrix;
}

/**
 * @brief After its display transform the 2D viewer takes each channel, negatives as 0, to the power
 * 1 / gamma, which ExponentTransform's default negative style does. A gamma of 0 or less shows
 * which values reach 1; OpenColorIO has no step, so the steepest ramp its RangeTransform accepts,
 * 2^-16 wide just below 1, stands in for it.
 **/
OCIO_NAMESPACE::TransformRcPtr
viewerGammaTransform(double gamma)
{
    if (gamma <= 0.) {
        OCIO_NAMESPACE::RangeTransformRcPtr range = OCIO_NAMESPACE::RangeTransform::Create();
        range->setMinInValue(1. - std::exp2(-16.));
        range->setMaxInValue(1.);
        range->setMinOutValue(0.);
        range->setMaxOutValue(1.);

        return range;
    }
    const double exponent = 1. / gamma;
    const double values[4] = { exponent, exponent, exponent, 1. };
    OCIO_NAMESPACE::ExponentTransformRcPtr power = OCIO_NAMESPACE::ExponentTransform::Create();
    power->setValue(values);

    return power;
}

std::string
exactText(double value)
{
    std::ostringstream os;
    os << std::hexfloat << value;

    return os.str();
}

/**
 * @brief configWithViewportView() of gain, then displayView, then gamma, as the 2D viewer applies
 * them. displayKey names displayView within config. Main thread only. Each gain and gamma is a
 * config of its own; a few are kept for toggling.
 **/
OCIO_NAMESPACE::ConstConfigRcPtr
cachedViewportConfig(const OCIO_NAMESPACE::ConstConfigRcPtr& config,
                     const std::string& displayKey,
                     double gain,
                     double gamma,
                     const OCIO_NAMESPACE::ConstTransformRcPtr& displayView)
{
    static std::map<std::string, OCIO_NAMESPACE::ConstConfigRcPtr> cache;
    const std::string key = displayKey + '\n' + exactText(gain) + '\n' + exactText(gamma);
    std::map<std::string, OCIO_NAMESPACE::ConstConfigRcPtr>::const_iterator found = cache.find(key);
    if (found != cache.end()) {
        return found->second;
    }
    const std::size_t kMaxConfigs = 8;
    if (cache.size() >= kMaxConfigs) {
        cache.clear();
    }
    OCIO_NAMESPACE::GroupTransformRcPtr chain = OCIO_NAMESPACE::GroupTransform::Create();
    if (gain != 1.) {
        chain->appendTransform(viewerGainTransform(gain));
    }
    chain->appendTransform(displayView->createEditableCopy());
    if (gamma != 1.) {
        chain->appendTransform(viewerGammaTransform(gamma));
    }
    OCIO_NAMESPACE::ConstConfigRcPtr result = configWithViewportView(config, chain);
    cache[key] = result;

    return result;
}

/**
 * @brief The sRGB encoding Hydra's colour correction applies without OpenColorIO, for when the
 * viewer's gain or gamma have to go with it, added to the config $OCIO names, the one OpenFX
 * plugins read unless told otherwise. Main thread only. Throws what OpenColorIO throws.
 **/
OCIO_NAMESPACE::ConstConfigRcPtr
srgbViewportConfig(double gain,
                   double gamma)
{
    static std::string loadedFrom;
    static OCIO_NAMESPACE::ConstConfigRcPtr base;
    const std::string env = TfGetenv("OCIO");
    if (!base || (env != loadedFrom)) {
        base = OCIO_NAMESPACE::Config::CreateFromEnv();
        loadedFrom = env;
    }
    OCIO_NAMESPACE::ExponentWithLinearTransformRcPtr encode = OCIO_NAMESPACE::ExponentWithLinearTransform::Create();
    const double exponent[4] = { 2.4, 2.4, 2.4, 1. };
    const double offset[4] = { 0.055, 0.055, 0.055, 0. };
    encode->setGamma(exponent);
    encode->setOffset(offset);
    encode->setDirection(OCIO_NAMESPACE::TRANSFORM_DIR_INVERSE);

    return cachedViewportConfig(base, std::string(base->getCacheID()) + "\nsRGB", gain, gamma, encode);
}

double
radiansToDegrees(double r)
{
    return r * 180. / M_PI;
}

/**
 * @brief Saves on construction, and restores on destruction, the GL state Storm (HgiGL and its
 * presentation into the target framebuffer) changes and the drawing around it relies on: the
 * attribute stacks, the bindings Storm leaves behind, and the capabilities the attribute stacks of
 * a compatibility context do not cover.
 **/
class StormGLStateGuard {
public:
    StormGLStateGuard()
        : _attribs(GL_ALL_ATTRIB_BITS)
    {
        glPushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &_drawFramebuffer);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &_readFramebuffer);
        glGetIntegerv(GL_RENDERBUFFER_BINDING, &_renderbuffer);
        glGetIntegerv(GL_CURRENT_PROGRAM, &_program);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &_vertexArray);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &_arrayBuffer);
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &_pixelPackBuffer);
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &_pixelUnpackBuffer);
        glGetIntegerv(GL_UNIFORM_BUFFER_BINDING, &_uniformBuffer);
        glGetIntegerv(GL_SHADER_STORAGE_BUFFER_BINDING, &_storageBuffer);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &_activeTexture);
        for (int i = 0; i < kNumUnits; ++i) {
            glActiveTexture(GL_TEXTURE0 + i);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &_texture2D[i]);
            glGetIntegerv(GL_SAMPLER_BINDING, &_sampler[i]);
        }
        glActiveTexture(_activeTexture);
        for (int i = 0; i < kNumCaps; ++i) {
            _capEnabled[i] = glIsEnabled(kCaps[i]);
        }
        if (GLAD_GL_VERSION_4_5) {
            glGetIntegerv(GL_CLIP_ORIGIN, &_clipOrigin);
            glGetIntegerv(GL_CLIP_DEPTH_MODE, &_clipDepthMode);
        }
    }

    ~StormGLStateGuard()
    {
        if (GLAD_GL_VERSION_4_5) {
            glClipControl(_clipOrigin, _clipDepthMode);
        }
        for (int i = 0; i < kNumCaps; ++i) {
            if (_capEnabled[i]) {
                glEnable(kCaps[i]);
            } else {
                glDisable(kCaps[i]);
            }
        }
        for (int i = 0; i < kNumUnits; ++i) {
            glActiveTexture(GL_TEXTURE0 + i);
            glBindTexture(GL_TEXTURE_2D, _texture2D[i]);
            glBindSampler(i, _sampler[i]);
        }
        glActiveTexture(_activeTexture);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, _storageBuffer);
        glBindBuffer(GL_UNIFORM_BUFFER, _uniformBuffer);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, _pixelUnpackBuffer);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, _pixelPackBuffer);
        glBindVertexArray(_vertexArray);
        glBindBuffer(GL_ARRAY_BUFFER, _arrayBuffer);
        glUseProgram(_program);
        glBindRenderbuffer(GL_RENDERBUFFER, _renderbuffer);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, _readFramebuffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, _drawFramebuffer);
        glPopClientAttrib();
        // _attribs pops the server attributes last.
    }

private:
    static const int kNumUnits = 8;
    static const int kNumCaps = 8;
    static const GLenum kCaps[kNumCaps];

    GLProtectAttrib _attribs;
    GLint _drawFramebuffer = 0;
    GLint _readFramebuffer = 0;
    GLint _renderbuffer = 0;
    GLint _program = 0;
    GLint _vertexArray = 0;
    GLint _arrayBuffer = 0;
    GLint _pixelPackBuffer = 0;
    GLint _pixelUnpackBuffer = 0;
    GLint _uniformBuffer = 0;
    GLint _storageBuffer = 0;
    GLint _activeTexture = GL_TEXTURE0;
    GLint _texture2D[kNumUnits] = {};
    GLint _sampler[kNumUnits] = {};
    GLboolean _capEnabled[kNumCaps] = {};
    GLint _clipOrigin = GL_LOWER_LEFT;
    GLint _clipDepthMode = GL_NEGATIVE_ONE_TO_ONE;
};

const GLenum StormGLStateGuard::kCaps[StormGLStateGuard::kNumCaps] = {
    GL_FRAMEBUFFER_SRGB, GL_PRIMITIVE_RESTART, GL_PRIMITIVE_RESTART_FIXED_INDEX, GL_PROGRAM_POINT_SIZE,
    GL_DEPTH_CLAMP, GL_TEXTURE_CUBE_MAP_SEAMLESS, GL_SAMPLE_ALPHA_TO_COVERAGE, GL_RASTERIZER_DISCARD
};

// Published so that a harness can hold the viewer to display states never editing the stage.
class StageNoticeCounter
    : public TfWeakBase {
public:
    void onObjectsChanged(const UsdNotice::ObjectsChanged& notice)
    {
        ++count;
        resyncedPaths += notice.GetResyncedPaths().size();
        infoOnlyPaths += notice.GetChangedInfoOnlyPaths().size();
    }

    std::atomic<U64> count { 0 };
    std::atomic<U64> resyncedPaths { 0 };
    std::atomic<U64> infoOnlyPaths { 0 };
};
} // anon namespace

std::atomic<U64> engineConstructionCount(0);

struct SceneViewportRenderer::Implementation {
    // The viewer lock: held while sync edits the stage, and while anything reads the stage or
    // touches the engine (whose stage scene index queues the stage's change notices without a
    // lock of its own, OpenUSD v26.08 usdImaging/stageSceneIndex.cpp). Shared by readers: OpenUSD
    // allows many, and a background frame is one that may run for seconds.
    std::shared_mutex stageMutex;

    // A try for stageMutex to edit failed, and the holders may be readers that announce no end.
    std::atomic<bool> exclusiveOwed { false };
    std::function<void()> readEnded;

    // For the main thread, and for work it waits on. Whoever made this fail lets the viewport know
    // when it lets go: edits and frames by their signals, other readers by readEnded.
    bool tryLockExclusive(std::unique_lock<std::shared_mutex>* lock)
    {
        *lock = std::unique_lock<std::shared_mutex>(stageMutex, std::try_to_lock);
        if (lock->owns_lock()) {
            return true;
        }
        exclusiveOwed = true;
        // The last reader may have gone before it could see the flag.
        if (lock->try_lock()) {
            exclusiveOwed = false;

            return true;
        }

        return false;
    }
    // Created by the first sync and kept for the renderer's life: the engine binds the first
    // stage it renders for good (v26.08 usdImagingGL/engine.cpp, PrepareBatch populates once).
    std::unique_ptr<SceneStage> sceneStage;
    // The same object, readable without the lock by those who only queue load requests.
    std::atomic<SceneStage*> loadStage { nullptr };
    SceneWriteBarrier::ParticipantId participant = 0;

    // Written by sync on whichever thread runs it, read by the main thread.
    std::atomic<bool> shown;
    std::atomic<U64> stageEditCount { 0 };
    std::atomic<U64> stageLoadEditCount { 0 };
    // The counts the engine last took in, by whichever render.
    std::atomic<U64> absorbedEditCount { 0 };
    std::atomic<U64> absorbedLoadEditCount { 0 };

    // Draw state deltas from the main thread, queued because applying one sends Hydra notices that
    // it must not wait for. drawRules is what a filter on a new engine starts from. drawMutex is
    // never held across a Hydra call, so the main thread's post cannot block on one.
    struct QueuedDrawDelta {
        SceneDrawDelta delta;
        std::size_t revealBatch;
    };
    mutable std::mutex drawMutex;
    std::vector<QueuedDrawDelta> drawQueue;
    DrawFilterStats drawStats;
    std::atomic<U64> drawPosted { 0 };
    std::atomic<U64> drawApplied { 0 };
    std::atomic<U64> drawAbsorbed { 0 };
    // A reveal shows one step per presented image; drawRevealStepDue says that image has been seen.
    std::atomic<bool> drawRevealing { false };
    std::atomic<bool> drawRevealStepDue { true };
    std::size_t drawRevealBatch = 0;
    SceneDrawFilter::Rules drawRules;
    SceneDrawFilter::WeakPtr drawFilter;
    StageNoticeCounter stageNotices;
    bool stageNoticesRegistered = false;

    std::atomic<bool> zUp;
    std::atomic<double> metersPerUnit;
    std::atomic<double> timeCodesPerSecond;
    std::atomic<double> frame;
    std::atomic<double> framesPerSecond;

    // Main thread only.
    DrawModeEnum drawMode = eDrawModeShaded;
    bool showGrid = true;
    bool showGuides = true;
    bool showProxy = true;
    bool showRender = false;
    bool showGuidePurpose = false;
    LightingModeEnum lightingMode = eLightingModeScene;
    bool sceneLit = false;
    FreeCamera freeCamera;
    ClipPlanes clipPlanes;
    // Read by the threads that work out the first framing.
    std::atomic<bool> framedOnce { false };
    std::string lookThrough;
    // The transform the camera at lookThroughOverridePath is drawn with instead of its prim's.
    std::string lookThroughOverridePath;
    GfMatrix4d lookThroughOverride = GfMatrix4d(1.);
    // The selected prims, the current one last.
    std::vector<std::string> selected;
    std::unique_ptr<UsdImagingGLEngine> engine;
    std::atomic<bool> engineHeld { false };
    std::atomic<U64> engineCount { 0 };
    // What the engine has yet to be told is selected: the main thread sets it without waiting for
    // whichever thread holds the engine.
    std::mutex engineSelectionMutex;
    std::vector<std::string> engineSelection;
    std::atomic<bool> engineSelectionDirty { false };
    // The delegate the next engine is made with, and why it could not be, if it could not.
    std::string rendererId = kRenderDelegateStormId;
    mutable std::mutex rendererErrorMutex;
    std::string rendererError;

    // What a background frame draws, copied on the thread that owns these settings, and what it left.
    struct BackgroundFrame {
        int width = 0;
        int height = 0;
        GfFrustum frustum;
        UsdImagingGLRenderParams params;
        LightingModeEnum lightingMode = eLightingModeScene;
        bool useOpenColorIO = false;
        DisplayTransform transform;
        float background[3] = { 0.f, 0.f, 0.f };

        // True while the fbo holds this frame's image and nothing has drawn over it.
        bool imageKept = false;
        unsigned int fbo = 0;
        U64 editCount = 0;
        U64 loadEditCount = 0;
        U64 previewGeneration = 0;
        U64 displayTransformGeneration = 0;
    };
    BackgroundFrame backgroundFrame;

    // What a traversal of the stage found, as of an edit count. The threads that change the stage
    // publish it before anyone is told of the change, and nothing else traverses, so no frame or
    // menu on the main thread walks a large stage.
    struct StageSummary {
        U64 editCount = 0;
        std::vector<SceneGuides::GuidePrim> guidePrims;
        bool hasLights = false;
        std::vector<std::string> cameraPaths;
    };
    mutable std::mutex summaryMutex;
    std::shared_ptr<const StageSummary> stageSummary;
    // Where a stage camera turns about. Kept from the last time the scene's bounds were worked
    // out: a heuristic, not worth a walk of the stage for each pose a drag asks for.
    GfRange3d pivotBounds;

    // The guides as of a summary and a time: need stageMutex.
    std::shared_ptr<const StageSummary> guidesSummary;
    std::vector<SceneGuides::Guide> guides;
    bool guidesValid = false;
    double guidesTimeCode = 0.;
    double guidesLength = 0.;
    std::atomic<double> guideSize { 1. };
    std::vector<DrawnGuide> drawnGuides;
    bool recordDrawnGuides = false;
    // Indices into guides, unselected cameras, unselected lights and selected ones, without the
    // look-through camera's.
    std::vector<int> guideBatches[3];
    // Cleared by the main thread when the selection changes, whichever thread holds the engine.
    std::atomic<bool> guideBatchesValid { false };

    DisplayTransform displayTransform;
    // What Hydra is given: displayTransform, or a baked copy of it when Hydra cannot draw it.
    DisplayTransform hydraTransform;
    bool useOpenColorIO = false;
    float backgroundDisplay[3] = { kBackgroundSrgb, kBackgroundSrgb, kBackgroundSrgb };
    // Set when the engine's colour correction task must be rebuilt from OCIO's current config.
    std::atomic<bool> colorCorrectionDirty { false };
    std::atomic<U64> displayTransformGeneration { 0 };

    // The transform handles, if any, and the drag of one under way.
    bool hasHandles = false;
    TransformHandles handles;
    SceneTransform::ToolEnum transformTool = SceneTransform::eToolSelect;
    SceneTransform::SpaceEnum transformSpace = SceneTransform::eSpaceWorld;
    SceneTransform::PivotEnum transformPivot = SceneTransform::ePivotSelectionCentre;
    int highlightedHandle = SceneGizmo::kNoHandle;
    int dragHandle = SceneGizmo::kNoHandle;
    SceneGizmo::Frame dragFrame;
    GfVec2d dragPressPixel;
    // A drag of the selection's widget moves the prims as they were at the press; a drag of an
    // op's handles changes the op's values, the op's result placed by dragOpSpace and each other
    // target's by its own space.
    bool dragSelection = false;
    std::vector<SceneTransformPrim> dragPrims;
    GfMatrix4d dragOpSpace = GfMatrix4d(1.);
    std::vector<std::pair<std::string, GfMatrix4d>> dragOpTargets;
    double dragChange = 0.;
    bool dragChanged = false;
    // The serial after which a synced scene has the drag written; kNoPreviewClear while none is
    // awaited. Set on the main thread, read by sync.
    static const U64 kNoPreviewClear = ~U64(0);
    std::atomic<U64> previewClearAfter;

    Implementation()
        : shown(false)
        , zUp(false)
        , metersPerUnit(0.01)
        , timeCodesPerSecond(24.)
        , frame(0.)
        , framesPerSecond(24.)
        , previewClearAfter(kNoPreviewClear)
    {
        freeCamera.azimuth = 35.;
        freeCamera.elevation = 25.;
    }

    // The stage shown, null when none is. Needs stageMutex.
    UsdStageRefPtr currentStage() const
    {
        return (shown && sceneStage) ? sceneStage->getStage() : UsdStageRefPtr();
    }

    std::shared_ptr<const StageSummary> getStageSummary() const
    {
        std::lock_guard<std::mutex> lock(summaryMutex);

        return stageSummary;
    }

    // Traverses the stage if it changed since the last summary. Needs stageMutex, shared being
    // enough: readers carry on meanwhile.
    void refreshStageSummary()
    {
        const UsdStageRefPtr stage = currentStage();
        std::shared_ptr<StageSummary> summary;

        if (stage) {
            // Loads compose prims too; both counts only grow, so their sum moves with either.
            const U64 editCount = sceneStage->getEditCount() + sceneStage->getLoadEditCount();
            const std::shared_ptr<const StageSummary> current = getStageSummary();
            if (current && (current->editCount == editCount)) {
                return;
            }
            summary = std::make_shared<StageSummary>();
            summary->editCount = editCount;
            summary->guidePrims = SceneGuides::findGuidePrims(stage, &summary->hasLights);
            for (const SceneGuides::GuidePrim& guidePrim : summary->guidePrims) {
                if (guidePrim.kind == SceneGuides::eGuideKindCamera) {
                    const UsdPrim prim = stage->GetPrimAtPath(guidePrim.path);
                    if (prim && !prim.IsInstanceProxy()) {
                        summary->cameraPaths.push_back(guidePrim.path.GetString());
                    }
                }
            }
        }
        std::lock_guard<std::mutex> lock(summaryMutex);
        stageSummary = summary;
    }

    double guideLength() const
    {
        const double size = guideSize.load();

        return (size > 0.) ? size : 1.;
    }

    // The guides at the shown time, built again when the stage or the time changed. Needs
    // stageMutex.
    const std::vector<SceneGuides::Guide>& currentGuides()
    {
        const std::shared_ptr<const StageSummary> summary = getStageSummary();
        const double time = timeCode();
        const double length = guideLength();
        if (!guidesValid || (guidesSummary != summary) || (guidesTimeCode != time) || (guidesLength != length)) {
            const UsdStageRefPtr stage = currentStage();
            guides = (stage && summary) ? SceneGuides::buildGuides(stage, summary->guidePrims, UsdTimeCode(time), length) : std::vector<SceneGuides::Guide>();
            guidesSummary = summary;
            guidesTimeCode = time;
            guidesLength = length;
            guidesValid = true;
            guideBatchesValid = false;
        }

        return guides;
    }

    const std::vector<int>* currentGuideBatches()
    {
        const std::vector<SceneGuides::Guide>& all = currentGuides();
        if (!guideBatchesValid) {
            for (std::vector<int>& batch : guideBatches) {
                batch.clear();
            }
            for (int i = 0; i < (int)all.size(); ++i) {
                const SceneGuides::Guide& guide = all[i];
                if (guide.path == lookThrough) {
                    continue;
                }
                const bool isCamera = (guide.kind == SceneGuides::eGuideKindCamera);
                guideBatches[isSelected(guide.path) ? 2 : (isCamera ? 0 : 1)].push_back(i);
            }
            guideBatchesValid = true;
        }

        return guideBatches;
    }

    bool isSelected(const std::string& path) const
    {
        return std::find(selected.begin(), selected.end(), path) != selected.end();
    }

    // Whether the shaded mode is lit by the stage's lights.
    bool usesSceneLights()
    {
        return usesSceneLights(lightingMode);
    }

    bool usesSceneLights(LightingModeEnum mode)
    {
        switch (mode) {
        case eLightingModeAuto: {
            const std::shared_ptr<const StageSummary> summary = getStageSummary();

            return summary && summary->hasLights && !hidesEveryLight();
        }
        case eLightingModeScene:
            return !hidesEveryLight();
        case eLightingModeHeadlight:
            return false;
        }

        return false;
    }

    // Hydra holds no light of the stage once every one is under a Hidden row, and nothing lights
    // the scene then. A stage with no light at all stays dark under Scene Lights, as hiding a mesh
    // must not light it. The headlight is a light in the filter's input too, so only stage prims
    // count, or adding it would reverse this on the next pass. Needs stageMutex and the engine.
    bool hidesEveryLight() const
    {
        const SceneDrawFilter* filter = get_pointer(drawFilter);
        if (!filter || !filter->hidesLights()) {
            return false;
        }
        const std::shared_ptr<const StageSummary> summary = getStageSummary();
        const UsdStageRefPtr stage = currentStage();
        if (!summary || !summary->hasLights || !stage) {
            return false;
        }

        return !filter->hasDrawnLights([&stage](const SdfPath& light) {
            return bool(stage->GetPrimAtPath(light));
        });
    }

    // The project frame mapped by seconds, as SceneOps::frameToTimeCode.
    double timeCode() const
    {
        const double fps = framesPerSecond.load();

        return (fps > 0.) ? (frame.load() * timeCodesPerSecond.load() / fps) : frame.load();
    }

    GfVec3d worldUp() const
    {
        return zUp ? GfVec3d(0., 0., 1.) : GfVec3d(0., 1., 0.);
    }

    GfVec3d pivot() const
    {
        return GfVec3d(freeCamera.pivot[0], freeCamera.pivot[1], freeCamera.pivot[2]);
    }

    // From the pivot towards the free camera's eye, unit length.
    GfVec3d eyeDirection() const
    {
        const double az = degreesToRadians(freeCamera.azimuth);
        const double el = degreesToRadians(freeCamera.elevation);
        const double ce = std::cos(el);

        if (zUp) {
            return GfVec3d(ce * std::sin(az), -ce * std::cos(az), std::sin(el));
        }

        return GfVec3d(ce * std::sin(az), std::sin(el), ce * std::cos(az));
    }

    bool isOrthographic() const
    {
        return freeCamera.viewMode != eViewModePerspective;
    }

    // The free camera's frame: back points from the pivot to the eye, up is the top of the view.
    // An orthographic view looks straight along its axis, with Top and Bottom upright as seen
    // from the front.
    void viewBasis(GfVec3d* back,
                   GfVec3d* up) const
    {
        const GfVec3d u = worldUp();
        // The axis the front view looks back along: +Z when Y is up, -Y when Z is.
        const GfVec3d front = zUp ? GfVec3d(0., -1., 0.) : GfVec3d(0., 0., 1.);
        const GfVec3d x(1., 0., 0.);

        *up = u;
        switch (freeCamera.viewMode) {
        case eViewModePerspective:
            *back = eyeDirection();
            break;
        case eViewModeFront:
            *back = front;
            break;
        case eViewModeBack:
            *back = -front;
            break;
        case eViewModeRight:
            *back = x;
            break;
        case eViewModeLeft:
            *back = -x;
            break;
        case eViewModeTop:
            *back = u;
            *up = -front;
            break;
        case eViewModeBottom:
            *back = -u;
            *up = front;
            break;
        }
    }

    // The free camera's eye, before an orthographic view backs it off.
    GfVec3d freeEye() const
    {
        GfVec3d back, up;

        viewBasis(&back, &up);

        return pivot() + back * std::max(freeCamera.distance, 1e-9);
    }

    void freeClipRange(double* nearClip,
                       double* farClip) const
    {
        const double distance = std::max(freeCamera.distance, 1e-9);

        if (clipPlanes.automatic) {
            *nearClip = distance * 0.01;
            *farClip = distance * 1000.;
        } else {
            *nearClip = clipPlanes.nearClip;
            *farClip = clipPlanes.farClip;
        }
    }

    // How far an orthographic view backs its eye off, so that what lies behind the eye of the
    // perspective view is still drawn; its clip distances are measured from the eye before that.
    double orthographicBackOff() const
    {
        return std::max(freeCamera.distance, 1e-9) * 500.;
    }

    // Needs stageMutex when looking through a camera.
    UsdGeomCamera lookThroughCamera() const
    {
        if (lookThrough.empty() || !SdfPath::IsValidPathString(lookThrough)) {
            return UsdGeomCamera();
        }
        const UsdStageRefPtr stage = currentStage();
        if (!stage) {
            return UsdGeomCamera();
        }
        UsdGeomCamera camera(stage->GetPrimAtPath(SdfPath(lookThrough)));

        return camera;
    }

    bool hasLookThroughOverride() const
    {
        return !lookThrough.empty() && (lookThroughOverridePath == lookThrough);
    }

    // The camera looked through at the shown time, drawn from the override when there is one.
    // Needs stageMutex.
    GfCamera lookThroughGfCamera(const UsdGeomCamera& camera) const
    {
        GfCamera gfCamera = camera.GetCamera(UsdTimeCode(timeCode()));

        if (hasLookThroughOverride()) {
            gfCamera.SetTransform(lookThroughOverride);
        }

        return gfCamera;
    }

    // Needs stageMutex.
    GfFrustum computeFrustum(int width,
                             int height) const
    {
        const double aspect = double(std::max(width, 1)) / double(std::max(height, 1));

        if (UsdGeomCamera camera = lookThroughCamera()) {
            GfCamera gfCamera = lookThroughGfCamera(camera);
            // Keeps the camera's vertical aperture, as usdview does without a camera mask.
            CameraUtilConformWindow(&gfCamera, CameraUtilMatchVertically, aspect);

            return gfCamera.GetFrustum();
        }

        const double distance = std::max(freeCamera.distance, 1e-9);
        GfVec3d back, up;
        viewBasis(&back, &up);
        double nearClip, farClip;
        freeClipRange(&nearClip, &farClip);
        GfFrustum frustum;
        if (!isOrthographic()) {
            GfMatrix4d view;
            view.SetLookAt(pivot() + back * distance, pivot(), up);
            frustum.SetPositionAndRotationFromMatrix(view.GetInverse());
            frustum.SetPerspective(freeCamera.fovY, aspect, nearClip, farClip);

            return frustum;
        }
        const double backOff = orthographicBackOff();
        GfMatrix4d view;
        view.SetLookAt(pivot() + back * (distance + backOff), pivot(), up);
        frustum.SetPositionAndRotationFromMatrix(view.GetInverse());
        const double halfHeight = distance * std::tan(degreesToRadians(freeCamera.fovY) * 0.5);
        const double nearDepth = clipPlanes.automatic ? backOff * 1e-3 : backOff + nearClip;
        frustum.SetOrthographic(-halfHeight * aspect, halfHeight * aspect, -halfHeight, halfHeight, nearDepth, backOff + farClip);

        return frustum;
    }

    UsdImagingGLRenderParams renderParams() const
    {
        UsdImagingGLRenderParams params;

        params.frame = UsdTimeCode(timeCode());
        params.clearColor = GfVec4f(kBackgroundLinear, kBackgroundLinear, kBackgroundLinear, 1.f);
        if (useOpenColorIO) {
            params.colorCorrectionMode = HdxColorCorrectionTokens->openColorIO;
            params.ocioDisplay = TfToken(hydraTransform.display);
            params.ocioView = TfToken(hydraTransform.view);
            params.ocioColorSpace = TfToken(hydraTransform.colorSpace);
        } else {
            params.colorCorrectionMode = HdxColorCorrectionTokens->sRGB;
        }
        params.highlight = true;
        params.showProxy = showProxy;
        params.showRender = showRender;
        params.showGuides = showGuidePurpose;
        params.enableSceneLights = sceneLit;
        switch (drawMode) {
        case eDrawModeUnshaded:
            params.drawMode = UsdImagingGLDrawMode::DRAW_SHADED_SMOOTH;
            params.enableLighting = false;
            break;
        case eDrawModeShaded:
            params.drawMode = UsdImagingGLDrawMode::DRAW_SHADED_SMOOTH;
            params.enableLighting = true;
            break;
        case eDrawModeWireframe:
            params.drawMode = UsdImagingGLDrawMode::DRAW_WIREFRAME;
            params.enableLighting = false;
            break;
        }

        return params;
    }

    // Needs an engine.
    void applySelection()
    {
        // The engine highlights differently from now on, so a kept image is out of date.
        backgroundFrame.imageKept = false;
        engineSelectionDirty = false;
        SdfPathVector paths;
        {
            std::lock_guard<std::mutex> lock(engineSelectionMutex);
            for (const std::string& path : engineSelection) {
                if (SdfPath::IsValidPathString(path)) {
                    paths.push_back(SdfPath(path));
                }
            }
        }
        if (paths.empty()) {
            engine->ClearSelected();
        } else {
            engine->SetSelected(paths);
        }
    }

    // Needs the GL context current and stageMutex. False if there is nothing to draw. The engine
    // is made once per GL context and kept through every edit of the stage: it hears of them
    // through the stage's notices and updates on its next render.
    bool ensureEngine()
    {
        if (!currentStage()) {
            return false;
        }
        if (engine) {
            if (engineSelectionDirty) {
                applySelection();
            }

            return true;
        }
        if (!getRendererErrorCopy().empty()) {
            return false;
        }
        setRendererErrorCopy(RenderDelegates::unavailableMessage(rendererId));
        if (!getRendererErrorCopy().empty()) {
            return false;
        }
        // Named even for Storm: with no id the engine takes the first delegate by priority that
        // loads, which need not be Storm.
        UsdImagingGLEngine::Parameters parameters;
        parameters.rendererPluginId = TfToken(rendererId);
        SceneDrawFilter::registerForAllRenderers();
        (void)SceneDrawFilter::takeAppendedInstanceId();
        {
            TfErrorMark mark;
            engine.reset(new UsdImagingGLEngine(parameters));
            ++engineCount;
            ++engineConstructionCount;
            if (engine->GetCurrentRendererId() != parameters.rendererPluginId) {
                std::string reasons;
                for (TfErrorMark::Iterator it = mark.GetBegin(); it != mark.GetEnd(); ++it) {
                    reasons += (reasons.empty() ? ": " : "; ") + it->GetCommentary();
                }
                mark.Clear();
                engine.reset();
                setRendererErrorCopy("The renderer " + rendererId + " could not be started" + reasons + ".");

                return false;
            }
        }
        drawFilter = SceneDrawFilter::find(SceneDrawFilter::takeAppendedInstanceId());
        if (SceneDrawFilter* filter = get_pointer(drawFilter)) {
            filter->seedRules(drawRules);
        }
        // Without a render output Storm draws straight into the bound framebuffer and the colour
        // correction task, which only acts on the color AOV, leaves scene-linear values on screen.
        engine->SetRendererAov(HdAovTokens->color);
        colorCorrectionDirty = true;
        engine->SetSelectionColor(GfVec4f(1.f, 0.8f, 0.1f, 1.f));
        applySelection();
        engineHeld = true;

        return true;
    }

    std::string getRendererErrorCopy() const
    {
        std::lock_guard<std::mutex> lock(rendererErrorMutex);

        return rendererError;
    }

    void setRendererErrorCopy(const std::string& error)
    {
        std::lock_guard<std::mutex> lock(rendererErrorMutex);

        rendererError = error;
    }

    // Needs stageMutex and a current GL context. Lets the engine's GL objects go; no Hgi to collect
    // with, as the engine's own goes with it, collecting as it does.
    void dropEngine()
    {
        StormGLSection section;

        engine.reset();
        drawFilter = SceneDrawFilter::WeakPtr();
        drawRevealing = false;
        engineHeld = false;
        backgroundFrame.imageKept = false;
    }

    // Needs stageMutex.
    void noteStageAbsorbed()
    {
        if (sceneStage) {
            absorbedEditCount = sceneStage->getEditCount();
            absorbedLoadEditCount = sceneStage->getLoadEditCount();
        }
        drawAbsorbed = drawApplied.load();
        if (!stageNoticesRegistered && sceneStage && sceneStage->getStage()) {
            stageNoticesRegistered = true;
            TfNotice::Register(TfCreateWeakPtr(&stageNotices), &StageNoticeCounter::onObjectsChanged, UsdStageWeakPtr(sceneStage->getStage()));
        }
        refreshDrawStats();
    }

    // Needs stageMutex. Reads the filter, which only the thread holding the engine may touch.
    void refreshDrawStats()
    {
        SceneDrawFilter* filter = get_pointer(drawFilter);
        std::lock_guard<std::mutex> lock(drawMutex);
        drawStats.filterBound = (filter != nullptr);
        drawStats.revealing = drawRevealing;
        drawStats.ruleCount = drawRules.size();
        if (!filter) {
            return;
        }
        drawStats.inputAdded = filter->getInputAddedCount();
        drawStats.inputRemoved = filter->getInputRemovedCount();
        drawStats.inputDirtied = filter->getInputDirtiedCount();
        drawStats.hiddenSent = filter->getHiddenSentCount();
        drawStats.shownSent = filter->getShownSentCount();
        if (drawStats.inputAdded != 0) {
            // The harness compares this with its own wall clock, and the filter keeps a steady one.
            const auto sinceAdded = SceneDrawFilter::Clock::now() - filter->getLastInputAddedTime();
            const auto addedAt = std::chrono::system_clock::now() - std::chrono::duration_cast<std::chrono::system_clock::duration>(sinceAdded);
            drawStats.lastInputAddedEpochMs = std::chrono::duration_cast<std::chrono::milliseconds>(addedAt.time_since_epoch()).count();
        }
    }

    // Needs stageMutex, the GL context current and an engine, between renders: the notices reach the
    // render index directly.
    void serviceDrawFilter()
    {
        std::vector<QueuedDrawDelta> queued;
        U64 posted = 0;
        {
            std::lock_guard<std::mutex> lock(drawMutex);
            queued.swap(drawQueue);
            posted = drawPosted;
        }
        SceneDrawFilter* filter = get_pointer(drawFilter);
        const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        for (const QueuedDrawDelta& entry : queued) {
            SceneDrawFilter::applyToRules(entry.delta, &drawRules);
            drawRevealBatch = entry.revealBatch;
            if (filter) {
                filter->applyDelta(entry.delta, entry.revealBatch);
            }
            drawRevealStepDue = true;
        }
        std::size_t stepped = 0;
        if (filter && filter->isRevealing() && drawRevealStepDue.exchange(false)) {
            stepped = filter->stepReveal(drawRevealBatch);
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        drawRevealing = filter && filter->isRevealing();
        drawApplied = posted;
        if (!queued.empty() || (stepped != 0)) {
            std::lock_guard<std::mutex> lock(drawMutex);
            drawStats.lastApplyMs = ms;
        }
        refreshDrawStats();
    }

    // The GL state both kinds of frame start from.
    static void bindFrameTarget(unsigned int fbo,
                                int width,
                                int height)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, width, height);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_FRAMEBUFFER_SRGB);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
    }

    static void clearFrameTarget(const float background[3])
    {
        glClearColor(background[0], background[1], background[2], 1.f);
        glClearDepth(1.);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }

    // Takes the image a background frame left, if it is what this render would draw. It is good for
    // one render: the overlays go on top of it. Needs stageMutex.
    bool takeBackgroundImage(unsigned int fbo,
                             int width,
                             int height,
                             const GfFrustum& frustum,
                             const UsdImagingGLRenderParams& params)
    {
        BackgroundFrame& frame = backgroundFrame;
        const bool kept = frame.imageKept;

        frame.imageKept = false;

        return kept && !colorCorrectionDirty && sceneStage && (frame.fbo == fbo) && (frame.width == width) && (frame.height == height)
            && (frame.frustum.ComputeViewMatrix() == frustum.ComputeViewMatrix())
            && (frame.frustum.ComputeProjectionMatrix() == frustum.ComputeProjectionMatrix())
            && (frame.params == params)
            && std::equal(frame.background, frame.background + 3, backgroundDisplay)
            && (frame.editCount == sceneStage->getEditCount())
            && (frame.loadEditCount == sceneStage->getLoadEditCount())
            && (frame.previewGeneration == sceneStage->getPreviewGeneration());
    }

    // Needs the GL context current and an engine. Hydra's colour correction task reads OCIO's
    // process-wide current config when its parameters change (v26.08
    // hdx/colorCorrectionTask.cpp), so the config is made current here, and a config that changed
    // under unchanged names forces new parameters by passing through disabled.
    void prepareColorCorrection(bool openColorIO,
                                const DisplayTransform& transform)
    {
        if (!colorCorrectionDirty.exchange(false)) {
            return;
        }
        if (openColorIO) {
            try {
                OCIO_NAMESPACE::SetCurrentConfig(transform.config);
            } catch (const std::exception& e) {
                qWarning() << "SceneViewport: OpenColorIO:" << e.what();
            }
        }
        engine->SetColorCorrectionSettings(HdxColorCorrectionTokens->disabled);
    }

    // A light at the camera, as usdview's default camera light, or none when the stage's own
    // lights light the scene (Storm adds those itself with enableSceneLights). No ambient then
    // either, so a scene without lights shows its lit surfaces black.
    void setLights(bool lit)
    {
        GlfSimpleLight light;

        light.SetIsCameraSpaceLight(true);
        light.SetPosition(GfVec4f(0.f, 0.f, 0.f, 1.f));
        light.SetAmbient(GfVec4f(0.f, 0.f, 0.f, 1.f));
        light.SetDiffuse(GfVec4f(1.f, 1.f, 1.f, 1.f));
        light.SetSpecular(GfVec4f(0.2f, 0.2f, 0.2f, 1.f));
        GlfSimpleMaterial material;
        material.SetAmbient(GfVec4f(0.2f, 0.2f, 0.2f, 1.f));
        material.SetSpecular(GfVec4f(0.1f, 0.1f, 0.1f, 1.f));
        material.SetShininess(32.);
        const float ambient = lit ? 0.f : 0.01f;
        engine->SetLightingState(lit ? GlfSimpleLightVector() : GlfSimpleLightVector(1, light), material, GfVec4f(ambient, ambient, ambient, 1.f));
    }

    void setCamera(const GfFrustum& frustum,
                   int width,
                   int height,
                   unsigned int fbo,
                   bool lit)
    {
        engine->SetRenderBufferSize(GfVec2i(width, height));
        engine->SetFraming(CameraUtilFraming(GfRect2i(GfVec2i(0, 0), width, height)));
        engine->SetCameraState(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix());
        engine->SetPresentationOutput(HgiTokens->OpenGL, VtValue(uint32_t(fbo)));
        setLights(lit);
    }

    // Needs stageMutex: a camera looked through is a prim of the stage.
    SceneGizmo::View makeView(int width,
                              int height) const
    {
        const GfFrustum frustum = computeFrustum(width, height);

        return SceneGizmo::View(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix(), width, height);
    }

    // The selected prims the transform widget moves. Needs stageMutex.
    std::vector<UsdPrim> transformablePrims() const
    {
        return SceneTransformWidget::getTransformablePrims(currentStage(), selected);
    }

    // The widget on the selection, while its tool shows handles. Needs stageMutex.
    bool computeSelectionFrame(SceneGizmo::Frame* frame) const
    {
        if ((transformTool == SceneTransform::eToolSelect) || !currentStage()) {
            return false;
        }

        return SceneTransformWidget::computeFrame(transformablePrims(), timeCode(), transformSpace, transformPivot, frame);
    }

    // The op's values after a drag by change of handle.
    void changeOpValues(int handle,
                        double change,
                        GfVec3d* translate,
                        GfVec3d* rotate,
                        GfVec3d* scale) const
    {
        *translate = GfVec3d(handles.translate[0], handles.translate[1], handles.translate[2]);
        *rotate = GfVec3d(handles.rotate[0], handles.rotate[1], handles.rotate[2]);
        *scale = GfVec3d(handles.scale[0], handles.scale[1], handles.scale[2]);
        if ((handle < 0) || (handle >= SceneGizmo::kNumHandles)) {
            return;
        }
        const int axis = SceneGizmo::getHandleAxis(handle);
        switch (SceneGizmo::getHandleKind(handle)) {
        case SceneGizmo::eHandleKindTranslate:
            (*translate)[axis] += change;
            break;
        case SceneGizmo::eHandleKindRotate:
            (*rotate)[axis] += change;
            break;
        case SceneGizmo::eHandleKindScale:
            (*scale)[axis] *= change;
            break;
        }
    }

    GfMatrix4d composeOp(const GfVec3d& translate,
                         const GfVec3d& rotate,
                         const GfVec3d& scale) const
    {
        return SceneNodeAuthoring::composeTransformMatrix(translate, rotate, scale * handles.uniformScale,
                                                          GfVec3d(handles.pivot[0], handles.pivot[1], handles.pivot[2]),
                                                          (SceneRotateOrderEnum)handles.rotateOrder);
    }

    // The space placing the result of opName on primPath. Needs stageMutex.
    bool computeTargetSpace(const std::string& primPath,
                            const std::string& opName,
                            GfMatrix4d* space) const
    {
        const UsdStageRefPtr stage = currentStage();
        if (!stage || !SdfPath::IsValidPathString(primPath)) {
            return false;
        }
        const SdfPath path(primPath);
        if (!path.IsAbsolutePath() || !path.IsPrimPath()) {
            return false;
        }

        return SceneGizmo::computeOpSpace(stage->GetPrimAtPath(path), opName, timeCode(), space);
    }

    // Where an op's handles sit at the shown time, moved by a drag of them under way. Needs
    // stageMutex.
    bool computeOpFrame(SceneGizmo::Frame* frame) const
    {
        if (!hasHandles) {
            return false;
        }
        const bool dragged = (dragHandle != SceneGizmo::kNoHandle) && !dragSelection;
        GfMatrix4d space = dragOpSpace;
        if (!dragged && !computeTargetSpace(handles.primPath, handles.opName, &space)) {
            return false;
        }
        GfVec3d translate, rotate, scale;
        changeOpValues(dragged ? dragHandle : SceneGizmo::kNoHandle, dragChange, &translate, &rotate, &scale);
        *frame = SceneGizmo::computeFrame(space, translate, rotate,
                                          GfVec3d(handles.pivot[0], handles.pivot[1], handles.pivot[2]),
                                          (SceneRotateOrderEnum)handles.rotateOrder);

        return true;
    }

    /**
     * @brief Where the handles drawn sit at the shown time: the selection's widget, else an op's.
     * *selection tells which. False if none are drawn. Needs stageMutex.
     **/
    bool computeHandleFrame(SceneGizmo::Frame* frame,
                            bool* selection) const
    {
        if (computeSelectionFrame(frame)) {
            *selection = true;

            return true;
        }
        *selection = false;

        return computeOpFrame(frame);
    }

    bool computeHandleFrame(SceneGizmo::Frame* frame) const
    {
        bool selection = false;

        return computeHandleFrame(frame, &selection);
    }

    // The handles drawn and picked, length world units long. Needs stageMutex.
    std::vector<SceneGizmo::HandleShape> buildDrawnHandles(const SceneGizmo::Frame& frame,
                                                           bool selection,
                                                           double length) const
    {
        std::vector<SceneGizmo::HandleShape> shapes = SceneGizmo::buildHandles(frame, length);

        return selection ? SceneTransformWidget::filterHandles(shapes, transformTool) : shapes;
    }

    bool isDrawnHandle(int handle,
                       bool selection) const
    {
        return selection ? SceneTransformWidget::isToolHandle(transformTool, handle) : ((handle >= 0) && (handle < SceneGizmo::kNumHandles));
    }

    // Drops the drag preview. Needs stageMutex.
    void clearPreviewLocked()
    {
        previewClearAfter = kNoPreviewClear;
        if (sceneStage && sceneStage->hasPreviewTransforms()) {
            sceneStage->clearPreviewTransforms();
            guidesValid = false;
        }
    }

    // The handles in X red, Y green and Z blue, the highlighted or dragged one yellow, drawn over
    // everything. Needs stageMutex.
    void drawTransformHandles(const GfFrustum& frustum,
                              int width,
                              int height) const
    {
        SceneGizmo::Frame frame;
        bool selection = false;

        if (!computeHandleFrame(&frame, &selection)) {
            return;
        }
        const SceneGizmo::View view(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix(), width, height);
        const double length = SceneGizmo::getHandleLength(view, frame);
        if (!(length > 0.) || !std::isfinite(length)) {
            return;
        }
        const int active = (dragHandle != SceneGizmo::kNoHandle) ? dragHandle : highlightedHandle;

        glMatrixMode(GL_PROJECTION);
        glPushMatrix();
        glLoadMatrixd(frustum.ComputeProjectionMatrix().GetArray());
        glMatrixMode(GL_MODELVIEW);
        glPushMatrix();
        glLoadMatrixd(frustum.ComputeViewMatrix().GetArray());

        glDisable(GL_LIGHTING);
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_CULL_FACE);
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glEnable(GL_LINE_SMOOTH);

        static const float axisColours[3][3] = { { 0.9f, 0.25f, 0.25f }, { 0.3f, 0.8f, 0.3f }, { 0.3f, 0.45f, 0.95f } };
        if (selection && (transformTool == SceneTransform::eToolScale)) {
            // Stems to the scale boxes, which are not handles themselves, as thick as the arrows.
            glLineWidth(3.f);
            for (const SceneGizmo::HandleShape& shape : SceneGizmo::buildHandles(frame, length)) {
                if (SceneGizmo::getHandleKind(shape.handle) != SceneGizmo::eHandleKindTranslate) {
                    continue;
                }
                const float* colour = axisColours[SceneGizmo::getHandleAxis(shape.handle)];
                glColor4f(colour[0], colour[1], colour[2], 0.85f);
                glBegin(GL_LINES);
                glVertex3d(frame.origin[0], frame.origin[1], frame.origin[2]);
                glVertex3d(shape.points[1][0], shape.points[1][1], shape.points[1][2]);
                glEnd();
            }
        }
        std::vector<SceneGizmo::HandleShape> shapes = buildDrawnHandles(frame, selection, length);
        // Rings under arrows under boxes, as hitTest() favours the boxes.
        std::stable_sort(shapes.begin(), shapes.end(), [](const SceneGizmo::HandleShape& a, const SceneGizmo::HandleShape& b) {
            static const int layer[3] = { 1, 0, 2 }; // translate, rotate, scale
            return layer[SceneGizmo::getHandleKind(a.handle)] < layer[SceneGizmo::getHandleKind(b.handle)];
        });
        for (const SceneGizmo::HandleShape& shape : shapes) {
            const float* colour = axisColours[SceneGizmo::getHandleAxis(shape.handle)];
            if (shape.handle == active) {
                glColor4f(1.f, 0.85f, 0.1f, 1.f);
            } else {
                glColor4f(colour[0], colour[1], colour[2], 1.f);
            }
            const SceneGizmo::HandleKindEnum kind = SceneGizmo::getHandleKind(shape.handle);
            if (kind == SceneGizmo::eHandleKindScale) {
                glBegin(GL_QUADS);
            } else {
                glLineWidth((kind == SceneGizmo::eHandleKindTranslate) ? 3.f : 2.f);
                glBegin(shape.closed ? GL_LINE_LOOP : GL_LINE_STRIP);
            }
            for (const GfVec3d& p : shape.points) {
                glVertex3d(p[0], p[1], p[2]);
            }
            glEnd();
        }
        glLineWidth(1.f);

        glMatrixMode(GL_MODELVIEW);
        glPopMatrix();
        glMatrixMode(GL_PROJECTION);
        glPopMatrix();
    } // drawTransformHandles

    // A grid on the ground plane through the origin, with lines a power of ten metres apart, about
    // a tenth to half the camera's distance, and the two ground axes coloured. Depth-tested against
    // Storm's presented depth.
    void drawGrid(const GfFrustum& frustum) const
    {
        const double mpu = (metersPerUnit.load() > 0.) ? metersPerUnit.load() : 0.01;
        // An orthographic view's eye is backed far off: measure from where it would be.
        const GfVec3d eye = lookThroughCamera() ? frustum.GetPosition() : freeEye();
        const double distance = std::max(eye.GetLength(), freeCamera.distance);
        const double distanceMeters = std::max(distance * mpu, 1e-6);
        const double spacing = std::pow(10., std::floor(std::log10(distanceMeters * 0.5))) / mpu;
        const int numLines = 25;
        const double halfExtent = spacing * numLines;

        glMatrixMode(GL_PROJECTION);
        glPushMatrix();
        glLoadMatrixd(frustum.ComputeProjectionMatrix().GetArray());
        glMatrixMode(GL_MODELVIEW);
        glPushMatrix();
        glLoadMatrixd(frustum.ComputeViewMatrix().GetArray());

        glDisable(GL_LIGHTING);
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_CULL_FACE);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_FALSE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glLineWidth(1.f);

        // Ground coordinates (a, b) to world: XZ for a Y-up stage, XY for a Z-up one.
        auto vertex = [this](double a, double b) {
            if (zUp) {
                glVertex3d(a, b, 0.);
            } else {
                glVertex3d(a, 0., b);
            }
        };

        glBegin(GL_LINES);
        for (int i = -numLines; i <= numLines; ++i) {
            const double t = i * spacing;
            if (i == 0) {
                continue;
            }
            glColor4f(0.5f, 0.5f, 0.5f, 0.5f);
            vertex(t, -halfExtent);
            vertex(t, halfExtent);
            vertex(-halfExtent, t);
            vertex(halfExtent, t);
        }
        // X in red; the other ground axis (Z for Y-up, Y for Z-up) in its usual colour.
        glColor4f(0.8f, 0.25f, 0.25f, 0.9f);
        vertex(-halfExtent, 0.);
        vertex(halfExtent, 0.);
        if (zUp) {
            glColor4f(0.3f, 0.75f, 0.3f, 0.9f);
        } else {
            glColor4f(0.3f, 0.4f, 0.9f, 0.9f);
        }
        vertex(0., -halfExtent);
        vertex(0., halfExtent);
        glEnd();

        glMatrixMode(GL_MODELVIEW);
        glPopMatrix();
        glMatrixMode(GL_PROJECTION);
        glPopMatrix();
    }

    void recordDrawnGuide(const SceneGizmo::View& view,
                          const SceneGuides::Guide& guide,
                          int width,
                          int height)
    {
        for (std::size_t k = 0; k + 1 < guide.segments.size(); k += 2) {
            GfVec2d a, b;
            if (!SceneGizmo::project(view, guide.segments[k], &a) || !SceneGizmo::project(view, guide.segments[k + 1], &b)) {
                continue;
            }
            const GfVec2d middle = (a + b) * 0.5;
            if ((middle[0] >= 0.) && (middle[0] < width) && (middle[1] >= 0.) && (middle[1] < height)) {
                DrawnGuide drawn;
                drawn.path = guide.path;
                drawn.kind = SceneGuides::getGuideKindName(guide.kind);
                drawn.x = middle[0];
                drawn.y = middle[1];
                drawnGuides.push_back(drawn);
                break;
            }
        }
    }

    // The camera and light guides, but the camera looked through's, as one batch of lines per
    // colour, depth-tested against Storm's presented depth; selected ones in the selection colour.
    // Needs stageMutex.
    void drawGuides(const GfFrustum& frustum,
                    int width,
                    int height)
    {
        drawnGuides.clear();
        const std::vector<SceneGuides::Guide>& all = currentGuides();
        if (all.empty()) {
            return;
        }
        const std::vector<int>* batches = currentGuideBatches();
        if (recordDrawnGuides) {
            const SceneGizmo::View view(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix(), width, height);
            for (int pass = 0; pass < 3; ++pass) {
                for (int index : batches[pass]) {
                    recordDrawnGuide(view, all[index], width, height);
                }
            }
        }

        glMatrixMode(GL_PROJECTION);
        glPushMatrix();
        glLoadMatrixd(frustum.ComputeProjectionMatrix().GetArray());
        glMatrixMode(GL_MODELVIEW);
        glPushMatrix();
        glLoadMatrixd(frustum.ComputeViewMatrix().GetArray());

        glDisable(GL_LIGHTING);
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_CULL_FACE);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_FALSE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        // Unselected cameras, unselected lights, then the selection drawn thicker over them.
        for (int pass = 0; pass < 3; ++pass) {
            if (pass == 0) {
                glColor4f(0.45f, 0.85f, 0.85f, 1.f);
            } else if (pass == 1) {
                glColor4f(0.95f, 0.9f, 0.55f, 1.f);
            } else {
                glColor4f(1.f, 0.8f, 0.1f, 1.f);
            }
            glLineWidth((pass == 2) ? 2.f : 1.f);
            glBegin(GL_LINES);
            for (int index : batches[pass]) {
                for (const GfVec3d& p : all[index].segments) {
                    glVertex3d(p[0], p[1], p[2]);
                }
            }
            glEnd();
        }
        glLineWidth(1.f);

        glMatrixMode(GL_MODELVIEW);
        glPopMatrix();
        glMatrixMode(GL_PROJECTION);
        glPopMatrix();
    } // drawGuides
};

SceneViewportRenderer::SceneViewportRenderer()
    : _imp(new Implementation)
{
    // A layer reload edits layers the stage composes: it waits until no paint, pick or sync is
    // reading the stage, and keeps them out until it is done.
    std::shared_mutex* stageMutex = &_imp->stageMutex;
    _imp->participant = SceneWriteBarrier::registerParticipant([stageMutex]() { stageMutex->lock(); },
                                                               [stageMutex]() { stageMutex->unlock(); });
}

SceneViewportRenderer::~SceneViewportRenderer()
{
    SceneWriteBarrier::unregisterParticipant(_imp->participant);
    // The engine owns GL objects: whoever owns this must have called releaseGL() in its context.
    if (_imp->engine) {
        qDebug() << "SceneViewportRenderer destroyed without releaseGL(); leaking its Storm engine";
        (void)_imp->engine.release();
    }
}

bool
SceneViewportRenderer::syncScene(const ScenePayload& payload,
                                 std::string* error,
                                 U64 serial)
{
    return syncScene(payload, -1., 0., error, serial);
}

bool
SceneViewportRenderer::syncScene(const ScenePayload& payload,
                                 double frame,
                                 double framesPerSecond,
                                 std::string* error,
                                 U64 serial)
{
    std::string runtimeError;

    if (!SceneOps::checkUsdRuntime(&runtimeError)) {
        _imp->shown = false;
        if (error) {
            *error = runtimeError;
        }

        return false;
    }

    // Before the viewer lock, the order every reader takes them in.
    SceneWriteBarrier::ReadScope read;
    {
        std::lock_guard<std::shared_mutex> lock(_imp->stageMutex);
        if (!_imp->sceneStage) {
            // The viewer shows what was asked for: nothing loads until a load is requested.
            _imp->sceneStage.reset(new SceneStage(UsdStage::LoadNone));
            _imp->loadStage = _imp->sceneStage.get();
        }
        if (!_imp->sceneStage->isValid()) {
            _imp->shown = false;
            if (error) {
                *error = "OpenUSD could not open a stage for the scene.";
            }

            return false;
        }
        if ((serial != 0) && (serial > _imp->previewClearAfter.load())) {
            // In the same step as the sync, so no frame shows the scene without the drag.
            _imp->clearPreviewLocked();
        }
        _imp->sceneStage->sync(payload);
        if (framesPerSecond > 0.) {
            _imp->frame = frame;
            _imp->framesPerSecond = framesPerSecond;
        }
        const SceneMetadata& metadata = payload.getMetadata();
        _imp->zUp = (metadata.upAxis == SceneMetadata::eUpAxisZ);
        _imp->metersPerUnit = metadata.metersPerUnit;
        _imp->timeCodesPerSecond = metadata.timeCodesPerSecond;
        _imp->shown = !payload.isEmpty();
        _imp->stageEditCount = _imp->sceneStage->getEditCount();
    }
    std::shared_lock<std::shared_mutex> lock(_imp->stageMutex);
    _imp->refreshStageSummary();

    return true;
}

bool
SceneViewportRenderer::setScene(const ScenePayload& payload,
                                double frame,
                                double framesPerSecond,
                                std::string* error)
{
    setFrame(frame, framesPerSecond);
    if (!syncScene(payload, error)) {
        return false;
    }
    if (!_imp->framedOnce) {
        frameScene();
    }

    return true;
}

void
SceneViewportRenderer::setFrame(double frame,
                                double framesPerSecond)
{
    _imp->frame = frame;
    _imp->framesPerSecond = framesPerSecond;
}

void
SceneViewportRenderer::clearScene()
{
    // Keeps the stage and the engine: the next scene is synced into them.
    syncScene(ScenePayload(), nullptr);
}

bool
SceneViewportRenderer::hasScene() const
{
    return _imp->shown;
}

void
SceneViewportRenderer::readStage(const std::function<void(const SceneStage* stage)>& read) const
{
    SceneWriteBarrier::ReadScope readScope;
    {
        std::shared_lock<std::shared_mutex> lock(_imp->stageMutex);
        const bool shown = _imp->shown && _imp->sceneStage && _imp->sceneStage->isValid();
        read(shown ? _imp->sceneStage.get() : nullptr);
    }
    if (_imp->exclusiveOwed.exchange(false) && _imp->readEnded) {
        _imp->readEnded();
    }
}

bool
SceneViewportRenderer::tryReadStage(const std::function<void(const SceneStage* stage)>& read) const
{
    SceneWriteBarrier::ReadScope readScope;
    std::shared_lock<std::shared_mutex> lock(_imp->stageMutex, std::try_to_lock);

    if (!lock.owns_lock()) {
        return false;
    }
    const bool shown = _imp->shown && _imp->sceneStage && _imp->sceneStage->isValid();
    read(shown ? _imp->sceneStage.get() : nullptr);

    return true;
}

void
SceneViewportRenderer::setReadEndedCallback(const std::function<void()>& callback)
{
    _imp->readEnded = callback;
}

U64
SceneViewportRenderer::getStageEditCount() const
{
    return _imp->stageEditCount;
}

double
SceneViewportRenderer::getTimeCode() const
{
    return _imp->timeCode();
}

bool
SceneViewportRenderer::isFramed() const
{
    return _imp->framedOnce;
}

U64
SceneViewportRenderer::getEngineCount() const
{
    return _imp->engineCount.load();
}

bool
SceneViewportRenderer::hasEngine() const
{
    return _imp->engineHeld;
}

bool
SceneViewportRenderer::needsBackgroundFrame() const
{
    if (!_imp->shown || !_imp->loadStage.load() || !_imp->getRendererErrorCopy().empty()) {
        return false;
    }

    return !_imp->engineHeld || (_imp->absorbedEditCount != _imp->stageEditCount) || (_imp->absorbedLoadEditCount != _imp->stageLoadEditCount)
        || (_imp->drawAbsorbed != _imp->drawPosted) || (_imp->drawRevealing && _imp->drawRevealStepDue);
}

void
SceneViewportRenderer::postDrawDelta(const SceneDrawDelta& delta,
                                     std::size_t revealBatch)
{
    std::lock_guard<std::mutex> lock(_imp->drawMutex);
    _imp->drawQueue.push_back(Implementation::QueuedDrawDelta { delta, revealBatch });
    ++_imp->drawPosted;
}

void
SceneViewportRenderer::applyPendingDrawDeltas()
{
    // Before the viewer lock, the order every reader takes them in.
    SceneWriteBarrier::ReadScope read;
    {
        std::shared_lock<std::shared_mutex> lock(_imp->stageMutex);
        // Without an engine the queue waits for the frame that makes one.
        if (_imp->engine) {
            StormGLStateGuard guard;
            StormGLSection section;
            section.collectWith(_imp->engine->GetHgi());
            _imp->serviceDrawFilter();
        }
    }
    if (_imp->exclusiveOwed.exchange(false) && _imp->readEnded) {
        _imp->readEnded();
    }
}

bool
SceneViewportRenderer::isDrawRevealing() const
{
    return _imp->drawRevealing;
}

void
SceneViewportRenderer::noteDrawRevealPresented()
{
    _imp->drawRevealStepDue = true;
}

SceneViewportRenderer::DrawFilterStats
SceneViewportRenderer::getDrawFilterStats() const
{
    std::lock_guard<std::mutex> lock(_imp->drawMutex);
    DrawFilterStats stats = _imp->drawStats;
    stats.posted = _imp->drawPosted;
    stats.absorbed = _imp->drawAbsorbed;
    stats.stageEdits = _imp->stageEditCount;
    stats.stageLoadEdits = _imp->stageLoadEditCount;
    stats.stageNotices = _imp->stageNotices.count;
    stats.resyncedPaths = _imp->stageNotices.resyncedPaths;
    stats.infoOnlyPaths = _imp->stageNotices.infoOnlyPaths;

    return stats;
}

bool
SceneViewportRenderer::prepareBackgroundFrame(int width,
                                              int height)
{
    if ((width <= 0) || (height <= 0) || !AppManager::instance()->isOpenGLLoaded() || !needsBackgroundFrame()) {
        return false;
    }
    // A camera looked through is a prim of the stage.
    std::shared_lock<std::shared_mutex> lock(_imp->stageMutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        return false;
    }
    Implementation::BackgroundFrame& frame = _imp->backgroundFrame;
    frame.width = width;
    frame.height = height;
    frame.frustum = _imp->computeFrustum(width, height);
    frame.params = _imp->renderParams();
    frame.lightingMode = _imp->lightingMode;
    frame.useOpenColorIO = _imp->useOpenColorIO;
    frame.transform = _imp->hydraTransform;
    frame.displayTransformGeneration = _imp->displayTransformGeneration;
    std::copy(_imp->backgroundDisplay, _imp->backgroundDisplay + 3, frame.background);
    frame.imageKept = false;

    return true;
}

void
SceneViewportRenderer::renderBackgroundFrame(unsigned int fbo)
{
    Implementation::BackgroundFrame& frame = _imp->backgroundFrame;
    // Before the viewer lock, the order every reader takes them in.
    SceneWriteBarrier::ReadScope read;

    std::shared_lock<std::shared_mutex> lock(_imp->stageMutex);
    StormGLStateGuard guard;
    StormGLSection section;

    Implementation::bindFrameTarget(fbo, frame.width, frame.height);
    Implementation::clearFrameTarget(frame.background);
    if (!_imp->ensureEngine()) {
        return;
    }
    section.collectWith(_imp->engine->GetHgi());
    _imp->serviceDrawFilter();
    frame.params.enableSceneLights = _imp->usesSceneLights(frame.lightingMode);
    Implementation::bindFrameTarget(fbo, frame.width, frame.height);

    const UsdStageRefPtr stage = _imp->currentStage();
    _imp->setCamera(frame.frustum, frame.width, frame.height, fbo, frame.params.enableSceneLights);
    _imp->prepareColorCorrection(frame.useOpenColorIO, frame.transform);
    if (frame.displayTransformGeneration != _imp->displayTransformGeneration) {
        // Set while this frame ran, whose copy is the older one: the next render has to apply it.
        _imp->colorCorrectionDirty = true;
    }
    bool converged = false;
    for (int i = 0; (i < kMaxRenderPasses) && !converged; ++i) {
        _imp->engine->Render(stage->GetPseudoRoot(), frame.params);
        converged = _imp->engine->IsConverged();
        // The first render is what puts the stage's lights into Hydra, and so into the filter.
        const bool lit = _imp->usesSceneLights(frame.lightingMode);
        if (lit != frame.params.enableSceneLights) {
            frame.params.enableSceneLights = lit;
            _imp->setLights(lit);
            converged = false;
        }
    }
    _imp->noteStageAbsorbed();
    frame.fbo = fbo;
    frame.editCount = _imp->sceneStage->getEditCount();
    frame.loadEditCount = _imp->sceneStage->getLoadEditCount();
    frame.previewGeneration = _imp->sceneStage->getPreviewGeneration();
    frame.imageKept = converged;
}

void
SceneViewportRenderer::requestStageLoad(const std::vector<std::string>& paths,
                                        bool withDescendants)
{
    SceneStage* stage = _imp->loadStage;

    if (stage) {
        stage->requestLoad(toSdfPaths(paths), withDescendants);
    }
}

void
SceneViewportRenderer::requestStageUnload(const std::vector<std::string>& paths)
{
    SceneStage* stage = _imp->loadStage;

    if (stage) {
        stage->requestUnload(toSdfPaths(paths));
    }
}

void
SceneViewportRenderer::cancelStageLoads()
{
    SceneStage* stage = _imp->loadStage;

    if (stage) {
        stage->cancelPendingLoads();
    }
}

bool
SceneViewportRenderer::hasPendingStageLoads() const
{
    const SceneStage* stage = _imp->loadStage;

    return stage && stage->hasPendingLoads();
}

bool
SceneViewportRenderer::serviceStageLoads()
{
    SceneStage* stage = _imp->loadStage;

    if (!stage) {
        return false;
    }
    Implementation* imp = _imp.get();

    const bool serviced = stage->serviceLoads([imp](const std::function<void()>& section) {
        SceneWriteBarrier::ReadScope read;
        std::shared_lock<std::shared_mutex> lock(imp->stageMutex);
        section(); }, [imp, stage](const std::function<void()>& section) {
        SceneWriteBarrier::ReadScope read;
        std::lock_guard<std::shared_mutex> lock(imp->stageMutex);
        section();
        imp->stageLoadEditCount = stage->getLoadEditCount(); });
    SceneWriteBarrier::ReadScope read;
    std::shared_lock<std::shared_mutex> lock(imp->stageMutex);
    imp->refreshStageSummary();

    return serviced;
}

U64
SceneViewportRenderer::getStageLoadEditCount() const
{
    return _imp->stageLoadEditCount;
}

double
SceneViewportRenderer::getLastStageLoadHoldMs() const
{
    const SceneStage* stage = _imp->loadStage;

    return stage ? stage->getLastLoadHoldMs() : 0.;
}

std::vector<std::pair<std::string, std::string>>
SceneViewportRenderer::getStageLoadFailures() const
{
    std::vector<std::pair<std::string, std::string>> result;
    const SceneStage* stage = _imp->loadStage;

    if (stage) {
        for (const SceneStage::LoadFailure& failure : stage->getLoadFailures()) {
            result.emplace_back(failure.prim.GetString(), failure.error);
        }
    }

    return result;
}

U64
SceneViewportRenderer::getEngineConstructionCount()
{
    return engineConstructionCount.load();
}

void
SceneViewportRenderer::setDrawMode(DrawModeEnum mode)
{
    _imp->drawMode = mode;
}

SceneViewportRenderer::DrawModeEnum
SceneViewportRenderer::getDrawMode() const
{
    return _imp->drawMode;
}

void
SceneViewportRenderer::setDisplayTransform(const DisplayTransform& transform)
{
    const DisplayTransform& current = _imp->displayTransform;
    if ((transform.config == current.config) && (transform.colorSpace == current.colorSpace)
        && (transform.display == current.display) && (transform.view == current.view)
        && (transform.gain == current.gain) && (transform.gamma == current.gamma)) {
        return;
    }
    _imp->displayTransform = transform;
    _imp->hydraTransform = transform;
    ++_imp->displayTransformGeneration;
    _imp->colorCorrectionDirty = true;

    bool useOpenColorIO = transform.config && !transform.colorSpace.empty() && !transform.display.empty()
        && !transform.view.empty() && !TfGetenv("OCIO").empty();
    float background[3] = { kBackgroundSrgb, kBackgroundSrgb, kBackgroundSrgb };
    if (useOpenColorIO) {
        try {
            OCIO_NAMESPACE::ConstProcessorRcPtr processor = displayViewProcessor(transform.config, transform.colorSpace, transform.display, transform.view);
            const bool lutTextures = needsLutTextures(processor);
            if (lutTextures || (transform.gain != 1.) || (transform.gamma != 1.)) {
                const OCIO_NAMESPACE::ConstTransformRcPtr displayView = lutTextures
                    ? cachedLut(transform.config, transform.colorSpace, transform.display, transform.view, processor)
                    : OCIO_NAMESPACE::ConstTransformRcPtr(processor->createGroupTransform());
                DisplayTransform baked = transform;
                baked.config = cachedViewportConfig(transform.config,
                                                    displayTransformKey(transform.config, transform.colorSpace, transform.display, transform.view),
                                                    transform.gain, transform.gamma, displayView);
                baked.colorSpace = kBakeInputSpace;
                baked.display = kBakeDisplay;
                baked.view = kBakeView;
                processor = displayViewProcessor(baked.config, baked.colorSpace, baked.display, baked.view);
                if (needsLutTextures(processor)) {
                    throw std::runtime_error("the baked display transform still needs 1D or 2D LUT textures");
                }
                _imp->hydraTransform = baked;
            }
            float rgb[3] = { kBackgroundLinear, kBackgroundLinear, kBackgroundLinear };
            processor->getDefaultCPUProcessor()->applyRGB(rgb);
            for (int c = 0; c < 3; ++c) {
                background[c] = std::min(std::max(rgb[c], 0.f), 1.f);
            }
        } catch (const std::exception& e) {
            qWarning() << "SceneViewport: OpenColorIO:" << e.what();
            useOpenColorIO = false;
            background[0] = background[1] = background[2] = kBackgroundSrgb;
        }
    }
    if (!useOpenColorIO && ((transform.gain != 1.) || (transform.gamma != 1.)) && !TfGetenv("OCIO").empty()) {
        try {
            DisplayTransform graded = transform;
            graded.config = srgbViewportConfig(transform.gain, transform.gamma);
            graded.colorSpace = kBakeInputSpace;
            graded.display = kBakeDisplay;
            graded.view = kBakeView;
            OCIO_NAMESPACE::ConstProcessorRcPtr processor = displayViewProcessor(graded.config, graded.colorSpace, graded.display, graded.view);
            float rgb[3] = { kBackgroundLinear, kBackgroundLinear, kBackgroundLinear };
            processor->getDefaultCPUProcessor()->applyRGB(rgb);
            for (int c = 0; c < 3; ++c) {
                background[c] = std::min(std::max(rgb[c], 0.f), 1.f);
            }
            _imp->hydraTransform = graded;
            useOpenColorIO = true;
        } catch (const std::exception& e) {
            qWarning() << "SceneViewport: OpenColorIO:" << e.what();
            background[0] = background[1] = background[2] = kBackgroundSrgb;
        }
    }
    _imp->useOpenColorIO = useOpenColorIO;
    for (int c = 0; c < 3; ++c) {
        _imp->backgroundDisplay[c] = background[c];
    }
}

bool
SceneViewportRenderer::isUsingOpenColorIO() const
{
    return _imp->useOpenColorIO;
}

void
SceneViewportRenderer::setShowGrid(bool show)
{
    _imp->showGrid = show;
}

bool
SceneViewportRenderer::getShowGrid() const
{
    return _imp->showGrid;
}

void
SceneViewportRenderer::setShowGuides(bool show)
{
    _imp->showGuides = show;
}

bool
SceneViewportRenderer::getShowGuides() const
{
    return _imp->showGuides;
}

void
SceneViewportRenderer::setGuideSize(double size)
{
    _imp->guideSize = size;
}

double
SceneViewportRenderer::getGuideSize() const
{
    return _imp->guideSize.load();
}

void
SceneViewportRenderer::setShowProxy(bool show)
{
    _imp->showProxy = show;
}

bool
SceneViewportRenderer::getShowProxy() const
{
    return _imp->showProxy;
}

void
SceneViewportRenderer::setShowRender(bool show)
{
    _imp->showRender = show;
}

bool
SceneViewportRenderer::getShowRender() const
{
    return _imp->showRender;
}

void
SceneViewportRenderer::setShowGuidePurpose(bool show)
{
    _imp->showGuidePurpose = show;
}

bool
SceneViewportRenderer::getShowGuidePurpose() const
{
    return _imp->showGuidePurpose;
}

void
SceneViewportRenderer::setLightingMode(LightingModeEnum mode)
{
    _imp->lightingMode = mode;
}

SceneViewportRenderer::LightingModeEnum
SceneViewportRenderer::getLightingMode() const
{
    return _imp->lightingMode;
}

bool
SceneViewportRenderer::isSceneLit() const
{
    return _imp->sceneLit;
}

void
SceneViewportRenderer::setRecordDrawnGuides(bool record)
{
    _imp->recordDrawnGuides = record;
}

const std::vector<SceneViewportRenderer::DrawnGuide>&
SceneViewportRenderer::getDrawnGuides() const
{
    return _imp->drawnGuides;
}

void
SceneViewportRenderer::setFreeCamera(const FreeCamera& camera)
{
    _imp->freeCamera = camera;
    _imp->freeCamera.elevation = std::max(kMinElevation, std::min(kMaxElevation, camera.elevation));
    _imp->freeCamera.distance = std::max(camera.distance, 1e-9);
    _imp->framedOnce = true;
    _imp->lookThrough.clear();
}

const SceneViewportRenderer::FreeCamera&
SceneViewportRenderer::getFreeCamera() const
{
    return _imp->freeCamera;
}

void
SceneViewportRenderer::setViewMode(ViewModeEnum mode)
{
    _imp->lookThrough.clear();
    _imp->freeCamera.viewMode = mode;
    // Perspective (V) after an orthographic view starts from its angle.
    switch (mode) {
    case eViewModePerspective:
        break;
    case eViewModeFront:
        _imp->freeCamera.azimuth = 0.;
        _imp->freeCamera.elevation = 0.;
        break;
    case eViewModeBack:
        _imp->freeCamera.azimuth = 180.;
        _imp->freeCamera.elevation = 0.;
        break;
    case eViewModeRight:
        _imp->freeCamera.azimuth = 90.;
        _imp->freeCamera.elevation = 0.;
        break;
    case eViewModeLeft:
        _imp->freeCamera.azimuth = -90.;
        _imp->freeCamera.elevation = 0.;
        break;
    case eViewModeTop:
        _imp->freeCamera.azimuth = 0.;
        _imp->freeCamera.elevation = kMaxElevation;
        break;
    case eViewModeBottom:
        _imp->freeCamera.azimuth = 0.;
        _imp->freeCamera.elevation = kMinElevation;
        break;
    }
}

bool
SceneViewportRenderer::isUpAxisZ() const
{
    return _imp->zUp;
}

SceneViewportRenderer::ViewModeEnum
SceneViewportRenderer::getViewMode() const
{
    return _imp->freeCamera.viewMode;
}

void
SceneViewportRenderer::setClipPlanes(const ClipPlanes& planes)
{
    _imp->clipPlanes = planes;
    _imp->clipPlanes.nearClip = std::max(planes.nearClip, 1e-6);
    _imp->clipPlanes.farClip = std::max(planes.farClip, _imp->clipPlanes.nearClip * 1.0001);
}

const SceneViewportRenderer::ClipPlanes&
SceneViewportRenderer::getClipPlanes() const
{
    return _imp->clipPlanes;
}

void
SceneViewportRenderer::getFreeCameraClipRange(double* nearClip,
                                              double* farClip) const
{
    _imp->freeClipRange(nearClip, farClip);
}

namespace {
void
storeMatrix(const GfMatrix4d& matrix,
            double out[16])
{
    const double* values = matrix.GetArray();

    std::copy(values, values + 16, out);
}

void
setBusy(bool* busy)
{
    if (busy) {
        *busy = true;
    }
}
} // anon namespace

bool
SceneViewportRenderer::getViewPose(int width,
                                   int height,
                                   CameraPose* pose,
                                   bool* busy) const
{
    const double aspect = double(std::max(width, 1)) / double(std::max(height, 1));

    pose->aspect = aspect;
    storeMatrix(GfMatrix4d(1.), pose->parentToWorld);
    if (!_imp->lookThrough.empty()) {
        std::shared_lock<std::shared_mutex> lock(_imp->stageMutex, std::try_to_lock);
        if (!lock.owns_lock()) {
            setBusy(busy);

            return false;
        }
        const UsdGeomCamera camera = _imp->lookThroughCamera();
        if (!camera) {
            return false;
        }
        const UsdTimeCode time(_imp->timeCode());
        GfCamera gfCamera = _imp->lookThroughGfCamera(camera);
        CameraUtilConformWindow(&gfCamera, CameraUtilMatchVertically, aspect);
        UsdGeomXformCache xforms(time);
        storeMatrix(_imp->hasLookThroughOverride() ? _imp->lookThroughOverride : xforms.GetLocalToWorldTransform(camera.GetPrim()), pose->cameraToWorld);
        storeMatrix(xforms.GetParentToWorldTransform(camera.GetPrim()), pose->parentToWorld);
        pose->orthographic = (gfCamera.GetProjection() == GfCamera::Orthographic);
        // Apertures are in tenths of a scene unit, as GfCamera::APERTURE_UNIT says.
        pose->viewHeight = pose->orthographic ? gfCamera.GetVerticalAperture() * GfCamera::APERTURE_UNIT
                                              : gfCamera.GetVerticalAperture() / std::max(double(gfCamera.GetFocalLength()), 1e-9);
        const GfRange1f clipping = gfCamera.GetClippingRange();
        pose->nearClip = clipping.GetMin();
        pose->farClip = clipping.GetMax();
        // Turn about the scene's centre, at its depth ahead of the camera.
        const GfMatrix4d cameraToWorld = gfCamera.GetTransform();
        const GfVec3d eye = cameraToWorld.ExtractTranslation();
        const GfVec3d forward = -cameraToWorld.TransformDir(GfVec3d(0., 0., 1.)).GetNormalized();
        double depth = 0.;
        {
            std::lock_guard<std::mutex> summaryLock(_imp->summaryMutex);
            if (!_imp->pivotBounds.IsEmpty()) {
                depth = GfDot(_imp->pivotBounds.GetMidpoint() - eye, forward);
                if (depth <= 1e-6) {
                    depth = _imp->pivotBounds.GetSize().GetLength() * 0.5;
                }
            }
        }
        pose->pivotDepth = (depth > 1e-6) ? depth : 1.;

        return true;
    }
    const FreeCamera& freeCamera = _imp->freeCamera;
    const double distance = std::max(freeCamera.distance, 1e-9);
    GfVec3d back, up;
    _imp->viewBasis(&back, &up);
    GfMatrix4d view;
    view.SetLookAt(_imp->pivot() + back * distance, _imp->pivot(), up);
    storeMatrix(view.GetInverse(), pose->cameraToWorld);
    pose->orthographic = _imp->isOrthographic();
    const double tanHalf = std::tan(degreesToRadians(freeCamera.fovY) * 0.5);
    pose->viewHeight = pose->orthographic ? 2. * distance * tanHalf : 2. * tanHalf;
    _imp->freeClipRange(&pose->nearClip, &pose->farClip);
    pose->pivotDepth = distance;

    return true;
}

SceneViewportRenderer::FrameBounds
SceneViewportRenderer::computeFrameBounds(const std::vector<std::string>& selection,
                                          double timeCode) const
{
    FrameBounds bounds;

    bounds.selection = !selection.empty();
    // Before the viewer lock, the order every reader takes them in.
    SceneWriteBarrier::ReadScope read;
    std::shared_lock<std::shared_mutex> lock(_imp->stageMutex);
    const UsdStageRefPtr stage = _imp->currentStage();
    if (!stage) {
        return bounds;
    }
    const UsdTimeCode time(timeCode);
    // With useExtentsHint, an unloaded model counts by the extent hint authored over its payload
    // (usdGeom/bboxCache.h), so framing never reads a payload.
    UsdGeomBBoxCache bboxes(time, { UsdGeomTokens->default_, UsdGeomTokens->proxy }, true);
    GfRange3d range;
    if (selection.empty()) {
        range = bboxes.ComputeWorldBound(stage->GetPseudoRoot()).ComputeAlignedRange();
        std::lock_guard<std::mutex> summaryLock(_imp->summaryMutex);
        _imp->pivotBounds = range;
    } else {
        UsdGeomXformCache xforms(time);
        for (const std::string& path : selection) {
            if (!SdfPath::IsValidPathString(path)) {
                continue;
            }
            const UsdPrim prim = stage->GetPrimAtPath(SdfPath(path));
            if (!prim) {
                continue;
            }
            const GfRange3d bound = bboxes.ComputeWorldBound(prim).ComputeAlignedRange();
            if (bound.IsEmpty()) {
                // A camera, light or empty transform has no extent: frame where it is.
                range.UnionWith(xforms.GetLocalToWorldTransform(prim).Transform(GfVec3d(0.)));
            } else {
                range.UnionWith(bound);
            }
        }
    }
    if (!range.IsEmpty()) {
        for (int i = 0; i < 3; ++i) {
            bounds.min[i] = range.GetMin()[i];
            bounds.max[i] = range.GetMax()[i];
        }
        bounds.empty = false;
    }

    return bounds;
} // SceneViewportRenderer::computeFrameBounds

void
SceneViewportRenderer::applyFrameBounds(const FrameBounds& bounds)
{
    if (!bounds.selection) {
        _imp->lookThrough.clear();
    }
    if (bounds.empty) {
        return;
    }
    _imp->lookThrough.clear();
    const GfRange3d range(GfVec3d(bounds.min[0], bounds.min[1], bounds.min[2]), GfVec3d(bounds.max[0], bounds.max[1], bounds.max[2]));
    const GfVec3d centre = range.GetMidpoint();
    double radius = range.GetSize().GetLength() * 0.5;
    if (radius < 1e-6) {
        radius = bounds.selection ? kDefaultFrameRadius : 1e-6;
    }
    _imp->freeCamera.pivot[0] = centre[0];
    _imp->freeCamera.pivot[1] = centre[1];
    _imp->freeCamera.pivot[2] = centre[2];
    _imp->freeCamera.distance = radius / std::sin(degreesToRadians(_imp->freeCamera.fovY) * 0.5) * 1.05;
    _imp->framedOnce = true;
}

void
SceneViewportRenderer::frameScene()
{
    applyFrameBounds(computeFrameBounds(std::vector<std::string>(), _imp->timeCode()));
}

void
SceneViewportRenderer::frameSelection()
{
    applyFrameBounds(computeFrameBounds(_imp->selected, _imp->timeCode()));
}

void
SceneViewportRenderer::tumble(double azimuthDegrees,
                              double elevationDegrees)
{
    _imp->lookThrough.clear();
    // Tumbling an orthographic view leaves its axis.
    _imp->freeCamera.viewMode = eViewModePerspective;
    _imp->freeCamera.azimuth = std::fmod(_imp->freeCamera.azimuth + azimuthDegrees, 360.);
    _imp->freeCamera.elevation = std::max(kMinElevation, std::min(kMaxElevation, _imp->freeCamera.elevation + elevationDegrees));
}

void
SceneViewportRenderer::pan(double dxPixels,
                           double dyPixels,
                           int viewHeight)
{
    _imp->lookThrough.clear();
    GfVec3d back, viewUp;
    _imp->viewBasis(&back, &viewUp);
    const GfVec3d right = GfCross(viewUp, back).GetNormalized();
    const GfVec3d up = GfCross(back, right).GetNormalized();
    const double unitsPerPixel = 2. * _imp->freeCamera.distance * std::tan(degreesToRadians(_imp->freeCamera.fovY) * 0.5) / std::max(viewHeight, 1);
    // The scene follows the mouse, so the pivot moves the other way.
    const GfVec3d pivot = _imp->pivot() - right * (dxPixels * unitsPerPixel) + up * (dyPixels * unitsPerPixel);

    _imp->freeCamera.pivot[0] = pivot[0];
    _imp->freeCamera.pivot[1] = pivot[1];
    _imp->freeCamera.pivot[2] = pivot[2];
}

void
SceneViewportRenderer::dolly(double factor)
{
    _imp->lookThrough.clear();
    if (factor > 0.) {
        _imp->freeCamera.distance = std::max(_imp->freeCamera.distance * factor, 1e-9);
    }
}

std::vector<std::string>
SceneViewportRenderer::getStageCameraPaths() const
{
    const std::shared_ptr<const Implementation::StageSummary> summary = _imp->getStageSummary();

    return summary ? summary->cameraPaths : std::vector<std::string>();
}

void
SceneViewportRenderer::setLookThroughCamera(const std::string& path)
{
    _imp->lookThrough = path;
    _imp->guideBatchesValid = false;
    _imp->lookThroughOverridePath.clear();
}

const std::string&
SceneViewportRenderer::getLookThroughCamera() const
{
    return _imp->lookThrough;
}

void
SceneViewportRenderer::setLookThroughTransform(const double cameraToWorld[16])
{
    _imp->lookThroughOverridePath = _imp->lookThrough;
    std::copy(cameraToWorld, cameraToWorld + 16, _imp->lookThroughOverride.data());
}

void
SceneViewportRenderer::clearLookThroughTransform()
{
    _imp->lookThroughOverridePath.clear();
}

bool
SceneViewportRenderer::hasLookThroughTransform() const
{
    return _imp->hasLookThroughOverride();
}

void
SceneViewportRenderer::setSelectedPrims(const std::vector<std::string>& paths)
{
    _imp->selected = paths;
    _imp->guideBatchesValid = false;
    std::lock_guard<std::mutex> lock(_imp->engineSelectionMutex);
    _imp->engineSelection = paths;
    _imp->engineSelectionDirty = true;
}

const std::vector<std::string>&
SceneViewportRenderer::getSelectedPrims() const
{
    return _imp->selected;
}

void
SceneViewportRenderer::setTransformHandles(const TransformHandles& handles)
{
    _imp->hasHandles = true;
    _imp->handles = handles;
}

void
SceneViewportRenderer::clearTransformHandles()
{
    _imp->hasHandles = false;
    if (!_imp->dragSelection) {
        _imp->highlightedHandle = SceneGizmo::kNoHandle;
        _imp->dragHandle = SceneGizmo::kNoHandle;
    }
}

void
SceneViewportRenderer::setTransformTool(int tool)
{
    _imp->transformTool = (SceneTransform::ToolEnum)std::max(0, std::min(tool, (int)SceneTransform::eToolScale));
}

int
SceneViewportRenderer::getTransformTool() const
{
    return _imp->transformTool;
}

void
SceneViewportRenderer::setTransformSpace(int space)
{
    _imp->transformSpace = (space == SceneTransform::eSpaceLocal) ? SceneTransform::eSpaceLocal : SceneTransform::eSpaceWorld;
}

int
SceneViewportRenderer::getTransformSpace() const
{
    return _imp->transformSpace;
}

void
SceneViewportRenderer::setTransformPivot(int pivot)
{
    _imp->transformPivot = (pivot == SceneTransform::ePivotCurrentPrim) ? SceneTransform::ePivotCurrentPrim : SceneTransform::ePivotSelectionCentre;
}

int
SceneViewportRenderer::getTransformPivot() const
{
    return _imp->transformPivot;
}

bool
SceneViewportRenderer::hasTransformHandles() const
{
    SceneGizmo::Frame frame;
    std::shared_lock<std::shared_mutex> lock(_imp->stageMutex);

    return _imp->computeHandleFrame(&frame);
}

bool
SceneViewportRenderer::hasSelectionHandles(bool* busy) const
{
    SceneGizmo::Frame frame;
    std::shared_lock<std::shared_mutex> lock(_imp->stageMutex, std::try_to_lock);

    if (!lock.owns_lock()) {
        setBusy(busy);

        return false;
    }

    return _imp->computeSelectionFrame(&frame);
}

bool
SceneViewportRenderer::getDrawnTransformHandles(int width,
                                                int height,
                                                std::vector<int>* handles,
                                                std::vector<double>* pixels,
                                                std::vector<double>* frameValues,
                                                bool* busy) const
{
    SceneGizmo::Frame frame;
    bool selection = false;
    std::shared_lock<std::shared_mutex> lock(_imp->stageMutex, std::try_to_lock);

    if (!lock.owns_lock()) {
        setBusy(busy);

        return false;
    }
    handles->clear();
    pixels->clear();
    frameValues->clear();
    if ((width <= 0) || (height <= 0) || !_imp->computeHandleFrame(&frame, &selection)) {
        return false;
    }
    const SceneGizmo::View view = _imp->makeView(width, height);
    for (int handle = 0; handle < SceneGizmo::kNumHandles; ++handle) {
        GfVec2d pixel;
        if (_imp->isDrawnHandle(handle, selection) && SceneGizmo::getHandlePixel(view, frame, handle, &pixel)) {
            handles->push_back(handle);
            pixels->push_back(pixel[0]);
            pixels->push_back(pixel[1]);
        }
    }
    const GfVec3d* vectors[4] = { &frame.origin, &frame.axes[0], &frame.axes[1], &frame.axes[2] };
    for (const GfVec3d* v : vectors) {
        for (int i = 0; i < 3; ++i) {
            frameValues->push_back((*v)[i]);
        }
    }

    return true;
} // SceneViewportRenderer::getDrawnTransformHandles

int
SceneViewportRenderer::hitTestTransformHandles(double x,
                                               double y,
                                               int width,
                                               int height,
                                               bool* busy) const
{
    SceneGizmo::Frame frame;
    bool selection = false;
    std::shared_lock<std::shared_mutex> lock(_imp->stageMutex, std::try_to_lock);

    if (!lock.owns_lock()) {
        setBusy(busy);

        return SceneGizmo::kNoHandle;
    }
    if ((width <= 0) || (height <= 0) || !_imp->computeHandleFrame(&frame, &selection)) {
        return SceneGizmo::kNoHandle;
    }
    const SceneGizmo::View view = _imp->makeView(width, height);

    return SceneGizmo::hitTest(view, _imp->buildDrawnHandles(frame, selection, SceneGizmo::getHandleLength(view, frame)), GfVec2d(x, y));
}

void
SceneViewportRenderer::setHighlightedTransformHandle(int handle)
{
    _imp->highlightedHandle = handle;
}

int
SceneViewportRenderer::getHighlightedTransformHandle() const
{
    return _imp->highlightedHandle;
}

bool
SceneViewportRenderer::getTransformHandlePixel(int handle,
                                               int width,
                                               int height,
                                               double* x,
                                               double* y) const
{
    SceneGizmo::Frame frame;
    bool selection = false;
    std::shared_lock<std::shared_mutex> lock(_imp->stageMutex);

    if ((width <= 0) || (height <= 0) || !_imp->computeHandleFrame(&frame, &selection) || !_imp->isDrawnHandle(handle, selection)) {
        return false;
    }
    GfVec2d pixel;
    if (!SceneGizmo::getHandlePixel(_imp->makeView(width, height), frame, handle, &pixel)) {
        return false;
    }
    *x = pixel[0];
    *y = pixel[1];

    return true;
}

bool
SceneViewportRenderer::beginTransformDrag(int handle,
                                          double x,
                                          double y,
                                          int /*width*/,
                                          int /*height*/,
                                          bool* busy)
{
    SceneGizmo::Frame frame;
    bool selection = false;
    std::unique_lock<std::shared_mutex> lock;

    if (!_imp->tryLockExclusive(&lock)) {
        setBusy(busy);

        return false;
    }
    // A preview still waiting for its scene goes now: the new drag measures from the scene shown.
    _imp->clearPreviewLocked();
    _imp->dragHandle = SceneGizmo::kNoHandle;
    _imp->dragSelection = false;
    if (!_imp->computeHandleFrame(&frame, &selection) || !_imp->isDrawnHandle(handle, selection)) {
        return false;
    }
    _imp->dragPrims.clear();
    _imp->dragOpTargets.clear();
    if (selection) {
        const double time = _imp->timeCode();
        UsdGeomXformCache xforms((UsdTimeCode(time)));
        for (const UsdPrim& prim : _imp->transformablePrims()) {
            SceneTransformPrim described;
            SceneTransformWidget::describePrim(prim, &xforms, time, &described);
            _imp->dragPrims.push_back(described);
        }
    } else {
        if (!_imp->computeTargetSpace(_imp->handles.primPath, _imp->handles.opName, &_imp->dragOpSpace)) {
            return false;
        }
        _imp->dragOpTargets.push_back(std::make_pair(_imp->handles.primPath, _imp->dragOpSpace));
        for (const TransformHandles::Target& target : _imp->handles.otherTargets) {
            GfMatrix4d space;
            if ((target.primPath != _imp->handles.primPath) && _imp->computeTargetSpace(target.primPath, target.opName, &space)) {
                _imp->dragOpTargets.push_back(std::make_pair(target.primPath, space));
            }
        }
    }
    _imp->dragHandle = handle;
    _imp->dragSelection = selection;
    _imp->dragFrame = frame;
    _imp->dragPressPixel = GfVec2d(x, y);
    _imp->dragChange = 0.;
    _imp->dragChanged = false;

    return true;
} // SceneViewportRenderer::beginTransformDrag

bool
SceneViewportRenderer::isSelectionTransformDrag() const
{
    return (_imp->dragHandle != SceneGizmo::kNoHandle) && _imp->dragSelection;
}

bool
SceneViewportRenderer::getTransformDragChange(double x,
                                              double y,
                                              int width,
                                              int height,
                                              double* change,
                                              bool* busy) const
{
    if ((_imp->dragHandle == SceneGizmo::kNoHandle) || (width <= 0) || (height <= 0)) {
        return false;
    }
    std::shared_lock<std::shared_mutex> lock(_imp->stageMutex, std::defer_lock);

    // The free camera's view reads no prim, so its drags follow the mouse through a load.
    if (!_imp->lookThrough.empty() && !lock.try_lock()) {
        setBusy(busy);

        return false;
    }

    return SceneGizmo::computeDragChange(_imp->makeView(width, height), _imp->dragFrame, _imp->dragHandle, _imp->dragPressPixel, GfVec2d(x, y), change);
}

bool
SceneViewportRenderer::previewTransformDrag(double change,
                                            bool* busy)
{
    if (_imp->dragHandle == SceneGizmo::kNoHandle) {
        return false;
    }
    std::vector<std::pair<std::string, GfMatrix4d>> deltas;
    if (_imp->dragSelection) {
        const GfMatrix4d delta = SceneTransformWidget::computeDelta(_imp->dragFrame, _imp->dragHandle, change);
        for (const SceneTransformPrim& prim : _imp->dragPrims) {
            deltas.push_back(std::make_pair(prim.path, delta));
        }
    } else {
        // W = inner * op * space, so W' = W * space^-1 op^-1 op' space.
        GfVec3d translate, rotate, scale;
        _imp->changeOpValues(SceneGizmo::kNoHandle, 0., &translate, &rotate, &scale);
        const GfMatrix4d opInverse = _imp->composeOp(translate, rotate, scale).GetInverse();
        _imp->changeOpValues(_imp->dragHandle, change, &translate, &rotate, &scale);
        const GfMatrix4d changed = _imp->composeOp(translate, rotate, scale);
        for (const std::pair<std::string, GfMatrix4d>& target : _imp->dragOpTargets) {
            deltas.push_back(std::make_pair(target.first, target.second.GetInverse() * opInverse * changed * target.second));
        }
    }
    std::unique_lock<std::shared_mutex> lock;

    if (!_imp->tryLockExclusive(&lock)) {
        setBusy(busy);

        return false;
    }
    _imp->dragChange = change;
    _imp->dragChanged = true;
    if (!_imp->sceneStage || deltas.empty()) {
        return false;
    }
    _imp->guidesValid = false;

    return _imp->sceneStage->setPreviewTransforms(deltas, _imp->timeCode());
} // SceneViewportRenderer::previewTransformDrag

bool
SceneViewportRenderer::getTransformDrag(double change,
                                        double frame,
                                        SceneTransformDrag* drag) const
{
    if ((_imp->dragHandle == SceneGizmo::kNoHandle) || !_imp->dragSelection || _imp->dragPrims.empty()) {
        return false;
    }
    drag->prims = _imp->dragPrims;
    SceneTransformWidget::storeMatrix(SceneTransformWidget::computeDelta(_imp->dragFrame, _imp->dragHandle, change), drag->delta);
    drag->tool = _imp->transformTool;
    drag->frame = frame;
    for (int i = 0; i < 3; ++i) {
        drag->pivot[i] = _imp->dragFrame.origin[i];
    }

    return true;
}

void
SceneViewportRenderer::endTransformDrag()
{
    _imp->dragHandle = SceneGizmo::kNoHandle;
    _imp->dragSelection = false;
    _imp->dragPrims.clear();
    _imp->dragOpTargets.clear();
    _imp->dragChange = 0.;
    _imp->dragChanged = false;
}

bool
SceneViewportRenderer::clearTransformPreview()
{
    std::unique_lock<std::shared_mutex> lock;

    if (!_imp->tryLockExclusive(&lock)) {
        return false;
    }
    _imp->clearPreviewLocked();

    return true;
}

void
SceneViewportRenderer::clearTransformPreviewAfter(U64 serial)
{
    _imp->previewClearAfter = serial;
}

bool
SceneViewportRenderer::render(unsigned int fbo,
                              int width,
                              int height)
{
    std::lock_guard<std::shared_mutex> lock(_imp->stageMutex);

    return renderLocked(fbo, width, height);
}

SceneViewportRenderer::RenderStatusEnum
SceneViewportRenderer::renderIfIdle(unsigned int fbo,
                                    int width,
                                    int height)
{
    std::unique_lock<std::shared_mutex> lock(_imp->stageMutex, std::try_to_lock);

    if (!lock.owns_lock()) {
        return eRenderStatusBusy;
    }

    return renderLocked(fbo, width, height) ? eRenderStatusDone : eRenderStatusRefining;
}

bool
SceneViewportRenderer::renderLocked(unsigned int fbo,
                                    int width,
                                    int height)
{
    if ((width <= 0) || (height <= 0) || !AppManager::instance()->isOpenGLLoaded()) {
        return true;
    }
    StormGLStateGuard guard;
    StormGLSection section;

    if (!_imp->ensureEngine()) {
        Implementation::bindFrameTarget(fbo, width, height);
        Implementation::clearFrameTarget(_imp->backgroundDisplay);

        return true;
    }
    section.collectWith(_imp->engine->GetHgi());
    _imp->serviceDrawFilter();

    const UsdStageRefPtr stage = _imp->currentStage();
    const GfFrustum frustum = _imp->computeFrustum(width, height);
    _imp->sceneLit = _imp->usesSceneLights();
    UsdImagingGLRenderParams params = _imp->renderParams();
    const bool kept = _imp->takeBackgroundImage(fbo, width, height, frustum, params);
    Implementation::bindFrameTarget(fbo, width, height);
    bool converged = kept;
    if (!kept) {
        Implementation::clearFrameTarget(_imp->backgroundDisplay);
        _imp->setCamera(frustum, width, height, fbo, _imp->sceneLit);
        _imp->prepareColorCorrection(_imp->useOpenColorIO, _imp->hydraTransform);
        for (int i = 0; (i < kMaxRenderPasses) && !converged; ++i) {
            _imp->engine->Render(stage->GetPseudoRoot(), params);
            converged = _imp->engine->IsConverged();
            const bool lit = _imp->usesSceneLights();
            if (lit != _imp->sceneLit) {
                _imp->sceneLit = lit;
                params.enableSceneLights = lit;
                _imp->setLights(lit);
                converged = false;
            }
        }
    }
    _imp->noteStageAbsorbed();

    _imp->drawnGuides.clear();
    if (_imp->showGrid || _imp->showGuides || _imp->hasHandles) {
        // Storm leaves its own program, vertex array and framebuffer bound.
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, width, height);
        glUseProgram(0);
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glDisable(GL_FRAMEBUFFER_SRGB);
        if (_imp->showGrid) {
            _imp->drawGrid(frustum);
        }
        if (_imp->showGuides) {
            _imp->drawGuides(frustum, width, height);
        }
        _imp->drawTransformHandles(frustum, width, height);
    }

    return converged;
} // renderLocked

std::string
SceneViewportRenderer::pick(int x,
                            int y,
                            int width,
                            int height,
                            bool* busy)
{
    if ((width <= 0) || (height <= 0) || !AppManager::instance()->isOpenGLLoaded()) {
        return std::string();
    }
    std::unique_lock<std::shared_mutex> lock;
    if (!_imp->tryLockExclusive(&lock)) {
        setBusy(busy);

        return std::string();
    }
    StormGLStateGuard guard;
    StormGLSection section;

    if (!_imp->ensureEngine()) {
        return std::string();
    }
    section.collectWith(_imp->engine->GetHgi());
    const UsdStageRefPtr stage = _imp->currentStage();
    const GfFrustum frustum = _imp->computeFrustum(width, height);
    // The pixel's centre in normalized device coordinates, y up, and a one-pixel window.
    const GfVec2d centre(2. * (x + 0.5) / width - 1., 1. - 2. * (y + 0.5) / height);
    const GfFrustum pickFrustum = frustum.ComputeNarrowedFrustum(centre, GfVec2d(1. / width, 1. / height));

    UsdImagingGLEngine::PickParams pickParams;
    pickParams.resolveMode = HdxPickTokens->resolveNearestToCenter;
    UsdImagingGLEngine::IntersectionResultVector results;
    const bool hitPrim = _imp->engine->TestIntersection(pickParams, pickFrustum.ComputeViewMatrix(), pickFrustum.ComputeProjectionMatrix(),
                                                        stage->GetPseudoRoot(), _imp->renderParams(), &results)
        && !results.empty();

    int guide = -1;
    GfVec3d guidePoint;
    if (_imp->showGuides) {
        const SceneGizmo::View view(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix(), width, height);
        guide = SceneGuides::hitTest(view, _imp->currentGuides(), GfVec2d(x + 0.5, y + 0.5), _imp->lookThrough, &guidePoint);
    }
    if (guide >= 0) {
        // The nearer wins: depth is -z in the camera's space, for either projection.
        const GfMatrix4d viewMatrix = frustum.ComputeViewMatrix();
        if (!hitPrim || (viewMatrix.Transform(guidePoint)[2] >= viewMatrix.Transform(results.front().hitPoint)[2])) {
            return _imp->guides[guide].path;
        }
    }

    return hitPrim ? results.front().hitPrimPath.GetString() : std::string();
}

std::vector<std::string>
SceneViewportRenderer::pickRect(int x0,
                                int y0,
                                int x1,
                                int y1,
                                int width,
                                int height,
                                bool* busy)
{
    std::vector<std::string> paths;
    if ((width <= 0) || (height <= 0) || !AppManager::instance()->isOpenGLLoaded()) {
        return paths;
    }
    const int left = std::max(0, std::min(x0, x1));
    const int right = std::min(width - 1, std::max(x0, x1));
    const int top = std::max(0, std::min(y0, y1));
    const int bottom = std::min(height - 1, std::max(y0, y1));
    if ((left > right) || (top > bottom)) {
        return paths;
    }
    std::unique_lock<std::shared_mutex> lock;
    if (!_imp->tryLockExclusive(&lock)) {
        setBusy(busy);

        return paths;
    }
    StormGLStateGuard guard;
    StormGLSection section;

    if (!_imp->ensureEngine()) {
        return paths;
    }
    section.collectWith(_imp->engine->GetHgi());
    const UsdStageRefPtr stage = _imp->currentStage();
    const GfFrustum frustum = _imp->computeFrustum(width, height);
    // The rectangle's centre in normalized device coordinates, y up, and its size as a fraction
    // of the view, whole pixels included.
    const GfVec2d centre((left + right + 1.) / width - 1., 1. - (top + bottom + 1.) / height);
    const GfVec2d size(double(right - left + 1) / width, double(bottom - top + 1) / height);
    const GfFrustum pickFrustum = frustum.ComputeNarrowedFrustum(centre, size);

    UsdImagingGLEngine::PickParams pickParams;
    pickParams.resolveMode = HdxPickTokens->resolveUnique;
    UsdImagingGLEngine::IntersectionResultVector results;
    if (!_imp->engine->TestIntersection(pickParams, pickFrustum.ComputeViewMatrix(), pickFrustum.ComputeProjectionMatrix(),
                                        stage->GetPseudoRoot(), _imp->renderParams(), &results)) {
        results.clear();
    }
    for (const UsdImagingGLEngine::IntersectionResult& result : results) {
        const std::string path = result.hitPrimPath.GetString();
        if (!path.empty() && (std::find(paths.begin(), paths.end(), path) == paths.end())) {
            paths.push_back(path);
        }
    }
    if (_imp->showGuides) {
        const SceneGizmo::View view(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix(), width, height);
        const std::vector<SceneGuides::Guide>& guides = _imp->currentGuides();
        for (int index : SceneGuides::guidesInRect(view, guides, GfVec2d(left, top), GfVec2d(right + 1., bottom + 1.), _imp->lookThrough)) {
            if (std::find(paths.begin(), paths.end(), guides[index].path) == paths.end()) {
                paths.push_back(guides[index].path);
            }
        }
    }
    // Hits come in hash order; sorted, a marquee's selection does not depend on it.
    std::sort(paths.begin(), paths.end());

    return paths;
}

void
SceneViewportRenderer::releaseGL()
{
    std::lock_guard<std::shared_mutex> lock(_imp->stageMutex);

    _imp->dropEngine();
}

void
SceneViewportRenderer::setRendererId(const std::string& id)
{
    std::lock_guard<std::shared_mutex> lock(_imp->stageMutex);
    const std::string renderer = id.empty() ? std::string(kRenderDelegateStormId) : id;

    // The error is cleared even for the same id, so that choosing it again retries it.
    _imp->setRendererErrorCopy(std::string());
    if (renderer == _imp->rendererId) {
        return;
    }
    _imp->rendererId = renderer;
    _imp->dropEngine();
}

const std::string&
SceneViewportRenderer::getRendererId() const
{
    return _imp->rendererId;
}

std::string
SceneViewportRenderer::getRendererError() const
{
    return _imp->getRendererErrorCopy();
}

std::string
SceneViewportRenderer::getEngineRendererId() const
{
    std::lock_guard<std::shared_mutex> lock(_imp->stageMutex);

    return _imp->engine ? _imp->engine->GetCurrentRendererId().GetString() : std::string();
}

void
SceneViewportRenderer::getBackgroundColor(float rgb[3]) const
{
    for (int c = 0; c < 3; ++c) {
        rgb[c] = _imp->backgroundDisplay[c];
    }
}

// ---------------------------------------------------------------------------

NATRON_NAMESPACE_ANONYMOUS_ENTER

// The application's bindings, or null in a process without the GUI application manager.
GuiApplicationManager*
guiManager()
{
    return dynamic_cast<GuiApplicationManager*>(AppManager::instance());
}

bool
matchesMouseBinding(const char* actionID,
                    Qt::KeyboardModifiers modifiers,
                    Qt::MouseButton button)
{
    if (GuiApplicationManager* manager = guiManager()) {
        return manager->matchesMouseShortcut(kShortcutGroupViewer3D, actionID, modifiers, button);
    }
    // Without the application, the default preset's bindings.
    MouseAction action;
    if (std::string(actionID) == kShortcutIDMouseViewer3DDollyWheel) {
        action.gesture = eMouseGestureWheel;
    }
    std::vector<MouseBinding> bindings;
    getViewer3DNavigationPresetBindings((int)Settings::eViewer3DNavigationNuke, QString::fromUtf8(actionID), &bindings);
    action.setBindings(bindings);

    return action.matches(modifiers, button);
}

// The 3D viewer's default key binding of actionID, for a process without the application.
const Viewer3DKeyBinding*
defaultKeyBinding(const char* actionID)
{
    for (const Viewer3DKeyBinding& binding : getViewer3DDefaultKeyBindings()) {
        if (std::string(binding.actionID) == actionID) {
            return &binding;
        }
    }

    return nullptr;
}

bool
matchesKeyBinding(const char* actionID,
                  Qt::KeyboardModifiers modifiers,
                  int key)
{
    if (GuiApplicationManager* manager = guiManager()) {
        return manager->matchesKeybind(kShortcutGroupViewer3D, actionID, modifiers, key);
    }
    const Qt::KeyboardModifiers relevant = Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier | Qt::MetaModifier;
    const Viewer3DKeyBinding* binding = defaultKeyBinding(actionID);
    if (!binding) {
        return false;
    }

    return ((binding->key == key) && ((modifiers & relevant) == binding->modifiers)) || (binding->altKey && (binding->altKey == key) && ((modifiers & relevant) == binding->altModifiers));
}

// The transform widget's tools, their key actions, script names and menu labels.
struct TransformToolEntry {
    SceneTransform::ToolEnum tool;
    const char* actionID;
    const char* name;
    const char* label;
};

const TransformToolEntry kTransformTools[] = {
    { SceneTransform::eToolSelect, kShortcutIDActionViewer3DSelectTool, "select", kShortcutDescActionViewer3DSelectTool },
    { SceneTransform::eToolTranslate, kShortcutIDActionViewer3DTranslateTool, "translate", kShortcutDescActionViewer3DTranslateTool },
    { SceneTransform::eToolRotate, kShortcutIDActionViewer3DRotateTool, "rotate", kShortcutDescActionViewer3DRotateTool },
    { SceneTransform::eToolScale, kShortcutIDActionViewer3DScaleTool, "scale", kShortcutDescActionViewer3DScaleTool },
};

// The tool a key selects, -1 for none.
int
transformToolForKey(Qt::KeyboardModifiers modifiers,
                    int key)
{
    for (const TransformToolEntry& entry : kTransformTools) {
        if (matchesKeyBinding(entry.actionID, modifiers, key)) {
            return entry.tool;
        }
    }

    return -1;
}

// The script name of a tool, as the natronSceneTransformTool property reports it.
const char*
transformToolName(int tool)
{
    for (const TransformToolEntry& entry : kTransformTools) {
        if (entry.tool == tool) {
            return entry.name;
        }
    }

    return "";
}

// Whether a key is one of the transform widget's.
bool
isTransformKey(Qt::KeyboardModifiers modifiers,
               int key)
{
    return (transformToolForKey(modifiers, key) >= 0) || matchesKeyBinding(kShortcutIDActionViewer3DCycleTool, modifiers, key)
        || matchesKeyBinding(kShortcutIDActionViewer3DToggleSpace, modifiers, key) || matchesKeyBinding(kShortcutIDActionViewer3DTogglePivot, modifiers, key);
}

// Whether a mouse binding of the 3D viewer needs Space held.
bool
bindingsUseSpace()
{
    GuiApplicationManager* manager = guiManager();
    if (!manager) {
        return false;
    }
    const AppShortcuts& shortcuts = manager->getAllShortcuts();
    AppShortcuts::const_iterator group = shortcuts.find(QString::fromUtf8(kShortcutGroupViewer3D));
    if (group == shortcuts.end()) {
        return false;
    }
    for (GroupShortcuts::const_iterator it = group->second.begin(); it != group->second.end(); ++it) {
        if (!dynamic_cast<const MouseAction*>(it->second)) {
            continue;
        }
        for (std::list<Qt::KeyboardModifiers>::const_iterator m = it->second->modifiers.begin(); m != it->second->modifiers.end(); ++m) {
            if (*m & kShortcutModifierSpace) {
                return true;
            }
        }
    }

    return false;
}

QKeySequence
keySequenceOf(const char* actionID)
{
    if (GuiApplicationManager* manager = guiManager()) {
        std::list<QKeySequence> sequences = manager->getKeySequenceForAction(QString::fromUtf8(kShortcutGroupViewer3D), QString::fromUtf8(actionID));

        return sequences.empty() ? QKeySequence() : sequences.front();
    }
    const Viewer3DKeyBinding* binding = defaultKeyBinding(actionID);

    return binding ? QKeySequence(int(binding->modifiers) | int(binding->key)) : QKeySequence();
}

// A shortcut action's description in the context its translations are extracted under.
QString
shortcutText(const char* description)
{
    return QCoreApplication::translate("ActionWithShortcut", description);
}

// The orthographic and perspective views, their key actions and menu labels.
struct ViewModeEntry {
    SceneViewportRenderer::ViewModeEnum mode;
    const char* actionID;
    const char* label;
};

const ViewModeEntry kViewModes[] = {
    { SceneViewportRenderer::eViewModePerspective, kShortcutIDActionViewer3DPerspective, kShortcutDescActionViewer3DPerspective },
    { SceneViewportRenderer::eViewModeFront, kShortcutIDActionViewer3DFront, kShortcutDescActionViewer3DFront },
    { SceneViewportRenderer::eViewModeBack, kShortcutIDActionViewer3DBack, kShortcutDescActionViewer3DBack },
    { SceneViewportRenderer::eViewModeRight, kShortcutIDActionViewer3DRight, kShortcutDescActionViewer3DRight },
    { SceneViewportRenderer::eViewModeLeft, kShortcutIDActionViewer3DLeft, kShortcutDescActionViewer3DLeft },
    { SceneViewportRenderer::eViewModeTop, kShortcutIDActionViewer3DTop, kShortcutDescActionViewer3DTop },
    { SceneViewportRenderer::eViewModeBottom, kShortcutIDActionViewer3DBottom, kShortcutDescActionViewer3DBottom },
};

// The view mode a key press asks for, or -1.
int
viewModeForKey(Qt::KeyboardModifiers modifiers,
               int key)
{
    for (const ViewModeEntry& entry : kViewModes) {
        if (matchesKeyBinding(entry.actionID, modifiers, key)) {
            return (int)entry.mode;
        }
    }

    return -1;
}

// The draw modes, their key actions and menu labels.
struct DrawModeEntry {
    SceneViewportRenderer::DrawModeEnum mode;
    const char* actionID;
    const char* label;
    const char* name;
};

const DrawModeEntry kDrawModes[] = {
    { SceneViewportRenderer::eDrawModeUnshaded, kShortcutIDActionViewer3DUnshaded, kShortcutDescActionViewer3DUnshaded, "unshaded" },
    { SceneViewportRenderer::eDrawModeShaded, kShortcutIDActionViewer3DShaded, kShortcutDescActionViewer3DShaded, "shaded" },
    { SceneViewportRenderer::eDrawModeWireframe, kShortcutIDActionViewer3DWireframe, kShortcutDescActionViewer3DWireframe, "wireframe" },
};

// The draw mode a key press asks for, or -1.
int
drawModeForKey(Qt::KeyboardModifiers modifiers,
               int key)
{
    for (const DrawModeEntry& entry : kDrawModes) {
        if (matchesKeyBinding(entry.actionID, modifiers, key)) {
            return (int)entry.mode;
        }
    }

    return -1;
}

const char*
drawModeName(SceneViewportRenderer::DrawModeEnum mode)
{
    for (const DrawModeEntry& entry : kDrawModes) {
        if (entry.mode == mode) {
            return entry.name;
        }
    }

    return "";
}

// The lighting modes, their menu labels and script names.
struct LightingModeEntry {
    SceneViewportRenderer::LightingModeEnum mode;
    const char* label;
    const char* name;
};

const LightingModeEntry kLightingModes[] = {
    { SceneViewportRenderer::eLightingModeAuto, QT_TRANSLATE_NOOP("SceneViewport", "Auto"), "auto" },
    { SceneViewportRenderer::eLightingModeScene, QT_TRANSLATE_NOOP("SceneViewport", "Scene Lights"), "scene" },
    { SceneViewportRenderer::eLightingModeHeadlight, QT_TRANSLATE_NOOP("SceneViewport", "Headlight"), "headlight" },
};

const char*
lightingModeName(SceneViewportRenderer::LightingModeEnum mode)
{
    for (const LightingModeEntry& entry : kLightingModes) {
        if (entry.mode == mode) {
            return entry.name;
        }
    }

    return "";
}

// The script name of a view mode, as the natronSceneViewMode property reports it.
const char*
viewModeName(SceneViewportRenderer::ViewModeEnum mode)
{
    switch (mode) {
    case SceneViewportRenderer::eViewModePerspective:
        return "perspective";
    case SceneViewportRenderer::eViewModeFront:
        return "front";
    case SceneViewportRenderer::eViewModeBack:
        return "back";
    case SceneViewportRenderer::eViewModeRight:
        return "right";
    case SceneViewportRenderer::eViewModeLeft:
        return "left";
    case SceneViewportRenderer::eViewModeTop:
        return "top";
    case SceneViewportRenderer::eViewModeBottom:
        return "bottom";
    }

    return "perspective";
}

QStringList
toStringList(const std::vector<std::string>& paths)
{
    QStringList ret;
    for (const std::string& path : paths) {
        ret.push_back(QString::fromStdString(path));
    }

    return ret;
}

GfMatrix4d
loadMatrix(const double values[16])
{
    GfMatrix4d matrix;

    std::copy(values, values + 16, matrix.data());

    return matrix;
}

// Sets a dimension of a locked camera's knob: at frame when it is animated, which keys it there.
// As a GUI edit, not a user one: a knob's widget skips refreshing on a user edit, which it takes
// for one typed into it, so the camera's panel would go on showing the old values.
void
setCameraKnobValue(const KnobDoublePtr& knob,
                   int dimension,
                   double frame,
                   bool animated,
                   double value)
{
    if (animated) {
        knob->setValueAtTime(frame, value, ViewSpec::all(), dimension, eValueChangedReasonNatronGuiEdited, nullptr);
    } else {
        knob->setValue(value, ViewSpec::all(), dimension, eValueChangedReasonNatronGuiEdited, nullptr);
    }
}

/**
 * @brief A move of a locked camera: the translate and rotate values its node had before and after
 * it, at a frame. The end of the move wrote the knobs before pushing it, so the first redo does nothing.
 * Undoing a dimension that was animated without a key at the frame removes the key it added.
 **/
class LockedCameraMoveCommand
    : public QUndoCommand {
public:
    struct Dimension {
        KnobDoubleWPtr knob;
        int dimension = 0;
        double oldValue = 0.;
        double newValue = 0.;
        bool animated = false;
        bool hadKey = false;
    };

    LockedCameraMoveCommand(const NodePtr& node,
                            double frame,
                            const std::vector<Dimension>& dimensions,
                            const QString& text)
        : QUndoCommand(text)
        , _node(node)
        , _frame(frame)
        , _dimensions(dimensions)
    {
    }

    void undo() override
    {
        apply(true);
    }

    void redo() override
    {
        if (_firstRedo) {
            _firstRedo = false;

            return;
        }
        apply(false);
    }

private:
    void apply(bool old)
    {
        NodePtr node = _node.lock();
        EffectInstancePtr effect = node ? node->getEffectInstance() : EffectInstancePtr();

        if (!effect) {
            return;
        }
        effect->beginChanges();
        for (const Dimension& d : _dimensions) {
            KnobDoublePtr knob = d.knob.lock();
            if (!knob) {
                continue;
            }
            if (old && d.animated && !d.hadKey) {
                // Public on the knob interface only.
                const KnobIPtr base = knob;
                base->deleteValueAtTime(eCurveChangeReasonInternal, _frame, ViewSpec::all(), d.dimension, false);
            } else {
                setCameraKnobValue(knob, d.dimension, _frame, d.animated, old ? d.oldValue : d.newValue);
            }
        }
        effect->endChanges();
    }

    NodeWPtr _node;
    double _frame;
    std::vector<Dimension> _dimensions;
    bool _firstRedo = true;
};
NATRON_NAMESPACE_ANONYMOUS_EXIT

struct SceneViewport::Implementation {
    SceneViewportRenderer renderer;
    SceneDrawStates drawStates;
    std::size_t drawRevealBatch = 0;
    QString error;
    bool glInitialized = false;

    // What a drag of the pressed button does.
    enum DragRoleEnum {
        eDragRoleNone,
        eDragRoleTumble,
        eDragRolePan,
        eDragRoleDolly,
        eDragRoleMarquee
    };

    // The mouse gesture under way: the button, where it was pressed, whether it has moved past
    // the drag threshold, what the drag does, and whether a release without a drag selects the
    // prim under the press (in selectionMode, which a marquee uses too) or opens the context menu.
    Qt::MouseButton button = Qt::NoButton;
    QPoint pressPos;
    QPoint lastPos;
    bool dragging = false;
    DragRoleEnum dragRole = eDragRoleNone;
    bool clickSelects = false;
    SelectionModeEnum selectionMode = eSelectionModeReplace;
    QString pressPrim;
    bool menuOnRelease = false;

    // What the view showed: a deferred pick means what was under the mouse then, or nothing.
    struct PressView {
        SceneViewportRenderer::FreeCamera camera;
        std::string lookThrough;
        int width = 0;
        int height = 0;

        bool operator==(const PressView& other) const
        {
            return std::equal(camera.pivot, camera.pivot + 3, other.camera.pivot) && (camera.distance == other.camera.distance)
                && (camera.azimuth == other.camera.azimuth) && (camera.elevation == other.camera.elevation) && (camera.fovY == other.camera.fovY)
                && (camera.viewMode == other.camera.viewMode) && (lookThrough == other.lookThrough) && (width == other.width) && (height == other.height);
        }
    };

    PressView currentView(const QWidget* widget) const
    {
        PressView view;

        view.camera = renderer.getFreeCamera();
        view.lookThrough = renderer.getLookThroughCamera();
        view.width = widget->width();
        view.height = widget->height();

        return view;
    }

    // The press under way found the stage held: it has yet to find out what it landed on.
    bool pressOwedHandle = false;
    bool pressOwedPick = false;
    PressView pressView;

    // A click or marquee released before the stage could say what it hit, oldest first.
    struct DeferredSelection {
        QPoint pos;
        QRect rect;
        bool marquee = false;
        // A handle at pos would have taken the press, so the click selects nothing there.
        bool checkHandle = false;
        // A drag that no binding gives a role leaves the selection alone if it hit nothing.
        bool emptySelects = true;
        SelectionModeEnum mode = eSelectionModeReplace;
        PressView view;
    };
    std::deque<DeferredSelection> deferredSelections;

    // The handle drag found the stage held: the last move has yet to be drawn, or if changeStale,
    // even measured, which the release needs before it can write the drag.
    bool handleMoveOwed = false;
    bool handleChangeStale = false;
    QPoint handleMovePos;
    bool handleEndOwed = false;

    bool previewClearOwed = false;
    bool cameraFromViewOwed = false;

    void clearPreview()
    {
        previewClearOwed = !renderer.clearTransformPreview();
    }

    // Space is down, while a binding of the 3D viewer uses it as a modifier.
    bool spaceHeld = false;

    // The project frame shown, and its rate once a scene or frame was set.
    double frame = 0.;
    double framesPerSecond = 0.;

    std::function<NodePtr()> transformNodeProvider;
    // The drag of a transform handle under way: the handle, the node whose op it moves (null
    // for the widget on the selection), and the last change it made. The renderer draws the
    // change as a preview; the release writes it once.
    int handle = SceneGizmo::kNoHandle;
    NodeWPtr handleNode;
    double handleChange = 0.;
    bool handleChanged = false;
    std::shared_ptr<SceneTransformTarget> transformTarget;
    SceneTransformRecorder transformRecorder;
    // Drops a preview whose written drag never reaches the scene shown, as when the node written
    // is not in the viewed branch. Long, since a heavy graph may take a while to evaluate before
    // it requests the scene with the drag.
    QTimer* previewTimer = nullptr;
    // While a written drag's preview is shown: the last request made before the write, so any
    // result for a later one ends the preview.
    bool previewAwaitsSync = false;
    U64 previewSerial = 0;

    std::function<NodePtr()> viewedNodeProvider;
    SceneViewport::SelectionActions selectionActions;
    std::function<NodePtr(const std::string&)> cameraNodeProvider;
    QPointer<UndoHistory> undoHistory;

    // Whether navigating through a stage camera moves its node, and why the last try could not.
    bool lockView = false;
    QString lockMessage;

    // A move of the locked camera under way. Its node's ops are taken to be the outermost of the
    // prim's, with the camera at world = inner * local * parentToWorld (row vectors), local being
    // the ops the knobs make; world is moved and local solved back from it. As the mouse moves,
    // the renderer draws the camera at world; the end of the move writes the knobs once, as one
    // undo step. Editing the knobs on every event would evaluate the graph and resync the stage
    // each time, and the view would only move as fast as that round trip.
    struct CameraLock {
        bool active = false;
        // The move found no node to edit: the rest of the gesture does nothing.
        bool refused = false;
        NodeWPtr node;
        KnobDoubleWPtr translate;
        KnobDoubleWPtr rotate;
        double frame = 0.;
        // Translate x, y, z then rotate x, y, z: before the move, now, whether the dimension
        // is animated and whether it had a key at frame.
        double oldValues[6] = { 0., 0., 0., 0., 0., 0. };
        double newValues[6] = { 0., 0., 0., 0., 0., 0. };
        bool animated[6] = { false, false, false, false, false, false };
        bool hadKey[6] = { false, false, false, false, false, false };
        GfVec3d scale = GfVec3d(1.);
        SceneRotateOrderEnum rotateOrder = eSceneRotateOrderXYZ;
        GfMatrix4d innerInverse = GfMatrix4d(1.);
        GfMatrix4d parentInverse = GfMatrix4d(1.);
        GfMatrix4d world = GfMatrix4d(1.);
        double pivotDepth = 1.;
        double viewHeight = 1.;
        bool orthographic = false;
    };
    CameraLock cameraLock;

    // After a move is written, the renderer keeps drawing the camera where the move left it until
    // a scene requested after the write is synced: the last request made before it.
    bool lockedMoveAwaitsSync = false;
    U64 lockedMoveSerial = 0;

    // Ends a run of wheel steps on the locked camera, once the wheel rests, as one move.
    QTimer* wheelMoveTimer = nullptr;

    USDEditTransform* getTransformNode(NodePtr* node) const
    {
        *node = transformNodeProvider ? transformNodeProvider() : NodePtr();
        if (!*node) {
            return nullptr;
        }

        return dynamic_cast<USDEditTransform*>((*node)->getEffectInstance().get());
    }

    // The device pixel of a widget position.
    static GfVec2d toPixel(const QWidget* widget,
                           const QPoint& pos)
    {
        const qreal dpr = widget->devicePixelRatio();

        return GfVec2d((pos.x() + 0.5) * dpr, (pos.y() + 0.5) * dpr);
    }

    // The worker syncing the renderer's stage, and the latest payload waiting for it. A newer
    // request replaces one the worker has not taken yet.
    std::thread worker;
    std::mutex queueMutex;
    std::condition_variable queueChanged;
    bool quit = false;
    bool hasPending = false;
    ScenePayload pending;
    // The live textures the pending payload shows at pendingFrame, which the worker renders
    // before it syncs: Hydra only reads them.
    LiveTextureSourceList pendingLiveTextures;
    double pendingFrame = 0.;
    double pendingFramesPerSecond = 0.;
    U64 pendingSerial = 0;

    // Main thread: the last request made, the last one synced, and the error the last request
    // shows once synced. A request whose payload shows live textures leaves the renderer at the
    // frame it had until the worker has rendered them, so Hydra never asks for one not stored.
    U64 requestedSerial = 0;
    U64 syncedSerial = 0;
    // A framing the worker has yet to take, newest only; the ids it was asked for and last answered.
    U64 pendingFramingId = 0;
    std::vector<std::string> pendingFramingSelection;
    U64 framingRequested = 0;
    U64 framingDone = 0;
    // The worker has yet to find where a newly looked-through stage camera turns about.
    bool pendingPivot = false;
    // A new engine reads each texture once and caches a fallback for one the store lacks under a
    // URI that does not change, so after the store was emptied it draws nothing until this sync.
    U64 paintHeldUntilSerial = 0;
    QString requestedError;
    ScenePayload requestedPayload;
    bool requestedLiveTextures = false;

    // The thread requests for payload loads are serviced on. It waits under this mutex's condition
    // for the stage to have requests; a request only needs to wake it.
    std::thread loadThread;
    std::mutex loadWakeMutex;
    std::condition_variable loadWake;
    bool loadQuit = false;

    // The thread the engine lives on, running jobs in order.
    std::thread renderThread;
    std::mutex renderJobsMutex;
    std::condition_variable renderJobsChanged;
    std::deque<std::function<void()>> renderJobs;
    bool renderQuit = false;
    // Whether the engine's context is the render thread's right now. Main thread.
    bool usesRenderThread = false;
    std::unique_ptr<QOffscreenSurface> renderSurface;
    // Render thread: its context, sharing the widget's, and the target it draws into.
    std::unique_ptr<QOpenGLContext> renderContext;
    GLuint renderFbo = 0;
    GLuint renderTexture = 0;
    GLuint renderDepth = 0;
    int renderWidth = 0;
    int renderHeight = 0;
    // The widget's context: framebuffer objects are not shared, so it has its own on the texture.
    GLuint presentFbo = 0;

    // Main thread.
    bool backgroundFrameRunning = false;
    // Whether the framebuffer holds a frame of a scene, which a composing status can stay out of.
    bool presentedScene = false;
    QElapsedTimer composingClock;
    QTimer* composingTimer = nullptr;

    void postToRenderThread(std::function<void()> job)
    {
        {
            std::lock_guard<std::mutex> lock(renderJobsMutex);
            renderJobs.push_back(std::move(job));
        }
        renderJobsChanged.notify_one();
    }

    // Waits for job, after any job before it. The caller's own state is safe for job to read, as
    // the mutex hand-overs order them.
    void callOnRenderThread(const std::function<void()>& job)
    {
        std::mutex doneMutex;
        std::condition_variable doneChanged;
        bool done = false;

        postToRenderThread([&]() {
            job();
            std::lock_guard<std::mutex> lock(doneMutex);
            done = true;
            doneChanged.notify_one();
        });
        PythonUnlockedWhileWaiting unlocked;
        std::unique_lock<std::mutex> lock(doneMutex);
        doneChanged.wait(lock, [&done]() { return done; });
    }

    void startRenderThread()
    {
        if (renderThread.joinable()) {
            return;
        }
        renderThread = std::thread([this]() {
            for (;;) {
                std::function<void()> job;
                {
                    std::unique_lock<std::mutex> lock(renderJobsMutex);
                    renderJobsChanged.wait(lock, [this]() { return renderQuit || !renderJobs.empty(); });
                    if (renderJobs.empty()) {
                        return;
                    }
                    job = std::move(renderJobs.front());
                    renderJobs.pop_front();
                }
                job();
            }
        });
    }

    // Main thread, the widget's context current. False if this platform cannot, and the engine
    // then stays in the widget's context.
    bool startRenderContext(QOpenGLContext* share)
    {
        if (!QOpenGLContext::supportsThreadedOpenGL() || !AppManager::instance()->isOpenGLLoaded()) {
            return false;
        }
        // A surface is created on the main thread and may be used from any.
        renderSurface.reset(new QOffscreenSurface());
        renderSurface->setFormat(share->format());
        renderSurface->create();
        if (!renderSurface->isValid()) {
            renderSurface.reset();

            return false;
        }
        startRenderThread();
        bool started = false;
        callOnRenderThread([&]() {
            renderContext.reset(new QOpenGLContext());
            renderContext->setFormat(share->format());
            renderContext->setShareContext(share);
            started = renderContext->create() && QOpenGLContext::areSharing(renderContext.get(), share) && renderContext->makeCurrent(renderSurface.get());
            if (!started) {
                renderContext.reset();
            }
        });
        if (!started) {
            renderSurface.reset();
        }
        usesRenderThread = started;

        return started;
    }

    // Render thread. The target of a width x height frame, reallocated when the size changes.
    GLuint ensureRenderTarget(int width,
                              int height)
    {
        if (renderFbo && (renderWidth == width) && (renderHeight == height)) {
            return renderFbo;
        }
        if (!renderFbo) {
            glGenFramebuffers(1, &renderFbo);
            glGenTextures(1, &renderTexture);
            glGenRenderbuffers(1, &renderDepth);
        }
        glBindTexture(GL_TEXTURE_2D, renderTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindRenderbuffer(GL_RENDERBUFFER, renderDepth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, renderFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, renderTexture, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, renderDepth);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        renderWidth = width;
        renderHeight = height;

        return renderFbo;
    }

    // Render thread: everything of the render context, the engine first.
    void releaseRenderContext()
    {
        if (!renderContext) {
            return;
        }
        renderer.releaseGL();
        glDeleteFramebuffers(1, &renderFbo);
        glDeleteTextures(1, &renderTexture);
        glDeleteRenderbuffers(1, &renderDepth);
        renderFbo = renderTexture = renderDepth = 0;
        renderWidth = renderHeight = 0;
        renderContext->doneCurrent();
        renderContext.reset();
    }

    // Main thread, the widget's context current, with the engine's context dying or the widget going.
    void releaseGLResources()
    {
        if (!usesRenderThread) {
            renderer.releaseGL();

            return;
        }
        callOnRenderThread([this]() { releaseRenderContext(); });
        if (presentFbo) {
            glDeleteFramebuffers(1, &presentFbo);
            presentFbo = 0;
        }
        renderSurface.reset();
        usesRenderThread = false;
        backgroundFrameRunning = false;
        presentedScene = false;
    }

    void stopRenderThread()
    {
        if (!renderThread.joinable()) {
            return;
        }
        postToRenderThread([this]() { releaseRenderContext(); });
        {
            std::lock_guard<std::mutex> lock(renderJobsMutex);
            renderQuit = true;
        }
        renderJobsChanged.notify_one();
        renderThread.join();
    }

    // Main thread, the widget's context current. The render thread has finished its work on the
    // texture when fence signals.
    void presentRenderTarget(GLuint target,
                             int width,
                             int height,
                             GLsync fence)
    {
        if (fence) {
            glWaitSync(fence, 0, GL_TIMEOUT_IGNORED);
            glDeleteSync(fence);
        }
        if (!presentFbo) {
            glGenFramebuffers(1, &presentFbo);
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, presentFbo);
        // Attached anew each time: a change made in another context is only seen after a bind, and
        // a resize has given the texture a new image.
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, renderTexture, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_FRAMEBUFFER_SRGB);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, target);
    }
};

SceneViewport::SceneViewport(QWidget* parent)
    : QOpenGLWidget(parent)
    , _imp(new Implementation)
{
    setFocusPolicy(Qt::ClickFocus);
    // Hovering highlights the transform handle under the pointer.
    setMouseTracking(true);
    setObjectName(QString::fromUtf8("SceneViewport"));
    // The selected prims, the current one last, for scripts.
    setProperty("natronSceneSelection", QStringList());
    // A paint skipped while the worker syncs leaves the framebuffer as it was. Assumed from Qt's
    // documentation of QOpenGLWidget::UpdateBehavior, which says PartialUpdate keeps the
    // framebuffer's content between paints; not checked against Qt's source.
    setUpdateBehavior(QOpenGLWidget::PartialUpdate);

    _imp->wheelMoveTimer = new QTimer(this);
    _imp->wheelMoveTimer->setSingleShot(true);
    _imp->wheelMoveTimer->setInterval(300);
    QObject::connect(_imp->wheelMoveTimer, &QTimer::timeout, this, [this]() {
        if (_imp->button == Qt::NoButton) {
            endCameraLock();
        }
    });
    _imp->previewTimer = new QTimer(this);
    _imp->previewTimer->setSingleShot(true);
    _imp->previewTimer->setInterval(30000);
    QObject::connect(_imp->previewTimer, &QTimer::timeout, this, [this]() {
        if (!_imp->previewAwaitsSync || (_imp->handle != SceneGizmo::kNoHandle)) {
            return;
        }
        if ((_imp->requestedSerial > _imp->previewSerial) && (_imp->syncedSerial < _imp->requestedSerial)) {
            // The scene with the drag is on its way: its result ends the preview.
            _imp->previewTimer->start();

            return;
        }
        _imp->previewAwaitsSync = false;
        _imp->clearPreview();
        update();
    });
    // Queued: it runs on the reader's thread, and readers end before the viewport goes.
    _imp->renderer.setReadEndedCallback([this]() { QMetaObject::invokeMethod(this, [this]() { serviceDeferredInput(); }, Qt::QueuedConnection); });

    SettingsPtr settings = appPTR ? appPTR->getCurrentSettings() : SettingsPtr();
    if (settings) {
        _imp->renderer.setGuideSize(settings->getViewer3DGuideSize());
        // The Viewer 3D Lighting preference is every 3D view's lighting mode.
        _imp->renderer.setLightingMode((SceneViewportRenderer::LightingModeEnum)settings->getViewer3DLighting());
        QObject::connect(settings.get(), &Settings::settingChanged, this, [this]() {
            SettingsPtr current = appPTR->getCurrentSettings();
            const double size = current ? current->getViewer3DGuideSize() : _imp->renderer.getGuideSize();
            const SceneViewportRenderer::LightingModeEnum lighting = current ? (SceneViewportRenderer::LightingModeEnum)current->getViewer3DLighting() : _imp->renderer.getLightingMode();
            if ((size != _imp->renderer.getGuideSize()) || (lighting != _imp->renderer.getLightingMode())) {
                _imp->renderer.setGuideSize(size);
                _imp->renderer.setLightingMode(lighting);
                publishState();
                update();
            }
        });
    }

    _imp->composingTimer = new QTimer(this);
    _imp->composingTimer->setSingleShot(true);
    _imp->composingTimer->setInterval(kComposingStatusDelayMs);
    QObject::connect(_imp->composingTimer, &QTimer::timeout, this, [this]() { update(); });

    Implementation* imp = _imp.get();
    _imp->loadThread = std::thread([this, imp]() {
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(imp->loadWakeMutex);
                imp->loadWake.wait(lock, [imp]() { return imp->loadQuit || imp->renderer.hasPendingStageLoads(); });
                if (imp->loadQuit) {
                    return;
                }
            }
            imp->renderer.serviceStageLoads();
            // Payloads without extent hints leave nothing to frame until one of them loads.
            const SceneViewportRenderer::FrameBounds bounds = imp->renderer.isFramed() ? SceneViewportRenderer::FrameBounds() : imp->renderer.computeFrameBounds(std::vector<std::string>(), imp->renderer.getTimeCode());
            // Dropped by Qt if the widget is gone by then.
            QMetaObject::invokeMethod(this, [this, bounds]() { onStageLoadsServiced(bounds); }, Qt::QueuedConnection);
        }
    });
    _imp->worker = std::thread([this, imp]() {
        for (;;) {
            ScenePayload payload;
            U64 serial = 0;
            LiveTextureSourceList liveTextures;
            double frame = 0.;
            double framesPerSecond = 0.;
            bool sync = false;
            U64 framingId = 0;
            std::vector<std::string> framingSelection;
            bool pivot = false;
            {
                std::unique_lock<std::mutex> lock(imp->queueMutex);
                imp->queueChanged.wait(lock, [imp]() { return imp->quit || imp->hasPending || (imp->pendingFramingId != 0) || imp->pendingPivot; });
                if (imp->quit) {
                    return;
                }
                sync = imp->hasPending;
                if (sync) {
                    payload = imp->pending;
                    imp->pending = ScenePayload();
                    imp->hasPending = false;
                    serial = imp->pendingSerial;
                    liveTextures.swap(imp->pendingLiveTextures);
                    frame = imp->pendingFrame;
                    framesPerSecond = imp->pendingFramesPerSecond;
                }
                framingId = imp->pendingFramingId;
                imp->pendingFramingId = 0;
                framingSelection.swap(imp->pendingFramingSelection);
                pivot = imp->pendingPivot;
                imp->pendingPivot = false;
            }
            if (sync) {
                std::string error;
                bool ok;
                if (liveTextures.empty()) {
                    ok = imp->renderer.syncScene(payload, &error, serial);
                } else {
                    LiveTextureStore::instance().prepare(liveTextures, frame);
                    liveTextures.clear();
                    ok = imp->renderer.syncScene(payload, frame, framesPerSecond, &error, serial);
                }
                const QString qerror = QString::fromStdString(error);
                // The first framing lands with the sync, so the first frame drawn is framed.
                const SceneViewportRenderer::FrameBounds bounds = (!ok || imp->renderer.isFramed()) ? SceneViewportRenderer::FrameBounds() : imp->renderer.computeFrameBounds(std::vector<std::string>(), imp->renderer.getTimeCode());
                // Dropped by Qt if the widget is gone by then.
                QMetaObject::invokeMethod(this, [this, serial, ok, qerror, bounds]() { onSynced(serial, ok, qerror, bounds); }, Qt::QueuedConnection);
            }
            if (framingId != 0) {
                const SceneViewportRenderer::FrameBounds bounds = imp->renderer.computeFrameBounds(framingSelection, imp->renderer.getTimeCode());
                QMetaObject::invokeMethod(this, [this, framingId, bounds]() { onFramingComputed(framingId, bounds); }, Qt::QueuedConnection);
            }
            if (pivot) {
                imp->renderer.computeFrameBounds(std::vector<std::string>(), imp->renderer.getTimeCode());
            }
        }
    });
    publishState();
}

SceneViewport::~SceneViewport()
{
    Q_EMIT closing();
    {
        std::lock_guard<std::mutex> lock(_imp->queueMutex);
        _imp->quit = true;
    }
    _imp->queueChanged.notify_all();
    if (_imp->worker.joinable()) {
        _imp->worker.join();
    }
    {
        std::lock_guard<std::mutex> lock(_imp->loadWakeMutex);
        _imp->loadQuit = true;
    }
    _imp->loadWake.notify_all();
    if (_imp->loadThread.joinable()) {
        _imp->loadThread.join();
    }
    GLContextLoss::releaseNow(this);
    _imp->stopRenderThread();
}

void
SceneViewport::requestSync(const ScenePayload& payload,
                           const QString& error)
{
    _imp->requestedError = error;
    _imp->requestedPayload = payload;
    LiveTextureSourceList liveTextures = SceneOps::getLiveTextures(payload);
    _imp->requestedLiveTextures = !liveTextures.empty();
    if (!_imp->requestedLiveTextures && (_imp->framesPerSecond > 0.)) {
        _imp->renderer.setFrame(_imp->frame, _imp->framesPerSecond);
    }
    {
        std::lock_guard<std::mutex> lock(_imp->queueMutex);
        _imp->pending = payload;
        _imp->hasPending = true;
        _imp->pendingLiveTextures.swap(liveTextures);
        _imp->pendingFrame = _imp->frame;
        _imp->pendingFramesPerSecond = _imp->framesPerSecond;
        _imp->pendingSerial = ++_imp->requestedSerial;
    }
    _imp->queueChanged.notify_one();
    publishState();
    if (!_imp->renderer.hasScene()) {
        // The composing status goes up without waiting for the worker.
        update();
    }
}

void
SceneViewport::onSynced(U64 serial,
                        bool ok,
                        const QString& error,
                        const SceneViewportRenderer::FrameBounds& bounds)
{
    _imp->syncedSerial = std::max(_imp->syncedSerial, serial);
    if (_imp->previewAwaitsSync && (serial > _imp->previewSerial)) {
        // A good sync normally drops the preview with the stage update; this covers a failed one
        // and one synced before the drag's write was known to the renderer.
        _imp->previewAwaitsSync = false;
        _imp->previewTimer->stop();
        _imp->clearPreview();
        update();
    }
    serviceDeferredInput();
    if (serial != _imp->requestedSerial) {
        // A newer request is on its way, but this one is drawn meanwhile: in playback each frame's
        // request is overtaken by the next before its sync ends, so drawing only caught-up syncs
        // would draw almost none. The renderer is consistent: the stage at the frame whose
        // textures were just rendered.
        publishState();
        if (ok) {
            update();
        }

        return;
    }
    _imp->error = ok ? _imp->requestedError : error;
    if (_imp->lockedMoveAwaitsSync && !_imp->cameraLock.active && (serial > _imp->lockedMoveSerial)) {
        // The stage has the written move: the camera's prim is where the view shows it.
        _imp->lockedMoveAwaitsSync = false;
        _imp->renderer.clearLookThroughTransform();
    }
    if (ok && _imp->renderer.hasScene() && !_imp->renderer.isFramed()) {
        _imp->renderer.applyFrameBounds(bounds);
        endLockOutsideLookThrough();
    }
    // With natronSceneLastPaintMs, lets a benchmark time an edit to the frame that shows it.
    setProperty("natronSceneSyncedAtMs", QVariant::fromValue<qlonglong>(QDateTime::currentMSecsSinceEpoch()));
    startBackgroundFrame();
    publishState();
    update();
    Q_EMIT sceneSynced();
}

bool
SceneViewport::isSceneSynced() const
{
    if ((_imp->syncedSerial != _imp->requestedSerial) || (_imp->framingDone != _imp->framingRequested)) {
        return false;
    }
    // A view nobody sees takes nothing in until it is shown.
    const bool awaitsEngine = _imp->usesRenderThread && isVisible() && (width() > 0) && (height() > 0);

    return !awaitsEngine || (!_imp->backgroundFrameRunning && !_imp->renderer.needsBackgroundFrame());
}

void
SceneViewport::startBackgroundFrame()
{
    if (!_imp->usesRenderThread || _imp->backgroundFrameRunning || !isVisible() || (_imp->syncedSerial < _imp->paintHeldUntilSerial)) {
        return;
    }
    const qreal dpr = devicePixelRatio();
    const int w = int(width() * dpr);
    const int h = int(height() * dpr);
    if (!_imp->renderer.prepareBackgroundFrame(w, h)) {
        return;
    }
    _imp->backgroundFrameRunning = true;
    _imp->composingClock.start();
    _imp->composingTimer->start();
    Implementation* imp = _imp.get();
    _imp->postToRenderThread([this, imp, w, h]() {
        imp->renderer.renderBackgroundFrame(imp->ensureRenderTarget(w, h));
        // The widget's context reads the image only after a later fence, but the commands must be
        // on their way before this thread goes idle.
        glFlush();
        // Dropped by Qt if the widget is gone by then.
        QMetaObject::invokeMethod(this, [this]() { onBackgroundFrameDone(); }, Qt::QueuedConnection);
    });
    publishState();
}

void
SceneViewport::onBackgroundFrameDone()
{
    _imp->backgroundFrameRunning = false;
    _imp->composingTimer->stop();
    // The stage may have changed again while the frame ran.
    startBackgroundFrame();
    serviceDeferredInput();
    publishState();
    update();
}

void
SceneViewport::onStageLoadsServiced(const SceneViewportRenderer::FrameBounds& bounds)
{
    if (!bounds.empty && !_imp->renderer.isFramed()) {
        _imp->renderer.applyFrameBounds(bounds);
        endLockOutsideLookThrough();
    }
    startBackgroundFrame();
    serviceDeferredInput();
    publishState();
    update();
    Q_EMIT stageLoadsChanged();
}

void
SceneViewport::serviceDeferredInput()
{
    if (_imp->previewClearOwed) {
        _imp->clearPreview();
        if (!_imp->previewClearOwed) {
            update();
        }
    }
    if (_imp->cameraFromViewOwed) {
        _imp->cameraFromViewOwed = false;
        createCameraFromView();
    }
    if (_imp->handle != SceneGizmo::kNoHandle) {
        if (_imp->handleMoveOwed) {
            dragHandle(_imp->handleMovePos);
        }
        if (_imp->handleEndOwed && !_imp->handleChangeStale) {
            _imp->handleEndOwed = false;
            endHandleDrag();
        }
    }
    const Implementation::PressView view = _imp->currentView(this);
    const qreal dpr = devicePixelRatio();
    while (!_imp->deferredSelections.empty()) {
        const Implementation::DeferredSelection deferred = _imp->deferredSelections.front();
        bool busy = false;
        if (deferred.view == view) {
            if (deferred.checkHandle) {
                const GfVec2d pixel = Implementation::toPixel(this, deferred.pos);
                if (_imp->renderer.hitTestTransformHandles(pixel[0], pixel[1], int(width() * dpr), int(height() * dpr), &busy) != SceneGizmo::kNoHandle) {
                    _imp->deferredSelections.pop_front();
                    continue;
                }
                if (busy) {
                    return;
                }
            }
            QStringList paths;
            if (deferred.marquee) {
                paths = pickPrimsIn(deferred.rect, &busy);
            } else {
                const QString prim = pickPrimAt(deferred.pos, &busy);
                if (!prim.isEmpty()) {
                    paths.push_back(prim);
                }
            }
            if (busy) {
                return;
            }
            _imp->deferredSelections.pop_front();
            if (!paths.isEmpty() || deferred.emptySelects) {
                select(paths, deferred.mode);
            }
            continue;
        }
        // The view moved on: selecting what is there now is not what was clicked.
        _imp->deferredSelections.pop_front();
    }
    if (_imp->button == Qt::NoButton) {
        return;
    }
    const bool navigated = _imp->dragging && (_imp->dragRole != Implementation::eDragRoleMarquee) && (_imp->dragRole != Implementation::eDragRoleNone);
    if (_imp->pressOwedHandle) {
        if (navigated || !(_imp->pressView == view)) {
            _imp->pressOwedHandle = false;
        } else {
            bool busy = false;
            if (beginHandleDrag(_imp->pressPos, &busy)) {
                const QPoint pos = _imp->lastPos;
                clearGesture();
                dragHandle(pos);

                return;
            }
            if (busy) {
                return;
            }
            _imp->pressOwedHandle = false;
        }
    }
    // Left owed if the view moved: the release drops it with the view it remembers.
    if (_imp->pressOwedPick && (_imp->pressView == view)) {
        bool busy = false;
        const QString prim = pickPrimAt(_imp->pressPos, &busy);
        if (!busy) {
            _imp->pressPrim = prim;
            _imp->pressOwedPick = false;
        }
    }
} // SceneViewport::serviceDeferredInput

void
SceneViewport::runWithEngineContext(const std::function<void()>& fn)
{
    if (_imp->usesRenderThread) {
        _imp->callOnRenderThread(fn);
    } else {
        makeCurrent();
        fn();
        doneCurrent();
    }
}

bool
SceneViewport::isComposing() const
{
    return _imp->backgroundFrameRunning || ((_imp->syncedSerial < _imp->requestedSerial) && !_imp->requestedPayload.isEmpty() && !_imp->renderer.hasScene());
}

void
SceneViewport::paintComposingStatus()
{
    if (!_imp->presentedScene) {
        float background[3];
        _imp->renderer.getBackgroundColor(background);
        glClearColor(background[0], background[1], background[2], 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    const QString text = tr("Composing scene...");
    QPainter painter(this);
    painter.setRenderHint(QPainter::TextAntialiasing);
    QRect box = painter.fontMetrics().boundingRect(text).adjusted(-10, -6, 10, 6);
    box.moveCenter(rect().center());
    // Opaque: paints repeat over a framebuffer that keeps its content, and text alone would pile up.
    painter.fillRect(box, QColor(40, 40, 40));
    painter.setPen(QColor(200, 200, 200));
    painter.drawText(box, Qt::AlignCenter, text);
    painter.end();
}

namespace {
std::vector<std::string>
toStdPaths(const QStringList& paths)
{
    std::vector<std::string> result;

    result.reserve(paths.size());
    for (const QString& path : paths) {
        result.push_back(path.toStdString());
    }

    return result;
}
} // anonymous namespace

void
SceneViewport::requestLoad(const QStringList& paths,
                           bool withDescendants)
{
    _imp->renderer.requestStageLoad(toStdPaths(paths), withDescendants);
    // Taking the mutex keeps the wake-up from falling between the thread's check and its wait.
    {
        std::lock_guard<std::mutex> lock(_imp->loadWakeMutex);
    }
    _imp->loadWake.notify_one();
    publishState();
}

void
SceneViewport::publishDrawStats()
{
    const SceneViewportRenderer::DrawFilterStats stats = _imp->renderer.getDrawFilterStats();
    QVariantMap published;
    published[QString::fromUtf8("filterBound")] = stats.filterBound;
    published[QString::fromUtf8("revealing")] = stats.revealing;
    published[QString::fromUtf8("lastApplyMs")] = stats.lastApplyMs;
    published[QString::fromUtf8("ruleCount")] = QVariant::fromValue<qulonglong>(stats.ruleCount);
    published[QString::fromUtf8("posted")] = QVariant::fromValue<qulonglong>(stats.posted);
    published[QString::fromUtf8("absorbed")] = QVariant::fromValue<qulonglong>(stats.absorbed);
    published[QString::fromUtf8("inputAdded")] = QVariant::fromValue<qulonglong>(stats.inputAdded);
    published[QString::fromUtf8("inputRemoved")] = QVariant::fromValue<qulonglong>(stats.inputRemoved);
    published[QString::fromUtf8("inputDirtied")] = QVariant::fromValue<qulonglong>(stats.inputDirtied);
    published[QString::fromUtf8("hiddenSent")] = QVariant::fromValue<qulonglong>(stats.hiddenSent);
    published[QString::fromUtf8("shownSent")] = QVariant::fromValue<qulonglong>(stats.shownSent);
    published[QString::fromUtf8("lastInputAddedEpochMs")] = QVariant::fromValue<qlonglong>(stats.lastInputAddedEpochMs);
    published[QString::fromUtf8("stageEdits")] = QVariant::fromValue<qulonglong>(stats.stageEdits);
    published[QString::fromUtf8("stageLoadEdits")] = QVariant::fromValue<qulonglong>(stats.stageLoadEdits);
    published[QString::fromUtf8("stageNotices")] = QVariant::fromValue<qulonglong>(stats.stageNotices);
    published[QString::fromUtf8("resyncedPaths")] = QVariant::fromValue<qulonglong>(stats.resyncedPaths);
    published[QString::fromUtf8("infoOnlyPaths")] = QVariant::fromValue<qulonglong>(stats.infoOnlyPaths);
    setProperty("natronSceneDrawStats", published);
}

void
SceneViewport::sendDrawDelta(const SceneDrawDelta& delta)
{
    if (!delta.drawn.empty() || !delta.expanded.empty() || !delta.boxed.empty() || !delta.hidden.empty() || !delta.cleared.empty()) {
        _imp->renderer.postDrawDelta(delta, _imp->drawRevealBatch);
        if (_imp->usesRenderThread) {
            Implementation* imp = _imp.get();
            _imp->postToRenderThread([imp]() { imp->renderer.applyPendingDrawDeltas(); });
        }
    }
    for (const SceneDrawLoadRequest& load : delta.loads) {
        requestLoad(QStringList(QString::fromStdString(load.path)), load.withDescendants);
    }
    publishState();
    update();
}

bool
SceneViewport::setDrawState(const QString& path,
                            const QString& state)
{
    const std::string pathString = path.toStdString();
    SceneDrawStateEnum parsed = eSceneDrawStateFull;
    if (!sceneDrawStateFromName(state.toStdString(), &parsed) || !SdfPath::IsValidPathString(pathString) || !SdfPath(pathString).IsAbsoluteRootOrPrimPath()) {
        return false;
    }
    sendDrawDelta(_imp->drawStates.setState(pathString, parsed));

    return true;
}

void
SceneViewport::clearDrawState(const QString& path)
{
    sendDrawDelta(_imp->drawStates.clearState(path.toStdString()));
}

void
SceneViewport::clearDrawStates()
{
    sendDrawDelta(_imp->drawStates.clearAllStates());
}

QString
SceneViewport::resolvedDrawState(const QString& path) const
{
    return QString::fromUtf8(sceneDrawStateName(_imp->drawStates.resolve(path.toStdString()).state));
}

void
SceneViewport::setDrawRevealBatch(int prims)
{
    _imp->drawRevealBatch = std::size_t((std::max)(prims, 0));
}

void
SceneViewport::requestUnload(const QStringList& paths)
{
    _imp->renderer.requestStageUnload(toStdPaths(paths));
    {
        std::lock_guard<std::mutex> lock(_imp->loadWakeMutex);
    }
    _imp->loadWake.notify_one();
    publishState();
}

void
SceneViewport::cancelPendingLoads()
{
    _imp->renderer.cancelStageLoads();
    publishState();
    Q_EMIT stageLoadsChanged();
}

void
SceneViewport::setScene(const ScenePayload& payload,
                        double frame,
                        double framesPerSecond)
{
    _imp->frame = frame;
    _imp->framesPerSecond = framesPerSecond;
    requestSync(payload, QString());
}

void
SceneViewport::setFrame(double frame,
                        double framesPerSecond)
{
    if ((frame != _imp->frame) && !_imp->cameraLock.active) {
        // A written move holds at its own frame only; an animated camera is elsewhere at others.
        _imp->lockedMoveAwaitsSync = false;
        _imp->renderer.clearLookThroughTransform();
    }
    if ((frame != _imp->frame) && (_imp->handle == SceneGizmo::kNoHandle)) {
        _imp->previewAwaitsSync = false;
        _imp->previewTimer->stop();
        _imp->clearPreview();
    }
    _imp->frame = frame;
    _imp->framesPerSecond = framesPerSecond;
    if (_imp->requestedLiveTextures) {
        // The same scene, through the worker, which renders the frame's textures first.
        requestSync(_imp->requestedPayload, _imp->requestedError);

        return;
    }
    _imp->renderer.setFrame(frame, framesPerSecond);
    update();
}

void
SceneViewport::setError(const QString& message)
{
    // Shown at once; the stage lets go of the scene's layers once the worker syncs it empty. The
    // engine stays.
    _imp->error = message;
    requestSync(ScenePayload(), message);
    update();
}

void
SceneViewport::clearScene()
{
    _imp->error.clear();
    requestSync(ScenePayload(), QString());
    update();
}

// Lets GUI tests and scripts read the state through the widget.
void
SceneViewport::publishState()
{
    setProperty("natronSceneError", _imp->error);
    setProperty("natronSceneShown", _imp->renderer.hasScene());
    setProperty("natronSceneSynced", isSceneSynced());
    setProperty("natronSceneComposing", isComposing());
    // The payload loads: whether requests wait, how many commits edited the stage, how long the last
    // batch held the viewer lock, and each failed prim's path and error.
    setProperty("natronSceneLoadsPending", _imp->renderer.hasPendingStageLoads());
    setProperty("natronSceneLoadEdits", QVariant::fromValue<qulonglong>(_imp->renderer.getStageLoadEditCount()));
    setProperty("natronSceneLoadHoldMs", _imp->renderer.getLastStageLoadHoldMs());
    publishDrawStats();
    {
        QVariantList failures;
        for (const std::pair<std::string, std::string>& failure : _imp->renderer.getStageLoadFailures()) {
            failures.push_back(QVariantList { QString::fromStdString(failure.first), QString::fromStdString(failure.second) });
        }
        setProperty("natronSceneLoadFailures", failures);
    }
    // How many scenes were handed to the worker: navigation alone must not add to it.
    setProperty("natronSceneSyncRequests", QVariant::fromValue<qulonglong>(_imp->requestedSerial));
    setProperty("natronSceneLookThrough", QString::fromStdString(_imp->renderer.getLookThroughCamera()));
    setProperty("natronSceneEngineCount", QVariant::fromValue<qulonglong>(_imp->renderer.getEngineCount()));
    setProperty("natronSceneEngineHeld", _imp->renderer.hasEngine());
    // The free camera's view (perspective, front, back, right, left, top or bottom), its clip
    // planes (automatic, near, far), whether the view is locked to the camera looked through, and
    // why the last move of a locked camera could not be made.
    setProperty("natronSceneViewMode", QString::fromUtf8(viewModeName(_imp->renderer.getViewMode())));
    const SceneViewportRenderer::ClipPlanes& clip = _imp->renderer.getClipPlanes();
    setProperty("natronSceneClip", QVariantList { clip.automatic, clip.nearClip, clip.farClip });
    setProperty("natronSceneLockView", _imp->lockView);
    setProperty("natronSceneLockMessage", _imp->lockMessage);
    setProperty("natronSceneRenderer", QString::fromStdString(_imp->renderer.getRendererId()));
    setProperty("natronSceneRendererError", QString::fromStdString(_imp->renderer.getRendererError()));
    // The display settings: the draw mode's and lighting mode's script names, and the proxy,
    // render and guide purposes shown.
    setProperty("natronSceneDrawMode", QString::fromUtf8(drawModeName(_imp->renderer.getDrawMode())));
    setProperty("natronSceneShowGrid", _imp->renderer.getShowGrid());
    setProperty("natronSceneShowGuides", _imp->renderer.getShowGuides());
    setProperty("natronSceneGuideSize", _imp->renderer.getGuideSize());
    setProperty("natronScenePurposes", QVariantList { _imp->renderer.getShowProxy(), _imp->renderer.getShowRender(), _imp->renderer.getShowGuidePurpose() });
    setProperty("natronSceneLighting", QString::fromUtf8(lightingModeName(_imp->renderer.getLightingMode())));
    // The transform widget's tool, frame and origin, by script name.
    setProperty("natronSceneTransformTool", QString::fromUtf8(transformToolName(_imp->renderer.getTransformTool())));
    setProperty("natronSceneTransformSpace", QString::fromUtf8((_imp->renderer.getTransformSpace() == SceneTransform::eSpaceLocal) ? "local" : "world"));
    setProperty("natronSceneTransformPivot", QString::fromUtf8((_imp->renderer.getTransformPivot() == SceneTransform::ePivotCurrentPrim) ? "currentPrim" : "selectionCentre"));
}

// Keeps what it last published if an edit holds the stage; the paint that edit asks for publishes.
void
SceneViewport::publishTransformState()
{
    const qreal dpr = devicePixelRatio();
    std::vector<int> handles;
    std::vector<double> pixels, frame;
    bool busy = false;
    _imp->renderer.getDrawnTransformHandles(int(width() * dpr), int(height() * dpr), &handles, &pixels, &frame, &busy);
    if (busy) {
        return;
    }
    QVariantList drawn;
    for (std::size_t i = 0; i < handles.size(); ++i) {
        drawn.push_back(QVariantList { handles[i], pixels[2 * i] / dpr, pixels[2 * i + 1] / dpr });
    }
    setProperty("natronSceneTransformHandles", drawn);
    QVariantList frameValues;
    for (double v : frame) {
        frameValues.push_back(v);
    }
    setProperty("natronSceneTransformFrame", frameValues);
}

const QString&
SceneViewport::getError() const
{
    return _imp->error;
}

SceneViewportRenderer&
SceneViewport::getRenderer()
{
    return _imp->renderer;
}

void
SceneViewport::setDrawMode(SceneViewportRenderer::DrawModeEnum mode)
{
    _imp->renderer.setDrawMode(mode);
    publishState();
    update();
}

void
SceneViewport::setShowGrid(bool show)
{
    _imp->renderer.setShowGrid(show);
    publishState();
    update();
}

void
SceneViewport::setShowGuides(bool show)
{
    _imp->renderer.setShowGuides(show);
    publishState();
    update();
}

void
SceneViewport::setShowPurposes(bool proxy,
                               bool render,
                               bool guide)
{
    _imp->renderer.setShowProxy(proxy);
    _imp->renderer.setShowRender(render);
    _imp->renderer.setShowGuidePurpose(guide);
    publishState();
    update();
}

void
SceneViewport::setLightingMode(SceneViewportRenderer::LightingModeEnum mode)
{
    SettingsPtr settings = appPTR ? appPTR->getCurrentSettings() : SettingsPtr();
    if (settings) {
        // Its settingChanged brings every other 3D view along.
        settings->setViewer3DLighting((Settings::Viewer3DLightingEnum)mode);
    }
    _imp->renderer.setLightingMode(mode);
    publishState();
    update();
}

void
SceneViewport::setRendererId(const QString& id)
{
    // Dropping the old engine needs its context.
    const std::string renderer = id.toStdString();
    runWithEngineContext([this, &renderer]() { _imp->renderer.setRendererId(renderer); });
    publishState();
    update();
}

void
SceneViewport::setDisplayTransform(const SceneViewportRenderer::DisplayTransform& transform)
{
    _imp->renderer.setDisplayTransform(transform);
    update();
}

void
SceneViewport::setLookThroughCamera(const QString& path)
{
    if (_imp->cameraLock.active) {
        endCameraLock();
    }
    _imp->lockedMoveAwaitsSync = false;
    _imp->renderer.setLookThroughCamera(path.toStdString());
    if (!path.isEmpty()) {
        {
            std::lock_guard<std::mutex> lock(_imp->queueMutex);
            _imp->pendingPivot = true;
        }
        _imp->queueChanged.notify_one();
    }
    endLockOutsideLookThrough();
    _imp->lockMessage.clear();
    publishState();
    update();
}

void
SceneViewport::requestFraming(const std::vector<std::string>& selection)
{
    {
        std::lock_guard<std::mutex> lock(_imp->queueMutex);
        _imp->pendingFramingId = ++_imp->framingRequested;
        _imp->pendingFramingSelection = selection;
    }
    _imp->queueChanged.notify_one();
    publishState();
}

void
SceneViewport::onFramingComputed(U64 id,
                                 const SceneViewportRenderer::FrameBounds& bounds)
{
    serviceDeferredInput();
    // A later request was taken with this one or follows it, and its answer is on its way.
    if (id != _imp->framingRequested) {
        return;
    }
    _imp->framingDone = id;
    _imp->renderer.applyFrameBounds(bounds);
    endLockOutsideLookThrough();
    publishState();
    update();
}

void
SceneViewport::frameScene()
{
    requestFraming(std::vector<std::string>());
}

void
SceneViewport::frameSelection()
{
    requestFraming(_imp->renderer.getSelectedPrims());
}

bool
SceneViewport::pickMustWait()
{
    // The engine is busy for as long as a background frame runs, and a stage change not yet taken
    // in would have the pick do that frame's work on the render thread, with the main thread
    // waiting for it. Asking for the frame here brings the retry that follows it.
    startBackgroundFrame();

    return _imp->backgroundFrameRunning || (_imp->usesRenderThread && _imp->renderer.needsBackgroundFrame());
}

QString
SceneViewport::pickPrimAt(const QPoint& pos,
                          bool* busy)
{
    std::string path;

    if (_imp->glInitialized && _imp->renderer.hasScene() && context()) {
        if (pickMustWait()) {
            if (busy) {
                *busy = true;
            }

            return QString();
        }
        const qreal dpr = devicePixelRatio();
        const int x = int(pos.x() * dpr);
        const int y = int(pos.y() * dpr);
        const int w = int(width() * dpr);
        const int h = int(height() * dpr);
        runWithEngineContext([&]() { path = _imp->renderer.pick(x, y, w, h, busy); });
    }

    return QString::fromStdString(path);
}

QStringList
SceneViewport::pickPrimsIn(const QRect& rect,
                           bool* busy)
{
    std::vector<std::string> paths;

    if (_imp->glInitialized && _imp->renderer.hasScene() && context()) {
        if (pickMustWait()) {
            if (busy) {
                *busy = true;
            }

            return QStringList();
        }
        const qreal dpr = devicePixelRatio();
        const QRect r = rect.normalized();
        const int left = int(r.left() * dpr);
        const int top = int(r.top() * dpr);
        const int right = int((r.right() + 1) * dpr) - 1;
        const int bottom = int((r.bottom() + 1) * dpr) - 1;
        const int w = int(width() * dpr);
        const int h = int(height() * dpr);
        runWithEngineContext([&]() { paths = _imp->renderer.pickRect(left, top, right, bottom, w, h, busy); });
    }

    return toStringList(paths);
}

void
SceneViewport::select(const QStringList& paths,
                      SelectionModeEnum mode)
{
    QStringList selection = getSelection();

    switch (mode) {
    case eSelectionModeReplace:
        selection = paths;
        break;
    case eSelectionModeAdd:
        // An added prim already selected becomes the current one.
        for (const QString& path : paths) {
            selection.removeAll(path);
            selection.push_back(path);
        }
        break;
    case eSelectionModeToggle:
        for (const QString& path : paths) {
            if (selection.contains(path)) {
                selection.removeAll(path);
            } else {
                selection.push_back(path);
            }
        }
        break;
    }
    setSelection(selection);
}

void
SceneViewport::setSelection(const QStringList& paths)
{
    if (paths == getSelection()) {
        return;
    }
    std::vector<std::string> p;
    for (const QString& path : paths) {
        p.push_back(path.toStdString());
    }
    _imp->renderer.setSelectedPrims(p);
    setProperty("natronSceneSelection", paths);
    update();
    Q_EMIT selectionChanged(paths);
}

QStringList
SceneViewport::getSelection() const
{
    return toStringList(_imp->renderer.getSelectedPrims());
}

void
SceneViewport::setTransformNodeProvider(const std::function<NodePtr()>& provider)
{
    _imp->transformNodeProvider = provider;
    update();
}

void
SceneViewport::setTransformTarget(const std::shared_ptr<SceneTransformTarget>& target)
{
    _imp->transformTarget = target;
}

void
SceneViewport::setTransformTool(int tool)
{
    if (_imp->handle != SceneGizmo::kNoHandle) {
        return;
    }
    _imp->renderer.setTransformTool(tool);
    _imp->renderer.setHighlightedTransformHandle(SceneGizmo::kNoHandle);
    publishState();
    update();
}

int
SceneViewport::getTransformTool() const
{
    return _imp->renderer.getTransformTool();
}

void
SceneViewport::cycleTransformTool()
{
    const int tool = _imp->renderer.getTransformTool();

    setTransformTool(((tool == SceneTransform::eToolSelect) || (tool == SceneTransform::eToolScale)) ? SceneTransform::eToolTranslate : tool + 1);
}

void
SceneViewport::setTransformSpace(int space)
{
    if (_imp->handle != SceneGizmo::kNoHandle) {
        return;
    }
    _imp->renderer.setTransformSpace(space);
    publishState();
    update();
}

void
SceneViewport::setTransformPivot(int pivot)
{
    if (_imp->handle != SceneGizmo::kNoHandle) {
        return;
    }
    _imp->renderer.setTransformPivot(pivot);
    publishState();
    update();
}

void
SceneViewport::setViewedNodeProvider(const std::function<NodePtr()>& provider)
{
    _imp->viewedNodeProvider = provider;
}

void
SceneViewport::setSelectionActions(const SelectionActions& actions)
{
    _imp->selectionActions = actions;
}

void
SceneViewport::setCameraNodeProvider(const std::function<NodePtr(const std::string&)>& provider)
{
    _imp->cameraNodeProvider = provider;
}

void
SceneViewport::setUndoHistory(UndoHistory* history)
{
    _imp->undoHistory = history;
}

void
SceneViewport::setViewMode(SceneViewportRenderer::ViewModeEnum mode)
{
    _imp->renderer.setViewMode(mode);
    endLockOutsideLookThrough();
    publishState();
    update();
}

void
SceneViewport::setClipPlanes(const SceneViewportRenderer::ClipPlanes& planes)
{
    _imp->renderer.setClipPlanes(planes);
    publishState();
    update();
}

void
SceneViewport::setLockView(bool locked)
{
    if (_imp->cameraLock.active) {
        endCameraLock();
    }
    _imp->lockView = locked;
    _imp->lockMessage.clear();
    publishState();
    update();
}

bool
SceneViewport::isLockView() const
{
    return _imp->lockView;
}

void
SceneViewport::lockViewToCamera()
{
    const std::string current = _imp->renderer.getLookThroughCamera();

    if (!current.empty()) {
        setLockView(true);

        return;
    }
    const std::vector<std::string> paths = _imp->renderer.getStageCameraPaths();
    if (paths.empty()) {
        _imp->lockView = false;
        _imp->lockMessage = tr("The scene has no camera to lock the view to");
        publishState();
        update();

        return;
    }
    if (paths.size() == 1) {
        lockViewToCamera(QString::fromStdString(paths.front()));

        return;
    }
    // Later: a key press or the context menu that asked may still be under way.
    QMetaObject::invokeMethod(this, [this, paths]() { showLockCameraMenu(paths); }, Qt::QueuedConnection);
}

void
SceneViewport::lockViewToCamera(const QString& path)
{
    setLookThroughCamera(path);
    setLockView(!path.isEmpty());
}

void
SceneViewport::showLockCameraMenu(const std::vector<std::string>& paths)
{
    clearGesture();
    Menu menu(this);
    menu.setObjectName(QString::fromUtf8("SceneLockCameraMenu"));
    for (const std::string& path : paths) {
        const QString qpath = QString::fromStdString(path);
        QAction* action = menu.addAction(qpath);
        QObject::connect(action, &QAction::triggered, this, [this, qpath]() {
            lockViewToCamera(qpath);
        });
    }
    QPoint pos = mapFromGlobal(QCursor::pos());
    if (!rect().contains(pos)) {
        pos = rect().center();
    }
    menu.exec(mapToGlobal(pos));
    clearGesture();
}

bool
SceneViewport::navigatesLockedCamera() const
{
    return _imp->lockView && !_imp->renderer.getLookThroughCamera().empty();
}

void
SceneViewport::endLockOutsideLookThrough()
{
    if (!_imp->renderer.getLookThroughCamera().empty()) {
        return;
    }
    if (_imp->cameraLock.active) {
        endCameraLock();
    }
    if (_imp->lockView) {
        _imp->lockView = false;
        _imp->lockMessage.clear();
        publishState();
    }
}

bool
SceneViewport::beginCameraLock()
{
    Implementation::CameraLock& lock = _imp->cameraLock;

    lock = Implementation::CameraLock();
    const std::string path = _imp->renderer.getLookThroughCamera();
    NodePtr node = _imp->cameraNodeProvider ? _imp->cameraNodeProvider(path) : NodePtr();
    KnobDoublePtr translate, rotate, scale, uniformScale;
    KnobChoicePtr rotateOrder;
    if (node) {
        translate = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kSceneParamTranslate));
        rotate = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kSceneParamRotate));
        scale = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kSceneParamScale));
        uniformScale = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kSceneParamUniformScale));
        rotateOrder = std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(kSceneParamRotateOrder));
    }
    const qreal dpr = devicePixelRatio();
    SceneViewportRenderer::CameraPose pose;
    bool poseBusy = false;
    const bool havePose = _imp->renderer.getViewPose(int(width() * dpr), int(height() * dpr), &pose, &poseBusy);
    // Not refused: the next move of the drag or the wheel tries again.
    if (poseBusy) {
        return false;
    }
    const double frame = _imp->frame;
    GfVec3d s(1.);
    if (scale && uniformScale) {
        const double uniform = uniformScale->getValueAtTime(frame, 0);
        s = GfVec3d(scale->getValueAtTime(frame, 0) * uniform, scale->getValueAtTime(frame, 1) * uniform, scale->getValueAtTime(frame, 2) * uniform);
    }
    const bool scaled = (std::abs(s[0]) > 1e-12) && (std::abs(s[1]) > 1e-12) && (std::abs(s[2]) > 1e-12);
    if (!translate || !rotate || !rotateOrder || !havePose || !scaled) {
        // The view stays locked to the camera and does not move for a camera read from a
        // file.
        lock.refused = true;
        _imp->lockMessage = tr("%1 has no Camera node to move: the view is locked to it").arg(QString::fromStdString(path));
        publishState();
        update();

        return false;
    }
    lock.active = true;
    lock.node = node;
    lock.translate = translate;
    lock.rotate = rotate;
    lock.frame = frame;
    for (int i = 0; i < 6; ++i) {
        const KnobDoublePtr& knob = (i < 3) ? translate : rotate;
        const int dimension = i % 3;
        lock.oldValues[i] = knob->getValueAtTime(frame, dimension);
        lock.newValues[i] = lock.oldValues[i];
        lock.animated[i] = knob->isAnimated(dimension);
        lock.hadKey[i] = lock.animated[i] && (knob->getKeyFrameIndex(ViewSpec::current(), dimension, frame) >= 0);
    }
    lock.scale = s;
    lock.rotateOrder = (SceneRotateOrderEnum)rotateOrder->getValue();
    const GfMatrix4d local = SceneNodeAuthoring::composeTransformMatrix(GfVec3d(lock.oldValues[0], lock.oldValues[1], lock.oldValues[2]),
                                                                        GfVec3d(lock.oldValues[3], lock.oldValues[4], lock.oldValues[5]),
                                                                        s, GfVec3d(0.), lock.rotateOrder);
    lock.world = loadMatrix(pose.cameraToWorld);
    lock.parentInverse = loadMatrix(pose.parentToWorld).GetInverse();
    // world = inner * local * parentToWorld: what the ops after the node's add.
    const GfMatrix4d inner = lock.world * lock.parentInverse * local.GetInverse();
    lock.innerInverse = inner.GetInverse();
    lock.pivotDepth = pose.pivotDepth;
    lock.viewHeight = pose.viewHeight;
    lock.orthographic = pose.orthographic;
    _imp->lockMessage.clear();
    publishState();

    return true;
} // SceneViewport::beginCameraLock

void
SceneViewport::moveLockedCamera(int navigation,
                                const QPoint& delta,
                                double dollyFactor)
{
    Implementation::CameraLock& lock = _imp->cameraLock;
    KnobDoublePtr translate = lock.translate.lock();
    KnobDoublePtr rotate = lock.rotate.lock();
    NodePtr node = lock.node.lock();

    if (!lock.active || !translate || !rotate || !node) {
        return;
    }
    const GfMatrix4d& world = lock.world;
    const GfVec3d right = world.TransformDir(GfVec3d(1., 0., 0.)).GetNormalized();
    const GfVec3d up = world.TransformDir(GfVec3d(0., 1., 0.)).GetNormalized();
    const GfVec3d back = world.TransformDir(GfVec3d(0., 0., 1.)).GetNormalized();
    const GfVec3d eye = world.ExtractTranslation();
    const double depth = lock.pivotDepth;
    GfMatrix4d move(1.);

    // The free camera's gestures, turned into a move of the camera in the world.
    switch (navigation) {
    case Implementation::eDragRoleTumble: {
        const GfVec3d worldUp = _imp->renderer.isUpAxisZ() ? GfVec3d(0., 0., 1.) : GfVec3d(0., 1., 0.);
        const double azimuth = -delta.x() * 0.4;
        // The eye's elevation above the pivot stays within the free camera's.
        const double elevation = radiansToDegrees(std::asin(std::max(-1., std::min(1., GfDot(back, worldUp)))));
        const double target = std::max(kMinElevation, std::min(kMaxElevation, elevation + delta.y() * 0.4));
        const GfVec3d centre = eye - back * depth;
        GfMatrix4d turn;
        turn.SetRotate(GfRotation(worldUp, azimuth));
        const GfVec3d turnedRight = turn.TransformDir(right).GetNormalized();
        GfMatrix4d tilt;
        tilt.SetRotate(GfRotation(turnedRight, -(target - elevation)));
        move = GfMatrix4d().SetTranslate(-centre) * turn * tilt * GfMatrix4d().SetTranslate(centre);
        break;
    }
    case Implementation::eDragRolePan: {
        const double unitsPerPixel = (lock.orthographic ? lock.viewHeight : lock.viewHeight * depth) / std::max(height(), 1);
        move.SetTranslate(-right * (delta.x() * unitsPerPixel) + up * (delta.y() * unitsPerPixel));
        break;
    }
    case Implementation::eDragRoleDolly:
        if (dollyFactor > 0.) {
            // An orthographic camera only moves: its aperture, not its distance, sets its zoom.
            move.SetTranslate(back * (depth * (dollyFactor - 1.)));
            lock.pivotDepth = std::max(depth * dollyFactor, 1e-9);
        }
        break;
    default:
        return;
    }
    lock.world = world * move;

    const GfMatrix4d local = lock.innerInverse * lock.world * lock.parentInverse;
    GfMatrix4d rotation(1.);
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            rotation[i][j] = local[i][j] / lock.scale[i];
        }
    }
    const GfVec3d t = local.ExtractTranslation();
    const GfVec3d r = SceneNodeAuthoring::decomposeRotationMatrix(rotation, lock.rotateOrder, GfVec3d(lock.newValues[3], lock.newValues[4], lock.newValues[5]));
    for (int i = 0; i < 6; ++i) {
        lock.newValues[i] = (i < 3) ? t[i] : r[i - 3];
    }
    double cameraToWorld[16];
    std::copy(lock.world.data(), lock.world.data() + 16, cameraToWorld);
    _imp->renderer.setLookThroughTransform(cameraToWorld);
} // SceneViewport::moveLockedCamera

void
SceneViewport::endCameraLock()
{
    Implementation::CameraLock lock = _imp->cameraLock;

    _imp->cameraLock = Implementation::CameraLock();
    _imp->wheelMoveTimer->stop();
    NodePtr node = lock.node.lock();
    if (!lock.active) {
        return;
    }
    if (!node) {
        if (!_imp->lockedMoveAwaitsSync) {
            _imp->renderer.clearLookThroughTransform();
        }

        return;
    }
    std::vector<LockedCameraMoveCommand::Dimension> dimensions;
    for (int i = 0; i < 6; ++i) {
        if (lock.newValues[i] == lock.oldValues[i]) {
            continue;
        }
        LockedCameraMoveCommand::Dimension d;
        d.knob = (i < 3) ? lock.translate : lock.rotate;
        d.dimension = i % 3;
        d.oldValue = lock.oldValues[i];
        d.newValue = lock.newValues[i];
        d.animated = lock.animated[i];
        d.hadKey = lock.hadKey[i];
        dimensions.push_back(d);
    }
    if (dimensions.empty()) {
        // Unless an earlier move still waits for its scene, the prim is where the view shows it.
        if (!_imp->lockedMoveAwaitsSync) {
            _imp->renderer.clearLookThroughTransform();
        }

        return;
    }
    // Before the write, which may request the scene that has it before returning.
    _imp->lockedMoveSerial = _imp->requestedSerial;
    _imp->lockedMoveAwaitsSync = true;
    EffectInstancePtr effect = node->getEffectInstance();
    if (effect) {
        effect->beginChanges();
    }
    for (const LockedCameraMoveCommand::Dimension& d : dimensions) {
        if (KnobDoublePtr knob = d.knob.lock()) {
            setCameraKnobValue(knob, d.dimension, lock.frame, d.animated, d.newValue);
        }
    }
    if (effect) {
        effect->endChanges();
    }
    LockedCameraMoveCommand* command = new LockedCameraMoveCommand(node, lock.frame, dimensions, tr("Move %1").arg(QString::fromUtf8(node->getLabel().c_str())));
    if (_imp->undoHistory) {
        _imp->undoHistory->push(command, UndoScope(UndoScope::holderForNode(node)));
    } else {
        // The edits are made: without a history there is nothing to record them in.
        delete command;
    }
} // SceneViewport::endCameraLock

NodePtr
SceneViewport::createCameraFromView()
{
    NodePtr viewed = _imp->viewedNodeProvider ? _imp->viewedNodeProvider() : NodePtr();
    AppInstancePtr app = viewed ? viewed->getApp() : AppInstancePtr();
    const qreal dpr = devicePixelRatio();
    SceneViewportRenderer::CameraPose pose;
    bool poseBusy = false;

    if (!app || !_imp->renderer.getViewPose(int(width() * dpr), int(height() * dpr), &pose, &poseBusy)) {
        _imp->cameraFromViewOwed = poseBusy;

        return NodePtr();
    }
    const GfMatrix4d cameraToWorld = loadMatrix(pose.cameraToWorld);
    // A stage camera may be scaled: the new one keeps its orientation alone.
    GfMatrix4d rotation(1.);
    for (int i = 0; i < 3; ++i) {
        const GfVec3d axis = GfVec3d(cameraToWorld[i][0], cameraToWorld[i][1], cameraToWorld[i][2]).GetNormalized();
        for (int j = 0; j < 3; ++j) {
            rotation[i][j] = axis[j];
        }
    }
    GfVec3d eye = cameraToWorld.ExtractTranslation();
    double nearClip = pose.nearClip;
    double farClip = pose.farClip;
    if (pose.orthographic && _imp->renderer.getLookThroughCamera().empty()) {
        // Backed off as the orthographic view is, so that it sees what lies behind its eye too.
        const double backOff = pose.pivotDepth * 500.;
        eye += GfVec3d(rotation[2][0], rotation[2][1], rotation[2][2]) * backOff;
        nearClip = _imp->renderer.getClipPlanes().automatic ? backOff * 1e-3 : backOff + nearClip;
        farClip += backOff;
    }
    const GfVec3d rotate = SceneNodeAuthoring::decomposeRotationMatrix(rotation, eSceneRotateOrderXYZ, GfVec3d(0.));

    CreateNodeArgs args(PLUGINID_NATRON_USDCAMERA, viewed->getGroup());
    double x = 0., y = 0.;
    viewed->getPosition(&x, &y);
    args.setProperty<double>(kCreateNodeArgsPropNodeInitialPosition, x + 150., 0);
    args.setProperty<double>(kCreateNodeArgsPropNodeInitialPosition, y, 1);
    args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
    args.addParamDefaultValueN<double>(kSceneParamTranslate, std::vector<double> { eye[0], eye[1], eye[2] });
    args.addParamDefaultValueN<double>(kSceneParamRotate, std::vector<double> { rotate[0], rotate[1], rotate[2] });
    args.addParamDefaultValue<int>(kSceneParamRotateOrder, (int)eSceneRotateOrderXYZ);
    // The knobs are in millimetres; a film back 24 mm high, with the lens that gives the view's
    // field of view, or for an orthographic camera an aperture of the view's height: USD reads
    // apertures in tenths of a scene unit, which the camera node makes 100 x metersPerUnit mm.
    const double verticalAperture = pose.orthographic ? pose.viewHeight * 1000. * app->getProject()->getProjectMetersPerUnit() : 24.;
    args.addParamDefaultValue<int>(kUSDCameraParamProjection, pose.orthographic ? 1 : 0);
    args.addParamDefaultValue<double>(kUSDCameraParamVerticalAperture, verticalAperture);
    args.addParamDefaultValue<double>(kUSDCameraParamHorizontalAperture, verticalAperture * pose.aspect);
    if (!pose.orthographic) {
        args.addParamDefaultValue<double>(kUSDCameraParamFocalLength, verticalAperture / std::max(pose.viewHeight, 1e-9));
    }
    args.addParamDefaultValue<double>(kUSDCameraParamNear, nearClip);
    args.addParamDefaultValue<double>(kUSDCameraParamFar, farClip);

    return app->createNode(args);
} // SceneViewport::createCameraFromView

void
SceneViewport::refreshTransformHandles()
{
    NodePtr node;
    USDEditTransform* effect = _imp->getTransformNode(&node);
    std::string primPath, opName;

    if (!effect || !effect->getHandleTarget(&primPath, &opName)) {
        _imp->renderer.clearTransformHandles();

        return;
    }
    const SceneTransformKnobs& knobs = effect->getTransformKnobs();
    KnobDoublePtr translate = knobs.translate.lock();
    KnobDoublePtr rotate = knobs.rotate.lock();
    KnobDoublePtr scale = knobs.scale.lock();
    KnobDoublePtr uniformScale = knobs.uniformScale.lock();
    KnobChoicePtr rotateOrder = knobs.rotateOrder.lock();
    KnobDoublePtr pivot = effect->getPivotKnob();
    if (!translate || !rotate || !scale || !uniformScale || !rotateOrder || !pivot) {
        _imp->renderer.clearTransformHandles();

        return;
    }
    SceneViewportRenderer::TransformHandles handles;
    handles.primPath = primPath;
    handles.opName = opName;
    for (int d = 0; d < 3; ++d) {
        handles.translate[d] = translate->getValueAtTime(_imp->frame, d);
        handles.rotate[d] = rotate->getValueAtTime(_imp->frame, d);
        handles.scale[d] = scale->getValueAtTime(_imp->frame, d);
        handles.pivot[d] = pivot->getValueAtTime(_imp->frame, d);
    }
    handles.uniformScale = uniformScale->getValueAtTime(_imp->frame, 0);
    handles.rotateOrder = rotateOrder->getValue();
    std::vector<std::string> paths, opNames;
    effect->getMatchedPrims(&paths, &opNames);
    for (std::size_t i = 0; i < paths.size(); ++i) {
        SceneViewportRenderer::TransformHandles::Target target;
        target.primPath = paths[i];
        target.opName = opNames[i];
        handles.otherTargets.push_back(target);
    }
    _imp->renderer.setTransformHandles(handles);
} // SceneViewport::refreshTransformHandles

void
SceneViewport::releaseEngine()
{
    // The engine's GL objects belong to its context, and releaseGL() takes the viewer lock, so the
    // worker is not syncing into the stage the engine listens to while it goes.
    if (_imp->glInitialized && context()) {
        runWithEngineContext([this]() { _imp->renderer.releaseGL(); });
    }
    if (_imp->requestedLiveTextures) {
        // Free GPU Memory emptied the store too, and only the worker's sync renders textures.
        requestSync(_imp->requestedPayload, _imp->requestedError);
        _imp->paintHeldUntilSerial = _imp->requestedSerial;
    } else {
        publishState();
    }
    // A hidden view makes its engine when it is next shown.
    update();
}

void
SceneViewport::initializeGL()
{
    _imp->glInitialized = true;
    _imp->startRenderContext(context());
    // A new context (the pane was floated or docked) cannot use the old one's engine, and the engine
    // context shared with the old one has to go with it.
    GLContextLoss::watch(this, [this]() {
        _imp->releaseGLResources();
    });
}

void
SceneViewport::resizeGL(int /*w*/,
                        int /*h*/)
{
    // Qt gave the widget a new framebuffer, with nothing of the old image in it.
    _imp->presentedScene = false;
}

void
SceneViewport::paintGL()
{
    if (_imp->syncedSerial < _imp->paintHeldUntilSerial) {
        // onSynced asks for the repaint.
        return;
    }
    const qreal dpr = devicePixelRatio();
    const int w = int(width() * dpr);
    const int h = int(height() * dpr);
    if (_imp->usesRenderThread && (w > 0) && (h > 0)) {
        startBackgroundFrame();
        if (_imp->backgroundFrameRunning || _imp->renderer.needsBackgroundFrame()) {
            if (!_imp->backgroundFrameRunning) {
                // A sync or a load holds the viewer lock and asks for no paint if it edits nothing.
                update();
            }
            if (!_imp->presentedScene || (_imp->composingClock.isValid() && (_imp->composingClock.elapsed() >= kComposingStatusDelayMs))) {
                paintComposingStatus();
            }

            return;
        }
    }
    refreshTransformHandles();
    {
        // The free camera this paint draws with, for scripts: pivot x, y, z, distance, azimuth,
        // elevation and vertical field of view.
        const SceneViewportRenderer::FreeCamera& camera = _imp->renderer.getFreeCamera();
        setProperty("natronSceneFreeCamera", QVariantList { camera.pivot[0], camera.pivot[1], camera.pivot[2], camera.distance, camera.azimuth, camera.elevation, camera.fovY });
        // A tumble turns an orthographic view to perspective.
        setProperty("natronSceneViewMode", QString::fromUtf8(viewModeName(camera.viewMode)));
        // Free navigation stops looking through a camera.
        setProperty("natronSceneLookThrough", QString::fromStdString(_imp->renderer.getLookThroughCamera()));
    }
    _imp->renderer.setRecordDrawnGuides(property("natronSceneRecordGuides").toBool());
    SceneViewportRenderer::RenderStatusEnum status = SceneViewportRenderer::eRenderStatusBusy;
    if (_imp->usesRenderThread) {
        GLuint target = 0;
        GLsync fence = nullptr;
        Implementation* imp = _imp.get();
        imp->callOnRenderThread([imp, w, h, &status, &target, &fence]() {
            target = imp->ensureRenderTarget(w, h);
            status = imp->renderer.renderIfIdle(target, w, h);
            if (status != SceneViewportRenderer::eRenderStatusBusy) {
                fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
                // A fence another context waits on has to reach the GPU first.
                glFlush();
            }
        });
        if (status != SceneViewportRenderer::eRenderStatusBusy) {
            imp->presentRenderTarget(defaultFramebufferObject(), w, h, fence);
        }
    } else {
        status = _imp->renderer.renderIfIdle(defaultFramebufferObject(), w, h);
    }
    if (status == SceneViewportRenderer::eRenderStatusBusy) {
        // The worker is syncing; it asks for a repaint when it is done.
        if (!_imp->presentedScene && isComposing()) {
            paintComposingStatus();
        }

        return;
    }
    _imp->presentedScene = _imp->renderer.hasScene();
    if (_imp->renderer.isDrawRevealing()) {
        _imp->renderer.noteDrawRevealPresented();
        update();
    }
    const bool converged = (status == SceneViewportRenderer::eRenderStatusDone);
    publishTransformState();
    publishDrawStats();
    // A paint gets here only once the frame has taken in every delta posted so far, unlike the stats,
    // which also change on the way to the screen.
    setProperty("natronSceneDrawPresented", QVariant::fromValue<qulonglong>(_imp->renderer.getDrawFilterStats().absorbed));
    // The engine is made at the first render in a context.
    setProperty("natronSceneEngineCount", QVariant::fromValue<qulonglong>(_imp->renderer.getEngineCount()));
    setProperty("natronSceneEngineHeld", _imp->renderer.hasEngine());
    const QString rendererError = QString::fromStdString(_imp->renderer.getRendererError());
    setProperty("natronSceneRendererError", rendererError);
    // Whether the stage's own lights lit this paint, and the guides it drew: path, kind and a
    // point on each in widget pixels.
    setProperty("natronSceneSceneLit", _imp->renderer.isSceneLit());
    {
        QVariantList guides;
        for (const SceneViewportRenderer::DrawnGuide& guide : _imp->renderer.getDrawnGuides()) {
            guides.push_back(QVariantList { QString::fromStdString(guide.path), QString::fromStdString(guide.kind), guide.x / dpr, guide.y / dpr });
        }
        setProperty("natronSceneGuides", guides);
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::TextAntialiasing);
    if (!_imp->error.isEmpty() || !rendererError.isEmpty()) {
        painter.setPen(QColor(230, 90, 80));
        painter.drawText(rect().adjusted(12, 12, -12, -12), Qt::AlignCenter | Qt::TextWordWrap, _imp->error.isEmpty() ? rendererError : _imp->error);
    } else if (isComposing()) {
        painter.end();
        paintComposingStatus();
        painter.begin(this);
    } else if (_imp->renderer.hasScene()) {
        QString mode;
        for (const DrawModeEntry& entry : kDrawModes) {
            if (entry.mode == _imp->renderer.getDrawMode()) {
                mode = shortcutText(entry.label);
            }
        }
        const std::string& camera = _imp->renderer.getLookThroughCamera();
        QString view = tr("Free camera");
        if (!camera.empty()) {
            view = QString::fromStdString(camera);
        } else if (_imp->renderer.getViewMode() != SceneViewportRenderer::eViewModePerspective) {
            for (const ViewModeEntry& entry : kViewModes) {
                if (entry.mode == _imp->renderer.getViewMode()) {
                    view = shortcutText(entry.label);
                }
            }
        }
        if (navigatesLockedCamera()) {
            view += tr(" (locked)");
        }
        QString hud = tr("%1 | %2 | time %3")
                          .arg(mode)
                          .arg(view)
                          .arg(_imp->renderer.getTimeCode(), 0, 'g', 6);
        const std::vector<std::string>& selected = _imp->renderer.getSelectedPrims();
        if (selected.size() == 1) {
            hud += tr(" | %1").arg(QString::fromStdString(selected.back()));
        } else if (selected.size() > 1) {
            hud += tr(" | %1 (+%2)").arg(QString::fromStdString(selected.back())).arg(selected.size() - 1);
        }
        painter.setPen(QColor(200, 200, 200));
        painter.drawText(rect().adjusted(8, 6, -8, -6), Qt::AlignLeft | Qt::AlignTop, hud);
        if (!_imp->lockMessage.isEmpty()) {
            painter.setPen(QColor(230, 160, 80));
            painter.drawText(rect().adjusted(8, 6, -8, -6), Qt::AlignLeft | Qt::AlignBottom, _imp->lockMessage);
        }
    }
    if (_imp->dragging && (_imp->dragRole == Implementation::eDragRoleMarquee)) {
        painter.setPen(QPen(QColor(230, 230, 230), 1., Qt::DashLine));
        painter.setBrush(QColor(255, 255, 255, 30));
        painter.drawRect(QRect(_imp->pressPos, _imp->lastPos).normalized());
    }
    painter.end();
    if (converged) {
        // When the last finished frame was drawn, for benchmarks and scripts.
        setProperty("natronSceneLastPaintMs", QVariant::fromValue<qlonglong>(QDateTime::currentMSecsSinceEpoch()));
    } else {
        update();
    }
} // paintGL

void
SceneViewport::mousePressEvent(QMouseEvent* e)
{
    // A handle drag is still to be written, and this press would start over it.
    if (_imp->handleEndOwed) {
        e->accept();

        return;
    }
    // Clicks still waiting go first, whichever way this one turns out.
    serviceDeferredInput();
    const bool queued = !_imp->deferredSelections.empty();
    bool handleOwed = false;
    if ((e->button() == Qt::LeftButton) && (e->modifiers() == Qt::NoModifier) && !_imp->spaceHeld) {
        if (queued) {
            handleOwed = true;
        } else if (beginHandleDrag(e->position().toPoint(), &handleOwed)) {
            e->accept();

            return;
        }
    }
    clearGesture();
    const Qt::KeyboardModifiers modifiers = mouseBindingModifiers(e->modifiers(), _imp->spaceHeld);
    const Qt::MouseButton button = e->button();
    Implementation::DragRoleEnum role = Implementation::eDragRoleNone;
    if (matchesMouseBinding(kShortcutIDMouseViewer3DTumble, modifiers, button)) {
        role = Implementation::eDragRoleTumble;
    } else if (matchesMouseBinding(kShortcutIDMouseViewer3DPan, modifiers, button)) {
        role = Implementation::eDragRolePan;
    } else if (matchesMouseBinding(kShortcutIDMouseViewer3DDolly, modifiers, button)) {
        role = Implementation::eDragRoleDolly;
    } else if (matchesMouseBinding(kShortcutIDMouseViewer3DMarquee, modifiers, button)) {
        role = Implementation::eDragRoleMarquee;
    }
    bool clickSelects = true;
    SelectionModeEnum mode = eSelectionModeReplace;
    if (matchesMouseBinding(kShortcutIDMouseViewer3DSelect, modifiers, button)) {
        mode = eSelectionModeReplace;
    } else if (matchesMouseBinding(kShortcutIDMouseViewer3DAddToSelection, modifiers, button)) {
        mode = eSelectionModeAdd;
    } else if (matchesMouseBinding(kShortcutIDMouseViewer3DToggleSelection, modifiers, button)) {
        mode = eSelectionModeToggle;
    } else {
        clickSelects = false;
    }
    // Adding or toggling by drag is a marquee of that mode, unless the drag is bound to another role.
    if (clickSelects && (mode != eSelectionModeReplace) && (role == Implementation::eDragRoleNone)) {
        role = Implementation::eDragRoleMarquee;
    }
    if ((role == Implementation::eDragRoleNone) && !clickSelects && !handleOwed) {
        // A right press with no binding opens the context menu.
        QOpenGLWidget::mousePressEvent(e);

        return;
    }
    const QPoint pos = e->position().toPoint();
    // A marquee may start on a prim: a drag selects by rectangle wherever it starts,
    // and a release without a drag selects what is under the press.
    bool pickOwed = false;
    if (clickSelects) {
        if (queued) {
            pickOwed = true;
        } else {
            _imp->pressPrim = pickPrimAt(pos, &pickOwed);
        }
    }
    _imp->pressOwedHandle = handleOwed;
    _imp->pressOwedPick = pickOwed;
    _imp->pressView = _imp->currentView(this);
    _imp->button = button;
    _imp->pressPos = pos;
    _imp->lastPos = pos;
    _imp->dragging = false;
    _imp->dragRole = role;
    _imp->clickSelects = clickSelects;
    _imp->selectionMode = mode;
    // A plain right press bound to a gesture defers the context menu to a release without a drag.
    _imp->menuOnRelease = (button == Qt::RightButton) && (modifiers == Qt::NoModifier);
    e->accept();
}

void
SceneViewport::mouseMoveEvent(QMouseEvent* e)
{
    const QPoint pos = e->position().toPoint();

    if (_imp->handle != SceneGizmo::kNoHandle) {
        if (!_imp->handleEndOwed) {
            dragHandle(pos);
        }
        e->accept();

        return;
    }
    // A release that went to a popup or another window never reaches this widget.
    if ((_imp->button != Qt::NoButton) && !(e->buttons() & _imp->button)) {
        clearGesture();
    }
    if (_imp->button == Qt::NoButton) {
        highlightHandleAt(pos);
        QOpenGLWidget::mouseMoveEvent(e);

        return;
    }
    if (!_imp->dragging && ((pos - _imp->pressPos).manhattanLength() < 4)) {
        return;
    }
    _imp->dragging = true;
    const QPoint delta = pos - _imp->lastPos;
    _imp->lastPos = pos;

    const bool navigates = (_imp->dragRole == Implementation::eDragRoleTumble) || (_imp->dragRole == Implementation::eDragRolePan) || (_imp->dragRole == Implementation::eDragRoleDolly);
    if (navigates && navigatesLockedCamera()) {
        if (!_imp->cameraLock.active && !_imp->cameraLock.refused) {
            beginCameraLock();
        }
        if (_imp->cameraLock.active) {
            moveLockedCamera(_imp->dragRole, delta, std::exp(-(delta.x() + delta.y()) * 0.01));
        }
        update();
        e->accept();

        return;
    }

    switch (_imp->dragRole) {
    case Implementation::eDragRoleTumble:
        _imp->renderer.tumble(-delta.x() * 0.4, delta.y() * 0.4);
        break;
    case Implementation::eDragRolePan:
        _imp->renderer.pan(delta.x(), delta.y(), height());
        break;
    case Implementation::eDragRoleDolly:
        _imp->renderer.dolly(std::exp(-(delta.x() + delta.y()) * 0.01));
        break;
    case Implementation::eDragRoleMarquee:
    case Implementation::eDragRoleNone:
        break;
    }
    if (navigates) {
        endLockOutsideLookThrough();
    }
    update();
    e->accept();
}

void
SceneViewport::mouseReleaseEvent(QMouseEvent* e)
{
    if (_imp->handle != SceneGizmo::kNoHandle) {
        endHandleDrag();
        e->accept();

        return;
    }
    if ((_imp->button == Qt::NoButton) || (e->button() != _imp->button)) {
        e->accept();

        return;
    }
    const bool dragged = _imp->dragging;
    const Implementation::DragRoleEnum role = _imp->dragRole;
    const bool clickSelects = _imp->clickSelects;
    const SelectionModeEnum mode = _imp->selectionMode;
    const QString pressPrim = _imp->pressPrim;
    const bool menu = _imp->menuOnRelease;
    const QRect marquee = QRect(_imp->pressPos, e->position().toPoint()).normalized();
    Implementation::DeferredSelection deferred;
    deferred.pos = _imp->pressPos;
    deferred.rect = marquee;
    deferred.checkHandle = _imp->pressOwedHandle;
    deferred.mode = mode;
    deferred.view = _imp->pressView;
    const bool owed = _imp->pressOwedPick || _imp->pressOwedHandle;
    clearGesture();

    if (dragged && (role == Implementation::eDragRoleMarquee)) {
        bool busy = !_imp->deferredSelections.empty();
        QStringList paths;
        if (!busy) {
            paths = pickPrimsIn(marquee, &busy);
        }
        if (busy) {
            deferred.marquee = true;
            deferred.checkHandle = false;
            _imp->deferredSelections.push_back(deferred);
            serviceDeferredInput();
        } else {
            select(paths, mode);
        }
    } else if ((!dragged || (role == Implementation::eDragRoleNone)) && clickSelects && owed) {
        deferred.emptySelects = !dragged;
        _imp->deferredSelections.push_back(deferred);
        serviceDeferredInput();
    } else if (!dragged && clickSelects) {
        select(pressPrim.isEmpty() ? QStringList() : QStringList(pressPrim), mode);
    } else if (dragged && clickSelects && (role == Implementation::eDragRoleNone) && !pressPrim.isEmpty()) {
        // A drag that no binding gives a role is a click on the prim it started from.
        select(QStringList(pressPrim), mode);
    } else if (!dragged && menu) {
        showContextMenu(e->globalPosition().toPoint());
    }
    e->accept();
}

void
SceneViewport::clearGesture()
{
    const bool marquee = _imp->dragging && (_imp->dragRole == Implementation::eDragRoleMarquee);

    // The end of a drag, wherever its release went.
    endCameraLock();
    _imp->button = Qt::NoButton;
    _imp->dragging = false;
    _imp->dragRole = Implementation::eDragRoleNone;
    _imp->clickSelects = false;
    _imp->pressPrim.clear();
    _imp->menuOnRelease = false;
    _imp->pressOwedHandle = false;
    _imp->pressOwedPick = false;
    if (marquee) {
        update();
    }
}

bool
SceneViewport::beginHandleDrag(const QPoint& pos,
                               bool* busy)
{
    refreshTransformHandles();
    const qreal dpr = devicePixelRatio();
    const int w = int(width() * dpr);
    const int h = int(height() * dpr);
    const GfVec2d pixel = Implementation::toPixel(this, pos);
    const int handle = _imp->renderer.hitTestTransformHandles(pixel[0], pixel[1], w, h, busy);
    if (handle == SceneGizmo::kNoHandle) {
        return false;
    }
    bool selectionBusy = false;
    const bool selection = _imp->renderer.hasSelectionHandles(&selectionBusy);
    if (selectionBusy) {
        if (busy) {
            *busy = true;
        }

        return false;
    }
    NodePtr node;
    if (!selection && !_imp->getTransformNode(&node)) {
        return false;
    }
    if (!_imp->renderer.beginTransformDrag(handle, pixel[0], pixel[1], w, h, busy)) {
        return false;
    }
    _imp->previewClearOwed = false;
    _imp->handleMoveOwed = false;
    _imp->handleChangeStale = false;
    _imp->handleEndOwed = false;
    _imp->previewTimer->stop();
    _imp->previewAwaitsSync = false;
    _imp->handle = handle;
    _imp->handleNode = node;
    _imp->handleChange = 0.;
    _imp->handleChanged = false;
    update();

    return true;
} // SceneViewport::beginHandleDrag

void
SceneViewport::dragHandle(const QPoint& pos)
{
    const qreal dpr = devicePixelRatio();
    const GfVec2d pixel = Implementation::toPixel(this, pos);
    double change = 0.;
    bool busy = false;

    _imp->handleMoveOwed = false;
    _imp->handleChangeStale = false;
    if ((_imp->handle == SceneGizmo::kNoHandle) || !_imp->renderer.getTransformDragChange(pixel[0], pixel[1], int(width() * dpr), int(height() * dpr), &change, &busy)) {
        if (busy) {
            _imp->handleMoveOwed = true;
            _imp->handleChangeStale = true;
            _imp->handleMovePos = pos;
        }

        return;
    }
    // Only drawn: writing the nodes on every move would evaluate the graph and sync the scene
    // each time, and the prims would only move as fast as that round trip.
    _imp->handleChange = change;
    _imp->handleChanged = true;
    _imp->renderer.previewTransformDrag(change, &busy);
    if (busy) {
        _imp->handleMoveOwed = true;
        _imp->handleMovePos = pos;
    }
    update();
}

void
SceneViewport::endHandleDrag()
{
    if (_imp->handle == SceneGizmo::kNoHandle) {
        return;
    }
    if (_imp->handleChangeStale) {
        dragHandle(_imp->handleMovePos);
        // The drag can't be written until its last move is measured; it ends when that can be.
        if (_imp->handleChangeStale) {
            _imp->handleEndOwed = true;

            return;
        }
    }
    _imp->handleMoveOwed = false;
    _imp->handleEndOwed = false;
    const double change = _imp->handleChange;
    const bool changed = _imp->handleChanged;
    // Before any write, which may request the scene that has it before returning.
    const U64 serial = _imp->requestedSerial;
    bool written = false;

    if (changed) {
        if (_imp->renderer.isSelectionTransformDrag()) {
            SceneTransformDrag drag;
            if (_imp->renderer.getTransformDrag(change, _imp->frame, &drag)) {
                _imp->transformRecorder.commit(drag);
                QVariantList delta;
                for (int i = 0; i < 16; ++i) {
                    delta.push_back(drag.delta[i]);
                }
                QStringList paths;
                for (const SceneTransformPrim& prim : drag.prims) {
                    paths.push_back(QString::fromStdString(prim.path));
                }
                setProperty("natronSceneTransformDelta", QVariantList { QString::fromUtf8(transformToolName(drag.tool)), paths, delta });
                setProperty("natronSceneTransformDrags", _imp->transformRecorder.getCount());
                if (_imp->transformTarget && !property("natronSceneTransformRecordOnly").toBool()) {
                    written = _imp->transformTarget->commit(drag);
                }
            }
        } else {
            written = commitOpHandleDrag(change);
        }
    }
    _imp->renderer.endTransformDrag();
    _imp->handle = SceneGizmo::kNoHandle;
    _imp->handleNode.reset();
    _imp->handleChange = 0.;
    _imp->handleChanged = false;
    if (written && (_imp->syncedSerial <= serial)) {
        // The preview stays until the scene with the written drag is shown.
        _imp->renderer.clearTransformPreviewAfter(serial);
        _imp->previewAwaitsSync = true;
        _imp->previewSerial = serial;
        _imp->previewTimer->start();
    } else {
        _imp->clearPreview();
    }
    update();
} // SceneViewport::endHandleDrag

bool
SceneViewport::commitOpHandleDrag(double change)
{
    NodePtr node = _imp->handleNode.lock();
    USDEditTransform* effect = node ? dynamic_cast<USDEditTransform*>(node->getEffectInstance().get()) : nullptr;

    if (!effect) {
        return false;
    }
    const SceneTransformKnobs& knobs = effect->getTransformKnobs();
    KnobDoublePtr knob;
    switch (SceneGizmo::getHandleKind(_imp->handle)) {
    case SceneGizmo::eHandleKindTranslate:
        knob = knobs.translate.lock();
        break;
    case SceneGizmo::eHandleKindRotate:
        knob = knobs.rotate.lock();
        break;
    case SceneGizmo::eHandleKindScale:
        knob = knobs.scale.lock();
        break;
    }
    if (!knob) {
        return false;
    }
    const int axis = SceneGizmo::getHandleAxis(_imp->handle);
    const double old = knob->getValueAtTime(_imp->frame, axis);
    const double value = (SceneGizmo::getHandleKind(_imp->handle) == SceneGizmo::eHandleKindScale) ? old * change : old + change;
    if (value == old) {
        return false;
    }
    applySceneKnobEdits(node, _imp->frame, { makeSceneKnobEdit(knob, axis, _imp->frame, value) }, _imp->undoHistory.data(),
                        tr("Move %1").arg(QString::fromUtf8(node->getLabel().c_str())));

    return true;
} // SceneViewport::commitOpHandleDrag

void
SceneViewport::highlightHandleAt(const QPoint& pos)
{
    const qreal dpr = devicePixelRatio();
    const GfVec2d pixel = Implementation::toPixel(this, pos);
    bool busy = false;
    const int handle = _imp->renderer.hitTestTransformHandles(pixel[0], pixel[1], int(width() * dpr), int(height() * dpr), &busy);

    // The highlight stays as it is: it can't tell what is under the pointer.
    if (!busy && (handle != _imp->renderer.getHighlightedTransformHandle())) {
        _imp->renderer.setHighlightedTransformHandle(handle);
        update();
    }
}

void
SceneViewport::wheelEvent(QWheelEvent* e)
{
    // X11 turns a vertical wheel horizontal while Alt is held.
    const QPoint angle = e->angleDelta();
    const double steps = ((angle.y() != 0) ? angle.y() : angle.x()) / 120.;

    if ((steps == 0.) || !matchesMouseBinding(kShortcutIDMouseViewer3DDollyWheel, mouseBindingModifiers(e->modifiers(), _imp->spaceHeld), Qt::NoButton)) {
        e->ignore();

        return;
    }
    if (navigatesLockedCamera()) {
        // A run of wheel steps is one move, written once the wheel rests; a drag under way owns
        // the move and ends it itself.
        if (_imp->cameraLock.active || beginCameraLock()) {
            moveLockedCamera(Implementation::eDragRoleDolly, QPoint(), std::pow(0.85, steps));
            if (_imp->button == Qt::NoButton) {
                _imp->wheelMoveTimer->start();
            }
        }
        update();
        e->accept();

        return;
    }
    _imp->renderer.dolly(std::pow(0.85, steps));
    endLockOutsideLookThrough();
    update();
    e->accept();
}

bool
SceneViewport::event(QEvent* e)
{
    // The 3D viewer's keys win over the window shortcuts of the viewer tab around it.
    if (e->type() == QEvent::ShortcutOverride) {
        QKeyEvent* k = static_cast<QKeyEvent*>(e);
        if (matchesKeyBinding(kShortcutIDActionViewer3DFrameSelection, k->modifiers(), k->key()) || matchesKeyBinding(kShortcutIDActionViewer3DFrameAll, k->modifiers(), k->key())
            || matchesKeyBinding(kShortcutIDActionViewer3DLockView, k->modifiers(), k->key()) || (viewModeForKey(k->modifiers(), k->key()) >= 0)
            || (drawModeForKey(k->modifiers(), k->key()) >= 0) || matchesKeyBinding(kShortcutIDActionViewer3DShowGrid, k->modifiers(), k->key())
            || matchesKeyBinding(kShortcutIDActionViewer3DShowGuides, k->modifiers(), k->key()) || isTransformKey(k->modifiers(), k->key())
            || matchesKeyBinding(kShortcutIDActionViewer3DSetSelectionOnNode, k->modifiers(), k->key())
            || matchesKeyBinding(kShortcutIDActionViewer3DCreateFromSelection, k->modifiers(), k->key())
            || ((k->key() == Qt::Key_Escape) && (_imp->handle != SceneGizmo::kNoHandle)) || ((k->key() == Qt::Key_Space) && bindingsUseSpace())) {
            e->accept();

            return true;
        }
    }
    // QWidget::event() spends Tab on moving the focus before keyPressEvent() sees it.
    if (e->type() == QEvent::KeyPress) {
        QKeyEvent* k = static_cast<QKeyEvent*>(e);
        if (matchesKeyBinding(kShortcutIDActionViewer3DCreateFromSelection, k->modifiers(), k->key())) {
            keyPressEvent(k);

            return true;
        }
    }

    return QOpenGLWidget::event(e);
}

void
SceneViewport::keyPressEvent(QKeyEvent* e)
{
    if ((e->key() == Qt::Key_Space) && bindingsUseSpace()) {
        _imp->spaceHeld = true;
        e->accept();

        return;
    }
    if ((e->key() == Qt::Key_Escape) && (_imp->handle != SceneGizmo::kNoHandle)) {
        // Drops the drag under way: nothing is written.
        _imp->handleChanged = false;
        endHandleDrag();
        clearGesture();
        e->accept();

        return;
    }
    const int tool = transformToolForKey(e->modifiers(), e->key());
    if (tool >= 0) {
        setTransformTool(tool);
        e->accept();

        return;
    }
    if (matchesKeyBinding(kShortcutIDActionViewer3DCycleTool, e->modifiers(), e->key())) {
        cycleTransformTool();
        e->accept();

        return;
    }
    if (matchesKeyBinding(kShortcutIDActionViewer3DSetSelectionOnNode, e->modifiers(), e->key())) {
        if (_imp->selectionActions.setOnNode) {
            _imp->selectionActions.setOnNode(getSelection());
        }
        e->accept();

        return;
    }
    if (matchesKeyBinding(kShortcutIDActionViewer3DCreateFromSelection, e->modifiers(), e->key())) {
        if (_imp->selectionActions.createFromSelection) {
            _imp->selectionActions.createFromSelection(getSelection());
        }
        e->accept();

        return;
    }
    if (matchesKeyBinding(kShortcutIDActionViewer3DToggleSpace, e->modifiers(), e->key())) {
        setTransformSpace((_imp->renderer.getTransformSpace() == SceneTransform::eSpaceLocal) ? SceneTransform::eSpaceWorld : SceneTransform::eSpaceLocal);
        e->accept();

        return;
    }
    if (matchesKeyBinding(kShortcutIDActionViewer3DTogglePivot, e->modifiers(), e->key())) {
        setTransformPivot((_imp->renderer.getTransformPivot() == SceneTransform::ePivotCurrentPrim) ? SceneTransform::ePivotSelectionCentre : SceneTransform::ePivotCurrentPrim);
        e->accept();

        return;
    }
    if (matchesKeyBinding(kShortcutIDActionViewer3DFrameSelection, e->modifiers(), e->key())) {
        frameSelection();
        e->accept();

        return;
    }
    if (matchesKeyBinding(kShortcutIDActionViewer3DFrameAll, e->modifiers(), e->key())) {
        frameScene();
        e->accept();

        return;
    }
    const int mode = viewModeForKey(e->modifiers(), e->key());
    if (mode >= 0) {
        setViewMode((SceneViewportRenderer::ViewModeEnum)mode);
        e->accept();

        return;
    }
    const int drawMode = drawModeForKey(e->modifiers(), e->key());
    if (drawMode >= 0) {
        setDrawMode((SceneViewportRenderer::DrawModeEnum)drawMode);
        e->accept();

        return;
    }
    if (matchesKeyBinding(kShortcutIDActionViewer3DShowGrid, e->modifiers(), e->key())) {
        setShowGrid(!_imp->renderer.getShowGrid());
        e->accept();

        return;
    }
    if (matchesKeyBinding(kShortcutIDActionViewer3DShowGuides, e->modifiers(), e->key())) {
        setShowGuides(!_imp->renderer.getShowGuides());
        e->accept();

        return;
    }
    if (matchesKeyBinding(kShortcutIDActionViewer3DLockView, e->modifiers(), e->key())) {
        if (navigatesLockedCamera()) {
            setLockView(false);
        } else {
            lockViewToCamera();
        }
        e->accept();

        return;
    }
    // Everything else is the viewer tab's.
    e->ignore();
}

void
SceneViewport::keyReleaseEvent(QKeyEvent* e)
{
    if ((e->key() == Qt::Key_Space) && _imp->spaceHeld) {
        if (!e->isAutoRepeat()) {
            _imp->spaceHeld = false;
        }
        e->accept();

        return;
    }
    e->ignore();
}

void
SceneViewport::focusOutEvent(QFocusEvent* e)
{
    // A release or key release that goes elsewhere never comes back.
    if (_imp->handle != SceneGizmo::kNoHandle) {
        endHandleDrag();
    }
    clearGesture();
    _imp->spaceHeld = false;
    QOpenGLWidget::focusOutEvent(e);
}

void
SceneViewport::contextMenuEvent(QContextMenuEvent* e)
{
    // A right press bound to a gesture is that gesture's.
    if ((e->reason() == QContextMenuEvent::Mouse) && (_imp->button != Qt::NoButton)) {
        e->accept();

        return;
    }
    showContextMenu(e->globalPos());
    e->accept();
}

void
SceneViewport::showContextMenu(const QPoint& globalPos)
{
    // The menu takes the release, so no drag may outlive it.
    clearGesture();
    Menu menu(this);

    QActionGroup* modes = new QActionGroup(&menu);
    for (const DrawModeEntry& item : kDrawModes) {
        QAction* action = menu.addAction(shortcutText(item.label));
        action->setCheckable(true);
        action->setChecked(_imp->renderer.getDrawMode() == item.mode);
        action->setActionGroup(modes);
        action->setShortcut(keySequenceOf(item.actionID));
        const SceneViewportRenderer::DrawModeEnum mode = item.mode;
        QObject::connect(action, &QAction::triggered, this, [this, mode]() {
            setDrawMode(mode);
        });
    }

    QMenu* renderers = menu.addMenu(tr("Renderer"));
    QActionGroup* rendererGroup = new QActionGroup(renderers);
    const std::string& rendererId = _imp->renderer.getRendererId();
    for (const RenderDelegates::Delegate& delegate : RenderDelegates::list()) {
        QString label = QString::fromStdString(delegate.displayName);
        if (!delegate.available) {
            label = tr("%1 (unavailable)").arg(label);
        }
        QAction* action = renderers->addAction(label);
        action->setCheckable(true);
        action->setChecked(rendererId == delegate.id);
        action->setActionGroup(rendererGroup);
        if (!delegate.available) {
            action->setToolTip(QString::fromStdString(delegate.error));
        }
        const QString id = QString::fromStdString(delegate.id);
        QObject::connect(action, &QAction::triggered, this, [this, id]() {
            setRendererId(id);
        });
    }

    menu.addSeparator();
    QMenu* lookThrough = menu.addMenu(tr("Look Through"));
    QActionGroup* cameras = new QActionGroup(lookThrough);
    std::vector<std::string> paths = _imp->renderer.getStageCameraPaths();
    paths.insert(paths.begin(), std::string());
    for (const std::string& path : paths) {
        QAction* action = lookThrough->addAction(path.empty() ? tr("Free Camera") : QString::fromStdString(path));
        action->setCheckable(true);
        action->setChecked(_imp->renderer.getLookThroughCamera() == path);
        action->setActionGroup(cameras);
        const QString qpath = QString::fromStdString(path);
        QObject::connect(action, &QAction::triggered, this, [this, qpath]() {
            setLookThroughCamera(qpath);
        });
    }
    // Looking through a camera and locking to it in one pick.
    if (paths.size() > 1) {
        lookThrough->addSeparator();
    }
    for (std::size_t i = 1; i < paths.size(); ++i) {
        const QString qpath = QString::fromStdString(paths[i]);
        QAction* action = lookThrough->addAction(tr("Lock to %1").arg(qpath));
        action->setCheckable(true);
        action->setChecked(navigatesLockedCamera() && (_imp->renderer.getLookThroughCamera() == paths[i]));
        QObject::connect(action, &QAction::triggered, this, [this, qpath]() {
            lockViewToCamera(qpath);
        });
    }

    QMenu* views = menu.addMenu(tr("View"));
    QActionGroup* viewModes = new QActionGroup(views);
    for (const ViewModeEntry& entry : kViewModes) {
        QAction* action = views->addAction(shortcutText(entry.label));
        action->setCheckable(true);
        action->setChecked(_imp->renderer.getLookThroughCamera().empty() && (_imp->renderer.getViewMode() == entry.mode));
        action->setActionGroup(viewModes);
        action->setShortcut(keySequenceOf(entry.actionID));
        const SceneViewportRenderer::ViewModeEnum mode = entry.mode;
        QObject::connect(action, &QAction::triggered, this, [this, mode]() {
            setViewMode(mode);
        });
    }

    QAction* lockView = menu.addAction(shortcutText(kShortcutDescActionViewer3DLockView));
    lockView->setCheckable(true);
    lockView->setChecked(navigatesLockedCamera());
    lockView->setShortcut(keySequenceOf(kShortcutIDActionViewer3DLockView));
    QObject::connect(lockView, &QAction::toggled, this, [this](bool locked) {
        if (locked) {
            lockViewToCamera();
        } else {
            setLockView(false);
        }
    });
    QAction* createCamera = menu.addAction(tr("Create Camera from View"));
    createCamera->setEnabled(bool(_imp->viewedNodeProvider) && _imp->renderer.hasScene());
    QObject::connect(createCamera, &QAction::triggered, this, [this]() {
        createCameraFromView();
    });

    menu.addSeparator();
    {
        const QStringList selection = getSelection();
        QAction* setOnNode = menu.addAction(tr(kShortcutDescActionViewer3DSetSelectionOnNode));
        setOnNode->setObjectName(QString::fromUtf8("sceneSetSelectionOnNode"));
        setOnNode->setShortcut(keySequenceOf(kShortcutIDActionViewer3DSetSelectionOnNode));
        const QString blocker = _imp->selectionActions.setOnNodeBlocker ? _imp->selectionActions.setOnNodeBlocker(selection) : QString();
        setOnNode->setEnabled(bool(_imp->selectionActions.setOnNode) && blocker.isEmpty());
        setOnNode->setToolTip(blocker);
        setOnNode->setStatusTip(blocker);
        QObject::connect(setOnNode, &QAction::triggered, this, [this, selection]() {
            if (_imp->selectionActions.setOnNode) {
                _imp->selectionActions.setOnNode(selection);
            }
        });
        QAction* create = menu.addAction(tr(kShortcutDescActionViewer3DCreateFromSelection));
        create->setObjectName(QString::fromUtf8("sceneCreateFromSelection"));
        create->setShortcut(keySequenceOf(kShortcutIDActionViewer3DCreateFromSelection));
        create->setEnabled(bool(_imp->selectionActions.createFromSelection) && _imp->renderer.hasScene());
        QObject::connect(create, &QAction::triggered, this, [this, selection]() {
            if (_imp->selectionActions.createFromSelection) {
                _imp->selectionActions.createFromSelection(selection);
            }
        });
    }

    // The free camera's clip planes: typing a distance makes them absolute.
    QMenu* clipping = menu.addMenu(tr("Clipping"));
    QAction* automaticClip = clipping->addAction(tr("Automatic Clipping"));
    automaticClip->setCheckable(true);
    automaticClip->setChecked(_imp->renderer.getClipPlanes().automatic);
    double nearClip = 0., farClip = 0.;
    _imp->renderer.getFreeCameraClipRange(&nearClip, &farClip);
    QDoubleSpinBox* clipBoxes[2] = { nullptr, nullptr };
    const struct
    {
        const char* name;
        QString label;
        double value;
    } clipItems[] = {
        { "nearClip", tr("Near Clip"), nearClip },
        { "farClip", tr("Far Clip"), farClip },
    };
    for (int i = 0; i < 2; ++i) {
        QWidget* row = new QWidget(clipping);
        QHBoxLayout* layout = new QHBoxLayout(row);
        layout->setContentsMargins(8, 2, 8, 2);
        layout->addWidget(new QLabel(clipItems[i].label, row));
        QDoubleSpinBox* box = new QDoubleSpinBox(row);
        box->setObjectName(QString::fromUtf8(clipItems[i].name));
        box->setDecimals(4);
        box->setRange(1e-4, 1e12);
        box->setValue(clipItems[i].value);
        box->setKeyboardTracking(false);
        layout->addWidget(box);
        QWidgetAction* action = new QWidgetAction(clipping);
        action->setDefaultWidget(row);
        clipping->addAction(action);
        clipBoxes[i] = box;
    }
    QDoubleSpinBox* nearBox = clipBoxes[0];
    QDoubleSpinBox* farBox = clipBoxes[1];
    auto applyClip = [this, nearBox, farBox, automaticClip]() {
        SceneViewportRenderer::ClipPlanes planes;
        planes.automatic = false;
        planes.nearClip = nearBox->value();
        planes.farClip = farBox->value();
        {
            const QSignalBlocker blocker(automaticClip);
            automaticClip->setChecked(false);
        }
        setClipPlanes(planes);
    };
    QObject::connect(nearBox, qOverload<double>(&QDoubleSpinBox::valueChanged), this, applyClip);
    QObject::connect(farBox, qOverload<double>(&QDoubleSpinBox::valueChanged), this, applyClip);
    QObject::connect(automaticClip, &QAction::toggled, this, [this, nearBox, farBox](bool automatic) {
        SceneViewportRenderer::ClipPlanes planes = _imp->renderer.getClipPlanes();
        planes.automatic = automatic;
        if (!automatic) {
            // Absolute planes start where the automatic ones are.
            _imp->renderer.getFreeCameraClipRange(&planes.nearClip, &planes.farClip);
        }
        setClipPlanes(planes);
        double n = 0., f = 0.;
        _imp->renderer.getFreeCameraClipRange(&n, &f);
        const QSignalBlocker nearBlocker(nearBox);
        const QSignalBlocker farBlocker(farBox);
        nearBox->setValue(n);
        farBox->setValue(f);
    });

    menu.addSeparator();
    QAction* frameSelected = menu.addAction(tr("Frame Selection"));
    frameSelected->setShortcut(keySequenceOf(kShortcutIDActionViewer3DFrameSelection));
    QObject::connect(frameSelected, &QAction::triggered, this, [this]() {
        frameSelection();
    });
    QAction* frame = menu.addAction(tr("Frame All"));
    frame->setShortcut(keySequenceOf(kShortcutIDActionViewer3DFrameAll));
    QObject::connect(frame, &QAction::triggered, this, [this]() {
        frameScene();
    });

    // The transform widget's tool, frame and origin.
    QMenu* transform = menu.addMenu(tr("Transform"));
    QActionGroup* tools = new QActionGroup(transform);
    for (const TransformToolEntry& entry : kTransformTools) {
        QAction* action = transform->addAction(tr(entry.label));
        action->setObjectName(QString::fromUtf8(entry.name));
        action->setCheckable(true);
        action->setChecked(_imp->renderer.getTransformTool() == entry.tool);
        action->setShortcut(keySequenceOf(entry.actionID));
        action->setActionGroup(tools);
        const int tool = entry.tool;
        QObject::connect(action, &QAction::triggered, this, [this, tool]() {
            setTransformTool(tool);
        });
    }
    transform->addSeparator();
    QActionGroup* spaces = new QActionGroup(transform);
    const struct
    {
        SceneTransform::SpaceEnum space;
        const char* name;
        QString label;
    } spaceItems[] = {
        { SceneTransform::eSpaceWorld, "world", tr("World Space") },
        { SceneTransform::eSpaceLocal, "local", tr("Local Space") },
    };
    for (const auto& item : spaceItems) {
        QAction* action = transform->addAction(item.label);
        action->setObjectName(QString::fromUtf8(item.name));
        action->setCheckable(true);
        action->setChecked(_imp->renderer.getTransformSpace() == item.space);
        action->setActionGroup(spaces);
        const int space = item.space;
        QObject::connect(action, &QAction::triggered, this, [this, space]() {
            setTransformSpace(space);
        });
    }
    transform->addSeparator();
    QActionGroup* pivots = new QActionGroup(transform);
    const struct
    {
        SceneTransform::PivotEnum pivot;
        const char* name;
        QString label;
    } pivotItems[] = {
        { SceneTransform::ePivotSelectionCentre, "selectionCentre", tr("Selection Centre Pivot") },
        { SceneTransform::ePivotCurrentPrim, "currentPrim", tr("Current Prim Pivot") },
    };
    for (const auto& item : pivotItems) {
        QAction* action = transform->addAction(item.label);
        action->setObjectName(QString::fromUtf8(item.name));
        action->setCheckable(true);
        action->setChecked(_imp->renderer.getTransformPivot() == item.pivot);
        action->setActionGroup(pivots);
        const int pivot = item.pivot;
        QObject::connect(action, &QAction::triggered, this, [this, pivot]() {
            setTransformPivot(pivot);
        });
    }

    // The purposes and lights Storm draws with, and the overlays drawn over it.
    QMenu* display = menu.addMenu(tr("Display"));
    QMenu* purposes = display->addMenu(tr("Purposes"));
    const struct
    {
        const char* name;
        QString label;
        bool shown;
    } purposeItems[] = {
        { "proxy", tr("Proxy"), _imp->renderer.getShowProxy() },
        { "render", tr("Render"), _imp->renderer.getShowRender() },
        { "guide", tr("Guide"), _imp->renderer.getShowGuidePurpose() },
    };
    QAction* purposeActions[3] = { nullptr, nullptr, nullptr };
    for (int i = 0; i < 3; ++i) {
        QAction* action = purposes->addAction(purposeItems[i].label);
        action->setObjectName(QString::fromUtf8(purposeItems[i].name));
        action->setCheckable(true);
        action->setChecked(purposeItems[i].shown);
        purposeActions[i] = action;
    }
    for (QAction* action : purposeActions) {
        QObject::connect(action, &QAction::toggled, this, [this, purposeActions]() {
            setShowPurposes(purposeActions[0]->isChecked(), purposeActions[1]->isChecked(), purposeActions[2]->isChecked());
        });
    }
    QMenu* lighting = display->addMenu(tr("Lighting"));
    QActionGroup* lightingModes = new QActionGroup(lighting);
    for (const LightingModeEntry& entry : kLightingModes) {
        QAction* action = lighting->addAction(tr(entry.label));
        action->setObjectName(QString::fromUtf8(entry.name));
        action->setCheckable(true);
        action->setChecked(_imp->renderer.getLightingMode() == entry.mode);
        action->setActionGroup(lightingModes);
        const SceneViewportRenderer::LightingModeEnum mode = entry.mode;
        QObject::connect(action, &QAction::triggered, this, [this, mode]() {
            setLightingMode(mode);
        });
    }
    display->addSeparator();
    QAction* grid = display->addAction(shortcutText(kShortcutDescActionViewer3DShowGrid));
    grid->setCheckable(true);
    grid->setChecked(_imp->renderer.getShowGrid());
    grid->setShortcut(keySequenceOf(kShortcutIDActionViewer3DShowGrid));
    QObject::connect(grid, &QAction::toggled, this, [this](bool show) {
        setShowGrid(show);
    });
    QAction* guides = display->addAction(shortcutText(kShortcutDescActionViewer3DShowGuides));
    guides->setCheckable(true);
    guides->setChecked(_imp->renderer.getShowGuides());
    guides->setShortcut(keySequenceOf(kShortcutIDActionViewer3DShowGuides));
    QObject::connect(guides, &QAction::toggled, this, [this](bool show) {
        setShowGuides(show);
    });

    menu.exec(globalPos);
    clearGesture();
} // showContextMenu

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_SceneViewport.cpp"
