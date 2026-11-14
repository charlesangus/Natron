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

#include "USDProject3D.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#include <QMetaObject>

#include "Engine/AppInstance.h"
#include "Engine/ChoiceOption.h"
#include "Engine/ContentHasher.h"
#include "Engine/Format.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Scene/HydraRenderImpl.h"
#include "Engine/Nodes/Scene/LiveTextureStore.h"
#include "Engine/Nodes/Scene/SceneCaches.h"
#include "Engine/Nodes/Scene/SceneNodeAuthoring.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneProjectionCameras.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"
#include "Engine/Project.h"
#include "Engine/ScenePayload.h"

NATRON_NAMESPACE_ENTER

namespace {
// The layer of the Image input the live texture reads.
const char* const kLiveTextureLayer = "rgba";
} // anonymous namespace

// The camera as the shader reads it, once, or at every frame of the project range.
struct USDProject3D::CameraSamples {
    std::vector<double> frames;
    std::vector<PXR_NS::GfMatrix4d> worldToProjector;
    std::vector<PXR_NS::GfVec4f> origin;
    // Camera Path names a camera the camera scene does not have: one zero matrix, which puts every
    // point behind the camera, so nothing projects until the stage that renders the material
    // resolves the path (SceneProjectionCameras).
    bool missing = false;
    // When missing, the project frames that stage reads the camera at: the hold, or the range when
    // the camera may move there.
    std::vector<double> deferredFrames;
    U64 digest = 0;
};

struct USDProject3D::Implementation {
    std::mutex lock;
    // The last samples and what they were read from, so an evaluation that changes nothing they
    // read does not read the camera scene again.
    bool samplesValid = false;
    U64 samplesKey = 0;
    std::shared_ptr<const CameraSamples> samples;
};

USDProject3D::USDProject3D(NodePtr node)
    : NativeEffectBase(node)
    , _imp(new Implementation())
    , _cameraMenuRefreshPending(false)
{
    _cameraMenuRefresh.setRerun([this]() { return refreshCameraMenu(); });
}

USDProject3D::~USDProject3D()
{
}

const NodeSpec*
USDProject3D::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

std::string
USDProject3D::getShaderPath()
{
    SceneCaches::registerLiveTextures(nullptr);
    const PXR_NS::PlugPluginPtr plugin = PXR_NS::PlugRegistry::GetInstance().GetPluginWithName("natronLiveTexture");

    return plugin ? plugin->FindPluginResource(kUSDProject3DShaderFile, true) : std::string();
}

void
USDProject3D::onScriptNameChanged(const std::string& /*fullyQualifiedName*/)
{
    followSceneNodeRename(_primPath.lock(), getSceneNodeDefaultPrimPath(this, "Material"));
}

