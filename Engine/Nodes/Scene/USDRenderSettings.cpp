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

#include "USDRenderSettings.h"

#include <string>
#include <vector>

#include <QMetaObject>

#include <pxr/imaging/hd/tokens.h>

#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/ContentHasher.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Scene/CyclesAovs.h"
#include "Engine/Nodes/Scene/RenderDelegates.h"
#include "Engine/Nodes/Scene/RenderSettingsKnobs.h"
#include "Engine/Nodes/Scene/RenderSettingsPrim.h"
#include "Engine/Nodes/Scene/SceneNodeAuthoring.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"

#include "Serialization/KnobSerialization.h"
#include "Serialization/NodeSerialization.h"

NATRON_NAMESPACE_ENTER

USDRenderSettings::USDRenderSettings(NodePtr node)
    : NativeEffectBase(node)
    , _cameraMenuRefreshPending(false)
{
    _cameraMenuRefresh.setRerun([this]() { return refreshCameraMenu(); });
}

USDRenderSettings::~USDRenderSettings()
{
}

const NodeSpec*
USDRenderSettings::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

void
USDRenderSettings::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    _primPath = createScenePrimPathKnob(this, page, kUSDRenderSettingsDefaultPrimPath, tr("The path of the RenderSettings prim the node authors."));

    KnobChoicePtr camera = createKnob<KnobChoice>(tr("Camera"));
    camera->setName(kUSDRenderSettingsParamCamera);
    camera->setHintToolTip(tr("The camera to render through, one of the input scene's cameras, authored as the prim's "
                              "camera relationship. A chosen camera the scene no longer has stays chosen."));
    camera->setAnimationEnabled(false);
    page->addKnob(camera);
    _camera = camera;

    const ExtentKnobs::FormatKnobSet formatKnobs = ExtentKnobs::createFormatKnobs(this, page);
    _formatSize = formatKnobs.size;
    _formatPar = formatKnobs.par;

    KnobChoicePtr conformPolicy = createKnob<KnobChoice>(tr("Conform Policy"));
    conformPolicy->setName(kUSDRenderSettingsParamConformPolicy);
    conformPolicy->setHintToolTip(tr("How the camera's aperture is conformed to the format when their aspect ratios "
                                     "differ (aspectRatioConformPolicy)."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(PXR_NS::UsdRenderTokens->expandAperture.GetString(), tr("Expand Aperture").toStdString(),
                                       tr("The aperture grows in one direction to take the format's shape: everything the camera "
                                          "saw stays in view.")
                                           .toStdString()));
        options.push_back(ChoiceOption(PXR_NS::UsdRenderTokens->cropAperture.GetString(), tr("Crop Aperture").toStdString(),
                                       tr("The aperture shrinks in one direction to take the format's shape: what does not fit is "
                                          "cropped.")
                                           .toStdString()));
        options.push_back(ChoiceOption(PXR_NS::UsdRenderTokens->adjustApertureWidth.GetString(), tr("Adjust Aperture Width").toStdString(),
                                       tr("The aperture's height is kept and its width set to the format's shape.").toStdString()));
        options.push_back(ChoiceOption(PXR_NS::UsdRenderTokens->adjustApertureHeight.GetString(), tr("Adjust Aperture Height").toStdString(),
                                       tr("The aperture's width is kept and its height set to the format's shape.").toStdString()));
        options.push_back(ChoiceOption(PXR_NS::UsdRenderTokens->adjustPixelAspectRatio.GetString(), tr("Adjust Pixel Aspect Ratio").toStdString(),
                                       tr("The aperture is kept and the pixel aspect ratio changed so that it fills the "
                                          "resolution.")
                                           .toStdString()));
        conformPolicy->populateChoices(options);
    }
    conformPolicy->setDefaultValue(0);
    conformPolicy->setAnimationEnabled(false);
    page->addKnob(conformPolicy);
    _conformPolicy = conformPolicy;

    KnobDoublePtr dataWindow = createKnob<KnobDouble>(tr("Data Window"), 4);
    dataWindow->setName(kUSDRenderSettingsParamDataWindow);
    dataWindow->setHintToolTip(tr("The part of the image to render, as xmin, ymin, xmax and ymax in normalized device "
                                  "coordinates of the format: (0, 0) is its bottom left corner and (1, 1) its top right "
                                  "(dataWindowNDC). Values outside 0 to 1 render overscan."));
    dataWindow->setDefaultValue(0., 0);
    dataWindow->setDefaultValue(0., 1);
    dataWindow->setDefaultValue(1., 2);
    dataWindow->setDefaultValue(1., 3);
    dataWindow->setDisplayMinimum(0.);
    dataWindow->setDisplayMaximum(1.);
    dataWindow->setAnimationEnabled(false);
    page->addKnob(dataWindow);
    _dataWindow = dataWindow;

    KnobBoolPtr disableMotionBlur = createKnob<KnobBool>(tr("Disable Motion Blur"));
    disableMotionBlur->setName(kUSDRenderSettingsParamDisableMotionBlur);
    disableMotionBlur->setHintToolTip(tr("Renders without motion blur, as if every shutter were instantaneous "
                                         "(disableMotionBlur)."));
    disableMotionBlur->setDefaultValue(false);
    disableMotionBlur->setAnimationEnabled(false);
    page->addKnob(disableMotionBlur);
    _disableMotionBlur = disableMotionBlur;

    KnobBoolPtr disableDepthOfField = createKnob<KnobBool>(tr("Disable Depth Of Field"));
    disableDepthOfField->setName(kUSDRenderSettingsParamDisableDepthOfField);
    disableDepthOfField->setHintToolTip(tr("Renders every camera as a pinhole, everything in focus "
                                           "(disableDepthOfField)."));
    disableDepthOfField->setDefaultValue(false);
    disableDepthOfField->setAnimationEnabled(false);
    page->addKnob(disableDepthOfField);
    _disableDepthOfField = disableDepthOfField;

    KnobBoolPtr transparentBackground = createKnob<KnobBool>(tr("Transparent Background"));
    transparentBackground->setName(kUSDRenderSettingsParamTransparentBackground);
    transparentBackground->setHintToolTip(tr("Hides the dome light and the world background from the camera, so the "
                                             "background renders with zero alpha; the dome still lights the scene. "
                                             "Authored as cycles:background:transparent for Cycles and "
                                             "storm:domeLightCameraVisibility for Storm. Off authors neither, and each "
                                             "renderer keeps its own default."));
    transparentBackground->setDefaultValue(false);
    transparentBackground->setAnimationEnabled(false);
    page->addKnob(transparentBackground);
    _transparentBackground = transparentBackground;

    KnobChoicePtr defaultRenderer = createKnob<KnobChoice>(tr("Default Renderer"));
    defaultRenderer->setName(kUSDRenderSettingsParamDefaultRenderer);
    defaultRenderer->setHintToolTip(tr("The Hydra render delegate that renders the prim unless a USDRender or the viewer "
                                       "chooses another, authored as " kRenderSettingsPrimRendererAttribute ". A renderer "
                                       "that is not installed or cannot run here stays chosen."));
    {
        std::vector<ChoiceOption> options;
        int stormIndex = 0;
        for (const RenderDelegates::Delegate& delegate : RenderDelegates::list()) {
            if (RenderDelegates::isStorm(delegate.id)) {
                stormIndex = (int)options.size();
            }
            if (delegate.available) {
                options.push_back(ChoiceOption(delegate.id, delegate.displayName, ""));
            } else {
                options.push_back(ChoiceOption(delegate.id,
                                               tr("%1 (unavailable)").arg(QString::fromStdString(delegate.displayName)).toStdString(),
                                               delegate.error));
            }
        }
        defaultRenderer->populateChoices(options);
        defaultRenderer->setDefaultValue(stormIndex);
    }
    defaultRenderer->setAnimationEnabled(false);
    page->addKnob(defaultRenderer);
    _defaultRenderer = defaultRenderer;

    KnobBoolPtr makeActive = createKnob<KnobBool>(tr("Make Active"));
    makeActive->setName(kUSDRenderSettingsParamMakeActive);
    makeActive->setHintToolTip(tr("Makes the prim the scene's render settings (the stage's renderSettingsPrimPath), "
                                  "which USDRender renders with unless it is told otherwise. Off, the input scene's "
                                  "render settings stay active."));
    makeActive->setDefaultValue(true);
    makeActive->setAnimationEnabled(false);
    page->addKnob(makeActive);
    _makeActive = makeActive;

    _settings.reset(new RenderSettingsKnobs(this));
    for (const std::string& id : RenderSettingsKnobs::rendererIds()) {
        _settings->addRenderer(id, RenderSettingsKnobs::descriptors(id), RenderSettingsKnobs::curatedFor(id));
    }
} // USDRenderSettings::initializeKnobs

