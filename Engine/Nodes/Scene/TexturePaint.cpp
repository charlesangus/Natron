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

#include "TexturePaint.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <QString>
#include <QStringList>

#include <SequenceParsing.h>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/ContentHasher.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeColorSampler.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/IO/OiioReadSupport.h"
#include "Engine/Nodes/Scene/LiveTextureStore.h"
#include "Engine/Nodes/Scene/MaterialBake.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/SceneRaster.h"
#include "Engine/Nodes/Scene/TexturePaintBaker.h"
#include "Engine/Nodes/Scene/TexturePaintCache.h"
#include "Engine/Nodes/Scene/TexturePaintInteract.h"
#include "Engine/Nodes/Scene/TexturePaintUndo.h"
#include "Engine/Nodes/Scene/TexturePaintView.h"
#include "Engine/Project.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoCurveContext.h"
#include "Engine/RotoDrawableItem.h"
#include "Engine/RotoLayer.h"
#include "Engine/RotoPaintInteract.h"
#include "Engine/ScenePayload.h"
#include "Engine/TimeLine.h"

NATRON_NAMESPACE_ENTER

namespace {
const int kResolutionCount = 4;

static_assert((1024 << (kResolutionCount - 1)) == TexturePaintView::kPaintSpaceSize,
              "Every Buffer Resolution must be the paint space at a mipmap level.");

std::vector<ChoiceOption>
resolutionOptions()
{
    std::vector<ChoiceOption> options;

    for (int i = 0; i < kResolutionCount; ++i) {
        const int size = 1024 << i;
        const std::string id = std::to_string(1 << i) + "K";
        options.push_back(ChoiceOption(id, id, QObject::tr("%1 pixels.").arg(size).toStdString()));
    }

    return options;
}

int
resolutionFromChoice(const KnobChoicePtr& knob,
                     int fallbackIndex)
{
    int index = knob ? knob->getValue() : fallbackIndex;

    if ((index < 0) || (index >= kResolutionCount)) {
        index = fallbackIndex;
    }

    return 1024 << index;
}

// The input scene as a composed stage, held for as long as this is.
struct InputScene {
    ScenePayload scene;
    SceneQueryPool::Lease lease;
    PXR_NS::UsdStageRefPtr stage;
    PXR_NS::UsdTimeCode time = PXR_NS::UsdTimeCode::Default();
};

// Leaves out->stage null when there is no input or its scene is empty.
bool
openInputScene(const EffectInstancePtr& input,
               double time,
               ViewIdx view,
               double fps,
               InputScene* out,
               std::string* error)
{
    if (!input) {
        return true;
    }
    if (input->evaluateScene(EffectInstance::EvaluateSceneArgs(time, view), &out->scene, error) != EffectInstance::eRenderRoIRetCodeOk) {
        if (error->empty()) {
            *error = QObject::tr("The input scene could not be evaluated.").toStdString();
        }

        return false;
    }
    if (out->scene.isEmpty()) {
        return true;
    }
    out->lease = SceneQueryPool::instance().borrow(out->scene, error);
    out->stage = out->lease.getStage();
    if (!out->stage) {
        if (error->empty()) {
            *error = QObject::tr("USD could not compose the input scene.").toStdString();
        }

        return false;
    }
    out->time = PXR_NS::UsdTimeCode(SceneOps::frameToTimeCode(out->scene, time, fps));

    return true;
}

const int kDefaultTileResolutionIndex = 2;
const int kDefaultBufferResolutionIndex = 1;

template <typename K>
std::shared_ptr<K>
makeSessionKnob(const RotoLayerPtr& layer,
                const char* scriptName,
                const QString& label,
                int dimension,
                const QString& hint)
{
    std::shared_ptr<K> knob = std::make_shared<K>((KnobHolder*)NULL, label.toStdString(), dimension, true);

    knob->populate();
    knob->setName(scriptName);
    knob->setHintToolTip(hint.toStdString());
    knob->setAnimationEnabled(false);
    layer->addHostKnob(knob);

    return knob;
}

template <typename K>
std::shared_ptr<K>
sessionKnob(const RotoLayerPtr& layer,
            const char* scriptName)
{
    return layer ? std::dynamic_pointer_cast<K>(layer->getHostKnobByName(scriptName)) : std::shared_ptr<K>();
}

// The purposes, UDIM tiles and first and last frames a bake record ("count;purposes;tiles;first,last") names.
void
parseBakeRecord(const std::string& record,
                std::vector<std::string>* purposes,
                std::vector<int>* tiles,
                std::pair<int, int>* frames = 0)
{
    purposes->clear();
    tiles->clear();
    if (frames) {
        *frames = std::make_pair(1, 1);
    }
    const QStringList fields = QString::fromStdString(record).split(QLatin1Char(';'));
    if (fields.size() != 4) {
        return;
    }
    if (frames) {
        const QStringList range = fields[3].split(QLatin1Char(','));
        bool firstOk = false;
        bool lastOk = false;
        const int first = (range.size() == 2) ? range[0].toInt(&firstOk) : 0;
        const int last = (range.size() == 2) ? range[1].toInt(&lastOk) : 0;
        if (firstOk && lastOk) {
            *frames = std::make_pair(first, last);
        }
    }
    for (const QString& purpose : fields[1].split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        purposes->push_back(purpose.toStdString());
    }
    for (const QString& tile : fields[2].split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        bool ok = false;
        const int number = tile.toInt(&ok);
        if (ok) {
            tiles->push_back(number);
        }
    }
}

// The record of one more bake than previous, of purposes and tiles.
std::string
nextBakeRecord(const std::string& previous,
               const QStringList& purposes,
               const QStringList& tiles,
               int firstFrame,
               int lastFrame)
{
    const QStringList fields = QString::fromStdString(previous).split(QLatin1Char(';'));
    const int count = fields.isEmpty() ? 0 : fields.front().toInt();

    return QString::fromUtf8("%1;%2;%3;%4,%5").arg(count + 1).arg(purposes.join(QLatin1Char(','))).arg(tiles.join(QLatin1Char(','))).arg(firstFrame).arg(lastFrame).toStdString();
}

// Changes whenever the file at path is written, replaced or deleted.
std::int64_t
fileStamp(const std::string& path)
{
    std::error_code ec;
    const std::filesystem::file_time_type modified = std::filesystem::last_write_time(path, ec);

    if (ec) {
        return -1;
    }
    const std::uintmax_t bytes = std::filesystem::file_size(path, ec);

    return (std::int64_t)modified.time_since_epoch().count() ^ (std::int64_t)(ec ? 0 : bytes);
}

// pattern with its frame tokens set to frame; pattern itself when it cannot be parsed.
std::string
expandFrame(const std::string& pattern,
            const std::vector<std::string>& views,
            int frame)
{
    try {
        return SequenceParsing::generateFileNameFromPattern(pattern, views, frame, 0);
    } catch (const std::exception&) {
        return pattern;
    }
}

std::string
planePurpose(const ImageLayerDesc& plane)
{
    return plane.isColorLayer() ? std::string(kTexturePaintPurposeDiffuseColor) : plane.getLayerID();
}
} // anon namespace

TexturePaint::TexturePaint(NodePtr node)
    : NativeEffectBase(node)
    , _sessionRenders(0)
{
}

TexturePaint::~TexturePaint()
{
    if (_baker) {
        _baker->stop();
    }
}

const NodeSpec*
TexturePaint::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

