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

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <QFileInfo>
#include <QString>
#include <QStringList>

#include "Engine/AppInstance.h"
#include "Engine/ContentHasher.h"
#include "Engine/FileStampRegistry.h"
#include "Engine/IsolatedJob.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"
#include "Engine/Project.h"
#include "Engine/PythonUndoCommands.h"
#include "Engine/UndoRecorder.h"

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

QString
upAxisName(SceneMetadata::UpAxisEnum upAxis)
{
    return (upAxis == SceneMetadata::eUpAxisZ) ? QString::fromUtf8("Z") : QString::fromUtf8("Y");
}

const PXR_NS::TfToken&
upAxisToken(SceneMetadata::UpAxisEnum upAxis)
{
    return (upAxis == SceneMetadata::eUpAxisZ) ? PXR_NS::UsdGeomTokens->z : PXR_NS::UsdGeomTokens->y;
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
    // The links out of their root the file's layer stack authors, as of the closure's walk.
    SceneLinkIndex links;
    // SceneOps::getFileLayerReloadCount() when closure was walked: authoring skips the walk,
    // which visits every spec of every layer, while closure starts with the layer it sublayers
    // and no reload has run since. Reload walks afresh, which also finds files that were missing.
    std::uint64_t closureReloadCount;
    // The stamps of closurePaths as of the last purge, 0 before any: keys the node, so a Reload
    // that read a changed file again, however deep, re-authors the layer. Taken only when the
    // files are read again, never by an evaluation, so an evaluation cannot re-key the node
    // while the layers still hold what was read before.
    std::atomic<U64> closureDigest;
    // What the metrics correction reads the file through: a payload of one layer sublayering
    // sourceFileLayer at the file's rate, so a pooled stage composes it (payloads included) in the
    // file's own time codes, and a toggle flip or a project change finds that stage again.
    PXR_NS::SdfLayerRefPtr sourceFileLayer;
    U64 sourceDigest;
    double sourceTimeCodesPerSecond;
    ScenePayload source;

    Implementation()
        : closureReloadCount(0)
        , closureDigest(0)
        , sourceDigest(0)
        , sourceTimeCodesPerSecond(0.)
    {
    }

    ScenePayload getCorrectionSource(const PXR_NS::SdfLayerRefPtr& fileLayer,
                                     double timeCodesPerSecond)
    {
        const U64 digest = closureDigest.load();
        std::lock_guard<std::mutex> lock(fileLayerMutex);

        if ((sourceFileLayer != fileLayer) || (sourceDigest != digest) || (sourceTimeCodesPerSecond != timeCodesPerSecond)) {
            PXR_NS::SdfLayerRefPtr wrapper = SceneOps::createNodeLayer("USDRead-source");
            wrapper->SetTimeCodesPerSecond(timeCodesPerSecond);
            wrapper->InsertSubLayerPath(fileLayer->GetIdentifier());
            ContentHasher h;
            h.appendString(fileLayer->GetIdentifier());
            h.appendU64(digest);
            h.appendDouble(timeCodesPerSecond);
            source = SceneOps::appendLayer(ScenePayload(), wrapper, h.finalize());
            sourceFileLayer = fileLayer;
            sourceDigest = digest;
            sourceTimeCodesPerSecond = timeCodesPerSecond;
        }

        return source;
    }

    void setClosure(const PXR_NS::SdfLayerRefPtr& layer)
    {
        // Read before the walk, so a reload during it leaves the result marked stale.
        const std::uint64_t reloadCount = SceneOps::getFileLayerReloadCount();
        std::vector<PXR_NS::SdfLayerRefPtr> layers = layer ? SceneOps::getFileLayerClosure(layer) : std::vector<PXR_NS::SdfLayerRefPtr>();
        std::vector<std::string> paths;
        paths.reserve(layers.size());
        for (const PXR_NS::SdfLayerRefPtr& l : layers) {
            paths.push_back(l->GetRealPath());
        }
        SceneLinkIndex fileLinks = SceneOps::findLayerStackLinks(layer);
        std::lock_guard<std::mutex> lock(fileLayerMutex);
        closure.swap(layers);
        closurePaths.swap(paths);
        links.swap(fileLinks);
        closureReloadCount = reloadCount;
    }

    // Walks layer's closure unless the last walk was of layer and no reload has run since: file
    // layers are never edited, so only a reload changes what they bring in.
    void updateClosure(const PXR_NS::SdfLayerRefPtr& layer)
    {
        {
            std::lock_guard<std::mutex> lock(fileLayerMutex);
            if (!closure.empty() && (closure.front() == layer) && (closureReloadCount == SceneOps::getFileLayerReloadCount())) {
                return;
            }
        }
        setClosure(layer);
    }
};

