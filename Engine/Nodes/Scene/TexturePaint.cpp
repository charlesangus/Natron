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

#include <memory>
#include <string>
#include <vector>

#include "Engine/ChoiceOption.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/SceneRaster.h"
#include "Engine/RotoCurveContext.h"
#include "Engine/RotoLayer.h"

NATRON_NAMESPACE_ENTER

namespace {
const int kResolutionCount = 4;

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

    return layer;
}

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