void
TexturePaint::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    _primPattern.create(this, page, kTexturePaintParamPrimPattern, tr("Prim Pattern"),
                        tr("The meshes to paint on; other prims the pattern matches are ignored. All of them share one "
                           "UDIM texture set, so their UV layouts should not overlap."));
    {
        KnobStringPtr knob = _primPattern.getKnob();
        if (knob) {
            knob->setDefaultValue("//*");
        }
    }

    KnobStringPtr uvPrimvar = createKnob<KnobString>(tr("UV Primvar"));
    uvPrimvar->setName(kTexturePaintParamUvPrimvar);
    uvPrimvar->setHintToolTip(tr("The texture coordinate primvar of the meshes, which lays out the UDIM tiles: tile 1001 "
                                 "covers 0 to 1 in u and v, the next tile in u is 1002, and each row of ten up in v adds "
                                 "10.")
                                  .toStdString());
    uvPrimvar->setDefaultValue("st");
    uvPrimvar->setAnimationEnabled(false);
    page->addKnob(uvPrimvar);
    _uvPrimvar = uvPrimvar;

    KnobChoicePtr tileResolution = createKnob<KnobChoice>(tr("Tile Resolution"));
    tileResolution->setName(kTexturePaintParamTileResolution);
    tileResolution->setHintToolTip(tr("The width and height of each UDIM tile of the texture set.").toStdString());
    tileResolution->populateChoices(resolutionOptions());
    tileResolution->setDefaultValue(kDefaultTileResolutionIndex);
    tileResolution->setAnimationEnabled(false);
    page->addKnob(tileResolution);
    _tileResolution = tileResolution;

    // 2K keeps a 1920x1080 view at or above one buffer pixel per screen pixel, so brush detail is
    // kept, while bounding the buffer's memory and the cost of projecting it into the tiles.
    KnobChoicePtr bufferResolution = createKnob<KnobChoice>(tr("Buffer Resolution"));
    bufferResolution->setName(kTexturePaintParamBufferResolution);
    bufferResolution->setHintToolTip(tr("The longer side of the paint buffer strokes are drawn into before they are "
                                        "projected into the tiles, whatever the size of the Viewer. The shorter side "
                                        "follows the view's aspect. A larger buffer keeps finer strokes when zoomed in "
                                        "on the mesh.")
                                         .toStdString());
    bufferResolution->populateChoices(resolutionOptions());
    bufferResolution->setDefaultValue(kDefaultBufferResolutionIndex);
    bufferResolution->setAnimationEnabled(false);
    page->addKnob(bufferResolution);
    _bufferResolution = bufferResolution;

    KnobChoicePtr textureMode = createKnob<KnobChoice>(tr("Texture Mode"));
    textureMode->setName(kTexturePaintParamTextureMode);
    textureMode->setHintToolTip(tr("Whether the texture is one image for every frame or changes over time.").toStdString());
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("static", tr("Static").toStdString(),
                                       tr("One texture set for every frame: strokes painted at any frame land in the "
                                          "same tiles.")
                                           .toStdString()));
        options.push_back(ChoiceOption("animated", tr("Animated").toStdString(),
                                       tr("A texture set per frame: strokes land in the tiles of the frame they are "
                                          "painted at, and the bake paths take a frame number.")
                                           .toStdString()));
        textureMode->populateChoices(options);
    }
    textureMode->setDefaultValue((int)eTextureModeStatic);
    textureMode->setAnimationEnabled(false);
    page->addKnob(textureMode);
    _textureMode = textureMode;

    KnobOutputFilePtr bakeInputsPath = createKnob<KnobOutputFile>(tr("Bake Inputs Path"));
    bakeInputsPath->setName(kTexturePaintParamBakeInputsPath);
    bakeInputsPath->setHintToolTip(tr("The files the look of the input scene is baked into, one per UDIM tile: the "
                                      "<UDIM> token stands for the tile number, and #### for the frame number when "
                                      "Texture Mode is Animated, which bakes every frame of the project range. They are "
                                      "the base the strokes are painted over.")
                                       .toStdString());
    bakeInputsPath->setAnimationEnabled(false);
    page->addKnob(bakeInputsPath);
    _bakeInputsPath = bakeInputsPath;

    KnobButtonPtr bakeInputs = createKnob<KnobButton>(tr("Bake Inputs"));
    bakeInputs->setName(kTexturePaintParamBakeInputs);
    bakeInputs->setHintToolTip(tr("Bakes the look of the materials bound to the matched meshes, at the current frame, "
                                  "into the Bake Inputs Path files: one per UDIM tile the meshes cover and material "
                                  "input, diffuseColor always and each other UsdPreviewSurface input a material sets, "
                                  "named with the input after the path's name. The strokes are then painted over "
                                  "these files rather than over the constant colour; a file of another size than "
                                  "Tile Resolution is resampled, and a file deleted since is an error.")
                                   .toStdString());
    page->addKnob(bakeInputs);
    _bakeInputs = bakeInputs;

    KnobStringPtr bakedInputs = createKnob<KnobString>(tr("Baked Inputs"));
    bakedInputs->setName(kTexturePaintParamBakedInputs);
    bakedInputs->setHintToolTip(tr("What the last Bake Inputs wrote: its count, purposes and UDIM tiles.").toStdString());
    bakedInputs->setAnimationEnabled(false);
    bakedInputs->setSecretByDefault(true);
    page->addKnob(bakedInputs);
    _bakedInputs = bakedInputs;

    KnobOutputFilePtr bakeOutputPath = createKnob<KnobOutputFile>(tr("Bake Output Path"));
    bakeOutputPath->setName(kTexturePaintParamBakeOutputPath);
    bakeOutputPath->setHintToolTip(tr("The files the painted texture is baked into, one per UDIM tile: the <UDIM> "
                                      "token stands for the tile number, and #### for the frame number when Texture Mode is "
                                      "Animated, which writes every frame of the project range. Baking freezes the "
                                      "sessions baked.")
                                       .toStdString());
    bakeOutputPath->setAnimationEnabled(false);
    page->addKnob(bakeOutputPath);
    _bakeOutputPath = bakeOutputPath;

    KnobButtonPtr bakeOutput = createKnob<KnobButton>(tr("Bake Output"));
    bakeOutput->setName(kTexturePaintParamBakeOutput);
    bakeOutput->setHintToolTip(tr("Writes the painted texture, at the current frame, into the Bake Output Path files: one "
                                  "half-float file per UDIM tile the meshes cover and painted plane, the colour plane "
                                  "under the path and any other plane named with its layer after the path's name. The "
                                  "sessions painted so far are frozen: the texture then starts from these files and only "
                                  "the sessions painted since are laid over them, so the frozen ones are never rendered "
                                  "again. Undoing the bake brings its sessions back live from their strokes.")
                                   .toStdString());
    page->addKnob(bakeOutput);
    _bakeOutput = bakeOutput;

    KnobStringPtr bakedOutput = createKnob<KnobString>(tr("Baked Output"));
    bakedOutput->setName(kTexturePaintParamBakedOutput);
    bakedOutput->setHintToolTip(tr("What the last Bake Output wrote: its count, purposes and UDIM tiles.").toStdString());
    bakedOutput->setAnimationEnabled(false);
    bakedOutput->setSecretByDefault(true);
    page->addKnob(bakedOutput);
    _bakedOutput = bakedOutput;

    KnobIntPtr bakeGeneration = createKnob<KnobInt>(tr("Bake Generation"));
    bakeGeneration->setName(kTexturePaintParamBakeGeneration);
    bakeGeneration->setHintToolTip(tr("How many bakes have landed. Each one changes it, so that the output refreshes.").toStdString());
    bakeGeneration->setAnimationEnabled(false);
    bakeGeneration->setSecretByDefault(true);
    bakeGeneration->setIsPersistent(false);
    page->addKnob(bakeGeneration);
    _bakeGeneration = bakeGeneration;

    KnobIntPtr outputTile = createKnob<KnobInt>(tr("Output Tile"));
    outputTile->setName(kTexturePaintParamOutputTile);
    outputTile->setHintToolTip(tr("The UDIM tile the node outputs as its image.").toStdString());
    outputTile->setDefaultValue(1001);
    outputTile->setMinimum(1001);
    outputTile->setDisplayMinimum(1001);
    outputTile->setDisplayMaximum(1100);
    outputTile->setAnimationEnabled(false);
    page->addKnob(outputTile);
    _outputTile = outputTile;

    const RotoPaintInteractPtr paint = RotoPaintInteract::create(this, true);
    _paintInteract = paint;
    paint->createSettingKnobs(this, page);
    const KnobButtonPtr resetCloneOffset = paint->createResetCloneOffsetKnob(page);
    paint->addSettingKnobsToViewerUI(this, resetCloneOffset);
    const KnobPagePtr toolbar = paint->createToolbarKnobs(0);
    paint->createToolKnobs(this, toolbar, true);
    if (const RotoContextPtr context = getNode()->getRotoContext()) {
        const std::weak_ptr<RotoPaintInteract> weakPaint = paint;
        QObject::connect(context.get(), &RotoContext::breakMultiStroke, this, [weakPaint]() {
            if (const RotoPaintInteractPtr p = weakPaint.lock()) {
                p->onBreakMultiStrokeTriggered();
            }
        });
    }
    const KnobButtonPtr brush = paint->brushAction.lock();
    paint->setCurrentTool(brush);
    paint->onRoleChangedInternal(paint->paintBrushToolGroup.lock());
    paint->onToolChangedInternal(brush);
} // TexturePaint::initializeKnobs

RotoPaintInteractPtr
TexturePaint::getPaintInteract() const
{
    return _paintInteract;
}

TexturePaintInteract*
TexturePaint::getViewInteract()
{
    if (!_viewInteract) {
        _viewInteract.reset(new TexturePaintInteract(this));
    }

    return _viewInteract.get();
}

void
TexturePaint::refreshExtraStateAfterTimeChanged(bool isPlayback,
                                                double time)
{
    NativeEffectBase::refreshExtraStateAfterTimeChanged(isPlayback, time);
    if (_paintInteract) {
        _paintInteract->refreshAfterTimeChanged(time);
    }
}

void
TexturePaint::onCurveLayerCreated(const RotoLayerPtr& layer)
{
    if (!layer) {
        return;
    }
    const SceneRaster::ExplicitCamera defaults;

    KnobDoublePtr cameraToWorld = makeSessionKnob<KnobDouble>(layer, kTexturePaintSessionParamCameraToWorld, tr("Camera To World"), 16,
                                                              tr("The session camera's transform, row-major with the "
                                                                 "translation in the last row."));
    for (int i = 0; i < 16; ++i) {
        cameraToWorld->setDefaultValue(defaults.cameraToWorld[i], i);
    }

    KnobDoublePtr viewHeight = makeSessionKnob<KnobDouble>(layer, kTexturePaintSessionParamViewHeight, tr("View Height"), 1,
                                                           tr("The height of the view at unit distance for a "
                                                              "perspective camera, or in world units for an "
                                                              "orthographic one."));
    viewHeight->setDefaultValue(defaults.viewHeight);

    KnobDoublePtr aspect = makeSessionKnob<KnobDouble>(layer, kTexturePaintSessionParamAspect, tr("Aspect"), 1,
                                                       tr("The view's width over its height."));
    aspect->setDefaultValue(defaults.aspect);

    KnobBoolPtr orthographic = makeSessionKnob<KnobBool>(layer, kTexturePaintSessionParamOrthographic, tr("Orthographic"), 1,
                                                         tr("Whether the session camera is orthographic."));
    orthographic->setDefaultValue(defaults.orthographic);

    KnobDoublePtr nearClip = makeSessionKnob<KnobDouble>(layer, kTexturePaintSessionParamNearClip, tr("Near Clip"), 1,
                                                         tr("The session camera's near clipping distance."));
    nearClip->setDefaultValue(defaults.nearClip);

    KnobDoublePtr farClip = makeSessionKnob<KnobDouble>(layer, kTexturePaintSessionParamFarClip, tr("Far Clip"), 1,
                                                        tr("The session camera's far clipping distance."));
    farClip->setDefaultValue(defaults.farClip);

    KnobBoolPtr frozen = makeSessionKnob<KnobBool>(layer, kTexturePaintSessionParamFrozen, tr("Frozen"), 1,
                                                   tr("Whether the session's strokes are baked into the Bake Output "
                                                      "files and so no longer projected."));
    frozen->setDefaultValue(false);
} // TexturePaint::onCurveLayerCreated

void
TexturePaint::onEffectCreated(bool /*mayCreateFileDialog*/,
                              const CreateNodeArgs& args)
{
    if (!args.getProperty<Serialization::NodeSerializationPtr>(kCreateNodeArgsPropNodeSerialization)) {
        // Named after the node, so that two TexturePaint nodes never bake over each other's files.
        const std::string name = getNode()->getScriptName_mt_safe();
        KnobOutputFilePtr inputs = _bakeInputsPath.lock();
        KnobOutputFilePtr output = _bakeOutputPath.lock();
        if (inputs) {
            inputs->setValue("[Project]/textures/" + name + "_inputs.<UDIM>.exr");
        }
        if (output) {
            output->setValue("[Project]/textures/" + name + "_output.<UDIM>.exr");
        }
        syncStrokeLifetimes();

        return;
    }

    // A loaded node's sessions get their views now, so its tiles render before any session is added.
    syncStrokeLifetimes();
    bool created = false;
    for (const RotoLayerPtr& session : getSessions()) {
        bool made = false;
        (void)ensureSessionView(session, &made);
        created = created || made;
    }
    if (created) {
        RotoContextPtr context = getNode()->getRotoContext();
        if (context) {
            context->refreshRotoPaintTree();
        }
    }
}

Route
TexturePaint::route(double /*time*/,
                    ViewIdx /*view*/)
{
    return getInput(0) ? Route::none() : Route::black();
}

ImageLayerDesc
TexturePaint::getPaintedPlane(double time,
                              ViewIdx view) const
{
    std::vector<ResolvedLayer> resolved;

    if (getNode()->resolveLayerKnob(time, view, &resolved) && !resolved.empty() && (resolved.front().desc.getNumComponents() > 0)) {
        return resolved.front().desc;
    }

    return ImageLayerDesc::getRGBAComponents();
}

bool
TexturePaint::sessionHasStrokes(const RotoLayerPtr& session) const
{
    RotoCurveContextPtr context = getNode()->getCurveContext();

    if (!context || !session) {
        return false;
    }
    for (const RotoDrawableItemPtr& item : context->getCurvesByRenderOrder(false)) {
        if (item && (getSessionOf(*item) == session)) {
            return true;
        }
    }

    return false;
}