USDRead::USDRead(NodePtr node)
    : NativeEffectBase(node)
    , _filename()
    , _frameOffset()
    , _reload()
    , _matchUpAxis()
    , _matchUnits()
    , _useFileMetrics()
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
    filename->setFileExtensions({ "usd", "usda", "usdc", "usdz", "abc" });
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

    KnobBoolPtr matchUpAxis = createKnob<KnobBool>(tr("Match Project Up Axis"));
    matchUpAxis->setName(kUSDReadParamMatchUpAxis);
    matchUpAxis->setHintToolTip(tr("Turns the file to the project's up axis: a Z-up file in a Y-up project is rotated -90 degrees "
                                   "about X, a Y-up file in a Z-up project +90 degrees. The rotation goes on the file's topmost "
                                   "transformable prims, in this node's layer; the file is not edited and prim paths do not change. "
                                   "The scene then reports the project's up axis."));
    matchUpAxis->setDefaultValue(false);
    matchUpAxis->setAnimationEnabled(false);
    matchUpAxis->setAddNewLine(false);
    page->addKnob(matchUpAxis);
    _matchUpAxis = matchUpAxis;

    KnobBoolPtr matchUnits = createKnob<KnobBool>(tr("Match Project Units"));
    matchUnits->setName(kUSDReadParamMatchUnits);
    matchUnits->setHintToolTip(tr("Scales the file to the project's meters per unit, by the file's meters per unit / the project's, "
                                  "on its topmost transformable prims. Camera clipping ranges, focus distances, focal lengths and "
                                  "apertures are rescaled to match. A file that authors no meters per unit is taken to be in "
                                  "centimetres (USD's fallback, 0.01); leave this off if it is really in the project's units. "
                                  "The scene then reports the project's meters per unit."));
    matchUnits->setDefaultValue(false);
    matchUnits->setAnimationEnabled(false);
    page->addKnob(matchUnits);
    _matchUnits = matchUnits;

    KnobButtonPtr useFileMetrics = createKnob<KnobButton>(tr("Use File's Metrics For Project"));
    useFileMetrics->setName(kUSDReadParamUseFileMetrics);
    useFileMetrics->setHintToolTip(tr("Sets the project's Up Axis and Meters Per Unit to the file's, USD's fallbacks (Y, 0.01) for "
                                      "those it does not author, as one undoable edit. Only sets them once: later changes to the "
                                      "file do not follow."));
    useFileMetrics->setAllDimensionsEnabled(false);
    page->addKnob(useFileMetrics);
    _useFileMetrics = useFileMetrics;
}

void
USDRead::refreshUseFileMetricsEnabled()
{
    KnobButtonPtr useFileMetrics = _useFileMetrics.lock();
    KnobFilePtr filename = _filename.lock();

    if (useFileMetrics) {
        useFileMetrics->setAllDimensionsEnabled(filename && !filename->getSingleFilePath().empty());
    }
}

void
USDRead::onKnobsLoaded()
{
    refreshUseFileMetricsEnabled();
}

