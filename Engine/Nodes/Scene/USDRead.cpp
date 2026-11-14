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
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <QString>
#include <QStringList>

#include "Engine/AppInstance.h"
#include "Engine/ContentHasher.h"
#include "Engine/IsolatedJob.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/SceneFileSource.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/SceneWriteBarrier.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"
#include "Engine/Project.h"
#include "Engine/PythonUndoCommands.h"
#include "Engine/SceneLoadSet.h"
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

struct RootPrims {
    std::vector<std::string> names;
    // The identifiers of the layers of the file's sublayer stack, sorted.
    std::vector<std::string> layers;
};

/**
 * The names of the prims some layer of layer's sublayer stack defines at its root, sorted. A
 * root prim the file only overs is left out: what defines it comes from elsewhere in the graph,
 * and its draw is not the file's to set. Only sublayers are followed: references and payloads
 * cannot add a root prim, and following them would open payload files.
 **/
RootPrims
definedRootPrims(const PXR_NS::SdfLayerRefPtr& layer)
{
    RootPrims roots;

    if (!layer) {
        return roots;
    }
    std::set<std::string> visited;
    std::set<std::string> found;
    std::vector<PXR_NS::SdfLayerRefPtr> pending(1, layer);
    PXR_NS::TfErrorMark mark;
    while (!pending.empty()) {
        const PXR_NS::SdfLayerRefPtr current = pending.back();
        pending.pop_back();
        if (!visited.insert(current->GetIdentifier()).second) {
            continue;
        }
        for (const PXR_NS::SdfPrimSpecHandle& spec : current->GetRootPrims()) {
            if (spec && (spec->GetSpecifier() == PXR_NS::SdfSpecifierDef)) {
                found.insert(spec->GetName());
            }
        }
        for (const std::string& subLayerPath : current->GetSubLayerPaths()) {
            const PXR_NS::SdfLayerRefPtr subLayer = PXR_NS::SdfLayer::FindOrOpenRelativeToLayer(current, subLayerPath);
            if (subLayer) {
                pending.push_back(subLayer);
            }
        }
    }
    // A missing sublayer is the stage's error to report when it composes.
    mark.Clear();
    roots.names.assign(found.begin(), found.end());
    roots.layers.assign(visited.begin(), visited.end());

    return roots;
}
} // anonymous namespace

struct USDRead::Implementation {
    SceneFileSource source;
    // What the metrics correction reads the file through: a payload of one layer sublayering
    // sourceFileLayer at the file's rate, so a pooled stage composes it (payloads included) in the
    // file's own time codes, and a toggle flip or a project change finds that stage again.
    std::mutex correctionMutex;
    PXR_NS::SdfLayerRefPtr sourceFileLayer;
    U64 sourceDigest;
    double sourceTimeCodesPerSecond;
    ScenePayload correctionSource;
    // What getInitialDrawRules() last read of the file, at the path and digest it read it at: each
    // evaluation asks again, and the file only changes with the digest.
    std::mutex rootPrimsMutex;
    bool rootPrimsRead = false;
    std::string rootPrimsPath;
    U64 rootPrimsDigest = 0;
    RootPrims rootPrims;

    Implementation()
        : sourceDigest(0)
        , sourceTimeCodesPerSecond(0.)
    {
    }

    RootPrims getRootPrims(const std::string& path)
    {
        const U64 digest = source.getDigest();
        {
            std::lock_guard<std::mutex> lock(rootPrimsMutex);
            if (rootPrimsRead && (rootPrimsPath == path) && (rootPrimsDigest == digest)) {
                return rootPrims;
            }
        }
        RootPrims read;
        {
            SceneWriteBarrier::ReadScope readScope;
            std::string error;
            read = definedRootPrims(SceneFileSource::openLayer(path, &error));
        }
        std::lock_guard<std::mutex> lock(rootPrimsMutex);
        rootPrimsRead = true;
        rootPrimsPath = path;
        rootPrimsDigest = digest;
        rootPrims = read;

        return read;
    }