std::list<ImageLayerDesc>
TexturePaint::getOutputPlanes(double time,
                              ViewIdx view) const
{
    std::list<ImageLayerDesc> planes;

    planes.push_back(ImageLayerDesc::getRGBAComponents());
    const ImageLayerDesc painted = getPaintedPlane(time, view);
    if (!painted.isColorLayer()) {
        for (const RotoLayerPtr& session : getSessions()) {
            if (sessionHasStrokes(session)) {
                planes.push_back(painted);
                break;
            }
        }
    }

    return planes;
}

void
TexturePaint::getComponentsNeededAndProduced(double time,
                                             ViewIdx view,
                                             EffectInstance::ComponentsNeededMap* comps,
                                             double* passThroughTime,
                                             int* passThroughView,
                                             int* passThroughInputNb)
{
    EffectInstance::getComponentsNeededAndProduced(time, view, comps, passThroughTime, passThroughView, passThroughInputNb);
    // Nothing is fetched from the scene input as an image.
    comps->erase(0);
    (*comps)[-1] = getOutputPlanes(time, view);
}

StatusEnum
TexturePaint::getPreferredMetadataAt(double /*time*/,
                                     ViewIdx /*view*/,
                                     NodeMetadata& metadata)
{
    const int size = getTileResolution();

    metadata.setBitDepth(-1, eImageBitDepthFloat);
    metadata.setComponentsType(-1, kNatronColorLayerID);
    metadata.setNComps(-1, 4);
    metadata.setIsFrameVarying(getTextureMode() == eTextureModeAnimated);
    metadata.setOutputFormat(RectI(0, 0, size, size));
    metadata.setPixelAspectRatio(-1, 1.);

    return eStatusOK;
}

ImageMetadata
TexturePaint::deriveOutputMetadata(double time,
                                   ViewIdx view)
{
    ImageMetadata metadata;
    const std::list<ImageLayerDesc> planes = getOutputPlanes(time, view);

    for (const ImageLayerDesc& plane : planes) {
        if (plane.isColorLayer()) {
            metadata.setString(kTexturePaintMetadataPurposePrefix "rgba", kTexturePaintPurposeDiffuseColor);
        } else {
            metadata.setString(kTexturePaintMetadataPurposePrefix + plane.getLayerID(), plane.getLayerID());
        }
    }

    return metadata;
}

StatusEnum
TexturePaint::getRegionOfDefinition(U64 /*hash*/,
                                    double /*time*/,
                                    const RenderScale& /*scale*/,
                                    ViewIdx /*view*/,
                                    RectD* rod)
{
    const int size = getTileResolution();

    rod->x1 = 0.;
    rod->y1 = 0.;
    rod->x2 = size;
    rod->y2 = size;

    return eStatusOK;
}

U64
TexturePaint::getTileCacheKey(ViewIdx view) const
{
    ContentHasher hasher;

    hasher.appendString(PLUGINID_NATRON_TEXTUREPAINT);
    hasher.appendI64(view.value());
    RotoCurveContextPtr context = getNode()->getCurveContext();
    hasher.appendU64(context ? context->getContentHash() : 0);
    const KnobIPtr knobs[] = {
        _primPattern.getKnob(),
        _uvPrimvar.lock(),
        _tileResolution.lock(),
        _bufferResolution.lock(),
        _textureMode.lock(),
        getNode()->getLayerKnob(),
    };
    for (const KnobIPtr& knob : knobs) {
        hasher.appendU64(knob ? knob->getContentHash() : 0);
    }
    const NodePtr input = getNode()->getInput(0);
    hasher.appendU64(input ? input->getHashValue() : 0);

    for (const RotoLayerPtr& session : getSessions()) {
        hasher.appendBool(isSessionFrozen(session));
    }

    // The baked files are edited or deleted on disk without any knob changing.
    const KnobIPtr bakeKnobs[] = { _bakedInputs.lock(), _bakeInputsPath.lock(), _bakedOutput.lock(), _bakeOutputPath.lock() };
    for (const KnobIPtr& knob : bakeKnobs) {
        hasher.appendU64(knob ? knob->getContentHash() : 0);
    }
    for (const std::vector<std::string>& files : { getBakedInputFiles(), getBakedOutputFiles() }) {
        for (const std::string& path : files) {
            hasher.appendString(path);
            hasher.appendI64(fileStamp(path));
        }
    }

    return hasher.finalize();
}

bool
TexturePaint::getBakeInputsPattern(std::string* pattern,
                                   std::string* error) const
{
    KnobOutputFilePtr knob = _bakeInputsPath.lock();

    *pattern = knob ? knob->getValue() : std::string();
    AppInstancePtr app = getApp();
    if (app && app->getProject()) {
        app->getProject()->canonicalizePath(*pattern);
    }
    if (pattern->empty() || !std::filesystem::path(*pattern).is_absolute()) {
        *error = tr("Bake Inputs Path \"%1\" is not an absolute path once expanded: save the project first, or give an "
                    "absolute path.")
                     .arg(QString::fromStdString(*pattern))
                     .toStdString();

        return false;
    }

    if ((getTextureMode() == eTextureModeAnimated) && !hasFrameToken(*pattern)) {
        *error = tr("%1 \"%2\" holds no frame number: Animated texture mode writes a file per frame, so put #### in it, as in tex.<UDIM>.####.exr.")
                     .arg(QString::fromUtf8("Bake Inputs Path"))
                     .arg(QString::fromStdString(*pattern))
                     .toStdString();

        return false;
    }

    return true;
}

void
TexturePaint::getBakedInputs(std::vector<std::string>* purposes,
                             std::vector<int>* tiles,
                             std::pair<int, int>* frames) const
{
    KnobStringPtr knob = _bakedInputs.lock();

    parseBakeRecord(knob ? knob->getValue() : std::string(), purposes, tiles, frames);
}

std::string
TexturePaint::getBakedInputPath(const std::string& purpose,
                                int tile,
                                double time) const
{
    std::vector<std::string> purposes;
    std::vector<int> tiles;
    std::pair<int, int> frames;

    getBakedInputs(&purposes, &tiles, &frames);
    if ((std::find(purposes.begin(), purposes.end(), purpose) == purposes.end()) || (std::find(tiles.begin(), tiles.end(), tile) == tiles.end())) {
        return std::string();
    }
    std::string pattern;
    std::string error;
    std::string path;
    if (!getBakeInputsPattern(&pattern, &error) || !bakedFilePath(pattern, purpose, tile, recordFrame(time, frames.first, frames.second), &path, &error)) {
        return std::string();
    }

    return path;
}

bool
TexturePaint::getBakeOutputPattern(std::string* pattern,
                                   std::string* error) const
{
    KnobOutputFilePtr knob = _bakeOutputPath.lock();

    *pattern = knob ? knob->getValue() : std::string();
    AppInstancePtr app = getApp();
    if (app && app->getProject()) {
        app->getProject()->canonicalizePath(*pattern);
    }
    if (pattern->empty() || !std::filesystem::path(*pattern).is_absolute()) {
        *error = tr("Bake Output Path \"%1\" is not an absolute path once expanded: save the project first, or give an "
                    "absolute path.")
                     .arg(QString::fromStdString(*pattern))
                     .toStdString();

        return false;
    }

    if ((getTextureMode() == eTextureModeAnimated) && !hasFrameToken(*pattern)) {
        *error = tr("%1 \"%2\" holds no frame number: Animated texture mode writes a file per frame, so put #### in it, as in tex.<UDIM>.####.exr.")
                     .arg(QString::fromUtf8("Bake Output Path"))
                     .arg(QString::fromStdString(*pattern))
                     .toStdString();

        return false;
    }

    return true;
}

bool
TexturePaint::hasBakedOutput() const
{
    KnobStringPtr knob = _bakedOutput.lock();
    std::vector<std::string> purposes;
    std::vector<int> tiles;

    parseBakeRecord(knob ? knob->getValue() : std::string(), &purposes, &tiles);

    return !purposes.empty() && !tiles.empty();
}

std::string
TexturePaint::getBakedOutputPath(const std::string& purpose,
                                 int tile,
                                 double time) const
{
    KnobStringPtr knob = _bakedOutput.lock();
    std::vector<std::string> purposes;
    std::vector<int> tiles;
    std::pair<int, int> frames;

    parseBakeRecord(knob ? knob->getValue() : std::string(), &purposes, &tiles, &frames);
    if ((std::find(purposes.begin(), purposes.end(), purpose) == purposes.end()) || (std::find(tiles.begin(), tiles.end(), tile) == tiles.end())) {
        return std::string();
    }
    std::string pattern;
    std::string error;
    std::string path;
    if (!getBakeOutputPattern(&pattern, &error) || !bakedFilePath(pattern, purpose, tile, recordFrame(time, frames.first, frames.second), &path, &error)) {
        return std::string();
    }

    return path;
}

std::vector<std::string>
TexturePaint::getBakedOutputFiles() const
{
    KnobStringPtr knob = _bakedOutput.lock();
    std::vector<std::string> purposes;
    std::vector<int> tiles;
    std::pair<int, int> frames;
    std::vector<std::string> files;

    parseBakeRecord(knob ? knob->getValue() : std::string(), &purposes, &tiles, &frames);
    for (int frame = frames.first; frame <= frames.second; ++frame) {
        for (int tile : tiles) {
            for (const std::string& purpose : purposes) {
                const std::string path = getBakedOutputPath(purpose, tile, frame);
                if (!path.empty()) {
                    files.push_back(path);
                }
            }
        }
    }

    return files;
}

std::string
TexturePaint::getBasePath(const std::string& purpose,
                          int tile,
                          double time) const
{
    const std::string output = getBakedOutputPath(purpose, tile, time);

    return output.empty() ? getBakedInputPath(purpose, tile, time) : output;
}

std::string
TexturePaint::getViewBaseFiles(double time) const
{
    std::set<int> tiles;
    std::vector<std::string> purposes;
    std::vector<int> recorded;

    getBakedInputs(&purposes, &recorded);
    tiles.insert(recorded.begin(), recorded.end());
    KnobStringPtr output = _bakedOutput.lock();
    parseBakeRecord(output ? output->getValue() : std::string(), &purposes, &recorded);
    tiles.insert(recorded.begin(), recorded.end());

    std::string lines;
    for (int tile : tiles) {
        const std::string path = getBasePath(kTexturePaintPurposeDiffuseColor, tile, time);
        if (!path.empty()) {
            lines += std::to_string(tile) + "\t" + std::to_string(fileStamp(path)) + "\t" + path + "\n";
        }
    }

    return lines;
}

std::string
TexturePaint::getTileDirectory() const
{
    AppInstancePtr app = getApp();
    const std::string projectFile = (app && app->getProject()) ? (app->getProject()->getProjectPath() + app->getProject()->getProjectFilename()).toStdString() : std::string();

    return TexturePaintCache::tileDirectory(appPTR->getDiskCacheLocation().toStdString(), projectFile, getNode()->getFullyQualifiedName());
}