void
USDRenderSettings::onKnobsAboutToBeLoaded(const Serialization::NodeSerializationPtr& serialization)
{
    const NodePtr node = getNode();

    _unmatchedKnobs.clear();
    for (const Serialization::KnobSerialization& saved : serialization->knobs) {
        if (!node->getKnobByName(NATRON_PYTHON_NAMESPACE::makeNameScriptFriendly(saved.name))) {
            _unmatchedKnobs.push_back(saved);
        }
    }
}

void
USDRenderSettings::appendUnmatchedKnobs(Serialization::NodeSerialization* serialization) const
{
    serialization->knobs.insert(serialization->knobs.end(), _unmatchedKnobs.begin(), _unmatchedKnobs.end());
}

void
USDRenderSettings::onKnobsLoaded()
{
    NativeEffectBase::onKnobsLoaded();
    if (_settings) {
        _settings->refreshEnabled();
    }
    keepLoadedRenderer();
    scheduleCameraMenuRefresh();
}

bool
USDRenderSettings::knobChanged(KnobI* k,
                               ValueChangedReasonEnum /*reason*/,
                               ViewSpec /*view*/,
                               double /*time*/,
                               bool /*originatedFromMainThread*/)
{
    KnobChoicePtr camera = _camera.lock();
    if (camera && (k == camera.get())) {
        return true;
    }

    return _settings && _settings->knobChanged(k);
}

