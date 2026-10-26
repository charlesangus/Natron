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

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include <QFileInfo>
#include <QString>

#include "Engine/ContentHasher.h"
#include "Engine/FileStampRegistry.h"
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
    mutable std::mutex fileLayerMutex;
    // The file's layer the current layer sublayers, held so it stays open until the next
    // authoring replaces it: a stage composed meanwhile finds it rather than reading the file.
    PXR_NS::SdfLayerRefPtr fileLayer;
    // Every file layer the file brings in, the file's own first, as of the last authoring or
    // Reload: what Reload reloads.
    std::vector<PXR_NS::SdfLayerRefPtr> closure;
    // Their files on disk, in the same order.
    std::vector<std::string> closurePaths;
    // The stamps of closurePaths as of the last purge, 0 before any: keys the node, so a Reload
    // that read a changed file again, however deep, re-authors the layer. Taken only when the
    // files are read again, never by an evaluation, so an evaluation cannot re-key the node
    // while the layers still hold what was read before.
    std::atomic<U64> closureDigest;

    Implementation()
        : closureDigest(0)
    {
    }

    void setClosure(const PXR_NS::SdfLayerRefPtr& layer)
    {
        std::vector<PXR_NS::SdfLayerRefPtr> layers = layer ? SceneOps::getFileLayerClosure(layer) : std::vector<PXR_NS::SdfLayerRefPtr>();
        std::vector<std::string> paths;
        paths.reserve(layers.size());
        for (const PXR_NS::SdfLayerRefPtr& l : layers) {
            paths.push_back(l->GetRealPath());
        }
        std::lock_guard<std::mutex> lock(fileLayerMutex);
        closure.swap(layers);
        closurePaths.swap(paths);
    }
};

USDRead::USDRead(NodePtr node)
    : NativeEffectBase(node)
    , _filename()
    , _frameOffset()
    , _reload()
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
    reload->setHintToolTip(tr("Reads the file again, and every file it sublayers, references or loads as a "
                              "payload, so files changed on disk are what the node outputs. Each changed file is "
                              "read again in place: every scene using it, the viewer's included, shows the new "
                              "content. Renders reading scenes are stopped or waited for first."));
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
    // The layers to read again: those the last authoring found, or, if this node has not
    // authored yet, those of the file's layer if another consumer has it open.
    std::vector<PXR_NS::SdfLayerRefPtr> layers;
    {
        std::lock_guard<std::mutex> lock(_imp->fileLayerMutex);
        layers = _imp->closure;
    }
    KnobFilePtr filename = _filename.lock();
    if (layers.empty() && filename) {
        const std::string path = filename->getSingleFilePath();
        PXR_NS::SdfLayerHandle open = path.empty() ? PXR_NS::SdfLayerHandle() : PXR_NS::SdfLayer::Find(path);
        if (open) {
            layers = SceneOps::getFileLayerClosure(PXR_NS::SdfLayerRefPtr(open));
        }
    }
    std::string error;
    if (!layers.empty() && !SceneOps::reloadFileLayers(layers, &error)) {
        setPersistentMessage(eMessageTypeError, tr("Reload failed: %1").arg(QString::fromStdString(error)).toStdString());
    } else {
        clearPersistentMessage(false);
    }
    // A reload can change what the file brings in. The file's layer is the same object, reloaded
    // in place.
    _imp->setClosure(layers.empty() ? PXR_NS::SdfLayerRefPtr() : layers.front());
    purgeCaches();
    // Same as NativeRead: the Reload bracket hashed this node before purgeCaches() moved its
    // hash inputs.
    NodePtr node = getNode();
    if (node) {
        node->markHashDirty();
    }
    if (filename) {
        filename->reloadOtherNodesReadingTheseFiles();
    }

    return true;
}

void
USDRead::purgeCaches()
{
    // Drops the stamps of the file and of every file it brings in, so the node's hash reads them
    // again. Edits no layer: Reload reloads them, behind SceneWriteBarrier.
    KnobFilePtr filename = _filename.lock();

    if (filename) {
        filename->invalidateFileStamp();
    }
    std::vector<std::string> paths;
    {
        std::lock_guard<std::mutex> lock(_imp->fileLayerMutex);
        paths = _imp->closurePaths;
    }
    FileStampRegistry& stamps = FileStampRegistry::instance();
    for (const std::string& path : paths) {
        if (!path.empty()) {
            stamps.invalidate(path);
        }
    }
    ContentHasher h;
    h.appendU64(paths.size());
    for (const std::string& path : paths) {
        h.appendU64(path.empty() ? 0 : stamps.stamp(path));
    }
    _imp->closureDigest = h.finalize();
    invalidateSceneLayer();
    clearActionsCache();
} // USDRead::purgeCaches

void
USDRead::appendNodeContentHash(ContentHasher& h) const
{
    // The file's own stamp is in the File knob's hash; this covers every file it brings in.
    h.appendU64(_imp->closureDigest.load());
}

bool
USDRead::authorLayer(const SceneLayerEdit& edit,
                     const std::string& path,
                     double frameOffset,
                     std::string* errorMessage)
{
    // One layer per file, shared with every other consumer through USD's layer registry, and
    // never edited here: Reload reloads it in place behind SceneWriteBarrier, and every stage
    // composing it follows.
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

    {
        std::lock_guard<std::mutex> lock(_imp->fileLayerMutex);
        _imp->fileLayer = fileLayer;
    }
    _imp->setClosure(fileLayer);

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

    // Nothing is sampled over the project's frame range: the offset is authored once.
    SceneLayerReads reads;
    reads.upstream = SceneLayerMemo::Key::eUpstreamReadNothing;
    reads.frameRange = false;
    if (!emitSceneLayer(ScenePayload(), reads, [this, path, frameOffset](const SceneLayerEdit& edit, std::string* error) { return authorLayer(edit, path, frameOffset, error); }, outputScene, errorMessage)) {
        setPersistentMessage(eMessageTypeError, *errorMessage);

        return eStatusFailed;
    }
    clearPersistentMessage(false);

    return eStatusOK;
} // USDRead::renderScene

NATRON_NAMESPACE_EXIT