std::vector<std::string>
TexturePaint::getBakedInputFiles() const
{
    std::vector<std::string> purposes;
    std::vector<int> tiles;
    std::pair<int, int> frames;
    std::vector<std::string> files;

    getBakedInputs(&purposes, &tiles, &frames);
    for (int frame = frames.first; frame <= frames.second; ++frame) {
        for (int tile : tiles) {
            for (const std::string& purpose : purposes) {
                const std::string path = getBakedInputPath(purpose, tile, frame);
                if (!path.empty()) {
                    files.push_back(path);
                }
            }
        }
    }

    return files;
}

struct TexturePaint::BakeInputsRun {
    double time = 0.;
    ViewIdx view;
    int firstFrame = 1;
    int lastFrame = 1;
    std::vector<std::string> viewNames;
    std::string pattern;
    std::string primvar;
    std::string expression;
    int size = 0;
    double fps = 24.;
    EffectInstancePtr input;
    std::vector<MaterialBake::BakedFile> baked;
    std::vector<std::string> warnings;
};

struct TexturePaint::BakeOutputRun {
    double time = 0.;
    ViewIdx view;
    int firstFrame = 1;
    int lastFrame = 1;
    std::vector<std::string> viewNames;
    std::string pattern;
    std::string backupDirectory;
    std::string previousRecord;
    std::string primvar;
    std::string expression;
    int size = 0;
    double fps = 24.;
    EffectInstancePtr input;
    std::list<ImageLayerDesc> planes;
    QStringList purposes;
    // The sessions with strokes that were not frozen when the bake began, with their stroke counts then.
    std::vector<std::pair<RotoLayerPtr, int>> sessions;
    std::vector<std::string> files;
    std::vector<std::pair<std::string, std::string>> backups;
    QStringList tileNames;
};

bool
TexturePaint::prepareBakeInputs(double time,
                                ViewIdx view,
                                BakeInputsRun* run,
                                std::string* error)
{
    if (!getBakeInputsPattern(&run->pattern, error)) {
        return false;
    }
    KnobStringPtr uvPrimvar = _uvPrimvar.lock();
    run->time = getTextureTime(time);
    run->view = view;
    getBakeFrames(&run->firstFrame, &run->lastFrame);
    run->primvar = uvPrimvar ? uvPrimvar->getValue() : std::string();
    run->expression = _primPattern.getExpression();
    run->size = getTileResolution();
    AppInstancePtr app = getApp();
    run->fps = (app && app->getProject()) ? app->getProject()->getProjectFrameRate() : 24.;
    if (app && app->getProject()) {
        run->viewNames = app->getProject()->getProjectViewNames();
    }
    run->input = getInput(0);

    return true;
}

bool
TexturePaint::runBakeInputs(BakeInputsRun& run,
                            const BakeControl& control,
                            std::string* error)
{
    const bool animated = getTextureMode() == eTextureModeAnimated;

    for (int frame = run.firstFrame; frame <= run.lastFrame; ++frame) {
        if (control.cancelled && control.cancelled()) {
            return false;
        }
        const double time = animated ? (double)frame : run.time;
        InputScene scene;
        if (!openInputScene(run.input, time, run.view, run.fps, &scene, error)) {
            return false;
        }
        if (!scene.stage) {
            *error = tr("There is no input scene to bake the look of.").toStdString();

            return false;
        }
        MaterialBake::TilesArgs args;
        args.stage = scene.stage;
        args.time = scene.time;
        args.stName = PXR_NS::TfToken(run.primvar.empty() ? std::string("st") : run.primvar);
        args.size = run.size;
        args.pathPattern = expandFrame(run.pattern, run.viewNames, frame);
        args.cancelled = control.cancelled;
        if (!TexturePaintCache::matchedMeshes(args.stage, run.expression, args.stName, &args.meshes, error)) {
            return false;
        }
        if (args.meshes.empty()) {
            *error = tr("Prim Pattern matches no mesh with the primvar %1 to bake the look of.").arg(QString::fromStdString(args.stName.GetString())).toStdString();

            return false;
        }
        LiveTextureStore::instance().prepare(SceneOps::getLiveTextures(scene.scene), time);
        std::unique_ptr<MaterialBake::Backend> backend = MaterialBake::createPreviewSurfaceBackend();
        std::vector<MaterialBake::BakedFile> baked;
        std::vector<std::string> warnings;
        if (!MaterialBake::bakeTiles(*backend, args, &baked, &warnings, error)) {
            return false;
        }
        run.baked.insert(run.baked.end(), baked.begin(), baked.end());
        for (const std::string& warning : warnings) {
            if (std::find(run.warnings.begin(), run.warnings.end(), warning) == run.warnings.end()) {
                run.warnings.push_back(warning);
            }
        }
    }

    return true;
}

void
TexturePaint::applyBakeInputs(BakeInputsRun& run,
                              std::vector<std::string>* files,
                              std::string* warning)
{
    QStringList purposes;
    QStringList tiles;
    for (const MaterialBake::BakedFile& file : run.baked) {
        files->push_back(file.path);
        const QString purpose = QString::fromStdString(file.purpose);
        if (!purposes.contains(purpose)) {
            purposes.push_back(purpose);
        }
        const QString tile = QString::number(file.udimTile);
        if (!tiles.contains(tile)) {
            tiles.push_back(tile);
        }
    }
    KnobStringPtr record = _bakedInputs.lock();
    if (record) {
        record->setValue(nextBakeRecord(record->getValue(), purposes, tiles, run.firstFrame, run.lastFrame));
    }
    syncSessionViews();
    for (const std::string& line : run.warnings) {
        *warning += (warning->empty() ? std::string() : std::string("\n")) + line;
    }
}

bool
TexturePaint::bakeInputs(double time,
                         ViewIdx view,
                         std::vector<std::string>* files,
                         std::string* warning,
                         std::string* error)
{
    files->clear();
    warning->clear();
    error->clear();
    BakeInputsRun run;
    if (!prepareBakeInputs(time, view, &run, error)) {
        return false;
    }
    bool ok = false;
    // Live textures render on threads of their own and never on the main thread, which waits here.
    std::thread worker([&]() {
        ok = runBakeInputs(run, BakeControl(), error);
    });
    worker.join();
    if (!ok) {
        if (error->empty()) {
            *error = tr("The inputs could not be baked.").toStdString();
        }

        return false;
    }
    applyBakeInputs(run, files, warning);

    return true;
} // TexturePaint::bakeInputs

bool
TexturePaint::prepareBakeOutput(double time,
                                ViewIdx view,
                                const std::string& backupDirectory,
                                BakeOutputRun* run,
                                std::string* error)
{
    if (!getBakeOutputPattern(&run->pattern, error)) {
        return false;
    }
    KnobStringPtr record = _bakedOutput.lock();
    KnobStringPtr uvPrimvar = _uvPrimvar.lock();
    run->time = getTextureTime(time);
    run->view = view;
    getBakeFrames(&run->firstFrame, &run->lastFrame);
    run->backupDirectory = backupDirectory;
    run->previousRecord = record ? record->getValue() : std::string();
    run->primvar = uvPrimvar ? uvPrimvar->getValue() : std::string();
    run->expression = _primPattern.getExpression();
    run->size = getTileResolution();
    AppInstancePtr app = getApp();
    run->fps = (app && app->getProject()) ? app->getProject()->getProjectFrameRate() : 24.;
    if (app && app->getProject()) {
        run->viewNames = app->getProject()->getProjectViewNames();
    }
    run->input = getInput(0);
    run->planes = getOutputPlanes(run->time, view);
    for (const ImageLayerDesc& plane : run->planes) {
        run->purposes.push_back(QString::fromStdString(planePurpose(plane)));
    }
    for (const RotoLayerPtr& session : getSessions()) {
        const int strokes = sessionStrokeCount(session);
        if ((strokes > 0) && !isSessionFrozen(session)) {
            run->sessions.push_back(std::make_pair(session, strokes));
        }
    }

    return true;
}

bool
TexturePaint::runBakeOutput(BakeOutputRun& run,
                            const BakeControl& control,
                            std::string* error)
{
    InputScene scene;
    if (!openInputScene(run.input, run.time, run.view, run.fps, &scene, error)) {
        return false;
    }
    if (!scene.stage) {
        *error = tr("There is no input scene to bake the painted texture of.").toStdString();

        return false;
    }
    const PXR_NS::TfToken stName(run.primvar.empty() ? std::string("st") : run.primvar);
    std::vector<PXR_NS::SdfPath> meshes;
    if (!TexturePaintCache::matchedMeshes(scene.stage, run.expression, stName, &meshes, error)) {
        return false;
    }
    std::set<int> tiles;
    for (const PXR_NS::SdfPath& mesh : meshes) {
        if (!SceneRaster::udimTiles(scene.stage, mesh, stName, scene.time, &tiles, error)) {
            return false;
        }
    }
    if (tiles.empty()) {
        *error = tr("Prim Pattern matches no mesh with the primvar %1 to bake the painted texture of.").arg(QString::fromStdString(stName.GetString())).toStdString();

        return false;
    }
    LiveTextureStore::instance().prepare(SceneOps::getLiveTextures(scene.scene), run.time);
    const bool animated = getTextureMode() == eTextureModeAnimated;
    const double steps = (double)tiles.size() * (double)(run.lastFrame - run.firstFrame + 1);
    double stepsDone = 0.;
    for (int frame = run.firstFrame; frame <= run.lastFrame; ++frame) {
        const double frameTime = animated ? (double)frame : run.time;
        const std::string framePattern = expandFrame(run.pattern, run.viewNames, frame);
        for (int tile : tiles) {
            if (control.cancelled && control.cancelled()) {
                return false;
            }
            if (control.progress) {
                control.progress(stepsDone / steps, animated ? tr("Baking output, frame %1 tile %2").arg(frame).arg(tile) : tr("Baking output, tile %1").arg(tile));
            }
            std::vector<std::string> paths;
            std::string note;
            if (!ensureTiles(frameTime, run.view, tile, run.planes, &paths, &note, error, control.cancelled)) {
                return false;
            }
            std::size_t index = 0;
            for (const QString& purpose : run.purposes) {
                if (control.cancelled && control.cancelled()) {
                    return false;
                }
                std::vector<float> rgba;
                if (!TexturePaintCache::readTile(paths[index++], RectI(0, 0, run.size, run.size), &rgba, error)) {
                    return false;
                }
                std::string path;
                if (!MaterialBake::purposePath(framePattern, purpose.toStdString(), tile, &path, error)) {
                    return false;
                }
                std::error_code ec;
                if (!run.backupDirectory.empty() && !run.previousRecord.empty() && std::filesystem::is_regular_file(path, ec)) {
                    const std::string copy = run.backupDirectory + "/" + std::to_string(run.backups.size()) + "_" + std::filesystem::path(path).filename().string();
                    std::filesystem::create_directories(run.backupDirectory, ec);
                    if (!std::filesystem::copy_file(path, copy, std::filesystem::copy_options::overwrite_existing, ec)) {
                        *error = tr("%1 cannot be kept to undo the bake: %2").arg(QString::fromStdString(path)).arg(QString::fromStdString(ec.message())).toStdString();

                        return false;
                    }
                    run.backups.push_back(std::make_pair(path, copy));
                }
                if (!TexturePaintCache::writeTile(path, run.size, rgba, std::string(), error)) {
                    return false;
                }
                run.files.push_back(path);
            }
            if (frame == run.firstFrame) {
                run.tileNames.push_back(QString::number(tile));
            }
            stepsDone += 1.;
        }
    }

    return true;
}