void
USDRenderSettings::keepLoadedRenderer()
{
    KnobChoicePtr renderer = _defaultRenderer.lock();

    if (!renderer) {
        return;
    }
    // Loading an id the menu lacks leaves the index on the default; the menu gains the id so that
    // the knob, its hash and the next save all name it.
    const ChoiceOption active = renderer->getActiveEntry();
    if (active.id.empty() || renderer->isActiveEntryPresentInEntries()) {
        return;
    }
    renderer->appendChoice(ChoiceOption(active.id,
                                        tr("%1 (not installed)").arg(QString::fromStdString(active.id)).toStdString(),
                                        RenderDelegates::unavailableMessage(active.id)));
    renderer->setValue(renderer->getNumEntries() - 1);
}

void
USDRenderSettings::onInputChanged(int inputNo)
{
    NativeEffectBase::onInputChanged(inputNo);
    scheduleCameraMenuRefresh();
}

void
USDRenderSettings::onNodeHashChanged(U64 hash)
{
    NativeEffectBase::onNodeHashChanged(hash);
    scheduleCameraMenuRefresh();
}

void
USDRenderSettings::scheduleCameraMenuRefresh()
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
USDRenderSettings::refreshCameraMenu()
{
    bool noViewer = false;
    const SceneEvaluationContext context = _cameraMenuRefresh.getContext(this, &noViewer);
    const EffectInstancePtr input = getInput(0);
    const double time = getCurrentTime();
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
        for (const PXR_NS::UsdPrim& prim : lease.getStage()->Traverse()) {
            if (job.isCancelled()) {
                return;
            }
            if (prim.IsA<PXR_NS::UsdGeomCamera>()) {
                result->lists[0].push_back(prim.GetPath().GetString());
            }
        }
        result->unsearched = SceneNodeAuthoring::countMenuUnsearched(lease.getStage(), context);
    };

    return _cameraMenuRefresh.start(this, context, noViewer, work, [this](const std::shared_ptr<SceneMenuJob>& job) { applyCameraMenu(job); });
}