void
USDProject3D::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    _primPath = createScenePrimPathKnob(this, page, getSceneNodeDefaultPrimPath(this, "Material"),
                                        tr("The path of the material prim the node authors. Its shaders are authored under it."));

    _camera.create(this, page, kUSDProject3DParamCamera, kUSDProject3DParamCameraPath,
                   tr("The cameras of the Camera input's scene: picking one sets Camera Path to it."),
                   tr("The path of the camera to project through. Empty, the first camera of the Camera input's "
                      "scene is used. A path the Camera input does not have, or with nothing connected to it, is read "
                      "from the scene the projection renders in, such as a USDMerge with the camera below the "
                      "geometry; USDRender warns when that scene has no camera at the path."));

    KnobChoicePtr projectionSpace = createKnob<KnobChoice>(tr("Projection Space"));
    projectionSpace->setName(kUSDProject3DParamProjectionSpace);
    projectionSpace->setHintToolTip(tr("What the projection is fixed to once the material is connected to a geometry node."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("world", tr("World").toStdString(),
                                       tr("The projection stays where the camera is: geometry moving after it slides through "
                                          "the image.")
                                           .toStdString()));
        options.push_back(ChoiceOption("geometry", tr("Geometry").toStdString(),
                                       tr("The projection is fixed to the geometry as the geometry node places it, so "
                                          "transforms after that node carry the image along. With Use Frame Hold on, the "
                                          "geometry is also taken at Frame Hold, so the image sticks to it however it "
                                          "moves.")
                                           .toStdString()));
        projectionSpace->populateChoices(options);
    }
    projectionSpace->setDefaultValue((int)eProjectionSpaceWorld);
    projectionSpace->setAnimationEnabled(false);
    page->addKnob(projectionSpace);
    _projectionSpace = projectionSpace;

    KnobChoicePtr projectOn = createKnob<KnobChoice>(tr("Project On"));
    projectOn->setName(kUSDProject3DParamProjectOn);
    projectOn->setHintToolTip(tr("Which faces the image falls on, by whether they face the camera. The others show Outside."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("both", tr("Both").toStdString(), tr("Every face.").toStdString()));
        options.push_back(ChoiceOption("front", tr("Front").toStdString(), tr("Faces facing the camera.").toStdString()));
        options.push_back(ChoiceOption("back", tr("Back").toStdString(), tr("Faces facing away from the camera.").toStdString()));
        projectOn->populateChoices(options);
    }
    projectOn->setDefaultValue((int)eProjectOnBoth);
    projectOn->setAnimationEnabled(false);
    page->addKnob(projectOn);
    _projectOn = projectOn;

    KnobBoolPtr crop = createKnob<KnobBool>(tr("Crop"));
    crop->setName(kUSDProject3DParamCrop);
    crop->setHintToolTip(tr("Outside the camera's frame, which the image's format fills, the image is transparent black. Off, "
                            "the image's data beyond its format shows there. Beyond the image's data it is transparent "
                            "black either way, as the Viewer shows it."));
    crop->setDefaultValue(true);
    crop->setAnimationEnabled(false);
    page->addKnob(crop);
    _crop = crop;

    KnobBoolPtr clipNearFar = createKnob<KnobBool>(tr("Clip Near Far"));
    clipNearFar->setName(kUSDProject3DParamClipNearFar);
    clipNearFar->setHintToolTip(tr("Nearer than the camera's near clipping plane or beyond its far one, the surface shows "
                                   "Outside."));
    clipNearFar->setDefaultValue(false);
    clipNearFar->setAnimationEnabled(false);
    page->addKnob(clipNearFar);
    _clipNearFar = clipNearFar;

    KnobChoicePtr outside = createKnob<KnobChoice>(tr("Outside"));
    outside->setName(kUSDProject3DParamOutside);
    outside->setHintToolTip(tr("What the surface shows where the image does not fall: behind the camera, beyond the "
                               "clipping planes with Clip Near Far on, and on faces Project On leaves out."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("black", tr("Black").toStdString(), tr("Opaque black.").toStdString()));
        options.push_back(ChoiceOption("transparent", tr("Transparent").toStdString(),
                                       tr("Nothing: the surface is not drawn there.").toStdString()));
        outside->populateChoices(options);
    }
    outside->setDefaultValue((int)eOutsideBlack);
    outside->setAnimationEnabled(false);
    page->addKnob(outside);
    _outside = outside;

    KnobChoicePtr fitMode = createKnob<KnobChoice>(tr("Fit Mode"));
    fitMode->setName(kUSDProject3DParamFitMode);
    fitMode->setHintToolTip(tr("How the camera's aperture is conformed to the Image input's format when their shapes differ, "
                               "as in USDRender."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("fill", tr("Fill").toStdString(),
                                       tr("The aperture fills the format: the side that does not fit is cropped.").toStdString()));
        options.push_back(ChoiceOption("horizontal", tr("Horizontal").toStdString(),
                                       tr("The aperture's width fills the format's; its height is cropped or extended.").toStdString()));
        options.push_back(ChoiceOption("vertical", tr("Vertical").toStdString(),
                                       tr("The aperture's height fills the format's; its width is cropped or extended.").toStdString()));
        fitMode->populateChoices(options);
    }
    fitMode->setDefaultValue((int)HydraRender::eFitModeFill);
    fitMode->setAnimationEnabled(false);
    page->addKnob(fitMode);
    _fitMode = fitMode;

    KnobBoolPtr useFrameHold = createKnob<KnobBool>(tr("Use Frame Hold"));
    useFrameHold->setName(kUSDProject3DParamUseFrameHold);
    useFrameHold->setHintToolTip(tr("Projects through the camera as it is at Frame Hold on every frame, so a moving camera "
                                    "leaves the projection where it was then. The image still plays."));
    useFrameHold->setDefaultValue(false);
    useFrameHold->setAnimationEnabled(false);
    page->addKnob(useFrameHold);
    _useFrameHold = useFrameHold;

    KnobDoublePtr frameHold = createKnob<KnobDouble>(tr("Frame Hold"));
    frameHold->setName(kUSDProject3DParamFrameHold);
    frameHold->setHintToolTip(tr("The frame the camera is held at when Use Frame Hold is on."));
    frameHold->setDefaultValue(1.);
    frameHold->setAnimationEnabled(false);
    frameHold->setAllDimensionsEnabled(false);
    page->addKnob(frameHold);
    _frameHold = frameHold;
} // USDProject3D::initializeKnobs