void
TexturePaint::applyBakeOutput(BakeOutputRun& run,
                              BakeOutputState* state)
{
    state->record = run.previousRecord;
    state->frozen.clear();
    // A session painted on while the bake ran has strokes the files lack: it stays live.
    for (const std::pair<RotoLayerPtr, int>& entry : run.sessions) {
        if (!isSessionFrozen(entry.first) && (sessionStrokeCount(entry.first) == entry.second)) {
            setSessionFrozen(entry.first, true);
            state->frozen.push_back(entry.first);
        }
    }
    KnobStringPtr record = _bakedOutput.lock();
    if (record) {
        state->bakedRecord = nextBakeRecord(run.previousRecord, run.purposes, run.tileNames, run.firstFrame, run.lastFrame);
        record->setValue(state->bakedRecord);
    }
    syncSessionViews();
}

bool
TexturePaint::writeBakeOutput(double time,
                              ViewIdx view,
                              const std::string& backupDirectory,
                              std::vector<std::string>* files,
                              std::vector<std::pair<std::string, std::string>>* backups,
                              BakeOutputState* state,
                              std::string* error)
{
    files->clear();
    error->clear();
    BakeOutputRun run;
    if (!prepareBakeOutput(time, view, backupDirectory, &run, error)) {
        return false;
    }
    bool ok = false;
    // Live textures render on threads of their own and never on the main thread, which waits here.
    std::thread worker([&]() {
        ok = runBakeOutput(run, BakeControl(), error);
    });
    worker.join();
    *files = run.files;
    *backups = run.backups;
    if (!ok) {
        if (error->empty()) {
            *error = tr("The painted texture could not be baked.").toStdString();
        }

        return false;
    }
    applyBakeOutput(run, state);

    return true;
} // TexturePaint::writeBakeOutput

void
TexturePaint::restoreBakeOutputState(const BakeOutputState& state)
{
    for (const RotoLayerWPtr& weak : state.frozen) {
        RotoLayerPtr session = weak.lock();
        if (session) {
            setSessionFrozen(session, false);
        }
    }
    KnobStringPtr record = _bakedOutput.lock();
    if (record) {
        record->setValue(state.record);
    }
    {
        // Tiles composed over the undone bake's files must not outlive it, whatever their key.
        std::lock_guard<std::mutex> lock(_bakeMutex);
        TexturePaintCache::removeStaleTiles(getTileDirectory(), 0);
    }
    syncSessionViews();
}

void
TexturePaint::reapplyBakeOutputState(const BakeOutputState& state)
{
    for (const RotoLayerWPtr& weak : state.frozen) {
        RotoLayerPtr session = weak.lock();
        if (session) {
            setSessionFrozen(session, true);
        }
    }
    KnobStringPtr record = _bakedOutput.lock();
    if (record) {
        record->setValue(state.bakedRecord);
    }
    {
        std::lock_guard<std::mutex> lock(_bakeMutex);
        TexturePaintCache::removeStaleTiles(getTileDirectory(), 0);
    }
    syncSessionViews();
}

void
TexturePaint::syncStrokeLifetimes()
{
    const RotoCurveContextPtr context = getNode()->getCurveContext();

    if (context) {
        context->setIgnoreItemLifetimes(getTextureMode() != eTextureModeAnimated);
    }
}

std::string
TexturePaint::makeBackupDirectory() const
{
    static std::atomic<unsigned int> bakes(0);

    return getTileDirectory() + "/undo/" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" + std::to_string(++bakes);
}

UndoCommandPtr
TexturePaint::bakeOutput(double time,
                         ViewIdx view,
                         std::vector<std::string>* files,
                         std::string* error)
{
    const std::string backupDirectory = makeBackupDirectory();
    std::vector<std::pair<std::string, std::string>> backups;
    BakeOutputState state;

    if (!writeBakeOutput(time, view, backupDirectory, files, &backups, &state, error)) {
        std::error_code ec;
        std::filesystem::remove_all(backupDirectory, ec);

        return UndoCommandPtr();
    }

    return std::make_shared<TexturePaintBakeOutputUndoCommand>(getNode(), state, *files, backups, backupDirectory);
}

bool
TexturePaint::ensureTiles(double time,
                          ViewIdx view,
                          int tile,
                          const std::list<ImageLayerDesc>& planes,
                          std::vector<std::string>* paths,
                          std::string* note,
                          std::string* error,
                          const std::function<bool()>& cancel)
{
    const int size = getTileResolution();
    const bool animated = getTextureMode() == eTextureModeAnimated;
    time = getTextureTime(time);
    syncStrokeLifetimes();
    const std::string frame = animated ? QString::number(time, 'g', 12).toStdString() : std::string();
    AppInstancePtr app = getApp();
    const std::string directory = getTileDirectory();

    std::lock_guard<std::mutex> lock(_bakeMutex);
    const U64 key = getTileCacheKey(view);
    bool cached = true;

    paths->clear();
    note->clear();
    for (const ImageLayerDesc& plane : planes) {
        const std::string name = plane.isColorLayer() ? std::string("rgba") : plane.getLayerID();
        paths->push_back(TexturePaintCache::tilePath(directory, key, frame, name, tile));
        std::string fileNote;
        if (!TexturePaintCache::readTileNote(paths->back(), size, &fileNote)) {
            cached = false;
        } else if (paths->size() == 1) {
            *note = fileNote;
        }
    }
    if (cached) {
        return true;
    }
    note->clear();

    // A base file deleted or unreadable since the bake is an error rather than the constant
    // colour, which would paint over a look other than the one painted against.
    std::vector<std::string> basePaths;
    QStringList resampled;
    for (const ImageLayerDesc& plane : planes) {
        const std::string purpose = planePurpose(plane);
        const std::string output = getBakedOutputPath(purpose, tile, time);
        const std::string basePath = output.empty() ? getBakedInputPath(purpose, tile, time) : output;
        basePaths.push_back(basePath);
        if (basePath.empty()) {
            continue;
        }
        std::error_code ec;
        if (!std::filesystem::is_regular_file(basePath, ec)) {
            *error = (output.empty() ? tr("The baked input %1 is missing: bake the inputs again.")
                                     : tr("The baked output %1 is missing: undo the bake or bake the output again."))
                         .arg(QString::fromStdString(basePath))
                         .toStdString();

            return false;
        }
        std::string headerError;
        const std::shared_ptr<const OiioReadSupport::Header> header = OiioReadSupport::readHeader(basePath, &headerError, FilePin::eDisk);
        if (!header || header->subimages.empty()) {
            *error = tr("The baked input %1 cannot be read: %2").arg(QString::fromStdString(basePath)).arg(QString::fromStdString(headerError)).toStdString();

            return false;
        }
        const OIIO::ImageSpec& spec = header->subimages.front();
        if ((spec.width != size) || (spec.height != size)) {
            resampled.push_back(tr("%1 is %2x%3").arg(QString::fromStdString(basePath)).arg(spec.width).arg(spec.height));
        }
    }

    InputScene scene;
    EffectInstancePtr input = getInput(0);
    const double fps = (app && app->getProject()) ? app->getProject()->getProjectFrameRate() : 24.;
    if (!openInputScene(input, time, view, fps, &scene, error)) {
        return false;
    }
    TexturePaintCache::TileArgs args;
    args.udimTile = tile;
    args.size = size;
    args.cancelled = [this, cancel]() -> bool {
        return aborted() || (cancel && cancel());
    };
    if (scene.stage) {
        args.stage = scene.stage;
        args.time = scene.time;
        KnobStringPtr uvPrimvar = _uvPrimvar.lock();
        const std::string primvar = uvPrimvar ? uvPrimvar->getValue() : std::string();
        args.stName = PXR_NS::TfToken(primvar.empty() ? std::string("st") : primvar);
        if (!TexturePaintCache::matchedMeshes(args.stage, _primPattern.getExpression(), args.stName, &args.meshes, error)) {
            return false;
        }
    }

    const ImageLayerDesc painted = getPaintedPlane(time, view);
    // Frozen sessions are in the Bake Output files; while those are the base they are not laid
    // over again.
    const bool skipFrozen = hasBakedOutput();
    std::vector<std::pair<RotoLayerPtr, NodePtr>> sessions;
    for (const RotoLayerPtr& session : getSessions()) {
        if (!sessionHasStrokes(session) || (skipFrozen && isSessionFrozen(session))) {
            continue;
        }
        NodePtr viewNode = findSessionView(session.get());
        if (!viewNode) {
            *error = tr("The session %1 has no view to paint over.").arg(QString::fromUtf8(session->getScriptName().c_str())).toStdString();

            return false;
        }
        sessions.push_back(std::make_pair(session, viewNode));
    }

    bool overlapsFound = false;
    std::size_t index = 0;
    for (const ImageLayerDesc& plane : planes) {
        const std::string& path = (*paths)[index++];
        const bool paints = (plane.isColorLayer() && painted.isColorLayer()) || (plane.getLayerID() == painted.getLayerID());
        std::vector<TexturePaintCache::PaintSession> layered;
        if (paints) {
            for (const std::pair<RotoLayerPtr, NodePtr>& session : sessions) {
                TexturePaintCache::PaintSession entry;
                (void)getSessionCamera(session.first, &entry.camera);
                const RotoLayerPtr layer = session.first;
                const NodePtr viewNode = session.second;
                entry.renderBuffer = [this, layer, viewNode, plane, time, view](SessionBuffer* buffer, std::string* bufferError) -> bool {
                    // renderNodeImage() resets the thread's render state, which this render thread still needs.
                    bool ok = false;
                    std::thread worker([&]() {
                        ok = renderSessionBufferFrom(viewNode, layer, plane, time, view, buffer, bufferError);
                    });
                    worker.join();

                    return ok;
                };
                layered.push_back(entry);
            }
        }
        std::vector<float> rgba;
        std::vector<TexturePaintCache::MeshPair> overlaps;
        args.basePath = basePaths[index - 1];
        if (!TexturePaintCache::composeTile(args, plane.isColorLayer(), layered, &rgba, overlapsFound ? 0 : &overlaps, error)) {
            return false;
        }
        if (!overlapsFound) {
            overlapsFound = true;
            QStringList pairs;
            for (const TexturePaintCache::MeshPair& pair : overlaps) {
                pairs.push_back(tr("%1 and %2").arg(QString::fromUtf8(pair.first.GetString().c_str()), QString::fromUtf8(pair.second.GetString().c_str())));
            }
            QStringList lines;
            if (!pairs.isEmpty()) {
                lines.push_back(tr("The UV layouts of these meshes overlap in tile %1, so their paint lands on each other: %2.")
                                    .arg(tile)
                                    .arg(pairs.join(QString::fromUtf8("; "))));
            }
            if (!resampled.isEmpty()) {
                lines.push_back(tr("Baked inputs of another size than the Tile Resolution, %1, are resampled to it: %2.")
                                    .arg(size)
                                    .arg(resampled.join(QString::fromUtf8("; "))));
            }
            *note = lines.join(QLatin1Char('\n')).toStdString();
        }
        if (!TexturePaintCache::writeTile(path, size, rgba, *note, error)) {
            return false;
        }
    }
    TexturePaintCache::removeStaleTiles(directory, key);

    return true;
} // TexturePaint::ensureTiles