bool
USDRenderSettings::applyCameraMenu(const std::shared_ptr<SceneMenuJob>& job)
{
    if (!_cameraMenuRefresh.take(job)) {
        return false;
    }
    KnobChoicePtr camera = _camera.lock();
    if (!camera) {
        return false;
    }
    const SceneMenuJob::Result result = job->getResult();
    std::vector<ChoiceOption> options;
    if (!result.lists.empty()) {
        for (const std::string& path : result.lists[0]) {
            options.push_back(ChoiceOption(path, path, ""));
        }
    }
    if (result.unsearched > 0) {
        options.push_back(makeSceneUnsearchedOption(result.unsearched, job->hasNoViewer()));
    }
    // Repopulating an unchanged menu would move the hash and come back here.
    const std::vector<ChoiceOption> current = camera->getEntries_mt_safe();
    bool same = current.size() == options.size();
    for (std::size_t i = 0; same && (i < options.size()); ++i) {
        same = (current[i].id == options[i].id) && (current[i].label == options[i].label);
    }
    if (!same) {
        camera->populateChoices(options);
    }
    // A menu holding only the note lands on it; once cameras are listed the first is chosen, as
    // it is when nothing was chosen.
    if (!options.empty() && !isSceneUnsearchedOption(options[0].id) && isSceneUnsearchedOption(camera->getActiveEntry().id)) {
        camera->setValue(0);
    }

    return true;
}

bool
USDRenderSettings::authorLayer(const SceneLayerEdit& edit,
                               const std::string& primPath,
                               std::string* errorMessage) const
{
    PXR_NS::UsdStageRefPtr stage = SceneNodeAuthoring::openLayerStage(edit, errorMessage);

    if (!stage) {
        if (errorMessage->empty()) {
            *errorMessage = tr("USD could not open the node's layer.").toStdString();
        }

        return false;
    }
    const PXR_NS::SdfPath path(primPath);
    PXR_NS::UsdRenderSettings settings = PXR_NS::UsdRenderSettings::Define(stage, path);
    if (!settings) {
        *errorMessage = tr("USD could not define a RenderSettings prim at %1.").arg(QString::fromStdString(primPath)).toStdString();

        return false;
    }
    KnobChoicePtr camera = _camera.lock();
    KnobIntPtr size = _formatSize.lock();
    KnobDoublePtr par = _formatPar.lock();
    KnobChoicePtr conformPolicy = _conformPolicy.lock();
    KnobDoublePtr dataWindow = _dataWindow.lock();
    KnobBoolPtr disableMotionBlur = _disableMotionBlur.lock();
    KnobBoolPtr disableDepthOfField = _disableDepthOfField.lock();
    KnobBoolPtr transparentBackground = _transparentBackground.lock();
    KnobChoicePtr renderer = _defaultRenderer.lock();
    KnobBoolPtr makeActive = _makeActive.lock();
    if (!camera || !size || !par || !conformPolicy || !dataWindow || !disableMotionBlur || !disableDepthOfField || !transparentBackground || !renderer || !makeActive) {
        *errorMessage = tr("The node's knobs are gone.").toStdString();

        return false;
    }

    // Every UsdRenderSettingsBase attribute is uniform: a default value, never time samples.
    bool ok = true;
    std::string cameraPath = camera->getActiveEntry().id;
    if (isSceneUnsearchedOption(cameraPath)) {
        cameraPath.clear();
    }
    if (!cameraPath.empty()) {
        if (!PXR_NS::SdfPath::IsValidPathString(cameraPath) || !PXR_NS::SdfPath(cameraPath).IsAbsolutePath()) {
            *errorMessage = tr("The camera %1 is not an absolute prim path.").arg(QString::fromStdString(cameraPath)).toStdString();

            return false;
        }
        ok = settings.CreateCameraRel().SetTargets(PXR_NS::SdfPathVector { PXR_NS::SdfPath(cameraPath) });
    }
    ok = ok && settings.CreateResolutionAttr().Set(PXR_NS::GfVec2i(size->getValue(0), size->getValue(1)));
    ok = ok && settings.CreatePixelAspectRatioAttr().Set((float)par->getValue());
    const std::string policy = conformPolicy->getActiveEntry().id;
    ok = ok && settings.CreateAspectRatioConformPolicyAttr().Set(policy.empty() ? PXR_NS::UsdRenderTokens->expandAperture : PXR_NS::TfToken(policy));
    ok = ok && settings.CreateDataWindowNDCAttr().Set(PXR_NS::GfVec4f((float)dataWindow->getValue(0), (float)dataWindow->getValue(1), (float)dataWindow->getValue(2), (float)dataWindow->getValue(3)));
    ok = ok && settings.CreateDisableMotionBlurAttr().Set(disableMotionBlur->getValue());
    ok = ok && settings.CreateDisableDepthOfFieldAttr().Set(disableDepthOfField->getValue());
    const std::string rendererId = renderer->getActiveEntry().id;
    if (ok && !rendererId.empty()) {
        const PXR_NS::UsdAttribute attribute = settings.GetPrim().CreateAttribute(PXR_NS::TfToken(kRenderSettingsPrimRendererAttribute),
                                                                                  PXR_NS::SdfValueTypeNames->Token, true,
                                                                                  PXR_NS::SdfVariabilityUniform);
        ok = attribute && attribute.Set(PXR_NS::TfToken(rendererId));
    }
    if (!ok) {
        *errorMessage = tr("USD could not author the RenderSettings prim at %1.").arg(QString::fromStdString(primPath)).toStdString();

        return false;
    }
    if (_settings && !_settings->author(settings.GetPrim(), errorMessage)) {
        return false;
    }
    // After the renderer pages, so that the switch wins over a Cycles page that authors the same attribute.
    if (transparentBackground->getValue()) {
        const struct {
            const char* rendererId;
            std::string key;
            bool value;
        } hidden[] = {
            { kRenderDelegateCyclesId, "cycles:background:transparent", true },
            { kRenderDelegateStormId, PXR_NS::HdRenderSettingsTokens->domeLightCameraVisibility.GetString(), false },
        };
        for (const auto& h : hidden) {
            const std::string name = RenderSettingsPrim::attributeName(h.rendererId, h.key);
            const PXR_NS::UsdAttribute attribute = settings.GetPrim().CreateAttribute(PXR_NS::TfToken(name), PXR_NS::SdfValueTypeNames->Bool, true);
            if (!attribute || !attribute.Set(h.value)) {
                *errorMessage = tr("USD could not author %1 on %2.").arg(QString::fromStdString(name), QString::fromStdString(primPath)).toStdString();

                return false;
            }
        }
    }
    if (makeActive->getValue()) {
        stage->GetRootLayer()->SetField(PXR_NS::SdfPath::AbsoluteRootPath(), PXR_NS::UsdRenderTokens->renderSettingsPrimPath, primPath);
    }

    return true;
} // USDRenderSettings::authorLayer

