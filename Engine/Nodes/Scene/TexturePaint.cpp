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
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "Engine/AppInstance.h"
#include "Engine/ChoiceOption.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeColorSampler.h"
#include "Engine/Nodes/Scene/SceneRaster.h"
#include "Engine/Nodes/Scene/TexturePaintView.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoCurveContext.h"
#include "Engine/RotoDrawableItem.h"
#include "Engine/RotoLayer.h"

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
} // anon namespace

TexturePaint::TexturePaint(NodePtr node)
    : NativeEffectBase(node)
{
}

TexturePaint::~TexturePaint()
{
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
                                      "<UDIM> token stands for the tile number. They are the base the strokes are "
                                      "painted over.")
                                       .toStdString());
    bakeInputsPath->setDefaultValue("[Project]/textures/texturePaint_inputs.<UDIM>.exr");
    bakeInputsPath->setAnimationEnabled(false);
    page->addKnob(bakeInputsPath);
    _bakeInputsPath = bakeInputsPath;

    KnobOutputFilePtr bakeOutputPath = createKnob<KnobOutputFile>(tr("Bake Output Path"));
    bakeOutputPath->setName(kTexturePaintParamBakeOutputPath);
    bakeOutputPath->setHintToolTip(tr("The files the painted texture is baked into, one per UDIM tile: the <UDIM> "
                                      "token stands for the tile number. Baking freezes the sessions baked.")
                                       .toStdString());
    bakeOutputPath->setDefaultValue("[Project]/textures/texturePaint_output.<UDIM>.exr");
    bakeOutputPath->setAnimationEnabled(false);
    page->addKnob(bakeOutputPath);
    _bakeOutputPath = bakeOutputPath;

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
} // TexturePaint::initializeKnobs

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

Route
TexturePaint::route(double /*time*/,
                    ViewIdx /*view*/)
{
    return Route::black();
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

StatusEnum
TexturePaint::render(const RenderActionArgs& /*args*/)
{
    return eStatusOK;
}

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
    if (k && ((k == primPattern.get()) || (k == _uvPrimvar.lock().get()))) {
        syncSessionViews();
    }

    return NativeEffectBase::knobChanged(k, reason, view, time, originatedFromMainThread);
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
} // TexturePaint::renderSessionBuffer

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