void
USDProject3D::onKnobsLoaded()
{
    NativeEffectBase::onKnobsLoaded();
    KnobBoolPtr useFrameHold = _useFrameHold.lock();
    KnobDoublePtr frameHold = _frameHold.lock();
    if (useFrameHold && frameHold) {
        frameHold->setAllDimensionsEnabled(useFrameHold->getValue());
    }
    scheduleCameraMenuRefresh();
}

void
USDProject3D::onInputChanged(int inputNo)
{
    NativeEffectBase::onInputChanged(inputNo);
    scheduleCameraMenuRefresh();
}

void
USDProject3D::onNodeHashChanged(U64 hash)
{
    NativeEffectBase::onNodeHashChanged(hash);
    scheduleCameraMenuRefresh();
}

bool
USDProject3D::knobChanged(KnobI* k,
                          ValueChangedReasonEnum reason,
                          ViewSpec /*view*/,
                          double /*time*/,
                          bool /*originatedFromMainThread*/)
{
    if (reason == eValueChangedReasonTimeChanged) {
        return false;
    }
    bool menuStale = false;
    if (_camera.knobChanged(k, reason, &menuStale)) {
        if (menuStale) {
            scheduleCameraMenuRefresh();
        }

        return true;
    }
    KnobBoolPtr useFrameHold = _useFrameHold.lock();
    KnobDoublePtr frameHold = _frameHold.lock();
    if (useFrameHold && frameHold && (k == useFrameHold.get())) {
        frameHold->setAllDimensionsEnabled(useFrameHold->getValue());

        return true;
    }

    return false;
}

void
USDProject3D::scheduleCameraMenuRefresh()
{
    if (_cameraMenuRefreshPending.exchange(true)) {
        return;
    }
    // The hooks that call this fire mid-recompute of a hash, before the upstream scene settled.
    QMetaObject::invokeMethod(this, [this]() {
        _cameraMenuRefreshPending = false;
        refreshCameraMenu(); }, Qt::QueuedConnection);
}

std::shared_ptr<SceneMenuJob>
USDProject3D::refreshCameraMenu()
{
    bool noViewer = false;
    const SceneEvaluationContext viewerContext = _cameraMenuRefresh.getContext(this, &noViewer);
    const int inputNb = kUSDProject3DInputCamera;
    const EffectInstancePtr input = getInput(inputNb);
    const double time = getCurrentTime();
    SceneEvaluationContext context = viewerContext;
    if (input) {
        context = SceneNodeAuthoring::getCameraMenuContext(viewerContext, _camera.getPath());
        if (!context.hasSameLoadSet(viewerContext)) {
            context.id = _cameraMenuRefresh.getMappedContextId();
        }
        context.reach = std::make_shared<SceneLoadReach>();
    }
    SceneMenuRefresh::WorkFunc work = [weakInput = EffectInstanceWPtr(input), time, context](const SceneMenuJob& job,
                                                                                             SceneMenuJob::Result* result) {
        result->lists.resize(1);
        // The menu lists what the whole stage holds.
        if (context.reach) {
            context.reach->add(std::vector<std::string>(1, std::string("/")));
        }
        ScenePayload scene;
        SceneQueryPool::Lease lease;
        if (!SceneNodeAuthoring::borrowMenuScene(weakInput, time, context, job, &scene, &lease)) {
            return;
        }
        for (const PXR_NS::SdfPath& path : HydraRender::cameraPaths(lease.getStage())) {
            result->lists[0].push_back(path.GetString());
        }
        result->unsearched = SceneNodeAuthoring::countMenuUnsearched(lease.getStage(), context);
    };

    return _cameraMenuRefresh.start(this, context, noViewer, work, [this](const std::shared_ptr<SceneMenuJob>& job) { applyCameraMenu(job); });
}

bool
USDProject3D::applyCameraMenu(const std::shared_ptr<SceneMenuJob>& job)
{
    if (!_cameraMenuRefresh.take(job)) {
        return false;
    }
    const SceneMenuJob::Result result = job->getResult();
    _camera.refreshMenu(result.lists.empty() ? std::vector<std::string>() : result.lists[0], true, result.unsearched, job->hasNoViewer());

    return true;
}