bool
USDRenderSettings::getHashNamespaceScope(std::string* path,
                                         std::string* defaultRenderer,
                                         bool* active) const
{
    KnobStringPtr primPath = _primPath.lock();
    KnobChoicePtr renderer = _defaultRenderer.lock();
    KnobBoolPtr makeActive = _makeActive.lock();
    NodePtr node = getNode();

    // A disabled node authors nothing: its scope would hide what upstream makes active.
    if (!primPath || !renderer || !makeActive || !node || node->isNodeDisabled()) {
        return false;
    }
    *path = primPath->getValue();
    if (!PXR_NS::SdfPath::IsValidPathString(*path)) {
        return false;
    }
    *path = PXR_NS::SdfPath(*path).GetString();
    *defaultRenderer = renderer->getActiveEntry().id;
    *active = makeActive->getValue();

    return true;
}

StatusEnum
USDRenderSettings::renderScene(const SceneRenderActionArgs& args,
                               ScenePayload* outputScene,
                               std::string* errorMessage)
{
    const ScenePayload upstream = args.getInputScene(0);
    PXR_NS::SdfPath path;

    if (!SceneNodeAuthoring::getPrimPath(_primPath.lock(), &path, errorMessage)) {
        setPersistentMessage(eMessageTypeError, *errorMessage);

        return eStatusFailed;
    }
    const std::string primPath = path.GetString();
    SceneLayerMemo::AuthorFunc author = [this, primPath](const SceneLayerEdit& edit,
                                                         std::string* error) {
        return authorLayer(edit, primPath, error);
    };

    // Of the input, the layer reads only the stage metadata, and no frame: nothing is sampled.
    ContentHasher facts;
    SceneNodeAuthoring::appendUpstreamMetadata(upstream, &facts);
    SceneLayerReads reads;
    reads.upstream = SceneLayerMemo::Key::eUpstreamReadFacts;
    reads.factsDigest = facts.finalize();
    reads.frameRange = false;
    if (!emitSceneLayer(upstream, reads, author, outputScene, errorMessage)) {
        setPersistentMessage(eMessageTypeError, *errorMessage);

        return eStatusFailed;
    }
    clearPersistentMessage(false);

    return eStatusOK;
} // USDRenderSettings::renderScene

NATRON_NAMESPACE_EXIT
