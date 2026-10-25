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

#include "USDRead.h"

#include <mutex>
#include <set>
#include <string>

#include <QFileInfo>
#include <QString>

#include "Engine/ContentHasher.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"

NATRON_NAMESPACE_ENTER

namespace {
// The commentary of every error posted since mark, one per line, and clears them so USD does not
// also report them on its own.
std::string
takeUsdErrors(PXR_NS::TfErrorMark& mark)
{
    std::string text;

    for (PXR_NS::TfErrorMark::Iterator it = mark.GetBegin(); it != mark.GetEnd(); ++it) {
        if (!text.empty()) {
            text += '\n';
        }
        text += it->GetCommentary();
    }
    mark.Clear();

    return text;
}

// layer and every layer it brings in -- sublayers, references, payloads -- that is open.
void
collectOpenLayers(const PXR_NS::SdfLayerHandle& layer,
                  std::set<PXR_NS::SdfLayerHandle>* layers)
{
    if (!layer || !layers->insert(layer).second) {
        return;
    }
    const std::set<std::string> dependencies = layer->GetCompositionAssetDependencies();
    for (std::set<std::string>::const_iterator it = dependencies.begin(); it != dependencies.end(); ++it) {
        collectOpenLayers(PXR_NS::SdfLayer::FindRelativeToLayer(layer, *it), layers);
    }
}

// What a USD stage would read as the layer's time codes per second.
double
getTimeCodesPerSecond(const PXR_NS::SdfLayerRefPtr& layer)
{
    if (layer->HasTimeCodesPerSecond()) {
        return layer->GetTimeCodesPerSecond();
    }
    if (layer->HasFramesPerSecond()) {
        return layer->GetFramesPerSecond();
    }

    return layer->GetTimeCodesPerSecond();
}
} // anonymous namespace

struct USDRead::Implementation {
    std::mutex fileLayerMutex;
    // The file the current layer sublayers, held open so it is the layer every stage composed
    // from that layer reads, and the one Reload reloads.
    PXR_NS::SdfLayerRefPtr fileLayer;
};

USDRead::USDRead(NodePtr node)
    : NativeEffectBase(node)
    , _filename()
    , _frameOffset()
    , _reload()
    , _reloadGeneration(0)
    , _imp(new Implementation)
{
}

USDRead::~USDRead()
{
}

const NodeSpec*
USDRead::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

void
USDRead::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));
    KnobFilePtr filename = createKnob<KnobFile>(tr("File"));

    filename->setName(kUSDReadParamFile);
    filename->setHintToolTip(tr("The USD file (.usd, .usda, .usdc, .usdz) or Alembic file (.abc) to read."));
    filename->setAddNewLine(false);
    page->addKnob(filename);
    _filename = filename;

    KnobButtonPtr reload = createKnob<KnobButton>(tr("Reload"));
    reload->setName(kUSDReadParamReload);
    reload->setHintToolTip(tr("Reads the file again, with every layer it sublayers, references or loads as a payload, "
                              "so a file changed on disk is what the node outputs."));
    page->addKnob(reload);
    _reload = reload;

    // Not animatable: the scene is one payload for every frame, so the offset is authored once
    // into the layer rather than sampled.
    KnobDoublePtr frameOffset = createKnob<KnobDouble>(tr("Frame Offset"));
    frameOffset->setName(kUSDReadParamFrameOffset);
    frameOffset->setHintToolTip(tr("Frames added to the project frame before it is converted to the file's timecode: "
                                   "frame f reads the file at (f + offset) x the file's time codes per second / the "
                                   "project frame rate."));
    frameOffset->setAnimationEnabled(false);
    frameOffset->setDefaultValue(0.);
    page->addKnob(frameOffset);
    _frameOffset = frameOffset;
}

bool
USDRead::knobChanged(KnobI* k,
                     ValueChangedReasonEnum /*reason*/,
                     ViewSpec /*view*/,
                     double /*time*/,
                     bool /*originatedFromMainThread*/)
{
    KnobButtonPtr reload = _reload.lock();

    if (!reload || (k != reload.get())) {
        return false;
    }
    purgeCaches();
    clearPersistentMessage(false);
    // Same as NativeRead: the Reload bracket hashed this node before purgeCaches() moved its
    // hash inputs.
    NodePtr node = getNode();
    if (node) {
        node->markHashDirty();
    }
    KnobFilePtr filename = _filename.lock();
    if (filename) {
        filename->reloadOtherNodesReadingTheseFiles();
    }

    return true;
}