StatusEnum
TexturePaint::render(const RenderActionArgs& args)
{
    KnobIntPtr outputTile = _outputTile.lock();
    const int tile = outputTile ? outputTile->getValue() : 1001;
    const int size = getTileResolution();

    std::list<ImageLayerDesc> planes;
    for (const std::pair<ImageLayerDesc, ImagePtr>& layer : args.outputLayers) {
        if (layer.second) {
            planes.push_back(layer.first);
        }
    }
    if (planes.empty()) {
        return eStatusOK;
    }
    std::vector<std::string> paths;
    std::string note;
    std::string error;
    if (!ensureTiles(args.time, args.view, tile, planes, &paths, &note, &error)) {
        if (!error.empty()) {
            setPersistentMessage(eMessageTypeError, error);
        }

        return eStatusFailed;
    }

    const unsigned int level = args.mappedScale.toMipmapLevel();
    const int step = 1 << level;
    std::size_t index = 0;
    for (const std::pair<ImageLayerDesc, ImagePtr>& layer : args.outputLayers) {
        const ImagePtr& image = layer.second;
        if (!image) {
            continue;
        }
        const std::string& path = paths[index++];
        if (image->getBitDepth() != eImageBitDepthFloat) {
            return eStatusFailed;
        }
        const RectI roi = args.roi.intersect(image->getBounds());
        if (roi.isNull()) {
            continue;
        }
        // Each pixel at the mipmap level takes the texel at the centre of the block it covers.
        RectI window(roi.x1 * step, roi.y1 * step, roi.x2 * step, roi.y2 * step);
        window = window.intersect(RectI(0, 0, size, size));
        std::vector<float> texels;
        if (!window.isNull() && !TexturePaintCache::readTile(path, window, &texels, &error)) {
            setPersistentMessage(eMessageTypeError, error);

            return eStatusFailed;
        }
        const bool color = layer.first.isColorLayer();
        const int nComps = (int)image->getComponentsCount();
        const std::size_t windowWidth = (std::size_t)window.width();
        Image::WriteAccess access(image.get());
        for (int y = roi.y1; y < roi.y2; ++y) {
            float* row = reinterpret_cast<float*>(access.pixelAt(roi.x1, y));
            if (!row) {
                continue;
            }
            const int ty = y * step + step / 2;
            for (int x = roi.x1; x < roi.x2; ++x) {
                const int tx = x * step + step / 2;
                float* out = row + (std::size_t)(x - roi.x1) * (std::size_t)nComps;
                const bool inside = !window.isNull() && window.contains(tx, ty);
                const float* texel = inside ? texels.data() + ((std::size_t)(ty - window.y1) * windowWidth + (std::size_t)(tx - window.x1)) * 4 : 0;
                for (int c = 0; c < nComps; ++c) {
                    // A one-channel colour plane is alpha.
                    const int channel = (color && (nComps == 1)) ? 3 : c;
                    out[c] = (texel && (channel < 4)) ? texel[channel] : 0.f;
                }
            }
        }
    }
    if (note.empty()) {
        clearPersistentMessage(false);
    } else {
        setPersistentMessage(eMessageTypeWarning, note);
    }

    return eStatusOK;
} // TexturePaint::render

FramesNeededMap
TexturePaint::getFramesNeeded(double /*time*/,
                              ViewIdx /*view*/)
{
    return FramesNeededMap();
}

void
TexturePaint::getRegionsOfInterest(double /*time*/,
                                   const RenderScale& /*scale*/,
                                   const RectD& /*outputRoD*/,
                                   const RectD& /*renderWindow*/,
                                   ViewIdx /*view*/,
                                   RoIMap* /*ret*/)
{
}

int
TexturePaint::getTileResolution() const
{
    return resolutionFromChoice(_tileResolution.lock(), kDefaultTileResolutionIndex);
}

int
TexturePaint::getBufferResolution() const
{
    return resolutionFromChoice(_bufferResolution.lock(), kDefaultBufferResolutionIndex);
}

double
TexturePaint::getTextureTime(double time) const
{
    if (getTextureMode() == eTextureModeAnimated) {
        return time;
    }
    int first = 1;
    int last = 1;
    getBakeFrames(&first, &last);

    return first;
}

void
TexturePaint::getBakeFrames(int* first,
                            int* last) const
{
    double projectFirst = 1.;
    double projectLast = 1.;
    AppInstancePtr app = getApp();

    if (app && app->getProject()) {
        app->getProject()->getFrameRange(&projectFirst, &projectLast);
    }
    *first = (int)std::floor(projectFirst);
    *last = (getTextureMode() == eTextureModeAnimated) ? std::max(*first, (int)std::floor(projectLast)) : *first;
}

bool
TexturePaint::hasFrameToken(const std::string& pattern)
{
    const std::vector<std::string> views;

    return expandFrame(pattern, views, 1) != expandFrame(pattern, views, 2);
}

bool
TexturePaint::bakedFilePath(const std::string& pattern,
                            const std::string& purpose,
                            int tile,
                            int frame,
                            std::string* path,
                            std::string* error) const
{
    std::vector<std::string> views;
    AppInstancePtr app = getApp();

    if (app && app->getProject()) {
        views = app->getProject()->getProjectViewNames();
    }

    return MaterialBake::purposePath(expandFrame(pattern, views, frame), purpose, tile, path, error);
}

int
TexturePaint::recordFrame(double time,
                          int firstFrame,
                          int lastFrame) const
{
    if (getTextureMode() != eTextureModeAnimated) {
        return firstFrame;
    }

    return std::clamp((int)std::floor(time + 0.5), firstFrame, std::max(firstFrame, lastFrame));
}

TexturePaint::TextureModeEnum
TexturePaint::getTextureMode() const
{
    KnobChoicePtr knob = _textureMode.lock();

    return (knob && (knob->getValue() == (int)eTextureModeAnimated)) ? eTextureModeAnimated : eTextureModeStatic;
}

RotoLayerPtr
TexturePaint::addSession(const SceneRaster::ExplicitCamera& camera)
{
    RotoCurveContextPtr context = getNode()->getCurveContext();

    if (!context) {
        return RotoLayerPtr();
    }
    // A layer is added into the deepest selected layer; sessions belong directly under the base layer.
    context->clearSelection(RotoItem::eSelectionReasonOther);
    RotoLayerPtr layer = context->addLayer();
    setSessionCamera(layer, camera);
    (void)getSessionView(layer);

    return layer;
}

RotoLayerPtr
TexturePaint::getSessionOf(const RotoItem& item)
{
    RotoLayerPtr layer = item.getParentLayer();

    while (layer && (layer->getHierarchyLevel() > 1)) {
        layer = layer->getParentLayer();
    }
    if (!layer || (layer->getHierarchyLevel() != 1) || !sessionKnob<KnobBool>(layer, kTexturePaintSessionParamFrozen)) {
        return RotoLayerPtr();
    }

    return layer;
}

bool
TexturePaint::isPaintTreeRoot(const RotoLayer& layer) const
{
    return (layer.getHierarchyLevel() == 1) && layer.getHostKnobByName(kTexturePaintSessionParamFrozen);
}

NodePtr
TexturePaint::findSessionView(const RotoLayer* session) const
{
    std::lock_guard<std::mutex> lock(_viewsMutex);
    std::map<const RotoLayer*, SessionView>::const_iterator found = _views.find(session);

    return ((found != _views.end()) && !found->second.session.expired()) ? found->second.node : NodePtr();
}

NodePtr
TexturePaint::getPaintTreeSourceFor(const RotoDrawableItem& item) const
{
    RotoLayerPtr session = getSessionOf(item);

    return session ? findSessionView(session.get()) : NodePtr();
}

NodePtr
TexturePaint::getPaintTreeRevealSource(const RotoDrawableItem& item,
                                       int /*inputNb*/) const
{
    return getPaintTreeSourceFor(item);
}

void
TexturePaint::syncSessionView(const RotoLayerPtr& session,
                              const NodePtr& viewNode)
{
    TexturePaintView* view = viewNode ? dynamic_cast<TexturePaintView*>(viewNode->getEffectInstance().get()) : 0;
    SceneRaster::ExplicitCamera camera;

    if (!view || !getSessionCamera(session, &camera)) {
        return;
    }
    view->beginChanges();
    view->setCamera(camera);
    view->setPrimPattern(_primPattern.getExpression());
    KnobStringPtr uvPrimvar = _uvPrimvar.lock();
    view->setUvPrimvar(uvPrimvar ? uvPrimvar->getValue() : std::string("st"));
    const AppInstancePtr app = getApp();
    const double now = (app && app->getTimeLine()) ? (double)app->getTimeLine()->currentFrame() : 1.;
    view->setBase(getViewBaseFiles(getTextureTime(now)), getTileResolution());
    view->endChanges();

    const NodePtr scene = getNode()->getInput(0);
    if (viewNode->getInput(0) != scene) {
        viewNode->disconnectInput(0);
        if (scene) {
            viewNode->connectInputBase(scene, 0);
        }
    }
}

NodePtr
TexturePaint::ensureSessionView(const RotoLayerPtr& session,
                                bool* created)
{
    *created = false;
    if (!session || (session->getHierarchyLevel() != 1) || !sessionKnob<KnobBool>(session, kTexturePaintSessionParamFrozen)) {
        return NodePtr();
    }
    NodePtr viewNode = findSessionView(session.get());
    if (!viewNode) {
        AppInstancePtr app = getApp();
        if (!app) {
            return NodePtr();
        }
        CreateNodeArgs args(PLUGINID_NATRON_TEXTUREPAINTVIEW, NodeCollectionPtr());
        args.setProperty<bool>(kCreateNodeArgsPropOutOfProject, true);
        args.setProperty<bool>(kCreateNodeArgsPropNoNodeGUI, true);
        args.setProperty<bool>(kCreateNodeArgsPropTrustPluginID, true);
        args.setProperty<std::string>(kCreateNodeArgsPropNodeInitialName,
                                      getNode()->getScriptName_mt_safe() + "_" + session->getScriptName() + "_View");
        args.setProperty<bool>(kCreateNodeArgsPropAllowNonUserCreatablePlugins, true);
        viewNode = app->createNode(args);
        if (!viewNode) {
            return NodePtr();
        }
        std::lock_guard<std::mutex> lock(_viewsMutex);
        for (std::map<const RotoLayer*, SessionView>::iterator it = _views.begin(); it != _views.end();) {
            if (it->second.session.expired()) {
                _views.erase(it++);
            } else {
                ++it;
            }
        }
        SessionView entry;
        entry.session = session;
        entry.node = viewNode;
        _views[session.get()] = entry;
        *created = true;
    }
    syncSessionView(session, viewNode);

    return viewNode;
} // TexturePaint::ensureSessionView