    ScenePayload getCorrectionSource(const PXR_NS::SdfLayerRefPtr& fileLayer,
                                     double timeCodesPerSecond)
    {
        const U64 digest = source.getDigest();
        std::lock_guard<std::mutex> lock(correctionMutex);

        if ((sourceFileLayer != fileLayer) || (sourceDigest != digest) || (sourceTimeCodesPerSecond != timeCodesPerSecond)) {
            PXR_NS::SdfLayerRefPtr wrapper = SceneOps::createNodeLayer("USDRead-source");
            wrapper->SetTimeCodesPerSecond(timeCodesPerSecond);
            wrapper->InsertSubLayerPath(fileLayer->GetIdentifier());
            ContentHasher h;
            h.appendString(fileLayer->GetIdentifier());
            h.appendU64(digest);
            h.appendDouble(timeCodesPerSecond);
            correctionSource = SceneOps::appendLayer(ScenePayload(), wrapper, h.finalize());
            sourceFileLayer = fileLayer;
            sourceDigest = digest;
            sourceTimeCodesPerSecond = timeCodesPerSecond;
        }

        return correctionSource;
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
    , _initialDraw()
    , _levels()
    , _toKind()
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
    page->addKnob(filename);
    _filename = filename;

    KnobButtonPtr reload = filename->createReloadButton(kUSDReadParamReload,
                                                        tr("Reads the file again, and every file it sublayers, references or loads as a "
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

    // The draw knobs stay out of the node's hash, so they never key the layer: a change of them
    // re-evaluates the node into the same stack with other draw rules.
    KnobChoicePtr initialDraw = createKnob<KnobChoice>(tr("Initial Draw"));
    initialDraw->setName(kUSDReadParamInitialDraw);
    initialDraw->setHintToolTip(tr("How viewers draw the file's top-level prims, the prims it defines at its root, until "
                                   "a draw state set in a viewer or by a node further down says otherwise. Prims another "
                                   "node adds to the scene are not affected. Only viewers read this: renders and "
                                   "written files are the same whatever it is set to."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kUSDReadInitialDrawBBox, tr("BBox").toStdString(),
                                       tr("Each top-level prim draws as one box.").toStdString()));
        options.push_back(ChoiceOption(kUSDReadInitialDrawLevels, tr("Levels").toStdString(),
                                       tr("The hierarchy opens Levels deep below each top-level prim, and the prims there "
                                          "draw as boxes.")
                                           .toStdString()));
        options.push_back(ChoiceOption(kUSDReadInitialDrawToKind, tr("To Kind").toStdString(),
                                       tr("The hierarchy opens below each top-level prim down to the first prim of the "
                                          "kind To Kind names, or of a kind derived from it, which draws as a box.")
                                           .toStdString()));
        options.push_back(ChoiceOption(kUSDReadInitialDrawFull, tr("Full").toStdString(),
                                       tr("The top-level prims draw in full, as far as their payloads are loaded.").toStdString()));
        options.push_back(ChoiceOption(kUSDReadInitialDrawFullLoad, tr("Full + Load").toStdString(),
                                       tr("The top-level prims draw in full, and viewers load every payload below them.").toStdString()));
        initialDraw->populateChoices(options);
    }
    initialDraw->setDefaultValue(0);
    initialDraw->setAnimationEnabled(false);
    initialDraw->setHashExempt(true);
    page->addKnob(initialDraw);
    _initialDraw = initialDraw;

    KnobIntPtr levels = createKnob<KnobInt>(tr("Levels"));
    levels->setName(kUSDReadParamLevels);
    levels->setHintToolTip(tr("How many levels of the hierarchy open below each top-level prim before prims draw as "
                              "boxes: 1 boxes the top-level prims' children. An unloaded payload on the way draws as a "
                              "box where it is."));
    levels->setDefaultValue(1);
    levels->setMinimum(1);
    levels->setDisplayMinimum(1);
    levels->setDisplayMaximum(10);
    levels->setAnimationEnabled(false);
    levels->setHashExempt(true);
    page->addKnob(levels);
    _levels = levels;

    KnobChoicePtr toKind = createKnob<KnobChoice>(tr("To Kind"));
    toKind->setName(kUSDReadParamToKind);
    toKind->setHintToolTip(tr("The kind the hierarchy opens down to below each top-level prim: the first prim of this "
                              "kind, or of a kind derived from it, draws as a box. An Assembly is a Group, and Groups and "
                              "Components are Models. An unloaded payload on the way draws as a box where it is."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(PXR_NS::KindTokens->component.GetString(), tr("Component").toStdString(), ""));
        options.push_back(ChoiceOption(PXR_NS::KindTokens->assembly.GetString(), tr("Assembly").toStdString(), ""));
        options.push_back(ChoiceOption(PXR_NS::KindTokens->group.GetString(), tr("Group").toStdString(), ""));
        options.push_back(ChoiceOption(PXR_NS::KindTokens->model.GetString(), tr("Model").toStdString(), ""));
        options.push_back(ChoiceOption(PXR_NS::KindTokens->subcomponent.GetString(), tr("Subcomponent").toStdString(), ""));
        toKind->populateChoices(options);
    }
    toKind->setDefaultValue(0);
    toKind->setAnimationEnabled(false);
    toKind->setHashExempt(true);
    page->addKnob(toKind);
    _toKind = toKind;

    refreshInitialDrawKnobs();
}

void
USDRead::refreshInitialDrawKnobs()
{
    KnobChoicePtr initialDraw = _initialDraw.lock();
    KnobIntPtr levels = _levels.lock();
    KnobChoicePtr toKind = _toKind.lock();

    if (!initialDraw) {
        return;
    }
    const std::string mode = initialDraw->getActiveEntry().id;
    if (levels) {
        levels->setSecret(mode != kUSDReadInitialDrawLevels);
    }
    if (toKind) {
        toKind->setSecret(mode != kUSDReadInitialDrawToKind);
    }
}

SceneDrawRules
USDRead::getInitialDrawRules(const std::string& path) const
{
    const RootPrims roots = _imp->getRootPrims(path);
    if (roots.names.empty()) {
        return SceneDrawRules();
    }

    SceneDrawRule rule;
    rule.layers = roots.layers;
    for (const std::string& name : roots.names) {
        if (!rule.pattern.empty()) {
            rule.pattern += ' ';
        }
        rule.pattern += '/';
        rule.pattern += name;
    }
    rule.origin = getScriptName_mt_safe();

    KnobChoicePtr initialDraw = _initialDraw.lock();
    const std::string mode = initialDraw ? initialDraw->getActiveEntry().id : std::string(kUSDReadInitialDrawBBox);
    if (mode == kUSDReadInitialDrawLevels) {
        KnobIntPtr levels = _levels.lock();
        rule.state = eSceneDrawStateBBox;
        rule.levels = (std::max)(1, levels ? levels->getValue() : 1);
    } else if (mode == kUSDReadInitialDrawToKind) {
        KnobChoicePtr toKind = _toKind.lock();
        rule.state = eSceneDrawStateBBox;
        rule.toKind = toKind ? toKind->getActiveEntry().id : std::string();
        if (rule.toKind.empty()) {
            rule.toKind = PXR_NS::KindTokens->component.GetString();
        }
    } else if (mode == kUSDReadInitialDrawFull) {
        rule.state = eSceneDrawStateFull;
    } else if (mode == kUSDReadInitialDrawFullLoad) {
        rule.state = eSceneDrawStateLoadAll;
    } else {
        rule.state = eSceneDrawStateBBox;
    }

    return SceneDrawRules(1, rule);
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
    refreshInitialDrawKnobs();
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
        // Runs on the main thread, and this needs only the root's metadata: no layer registered
        // for the file, no read of its whole content where the format can avoid it.
        fileLayer = PXR_NS::SdfLayerRefPtr(PXR_NS::SdfLayer::Find(path));
        if (!fileLayer) {
            fileLayer = PXR_NS::SdfLayer::OpenAsAnonymous(path, true);
        }
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
    KnobChoicePtr initialDraw = _initialDraw.lock();

    if (initialDraw && (k == initialDraw.get())) {
        refreshInitialDrawKnobs();

        return true;
    }
    if (filename && (k == filename.get())) {
        refreshUseFileMetricsEnabled();
        stampClosure();

        return true;
    }
    if (useFileMetrics && (k == useFileMetrics.get())) {
        useFileMetricsForProject();

        return true;
    }
    if (!reload || (k != reload.get())) {
        return false;
    }
    std::vector<std::string> reloaded;
    std::string error;
    if (!_imp->source.reload(filename ? filename->getSingleFilePath() : std::string(), &error, &reloaded)) {
        setPersistentMessage(eMessageTypeError, tr("Reload failed: %1").arg(QString::fromStdString(error)).toStdString());
    } else {
        clearPersistentMessage(false);
    }
    purgeCaches();
    // Same as NativeRead: the Reload bracket hashed this node before purgeCaches() moved its
    // hash inputs.
    NodePtr node = getNode();
    if (node) {
        node->markHashDirty();
    }
    SceneFileSource::reloadOtherReaders(reloaded, filename ? filename->getResolvedPath() : std::string(), this);

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
    _imp->source.purgeStamps();
    invalidateSceneLayer();
    clearActionsCache();
} // USDRead::purgeCaches

void
USDRead::purgeCachesKeepingFileStamps()
{
    // The layers stay as loaded; the next evaluation authors from them again.
    invalidateSceneLayer();
    clearActionsCache();
}

bool
USDRead::refreshFileDependencies(const std::vector<std::string>& paths)
{
    if (!_imp->source.refresh(paths)) {
        return false;
    }
    invalidateSceneLayer();
    clearActionsCache();

    return true;
}

void
USDRead::onEffectCreated(bool /*mayCreateFileDialog*/,
                         const CreateNodeArgs& /*args*/)
{
    // After the knobs are loaded, project or preset, and before any render.
    stampClosure();
}

void
USDRead::stampClosure()
{
    KnobFilePtr filename = _filename.lock();

    _imp->source.load(filename ? filename->getSingleFilePath() : std::string());
    NodePtr node = getNode();
    if (node) {
        node->markHashDirty();
    }
} // USDRead::stampClosure

void
USDRead::appendNodeContentHash(ContentHasher& h) const
{
    // The file's own stamp is in the File knob's hash; this covers every file it brings in.
    h.appendU64(_imp->source.getDigest());
}

bool
USDRead::authorLayer(const SceneLayerEdit& edit,
                     const std::string& path,
                     double frameOffset,
                     bool matchUpAxis,
                     bool matchUnits,
                     const SceneLoadSetPtr& loadSet,
                     std::string* errorMessage)
{
    // One layer per file, shared with every other consumer through USD's layer registry, and
    // never edited here: Reload reloads it in place behind SceneWriteBarrier, and every stage
    // composing it follows.
    PXR_NS::SdfLayerRefPtr fileLayer = SceneFileSource::openLayer(path, errorMessage);
    if (!fileLayer) {
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
    std::string renderSettingsPrimPath;
    if (fileLayer->HasField(root, PXR_NS::UsdRenderTokens->renderSettingsPrimPath, &renderSettingsPrimPath)) {
        layer->SetField(root, PXR_NS::UsdRenderTokens->renderSettingsPrimPath, renderSettingsPrimPath);
    }
    // The same rate as the file, so USD applies no rate conversion across the sublayer arc and
    // the offset below is the whole time mapping.
    const double timeCodesPerSecond = SceneFileSource::getTimeCodesPerSecond(fileLayer);
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
            SceneQueryPool::Lease lease = SceneQueryPool::instance().borrow(_imp->getCorrectionSource(fileLayer, timeCodesPerSecond), loadSet, errorMessage);
            if (!lease.isValid()) {
                return false;
            }
            SceneOps::authorMetricsCorrection(layer, lease.getStage(), correction);
        }
    }

    _imp->source.hold(fileLayer);
    if (edit.sublayerLinks) {
        *edit.sublayerLinks = _imp->source.getLinks();
    }

    return true;
} // USDRead::authorLayer

StatusEnum
USDRead::renderScene(const SceneRenderActionArgs& args,
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

    if (_imp->source.walk(path)) {
        NodePtr node = getNode();
        if (node) {
            node->markHashDirty();
        }
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
    // The correction reads the prims the evaluation loads, wherever they are, so the layer is
    // authored per load set and any load change reaches it.
    const SceneLoadSetPtr loadSet = args.context.loadSet;
    if (matchUpAxis || matchUnits) {
        ContentHasher loadHash;
        loadHash.appendBool(args.context.includesEverything());
        if (!args.context.includesEverything()) {
            loadHash.appendU64(loadSet->getHash());
        }
        reads.upstream = SceneLayerMemo::Key::eUpstreamReadFacts;
        reads.factsDigest = loadHash.finalize();
        if (args.context.reach) {
            args.context.reach->add(std::vector<std::string>(1, "/"));
        }
    }
    if (!emitSceneLayer(ScenePayload(), reads, [this, path, frameOffset, matchUpAxis, matchUnits, loadSet](const SceneLayerEdit& edit, std::string* error) { return authorLayer(edit, path, frameOffset, matchUpAxis, matchUnits, loadSet, error); }, outputScene, errorMessage)) {
        setPersistentMessage(eMessageTypeError, *errorMessage);

        return eStatusFailed;
    }
    *outputScene = outputScene->withDrawRules(getInitialDrawRules(path));

    // The scene's metrics as this node's layer leaves them: the project's for a converted aspect,
    // else the file's, USD's fallbacks where it authors none.
    const SceneMetadata& scene = outputScene->getMetadata();
    AppInstancePtr app = getApp();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();
    if (project) {
        const SceneMetadata::UpAxisEnum projectUpAxis = project->isProjectUpAxisZ() ? SceneMetadata::eUpAxisZ : SceneMetadata::eUpAxisY;
        const double projectMetersPerUnit = project->getProjectMetersPerUnit();
        // Whether the file authors each aspect, so a difference from a USD fallback says so.
        bool upAxisAuthored = false;
        bool metersPerUnitAuthored = false;
        std::string openError;
        PXR_NS::SdfLayerRefPtr fileLayer = SceneFileSource::openLayer(path, &openError);
        if (fileLayer) {
            const PXR_NS::SdfPath& root = PXR_NS::SdfPath::AbsoluteRootPath();
            upAxisAuthored = fileLayer->HasField(root, PXR_NS::UsdGeomTokens->upAxis);
            metersPerUnitAuthored = fileLayer->HasField(root, PXR_NS::UsdGeomTokens->metersPerUnit);
        }
        const QString fallback = tr(" (unauthored, USD fallback)");
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