void
USDRead::useFileMetricsForProject()
{
    KnobFilePtr filename = _filename.lock();
    const std::string path = filename ? filename->getSingleFilePath() : std::string();
    AppInstancePtr app = getApp();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();

    if (path.empty() || !project) {
        return;
    }
    PXR_NS::SdfLayerRefPtr fileLayer;
    {
        PXR_NS::TfErrorMark mark;
        fileLayer = PXR_NS::SdfLayer::FindOrOpen(path);
        const std::string diagnostic = takeUsdErrors(mark);
        if (!fileLayer) {
            setPersistentMessage(eMessageTypeError, diagnostic.empty() ? tr("USD could not read %1.").arg(QString::fromStdString(path)).toStdString() : diagnostic);

            return;
        }
    }
    const SceneMetadata file = SceneOps::readLayerMetadata(fileLayer, SceneMetadata());
    KnobChoicePtr upAxis = std::dynamic_pointer_cast<KnobChoice>(project->getKnobByName("upAxis"));
    KnobDoublePtr metersPerUnit = std::dynamic_pointer_cast<KnobDouble>(project->getKnobByName("metersPerUnit"));
    if (!upAxis || !metersPerUnit || IsolatedJob::refusesWrite(upAxis.get()) || IsolatedJob::refusesWrite(metersPerUnit.get())) {
        return;
    }
    // A button runs inside its own undo step (KnobGui), which this joins; the step records each
    // knob's state before the edit, the same way a Python edit of the project is undone. The
    // refusals were checked above, so recordPythonKnobEdit() never needs the GIL here.
    UndoStepGuard step(app, tr("Use File's Metrics For Project").toStdString());
    if (!recordPythonKnobEdit(upAxis) || !recordPythonKnobEdit(metersPerUnit)) {
        return;
    }
    upAxis->setValue((file.upAxis == SceneMetadata::eUpAxisZ) ? 1 : 0);
    metersPerUnit->setValue(file.metersPerUnit);
}