NodePtr
TexturePaint::getSessionView(const RotoLayerPtr& session)
{
    bool created = false;
    NodePtr viewNode = ensureSessionView(session, &created);

    if (created) {
        RotoContextPtr context = getNode()->getRotoContext();
        if (context) {
            context->refreshRotoPaintTree();
        }
    }

    return viewNode;
}

void
TexturePaint::syncSessionViews()
{
    std::vector<std::pair<RotoLayerPtr, NodePtr>> views;
    {
        std::lock_guard<std::mutex> lock(_viewsMutex);
        for (std::map<const RotoLayer*, SessionView>::const_iterator it = _views.begin(); it != _views.end(); ++it) {
            RotoLayerPtr session = it->second.session.lock();
            if (session) {
                views.push_back(std::make_pair(session, it->second.node));
            }
        }
    }
    for (const std::pair<RotoLayerPtr, NodePtr>& view : views) {
        syncSessionView(view.first, view.second);
    }
}

void
TexturePaint::quitBackgroundWork(bool blocking)
{
    if (!_baker) {
        return;
    }
    if (blocking) {
        _baker->stop(true);
    } else {
        _baker->requestStop(true);
    }
    std::lock_guard<std::mutex> lock(_pendingSessionBakesMutex);
    _pendingSessionBakes.clear();
}

void
TexturePaint::onInputChanged(int inputNo)
{
    syncSessionViews();
    NativeEffectBase::onInputChanged(inputNo);
}

bool
TexturePaint::knobChanged(KnobI* k,
                          ValueChangedReasonEnum reason,
                          ViewSpec view,
                          double time,
                          bool originatedFromMainThread)
{
    const KnobStringPtr primPattern = _primPattern.getKnob();
    const KnobIPtr viewKnobs[] = {
        primPattern,
        _uvPrimvar.lock(),
        _tileResolution.lock(),
        _bakeInputsPath.lock(),
        _bakeOutputPath.lock(),
    };
    if (k && (std::find_if(std::begin(viewKnobs), std::end(viewKnobs), [k](const KnobIPtr& knob) {
                  return knob.get() == k;
              })
              != std::end(viewKnobs))) {
        syncSessionViews();
    }
    KnobChoicePtr textureMode = _textureMode.lock();
    if (textureMode && (k == textureMode.get())) {
        syncStrokeLifetimes();
        if (getTextureMode() == eTextureModeAnimated) {
            for (const KnobOutputFilePtr& path : { _bakeInputsPath.lock(), _bakeOutputPath.lock() }) {
                const std::string value = path ? path->getValue() : std::string();
                if (path && !value.empty() && !hasFrameToken(value)) {
                    const std::size_t dot = value.find_last_of('.');
                    const std::size_t slash = value.find_last_of("/\\");
                    const bool hasExtension = (dot != std::string::npos) && ((slash == std::string::npos) || (dot > slash));
                    path->setValue(hasExtension ? value.substr(0, dot) + ".####" + value.substr(dot) : value + ".####");
                }
            }
        }
        syncSessionViews();
    }
    KnobButtonPtr bakeOutputButton = _bakeOutput.lock();
    if (bakeOutputButton && (k == bakeOutputButton.get())) {
        (void)enqueueBakeOutput(time, ViewIdx(0));

        return true;
    }
    KnobButtonPtr bake = _bakeInputs.lock();
    if (bake && (k == bake.get())) {
        (void)enqueueBakeInputs(time, ViewIdx(0));

        return true;
    }
    bool paintRet = false;
    if (k && _paintInteract) {
        const RotoToolEnum toolBefore = _paintInteract->selectedTool;
        if (_paintInteract->onKnobChanged(k, time, &paintRet)) {
            if (_viewInteract && (_paintInteract->selectedTool != toolBefore)) {
                _viewInteract->onToolChanged(time);
            }

            return paintRet;
        }
    }

    return NativeEffectBase::knobChanged(k, reason, view, time, originatedFromMainThread);
}

TexturePaintBaker*
TexturePaint::getBaker()
{
    if (!_baker) {
        _baker.reset(new TexturePaintBaker);
    }

    return _baker.get();
}

bool
TexturePaint::isBaking() const
{
    return _baker && _baker->isBaking();
}

quint64
TexturePaint::getBakeGeneration() const
{
    return _baker ? _baker->getGeneration() : 0;
}

bool
TexturePaint::isSessionBakePending(const RotoLayerPtr& session) const
{
    if (!session) {
        return false;
    }
    std::lock_guard<std::mutex> lock(_pendingSessionBakesMutex);

    return (_pendingSessionBakes.find(session.get()) != _pendingSessionBakes.end());
}

int
TexturePaint::sessionStrokeCount(const RotoLayerPtr& session) const
{
    RotoCurveContextPtr context = getNode()->getCurveContext();
    int count = 0;

    if (!context || !session) {
        return 0;
    }
    for (const RotoDrawableItemPtr& item : context->getCurvesByRenderOrder(false)) {
        if (item && (getSessionOf(*item) == session)) {
            ++count;
        }
    }

    return count;
}

void
TexturePaint::landBake(bool ok)
{
    KnobIntPtr generation = _bakeGeneration.lock();

    if (ok && generation) {
        generation->setValue((int)getBakeGeneration());
    }
}

namespace {
// Tells the baker's progress and cancellation to the worker bodies, which know neither the baker nor Qt signals.
template <typename Control>
Control
makeControl(TexturePaintBaker::Context& context)
{
    Control control;

    control.cancelled = [&context]() -> bool {
        return context.cancelled();
    };
    control.progress = [&context](double fraction, const QString& label) {
        context.progress(fraction, label);
    };

    return control;
}
} // anon namespace

bool
TexturePaint::enqueueBakeInputs(double time,
                                ViewIdx view)
{
    std::shared_ptr<BakeInputsRun> run = std::make_shared<BakeInputsRun>();
    std::string error;

    if (!prepareBakeInputs(time, view, run.get(), &error)) {
        setPersistentMessage(eMessageTypeError, error);

        return false;
    }
    std::shared_ptr<std::string> failure = std::make_shared<std::string>();
    TexturePaintBaker::Bake bake;
    bake.label = tr("Baking inputs");
    bake.work = [this, run, failure](TexturePaintBaker::Context& context) -> bool {
        BakeControl control = makeControl<BakeControl>(context);

        return runBakeInputs(*run, control, failure.get());
    };
    bake.completion = [this, run, failure](bool ok) {
        if (!ok) {
            setPersistentMessage(eMessageTypeError, failure->empty() ? tr("The inputs could not be baked.").toStdString() : *failure);
            landBake(false);

            return;
        }
        std::vector<std::string> files;
        std::string warning;
        applyBakeInputs(*run, &files, &warning);
        if (warning.empty()) {
            clearPersistentMessage(false);
        } else {
            setPersistentMessage(eMessageTypeWarning, warning);
        }
        landBake(true);
    };

    return getBaker()->enqueue(bake);
}

bool
TexturePaint::enqueueBakeOutput(double time,
                                ViewIdx view)
{
    std::shared_ptr<BakeOutputRun> run = std::make_shared<BakeOutputRun>();
    std::string error;
    const std::string backupDirectory = makeBackupDirectory();

    if (!prepareBakeOutput(time, view, backupDirectory, run.get(), &error)) {
        setPersistentMessage(eMessageTypeError, error);

        return false;
    }
    std::shared_ptr<std::string> failure = std::make_shared<std::string>();
    TexturePaintBaker::Bake bake;
    bake.label = tr("Baking output");
    bake.work = [this, run, failure](TexturePaintBaker::Context& context) -> bool {
        BakeControl control = makeControl<BakeControl>(context);

        return runBakeOutput(*run, control, failure.get());
    };
    bake.completion = [this, run, failure, time, view, backupDirectory](bool ok) {
        if (!ok) {
            std::error_code ec;
            std::filesystem::remove_all(backupDirectory, ec);
            setPersistentMessage(eMessageTypeError, failure->empty() ? tr("The painted texture could not be baked.").toStdString() : *failure);
            landBake(false);

            return;
        }
        BakeOutputState state;
        applyBakeOutput(*run, &state);
        clearPersistentMessage(false);
        landBake(true);
        pushUndoCommand(std::make_shared<TexturePaintBakeOutputUndoCommand>(getNode(), state, run->files, run->backups, backupDirectory));
    };

    return getBaker()->enqueue(bake);
}

bool
TexturePaint::runSessionBake(double time,
                             ViewIdx view,
                             const BakeControl& control,
                             std::string* error)
{
    AppInstancePtr app = getApp();
    const double fps = (app && app->getProject()) ? app->getProject()->getProjectFrameRate() : 24.;
    KnobStringPtr uvPrimvar = _uvPrimvar.lock();
    const std::string primvar = uvPrimvar ? uvPrimvar->getValue() : std::string();
    InputScene scene;

    if (!openInputScene(getInput(0), time, view, fps, &scene, error)) {
        return false;
    }
    if (!scene.stage) {
        return true;
    }
    const PXR_NS::TfToken stName(primvar.empty() ? std::string("st") : primvar);
    std::vector<PXR_NS::SdfPath> meshes;
    if (!TexturePaintCache::matchedMeshes(scene.stage, _primPattern.getExpression(), stName, &meshes, error)) {
        return false;
    }
    std::set<int> tiles;
    for (const PXR_NS::SdfPath& mesh : meshes) {
        if (!SceneRaster::udimTiles(scene.stage, mesh, stName, scene.time, &tiles, error)) {
            return false;
        }
    }
    LiveTextureStore::instance().prepare(SceneOps::getLiveTextures(scene.scene), time);
    const std::list<ImageLayerDesc> planes = getOutputPlanes(time, view);
    double tilesDone = 0.;
    for (int tile : tiles) {
        if (control.cancelled && control.cancelled()) {
            return false;
        }
        if (control.progress) {
            control.progress(tilesDone / (double)tiles.size(), tr("Baking session, tile %1").arg(tile));
        }
        std::vector<std::string> paths;
        std::string note;
        if (!ensureTiles(time, view, tile, planes, &paths, &note, error, control.cancelled)) {
            return false;
        }
        tilesDone += 1.;
    }

    return true;
}