double
USDProject3D::getPlateAspect(double time) const
{
    EffectInstancePtr input = getInput(kUSDProject3DInputImage);
    if (input) {
        input = input->getNearestNonIdentity(time);
    }
    const NodeMetadataConstPtr metadata = input ? input->getMetadata(time, ViewIdx(0)) : NodeMetadataConstPtr();
    if (metadata) {
        const RectI& format = metadata->getOutputFormat();
        const double par = metadata->getPixelAspectRatio(-1);
        if ((format.width() > 0) && (format.height() > 0) && (par > 0.)) {
            return format.width() * par / format.height();
        }
    }
    Format format;
    getApp()->getProject()->getProjectDefaultFormat(&format);

    return (format.height() > 0) ? format.width() * format.getPixelAspectRatio() / format.height() : 1.;
}

SceneEvaluationContext
USDProject3D::getSceneInputContext(int inputNb,
                                   const SceneEvaluationContext& ctx,
                                   double /*time*/,
                                   ViewIdx /*view*/) const
{
    return (inputNb == kUSDProject3DInputCamera) ? SceneNodeAuthoring::getCameraInputContext(ctx, _camera.getPath()) : ctx;
}

void
USDProject3D::mapSceneInputReach(int inputNb,
                                 double /*time*/,
                                 ViewIdx /*view*/,
                                 const std::vector<std::string>& in,
                                 std::vector<std::string>* out) const
{
    // A camera input read on its own context loads the same whatever the output loads.
    if (inputNb == kUSDProject3DInputCamera) {
        out->clear();
    } else {
        *out = in;
    }
}