bool
USDRead::knobChanged(KnobI* k,
                     ValueChangedReasonEnum /*reason*/,
                     ViewSpec /*view*/,
                     double /*time*/,
                     bool /*originatedFromMainThread*/)
{
    KnobButtonPtr reload = _reload.lock();
    KnobFilePtr filename = _filename.lock();
    KnobButtonPtr useFileMetrics = _useFileMetrics.lock();

    if (filename && (k == filename.get())) {
        refreshUseFileMetricsEnabled();

        return false;
    }
    if (useFileMetrics && (k == useFileMetrics.get())) {
        useFileMetricsForProject();

        return true;
    }
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
                     bool matchUpAxis,
                     bool matchUnits,
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
    // this layer, where SceneOps::appendLayer() reads it. An aspect the correction converts
    // carries the project's value instead.
    const PXR_NS::SdfLayerRefPtr& layer = edit.layer;
    const PXR_NS::SdfPath& root = PXR_NS::SdfPath::AbsoluteRootPath();
    PXR_NS::TfToken upAxis;
    if (matchUpAxis) {
        layer->SetField(root, PXR_NS::UsdGeomTokens->upAxis, upAxisToken(edit.upAxis));
    } else if (fileLayer->HasField(root, PXR_NS::UsdGeomTokens->upAxis, &upAxis)) {
        layer->SetField(root, PXR_NS::UsdGeomTokens->upAxis, upAxis);
    }
    double metersPerUnit = 0.;
    if (matchUnits) {
        layer->SetField(root, PXR_NS::UsdGeomTokens->metersPerUnit, edit.metersPerUnit);
    } else if (fileLayer->HasField(root, PXR_NS::UsdGeomTokens->metersPerUnit, &metersPerUnit)) {
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
    const PXR_NS::SdfLayerOffset offset(-frameOffset * timeCodesPerSecond / framesPerSecond);
    layer->InsertSubLayerPath(fileLayer->GetIdentifier());
    layer->SetSubLayerOffset(offset, 0);

    if (matchUpAxis || matchUnits) {
        const SceneMetadata file = SceneOps::readLayerMetadata(fileLayer, SceneMetadata());
        SceneMetricsCorrection correction;
        correction.fromUpAxis = file.upAxis;
        correction.fromMetersPerUnit = file.metersPerUnit;
        correction.toUpAxis = edit.upAxis;
        correction.toMetersPerUnit = edit.metersPerUnit;
        correction.correctUpAxis = matchUpAxis;
        correction.correctUnits = matchUnits;
        correction.sourceToLayer = offset;
        if ((SceneOps::getMetricsCorrectionAngle(correction) != 0.) || (SceneOps::getMetricsCorrectionScale(correction) != 1.)) {
            SceneQueryPool::Lease lease = SceneQueryPool::instance().borrow(_imp->getCorrectionSource(fileLayer, timeCodesPerSecond), errorMessage);
            if (!lease.isValid()) {
                return false;
            }
            SceneOps::authorMetricsCorrection(layer, lease.getStage(), correction);
        }
    }

    {
        std::lock_guard<std::mutex> lock(_imp->fileLayerMutex);
        _imp->fileLayer = fileLayer;
    }
    _imp->updateClosure(fileLayer);
    if (edit.sublayerLinks) {
        std::lock_guard<std::mutex> lock(_imp->fileLayerMutex);
        *edit.sublayerLinks = _imp->links;
    }

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
    KnobBoolPtr matchUpAxisKnob = _matchUpAxis.lock();
    KnobBoolPtr matchUnitsKnob = _matchUnits.lock();
    const bool matchUpAxis = matchUpAxisKnob && matchUpAxisKnob->getValue();
    const bool matchUnits = matchUnitsKnob && matchUnitsKnob->getValue();

    // Nothing is sampled over the project's frame range: the offset is authored once.
    SceneLayerReads reads;
    reads.upstream = SceneLayerMemo::Key::eUpstreamReadNothing;
    reads.frameRange = false;
    if (!emitSceneLayer(ScenePayload(), reads, [this, path, frameOffset, matchUpAxis, matchUnits](const SceneLayerEdit& edit, std::string* error) { return authorLayer(edit, path, frameOffset, matchUpAxis, matchUnits, error); }, outputScene, errorMessage)) {
        setPersistentMessage(eMessageTypeError, *errorMessage);

        return eStatusFailed;
    }

    // The scene's metrics as this node's layer leaves them: the project's for a converted aspect,
    // else the file's, USD's fallbacks where it authors none.
    const SceneMetadata& scene = outputScene->getMetadata();
    AppInstancePtr app = getApp();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();
    if (project) {
        bool upAxisAuthored = false;
        bool metersPerUnitAuthored = false;
        {
            std::lock_guard<std::mutex> lock(_imp->fileLayerMutex);
            if (_imp->fileLayer) {
                const PXR_NS::SdfPath& root = PXR_NS::SdfPath::AbsoluteRootPath();
                upAxisAuthored = _imp->fileLayer->HasField(root, PXR_NS::UsdGeomTokens->upAxis);
                metersPerUnitAuthored = _imp->fileLayer->HasField(root, PXR_NS::UsdGeomTokens->metersPerUnit);
            }
        }
        const QString fallback = tr(" (unauthored, USD fallback)");
        const SceneMetadata::UpAxisEnum projectUpAxis = project->isProjectUpAxisZ() ? SceneMetadata::eUpAxisZ : SceneMetadata::eUpAxisY;
        const double projectMetersPerUnit = project->getProjectMetersPerUnit();
        QStringList differences;
        if (scene.upAxis != projectUpAxis) {
            differences << tr("The file's up axis is %1%2; the project's is %3. Match Project Up Axis converts it.")
                               .arg(upAxisName(scene.upAxis))
                               .arg(upAxisAuthored ? QString() : fallback)
                               .arg(upAxisName(projectUpAxis));
        }
        if (!PXR_NS::UsdGeomLinearUnitsAre(scene.metersPerUnit, projectMetersPerUnit)) {
            differences << tr("The file's meters per unit is %1%2; the project's is %3. Match Project Units converts it.")
                               .arg(scene.metersPerUnit)
                               .arg(metersPerUnitAuthored ? QString() : fallback)
                               .arg(projectMetersPerUnit);
        }
        if (!differences.isEmpty()) {
            setPersistentMessage(eMessageTypeWarning, differences.join(QLatin1Char('\n')).toStdString());

            return eStatusOK;
        }
    }
    clearPersistentMessage(false);

    return eStatusOK;
} // USDRead::renderScene

NATRON_NAMESPACE_EXIT