bool
TexturePaint::enqueueSessionBake(const RotoLayerPtr& session,
                                 double time,
                                 ViewIdx view)
{
    if (!session) {
        return false;
    }
    const RotoLayer* key = session.get();
    std::shared_ptr<std::string> failure = std::make_shared<std::string>();
    TexturePaintBaker::Bake bake;
    bake.label = tr("Baking session");
    bake.work = [this, failure, time, view](TexturePaintBaker::Context& context) -> bool {
        BakeControl control = makeControl<BakeControl>(context);

        return runSessionBake(time, view, control, failure.get());
    };
    bake.completion = [this, failure, key](bool ok) {
        {
            std::lock_guard<std::mutex> lock(_pendingSessionBakesMutex);
            std::map<const RotoLayer*, int>::iterator it = _pendingSessionBakes.find(key);
            if ((it != _pendingSessionBakes.end()) && (--it->second <= 0)) {
                _pendingSessionBakes.erase(it);
            }
        }
        if (!ok && !failure->empty()) {
            setPersistentMessage(eMessageTypeError, *failure);
        }
        landBake(ok);
    };
    {
        std::lock_guard<std::mutex> lock(_pendingSessionBakesMutex);
        ++_pendingSessionBakes[key];
    }
    if (!getBaker()->enqueue(bake)) {
        std::lock_guard<std::mutex> lock(_pendingSessionBakesMutex);
        if (--_pendingSessionBakes[key] <= 0) {
            _pendingSessionBakes.erase(key);
        }

        return false;
    }

    return true;
}

void
TexturePaint::getSessionBufferSize(const RotoLayerPtr& session,
                                   int* width,
                                   int* height) const
{
    SceneRaster::ExplicitCamera camera;
    const double aspect = getSessionCamera(session, &camera) ? camera.aspect : 1.;
    const RectD bounds = TexturePaintView::paintSpaceBounds(aspect);
    const RectI pixels = bounds.toPixelEnclosing(TexturePaintView::bufferMipmapLevel(getBufferResolution()), 1.);

    *width = pixels.width();
    *height = pixels.height();
}

namespace {
// The plane image holds over pixels as 4 floats per pixel, zero where it has no data.
bool
readPlane(const ImagePtr& image,
          const RectI& pixels,
          std::vector<float>* rgba,
          std::string* error)
{
    const std::size_t width = (std::size_t)pixels.width();

    rgba->assign(width * (std::size_t)pixels.height() * 4, 0.f);
    if (!image) {
        return true;
    }
    if (image->getBitDepth() != eImageBitDepthFloat) {
        *error = QObject::tr("The paint buffer did not render in floating point.").toStdString();

        return false;
    }
    const int nComps = (int)image->getComponentsCount();
    const RectI area = pixels.intersect(image->getBounds());
    if (area.isNull()) {
        return true;
    }
    Image::ReadAccess access(image.get());
    for (int y = area.y1; y < area.y2; ++y) {
        for (int x = area.x1; x < area.x2; ++x) {
            const float* in = reinterpret_cast<const float*>(access.pixelAt(x, y));
            if (!in) {
                continue;
            }
            float* out = rgba->data() + ((std::size_t)(y - pixels.y1) * width + (std::size_t)(x - pixels.x1)) * 4;
            for (int c = 0; (c < nComps) && (c < 4); ++c) {
                out[c] = in[c];
            }
        }
    }

    return true;
}
} // anon namespace

bool
TexturePaint::renderSessionBuffer(const RotoLayerPtr& session,
                                  const ImageLayerDesc& plane,
                                  double time,
                                  ViewIdx view,
                                  SessionBuffer* buffer,
                                  std::string* error)
{
    NodePtr viewNode = getSessionView(session);

    if (!viewNode) {
        *error = tr("The layer is not a session of this node.").toStdString();

        return false;
    }

    return renderSessionBufferFrom(viewNode, session, plane, time, view, buffer, error);
}

bool
TexturePaint::renderSessionBufferFrom(const NodePtr& viewNode,
                                      const RotoLayerPtr& session,
                                      const ImageLayerDesc& plane,
                                      double time,
                                      ViewIdx view,
                                      SessionBuffer* buffer,
                                      std::string* error)
{
    ++_sessionRenders;
    syncStrokeLifetimes();
    SceneRaster::ExplicitCamera camera;
    (void)getSessionCamera(session, &camera);
    const unsigned int level = TexturePaintView::bufferMipmapLevel(getBufferResolution());
    const RectI pixels = TexturePaintView::paintSpaceBounds(camera.aspect).toPixelEnclosing(level, 1.);
    const std::size_t count = (std::size_t)pixels.width() * (std::size_t)pixels.height();

    buffer->width = pixels.width();
    buffer->height = pixels.height();

    RectI rendered;
    std::vector<float> base;
    if (!readPlane(renderNodeImage(viewNode, time, view, level, pixels, plane, &rendered), pixels, &base, error)) {
        return false;
    }
    buffer->touched.assign(count, 0);

    KnobLayerSelect* layerKnob = dynamic_cast<KnobLayerSelect*>(getNode()->getLayerKnob().get());
    const std::string target = layerKnob ? layerKnob->getLayer() : std::string(kNatronColorLayerID);
    const bool painted = (ImageLayerDesc::isColorLayer(target) && plane.isColorLayer()) || (target == plane.getLayerID());

    // The top of the session's strokes is the merge of its last item in render order.
    NodePtr top;
    if (painted) {
        RotoCurveContextPtr context = getNode()->getCurveContext();
        const std::list<RotoDrawableItemPtr> items = context ? context->getCurvesByRenderOrder(false) : std::list<RotoDrawableItemPtr>();
        for (const RotoDrawableItemPtr& item : items) {
            if (item && (getSessionOf(*item) == session) && item->getMergeNode()) {
                top = item->getMergeNode();
            }
        }
    }
    if (!top) {
        buffer->rgba.swap(base);

        return true;
    }
    if (!readPlane(renderNodeImage(top, time, view, level, pixels, plane, &rendered), pixels, &buffer->rgba, error)) {
        return false;
    }

    bool selected[4] = { true, true, true, true };
    const std::vector<std::string> channels = layerKnob ? layerKnob->getChannels() : std::vector<std::string>();
    if (!channels.empty()) {
        const std::vector<std::string>& names = plane.getChannels();
        for (int c = 0; c < 4; ++c) {
            const bool exists = (std::size_t)c < names.size();
            selected[c] = exists && (std::find(channels.begin(), channels.end(), names[(std::size_t)c]) != channels.end());
        }
    }
    for (std::size_t i = 0; i < count; ++i) {
        float* out = buffer->rgba.data() + i * 4;
        const float* in = base.data() + i * 4;
        for (int c = 0; c < 4; ++c) {
            if (!selected[c]) {
                out[c] = in[c];
            } else if (out[c] != in[c]) {
                buffer->touched[i] = 1;
            }
        }
    }

    return true;
} // TexturePaint::renderSessionBufferFrom

std::list<RotoLayerPtr>
TexturePaint::getSessions() const
{
    std::list<RotoLayerPtr> sessions;
    RotoCurveContextPtr context = getNode()->getCurveContext();

    if (!context) {
        return sessions;
    }
    const std::list<RotoLayerPtr>& layers = context->getLayers();
    for (const RotoLayerPtr& layer : layers) {
        if (layer && (layer->getHierarchyLevel() == 0)) {
            for (const RotoItemPtr& item : layer->getItems()) {
                RotoLayerPtr child = std::dynamic_pointer_cast<RotoLayer>(item);
                if (child && sessionKnob<KnobBool>(child, kTexturePaintSessionParamFrozen)) {
                    sessions.push_back(child);
                }
            }
        }
    }

    return sessions;
}

bool
TexturePaint::getSessionCamera(const RotoLayerPtr& layer,
                               SceneRaster::ExplicitCamera* camera)
{
    KnobDoublePtr cameraToWorld = sessionKnob<KnobDouble>(layer, kTexturePaintSessionParamCameraToWorld);
    KnobDoublePtr viewHeight = sessionKnob<KnobDouble>(layer, kTexturePaintSessionParamViewHeight);
    KnobDoublePtr aspect = sessionKnob<KnobDouble>(layer, kTexturePaintSessionParamAspect);
    KnobBoolPtr orthographic = sessionKnob<KnobBool>(layer, kTexturePaintSessionParamOrthographic);
    KnobDoublePtr nearClip = sessionKnob<KnobDouble>(layer, kTexturePaintSessionParamNearClip);
    KnobDoublePtr farClip = sessionKnob<KnobDouble>(layer, kTexturePaintSessionParamFarClip);

    if (!cameraToWorld || !viewHeight || !aspect || !orthographic || !nearClip || !farClip) {
        return false;
    }
    for (int i = 0; i < 16; ++i) {
        camera->cameraToWorld[i] = cameraToWorld->getValue(i);
    }
    camera->viewHeight = viewHeight->getValue();
    camera->aspect = aspect->getValue();
    camera->orthographic = orthographic->getValue();
    camera->nearClip = nearClip->getValue();
    camera->farClip = farClip->getValue();

    return true;
}

void
TexturePaint::setSessionCamera(const RotoLayerPtr& layer,
                               const SceneRaster::ExplicitCamera& camera)
{
    KnobDoublePtr cameraToWorld = sessionKnob<KnobDouble>(layer, kTexturePaintSessionParamCameraToWorld);
    KnobDoublePtr viewHeight = sessionKnob<KnobDouble>(layer, kTexturePaintSessionParamViewHeight);
    KnobDoublePtr aspect = sessionKnob<KnobDouble>(layer, kTexturePaintSessionParamAspect);
    KnobBoolPtr orthographic = sessionKnob<KnobBool>(layer, kTexturePaintSessionParamOrthographic);
    KnobDoublePtr nearClip = sessionKnob<KnobDouble>(layer, kTexturePaintSessionParamNearClip);
    KnobDoublePtr farClip = sessionKnob<KnobDouble>(layer, kTexturePaintSessionParamFarClip);

    if (cameraToWorld) {
        for (int i = 0; i < 16; ++i) {
            cameraToWorld->setValue(camera.cameraToWorld[i], ViewSpec::all(), i);
        }
    }
    if (viewHeight) {
        viewHeight->setValue(camera.viewHeight);
    }
    if (aspect) {
        aspect->setValue(camera.aspect);
    }
    if (orthographic) {
        orthographic->setValue(camera.orthographic);
    }
    if (nearClip) {
        nearClip->setValue(camera.nearClip);
    }
    if (farClip) {
        farClip->setValue(camera.farClip);
    }

    RotoCurveContextPtr context = layer ? layer->getContext() : RotoCurveContextPtr();
    NodePtr host = context ? context->getNode() : NodePtr();
    TexturePaint* effect = host ? dynamic_cast<TexturePaint*>(host->getEffectInstance().get()) : 0;
    NodePtr viewNode = effect ? effect->findSessionView(layer.get()) : NodePtr();
    if (viewNode) {
        effect->syncSessionView(layer, viewNode);
    }
}

bool
TexturePaint::isSessionFrozen(const RotoLayerPtr& layer)
{
    KnobBoolPtr frozen = sessionKnob<KnobBool>(layer, kTexturePaintSessionParamFrozen);

    return frozen && frozen->getValue();
}

void
TexturePaint::setSessionFrozen(const RotoLayerPtr& layer,
                               bool frozen)
{
    KnobBoolPtr knob = sessionKnob<KnobBool>(layer, kTexturePaintSessionParamFrozen);

    if (knob) {
        knob->setValue(frozen);
    }
}

NATRON_NAMESPACE_EXIT