void
USDRead::purgeCaches()
{
    KnobFilePtr filename = _filename.lock();
    PXR_NS::SdfLayerRefPtr fileLayer;
    {
        std::lock_guard<std::mutex> lock(_imp->fileLayerMutex);
        fileLayer = _imp->fileLayer;
    }
    if (!fileLayer && filename) {
        // Another node may have the file open even though this one never read it.
        const std::string path = filename->getSingleFilePath();
        if (!path.empty()) {
            fileLayer = PXR_NS::SdfLayer::Find(path);
        }
    }
    if (fileLayer) {
        std::set<PXR_NS::SdfLayerHandle> layers;
        collectOpenLayers(fileLayer, &layers);
        PXR_NS::TfErrorMark mark;
        // Forced: a file rewritten within its modification time's resolution still reloads.
        PXR_NS::SdfLayer::ReloadLayers(layers, true);
        // A file that no longer reads keeps its old content; the next authoring reports it.
        (void)takeUsdErrors(mark);
    }
    if (filename) {
        filename->invalidateFileStamp();
    }
    ++_reloadGeneration;
    invalidateSceneLayer();
    clearActionsCache();
} // USDRead::purgeCaches

void
USDRead::appendNodeContentHash(ContentHasher& h) const
{
    h.appendU64(_reloadGeneration.load());
}

bool
USDRead::authorLayer(const SceneLayerEdit& edit,
                     const std::string& path,
                     double frameOffset,
                     std::string* errorMessage)
{
    PXR_NS::SdfLayerRefPtr fileLayer;
    std::string diagnostic;
    {
        PXR_NS::TfErrorMark mark;
        fileLayer = PXR_NS::SdfLayer::FindOrOpen(path);
        diagnostic = takeUsdErrors(mark);
    }
    if (!fileLayer) {
        // USD posts nothing for a file that is not there.
        if (!diagnostic.empty()) {
            *errorMessage = diagnostic;
        } else if (!QFileInfo::exists(QString::fromStdString(path))) {
            *errorMessage = tr("%1 does not exist.").arg(QString::fromStdString(path)).toStdString();
        } else {
            *errorMessage = tr("USD could not read %1.").arg(QString::fromStdString(path)).toStdString();
        }

        return false;
    }

    // Stage metadata is only read from a stage's root layer, so the file's is carried up into
    // this layer, where SceneOps::appendLayer() reads it.
    const PXR_NS::SdfLayerRefPtr& layer = edit.layer;
    const PXR_NS::SdfPath& root = PXR_NS::SdfPath::AbsoluteRootPath();
    PXR_NS::TfToken upAxis;
    if (fileLayer->HasField(root, PXR_NS::UsdGeomTokens->upAxis, &upAxis)) {
        layer->SetField(root, PXR_NS::UsdGeomTokens->upAxis, upAxis);
    }
    double metersPerUnit = 0.;
    if (fileLayer->HasField(root, PXR_NS::UsdGeomTokens->metersPerUnit, &metersPerUnit)) {
        layer->SetField(root, PXR_NS::UsdGeomTokens->metersPerUnit, metersPerUnit);
    }
    if (fileLayer->HasDefaultPrim()) {
        layer->SetDefaultPrim(fileLayer->GetDefaultPrim());
    }
    // The same rate as the file, so USD applies no rate conversion across the sublayer arc and
    // the offset below is the whole time mapping.
    const double timeCodesPerSecond = getTimeCodesPerSecond(fileLayer);
    layer->SetTimeCodesPerSecond(timeCodesPerSecond);

    // A stage time t reads the sublayer at t - offset, so (f + frameOffset) in project frames is
    // read at frame f's timecode.
    const double framesPerSecond = (edit.framesPerSecond > 0.) ? edit.framesPerSecond : timeCodesPerSecond;
    layer->InsertSubLayerPath(fileLayer->GetIdentifier());
    layer->SetSubLayerOffset(PXR_NS::SdfLayerOffset(-frameOffset * timeCodesPerSecond / framesPerSecond), 0);

    std::lock_guard<std::mutex> lock(_imp->fileLayerMutex);
    _imp->fileLayer = fileLayer;

    return true;
} // USDRead::authorLayer

StatusEnum
USDRead::renderScene(const SceneRenderActionArgs& /*args*/,
                     ScenePayload* outputScene,
                     std::string* errorMessage)
{
    KnobFilePtr filename = _filename.lock();
    const std::string path = filename ? filename->getSingleFilePath() : std::string();

    if (path.empty()) {
        if (filename && !filename->getValue().empty()) {
            *errorMessage = tr("%1 names a frame sequence; USDRead reads a single file.").arg(QString::fromStdString(filename->getValue())).toStdString();
        } else {
            *errorMessage = tr("No USD file to read.").toStdString();
        }
        setPersistentMessage(eMessageTypeError, *errorMessage);

        return eStatusFailed;
    }

    KnobDoublePtr frameOffsetKnob = _frameOffset.lock();
    const double frameOffset = frameOffsetKnob ? frameOffsetKnob->getValue() : 0.;

    if (!emitSceneLayer(ScenePayload(), [this, path, frameOffset](const SceneLayerEdit& edit, std::string* error) { return authorLayer(edit, path, frameOffset, error); }, outputScene, errorMessage)) {
        setPersistentMessage(eMessageTypeError, *errorMessage);

        return eStatusFailed;
    }
    clearPersistentMessage(false);

    return eStatusOK;
} // USDRead::renderScene

NATRON_NAMESPACE_EXIT