StatusEnum
USDProject3D::renderScene(const SceneRenderActionArgs& args,
                          ScenePayload* outputScene,
                          std::string* errorMessage)
{
    const ScenePayload cameraScene = args.getInputScene(kUSDProject3DInputCamera);

    KnobChoicePtr projectOnKnob = _projectOn.lock();
    KnobBoolPtr cropKnob = _crop.lock();
    KnobBoolPtr clipKnob = _clipNearFar.lock();
    KnobChoicePtr outsideKnob = _outside.lock();
    KnobChoicePtr fitKnob = _fitMode.lock();
    KnobChoicePtr spaceKnob = _projectionSpace.lock();
    KnobBoolPtr useFrameHoldKnob = _useFrameHold.lock();
    KnobDoublePtr frameHoldKnob = _frameHold.lock();
    KnobStringPtr primPathKnob = _primPath.lock();
    if (!projectOnKnob || !cropKnob || !clipKnob || !outsideKnob || !fitKnob || !spaceKnob || !useFrameHoldKnob || !frameHoldKnob || !primPathKnob) {
        *errorMessage = tr("The node is being destroyed").toStdString();

        return eStatusFailed;
    }
    const auto fail = [this, errorMessage]() {
        setPersistentMessage(eMessageTypeError, *errorMessage);

        return eStatusFailed;
    };

    const std::string shaderPath = getShaderPath();
    if (shaderPath.empty()) {
        *errorMessage = tr("The projection shader %1 is missing from this installation (Resources/usd/natronLiveTexture).")
                            .arg(QString::fromUtf8(kUSDProject3DShaderFile))
                            .toStdString();

        return fail();
    }
    std::string cameraPath = _camera.getPath();
    const int projectOn = std::clamp(projectOnKnob->getValue(), (int)eProjectOnBoth, (int)eProjectOnBack);
    const bool crop = cropKnob->getValue();
    const bool clip = clipKnob->getValue();
    const int outside = std::clamp(outsideKnob->getValue(), (int)eOutsideBlack, (int)eOutsideTransparent);
    const int fitValue = fitKnob->getValue();
    const HydraRender::FitModeEnum fit = (fitValue == (int)HydraRender::eFitModeHorizontal) ? HydraRender::eFitModeHorizontal : (fitValue == (int)HydraRender::eFitModeVertical) ? HydraRender::eFitModeVertical
                                                                                                                                                                                 : HydraRender::eFitModeFill;
    const int space = std::clamp(spaceKnob->getValue(), (int)eProjectionSpaceWorld, (int)eProjectionSpaceGeometry);
    const bool useFrameHold = useFrameHoldKnob->getValue();
    const double frameHold = frameHoldKnob->getValue();
    const double aspect = getPlateAspect(args.time);

    SceneLayerMemo::Key layerKey;
    if (!getSceneLayerKey(SceneLayerReads(), &layerKey, errorMessage)) {
        return eStatusFailed;
    }
    ContentHasher keyHasher;
    keyHasher.appendU64(layerKey.nodeDigest);
    keyHasher.appendDouble(layerKey.firstFrame);
    keyHasher.appendDouble(layerKey.lastFrame);
    keyHasher.appendDouble(layerKey.framesPerSecond);
    keyHasher.appendU64(cameraScene.getStackHash());
    keyHasher.appendString(cameraPath);
    keyHasher.appendDouble(aspect);
    // Looking for the first camera searches the scene as the camera input loads it.
    if (cameraPath.empty()) {
        const SceneEvaluationContext& cameraContext = args.getInputContext(kUSDProject3DInputCamera);
        keyHasher.appendBool(cameraContext.includesEverything());
        if (!cameraContext.includesEverything()) {
            keyHasher.appendU64(cameraContext.loadSet->getHash());
        }
    }
    const U64 samplesKey = keyHasher.finalize();

    std::shared_ptr<const CameraSamples> samples;
    {
        std::lock_guard<std::mutex> l(_imp->lock);
        if (_imp->samplesValid && (_imp->samplesKey == samplesKey)) {
            samples = _imp->samples;
        }
    }
    if (!cameraPath.empty() && !(PXR_NS::SdfPath::IsValidPathString(cameraPath) && PXR_NS::SdfPath(cameraPath).IsAbsolutePath() && PXR_NS::SdfPath(cameraPath).IsPrimPath())) {
        *errorMessage = tr("Camera Path \"%1\" is not an absolute prim path such as /World/Camera.").arg(QString::fromStdString(cameraPath)).toStdString();

        return fail();
    }
    if (!samples) {
        const auto missingSamples = [&layerKey, &cameraPath, aspect, fitValue, useFrameHold, frameHold]() {
            std::shared_ptr<CameraSamples> missing = std::make_shared<CameraSamples>();
            missing->missing = true;
            missing->worldToProjector.push_back(PXR_NS::GfMatrix4d(0.));
            missing->origin.push_back(PXR_NS::GfVec4f(0.f, 0.f, 0.f, 1.f));
            if (useFrameHold) {
                missing->deferredFrames.push_back(frameHold);
            } else {
                SceneLayerEdit range;
                range.firstFrame = layerKey.firstFrame;
                range.lastFrame = layerKey.lastFrame;
                missing->deferredFrames = SceneNodeAuthoring::getSampleFrames(range);
            }
            ContentHasher digest;
            digest.appendBool(true);
            digest.appendString(cameraPath);
            digest.appendDouble(aspect);
            digest.appendI64(fitValue);
            digest.appendDouble(layerKey.framesPerSecond);
            digest.appendU64(missing->deferredFrames.size());
            for (double frame : missing->deferredFrames) {
                digest.appendDouble(frame);
            }
            missing->digest = digest.finalize();

            return missing;
        };
        std::shared_ptr<CameraSamples> computed;
        if (cameraScene.isEmpty()) {
            if (cameraPath.empty()) {
                *errorMessage = tr("No camera to project through: connect one to the Camera input.").toStdString();

                return fail();
            }
            computed = missingSamples();
        } else {
            // A named camera needs only the payloads on its path; the first camera is searched for
            // in the whole scene as the camera input loads it.
            if (cameraPath.empty() && args.context.reach) {
                args.context.reach->add(std::vector<std::string>(1, "/"));
            }
            const SceneLoadSetPtr cameraLoadSet = cameraPath.empty() ? args.getInputContext(kUSDProject3DInputCamera).loadSet
                                                                     : SceneLoadSet::withPaths(args.getInputContext(kUSDProject3DInputCamera).loadSet, std::vector<std::string>(1, cameraPath));
            SceneQueryPool::Lease lease = SceneQueryPool::instance().borrow(cameraScene, cameraLoadSet, errorMessage);
            const PXR_NS::UsdStageRefPtr stage = lease.getStage();
            if (!stage) {
                if (errorMessage->empty()) {
                    *errorMessage = tr("USD could not compose the camera scene.").toStdString();
                }

                return fail();
            }
            if (cameraPath.empty()) {
                const std::vector<PXR_NS::SdfPath> cameras = HydraRender::cameraPaths(stage);
                if (cameras.empty()) {
                    *errorMessage = tr("The Camera input has no camera to project through.").toStdString();

                    return fail();
                }
                cameraPath = cameras.front().GetString();
            }
            const PXR_NS::SdfPath path(cameraPath);
            const PXR_NS::UsdGeomCamera camera(stage->GetPrimAtPath(path));
            if (!camera) {
                computed = missingSamples();
            } else {
                const double fps = layerKey.framesPerSecond;
                SceneLayerEdit range;
                range.firstFrame = layerKey.firstFrame;
                range.lastFrame = layerKey.lastFrame;
                computed = std::make_shared<CameraSamples>();
                // Frames empty means one sample, authored as the default value.
                std::vector<double> readFrames;
                if (useFrameHold) {
                    readFrames.push_back(frameHold);
                } else if (SceneProjectionCameras::cameraMightVary(camera)) {
                    computed->frames = SceneNodeAuthoring::getSampleFrames(range);
                    readFrames = computed->frames;
                } else {
                    readFrames.push_back(range.firstFrame);
                }
                ContentHasher digest;
                digest.appendU64(readFrames.size());
                for (double frame : readFrames) {
                    const PXR_NS::UsdTimeCode time(SceneOps::frameToTimeCode(cameraScene, frame, fps));
                    PXR_NS::GfMatrix4d matrix;
                    PXR_NS::GfVec4f origin;
                    if (!SceneProjectionCameras::computeProjector(stage, path, time, aspect, fit, &matrix, &origin, errorMessage)) {
                        return fail();
                    }
                    computed->worldToProjector.push_back(matrix);
                    computed->origin.push_back(origin);
                    digest.appendDouble(frame);
                    digest.appendBytes(matrix.data(), 16 * sizeof(double));
                    digest.appendBytes(origin.data(), 4 * sizeof(float));
                }
                digest.appendU64(computed->frames.size());
                computed->digest = digest.finalize();
            }
            lease.release();
        }

        samples = computed;
        std::lock_guard<std::mutex> l(_imp->lock);
        _imp->samplesValid = true;
        _imp->samplesKey = samplesKey;
        _imp->samples = samples;
    }

    // The image's identity and hash, without rendering it: the layer reads nothing else of it.
    LiveTextureSource plate;
    const bool hasPlate = SceneNodeAuthoring::getLiveTextureSource(*this, kUSDProject3DInputImage, kLiveTextureLayer, &plate);
    ContentHasher facts;
    facts.appendU64(samples->digest);
    facts.appendString(shaderPath);
    facts.appendU64((U64)space);
    facts.appendBool(useFrameHold);
    if (useFrameHold) {
        facts.appendDouble(frameHold);
    }
    facts.appendBool(hasPlate);
    if (hasPlate) {
        SceneNodeAuthoring::appendLiveTextureSource(plate, &facts);
        // The texture's window falls back on the project format for an image without one.
        Format projectFormat;
        getApp()->getProject()->getProjectDefaultFormat(&projectFormat);
        for (int side : { projectFormat.x1, projectFormat.y1, projectFormat.x2, projectFormat.y2 }) {
            facts.appendI64(side);
        }
    }
    SceneLayerReads reads;
    reads.upstream = SceneLayerMemo::Key::eUpstreamReadFacts;
    reads.factsDigest = facts.finalize();
    reads.frameRange = !samples->frames.empty() || (hasPlate && plate.varying);

    PXR_NS::SdfPath path;
    if (!SceneNodeAuthoring::getPrimPath(primPathKnob, &path, errorMessage)) {
        return fail();
    }
    const double framesPerSecond = layerKey.framesPerSecond;
    SceneLayerMemo::AuthorFunc author = [samples, path, shaderPath, space, useFrameHold, frameHold, projectOn, crop, clip, outside, hasPlate, plate, cameraPath, aspect, fitValue,
                                         framesPerSecond](const SceneLayerEdit& edit, std::string* error) {
        PXR_NS::UsdStageRefPtr stage = SceneNodeAuthoring::openLayerStage(edit, error);
        if (!stage) {
            return false;
        }
        const QString pathString = QString::fromStdString(path.GetString());
        const PXR_NS::UsdShadeMaterial material = PXR_NS::UsdShadeMaterial::Define(stage, path);
        PXR_NS::UsdShadeShader shader = PXR_NS::UsdShadeShader::Define(stage, path.AppendChild(PXR_NS::TfToken(kUSDProject3DShaderName)));
        if (!material || !shader || !shader.SetSourceAsset(PXR_NS::SdfAssetPath(shaderPath), PXR_NS::TfToken("glslfx"))) {
            *error = tr("USD could not author the projection material at %1.").arg(pathString).toStdString();

            return false;
        }
        const PXR_NS::UsdShadeInput matrix = shader.CreateInput(PXR_NS::TfToken(kUSDProject3DInputWorldToProjector), PXR_NS::SdfValueTypeNames->Matrix4d);
        const PXR_NS::UsdShadeInput origin = shader.CreateInput(PXR_NS::TfToken(kUSDProject3DInputProjectorOrigin), PXR_NS::SdfValueTypeNames->Float4);
        bool ok = matrix && origin;
        if (ok && samples->frames.empty()) {
            ok = matrix.Set(samples->worldToProjector.front()) && origin.Set(samples->origin.front());
        }
        for (std::size_t i = 0; ok && (i < samples->frames.size()); ++i) {
            const PXR_NS::UsdTimeCode time = SceneNodeAuthoring::frameToTimeCode(edit, samples->frames[i]);
            ok = matrix.Set(samples->worldToProjector[i], time) && origin.Set(samples->origin[i], time);
        }
        if (ok && samples->missing) {
            const PXR_NS::UsdPrim prim = shader.GetPrim();
            const PXR_NS::UsdAttribute camera = prim.CreateAttribute(PXR_NS::TfToken(kUSDProject3DAttrCamera), PXR_NS::SdfValueTypeNames->String, true);
            const PXR_NS::UsdAttribute cameraAspect = prim.CreateAttribute(PXR_NS::TfToken(kUSDProject3DAttrCameraAspect), PXR_NS::SdfValueTypeNames->Double, true);
            const PXR_NS::UsdAttribute cameraFit = prim.CreateAttribute(PXR_NS::TfToken(kUSDProject3DAttrCameraFit), PXR_NS::SdfValueTypeNames->Int, true);
            const PXR_NS::UsdAttribute cameraFrames = prim.CreateAttribute(PXR_NS::TfToken(kUSDProject3DAttrCameraFrames), PXR_NS::SdfValueTypeNames->DoubleArray, true);
            const PXR_NS::UsdAttribute cameraRate = prim.CreateAttribute(PXR_NS::TfToken(kUSDProject3DAttrCameraFramesPerSecond), PXR_NS::SdfValueTypeNames->Double, true);
            const PXR_NS::VtDoubleArray frames(samples->deferredFrames.begin(), samples->deferredFrames.end());
            ok = camera && camera.Set(cameraPath) && cameraAspect && cameraAspect.Set(aspect) && cameraFit && cameraFit.Set(fitValue) && cameraFrames && cameraFrames.Set(frames)
                && cameraRate && cameraRate.Set(framesPerSecond);
            if (ok && edit.deferredProjections) {
                *edit.deferredProjections = true;
            }
        }
        // Read by the geometry node the material is grafted under, which takes itself at the same frame.
        if (ok && (space == (int)eProjectionSpaceGeometry) && useFrameHold) {
            const PXR_NS::UsdAttribute hold = shader.GetPrim().CreateAttribute(PXR_NS::TfToken(kUSDProject3DAttrFrameHold), PXR_NS::SdfValueTypeNames->Double, true);
            ok = hold && hold.Set(frameHold);
        }
        const std::pair<const char*, int> ints[] = {
            { kUSDProject3DInputProjectionSpace, space },
            { "projectOn", projectOn },
            { "crop", crop ? 1 : 0 },
            { "clip", clip ? 1 : 0 },
            { "outside", outside },
        };
        for (const std::pair<const char*, int>& value : ints) {
            const PXR_NS::UsdShadeInput input = ok ? shader.CreateInput(PXR_NS::TfToken(value.first), PXR_NS::SdfValueTypeNames->Int) : PXR_NS::UsdShadeInput();
            ok = input && input.Set(value.second);
        }
        const PXR_NS::UsdShadeOutput surface = ok ? shader.CreateOutput(PXR_NS::TfToken("surface"), PXR_NS::SdfValueTypeNames->Token) : PXR_NS::UsdShadeOutput();
        ok = surface && material.CreateSurfaceOutput().ConnectToSource(surface);
        if (!ok) {
            *error = tr("USD could not author the projection shader of the material at %1.").arg(pathString).toStdString();

            return false;
        }

        // The plate, always a texture: the shader samples it at its own coordinates, which Storm
        // generates only for a texture input. Its st is never read but must be connected. Without
        // an image the texture has no file and shows its fallback, black.
        PXR_NS::UsdShadeShader reader = PXR_NS::UsdShadeShader::Define(stage, path.AppendChild(PXR_NS::TfToken(kUSDProject3DReaderName)));
        PXR_NS::UsdShadeShader texture = PXR_NS::UsdShadeShader::Define(stage, path.AppendChild(PXR_NS::TfToken(kUSDProject3DTextureName)));
        const PXR_NS::UsdShadeInput varname = reader ? reader.CreateInput(PXR_NS::TfToken("varname"), PXR_NS::SdfValueTypeNames->String) : PXR_NS::UsdShadeInput();
        const PXR_NS::UsdShadeOutput readerOutput = reader ? reader.CreateOutput(PXR_NS::TfToken("result"), PXR_NS::SdfValueTypeNames->Float2) : PXR_NS::UsdShadeOutput();
        ok = reader && reader.CreateIdAttr(PXR_NS::VtValue(PXR_NS::TfToken("UsdPrimvarReader_float2"))) && varname && varname.Set(std::string("st")) && readerOutput
            && texture && texture.CreateIdAttr(PXR_NS::VtValue(PXR_NS::TfToken("UsdUVTexture")));
        const PXR_NS::UsdShadeInput st = ok ? texture.CreateInput(PXR_NS::TfToken("st"), PXR_NS::SdfValueTypeNames->Float2) : PXR_NS::UsdShadeInput();
        ok = st && st.ConnectToSource(readerOutput);
        // The texture covers the image's data window, beyond which the image is transparent black:
        // clamping would stretch its edge pixels across the rest of the frame.
        const PXR_NS::TfToken wrap("black");
        for (const char* name : { "wrapS", "wrapT" }) {
            const PXR_NS::UsdShadeInput input = ok ? texture.CreateInput(PXR_NS::TfToken(name), PXR_NS::SdfValueTypeNames->Token) : PXR_NS::UsdShadeInput();
            ok = input && input.Set(wrap);
        }
        const PXR_NS::UsdShadeInput colorSpace = ok ? texture.CreateInput(PXR_NS::TfToken("sourceColorSpace"), PXR_NS::SdfValueTypeNames->Token) : PXR_NS::UsdShadeInput();
        ok = colorSpace && colorSpace.Set(PXR_NS::TfToken("auto"));
        // The texture holds the image's window, the format and the data beyond it; plateTransform
        // maps the format inside it, so overscan projects when Crop is off.
        const PXR_NS::UsdShadeInput window = ok ? shader.CreateInput(PXR_NS::TfToken(kUSDProject3DInputPlateTransform), PXR_NS::SdfValueTypeNames->Float4) : PXR_NS::UsdShadeInput();
        ok = bool(window);
        if (ok && hasPlate) {
            const LiveTextureSource live = SceneNodeAuthoring::recordLiveTexture(edit, plate);
            const PXR_NS::UsdShadeInput file = texture.CreateInput(PXR_NS::TfToken("file"), PXR_NS::SdfValueTypeNames->Asset);
            ok = file && SceneNodeAuthoring::authorValue(file.GetAttr(), edit, live.varying, [&live](double frame) { return PXR_NS::SdfAssetPath(live.getUri(frame)); });
            ok = ok && SceneNodeAuthoring::authorValue(window.GetAttr(), edit, SceneNodeAuthoring::liveTextureWindowVaries(live), [&live](double frame) {
                     double scale[2];
                     double translation[2];
                     live.getWindow(frame).getUvTransform(scale, translation);

                     return PXR_NS::GfVec4f((float)scale[0], (float)scale[1], (float)translation[0], (float)translation[1]);
                 });
        } else if (ok) {
            ok = window.Set(PXR_NS::GfVec4f(1.f, 1.f, 0.f, 0.f));
        }
        const PXR_NS::UsdShadeOutput rgba = ok ? texture.CreateOutput(PXR_NS::TfToken("rgba"), PXR_NS::SdfValueTypeNames->Float4) : PXR_NS::UsdShadeOutput();
        const PXR_NS::UsdShadeInput plateInput = rgba ? shader.CreateInput(PXR_NS::TfToken("plate"), PXR_NS::SdfValueTypeNames->Float4) : PXR_NS::UsdShadeInput();
        if (!plateInput || !plateInput.ConnectToSource(rgba)) {
            *error = tr("USD could not author the image texture of the material at %1.").arg(pathString).toStdString();

            return false;
        }

        return true;
    };
    if (!emitSceneLayer(ScenePayload(), reads, author, outputScene, errorMessage)) {
        return fail();
    }

    // Checked on every evaluation: the layer can stay the same while the texture fails.
    QStringList warnings;
    if (!hasPlate) {
        warnings.push_back(tr("Nothing is connected to the Image input: the projection is black."));
    } else {
        const std::string uri = plate.getUri(args.time);
        const std::string failure = LiveTextureStore::instance().getFailure(uri);
        if (!failure.empty()) {
            warnings.push_back(tr("The live texture %1 shows the renderer's fallback: %2.").arg(QString::fromStdString(uri)).arg(QString::fromStdString(failure)));
        }
    }
    if (warnings.isEmpty()) {
        clearPersistentMessage(false);
    } else {
        setPersistentMessage(eMessageTypeWarning, warnings.join(QLatin1Char('\n')).toStdString());
    }

    return eStatusOK;
} // USDProject3D::renderScene

NATRON_NAMESPACE_EXIT
