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

#include "USDWrite.h"

#include <cmath>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include "Engine/AppInstance.h"
#include "Engine/ChoiceOption.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/ScenePayloadImpl.h"
#include "Engine/Nodes/Scene/SceneWriteBarrier.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"
#include "Engine/Project.h"

NATRON_NAMESPACE_ENTER

namespace {
void
setSecretAndDisabled(const KnobIPtr& knob,
                     bool secret)
{
    if (!knob) {
        return;
    }
    knob->setSecret(secret);
    knob->setAllDimensionsEnabled(!secret);
}

// The messages of the USD errors posted since mark was set, which are cleared.
std::string
takeUsdErrors(PXR_NS::TfErrorMark& mark)
{
    std::string text;

    for (PXR_NS::TfErrorMark::Iterator it = mark.GetBegin(); it != mark.GetEnd(); ++it) {
        if (!text.empty()) {
            text += "; ";
        }
        text += it->GetCommentary();
    }
    mark.Clear();

    return text;
}

// Whether an authored asset path names a file: not empty, not an anonymous layer, and not a URI
// whose scheme an asset resolver handles.
bool
isFilesystemPath(const std::string& path)
{
    if (path.empty() || PXR_NS::SdfLayer::IsAnonymousLayerIdentifier(path)) {
        return false;
    }
    const std::size_t colon = path.find(':');

    return (colon == std::string::npos) || (path.find('/') < colon);
}

// path made absolute, against the working directory when relative. Scene nodes author anchored
// paths (SceneNodeAuthoring::getAnchoredFilePath), so this only cleans those up.
std::string
absolutePath(const std::string& path)
{
    const QString qpath = QString::fromStdString(path);

    return QDir::cleanPath(QDir::current().absoluteFilePath(qpath)).toStdString();
}

// The absolute path of the file an authored asset path names, or an empty string when it names none.
QString
referencedFilePath(const std::string& assetPath)
{
    std::string path;
    PXR_NS::SdfLayer::FileFormatArguments arguments;

    if (!PXR_NS::SdfLayer::SplitIdentifier(assetPath, &path, &arguments) || !isFilesystemPath(path)) {
        return QString();
    }

    return QString::fromStdString(absolutePath(path));
}

/**
 * @brief Removes the files beside outputInfo that carry its sidecar names, name_layerN.ext as the
 * layered write names them, except those in keep: what an earlier write of more layer runs left,
 * which nothing written references. A file that cannot be removed stays, as the written files do
 * not depend on it.
 **/
void
removeStaleSidecars(const QFileInfo& outputInfo,
                    const std::set<QString>& keep)
{
    const QDir dir(outputInfo.absolutePath());
    const QRegularExpression sidecar(QString::fromUtf8("^") + QRegularExpression::escape(outputInfo.completeBaseName()) + QString::fromUtf8("_layer[1-9][0-9]*\\.") + QRegularExpression::escape(outputInfo.suffix()) + QString::fromUtf8("$"));
    const QStringList names = dir.entryList(QDir::Files | QDir::Hidden | QDir::System);

    for (const QString& name : names) {
        if (!sidecar.match(name).hasMatch()) {
            continue;
        }
        const QString path = QDir::cleanPath(dir.absoluteFilePath(name));
        if (keep.count(path) == 0) {
            QFile::remove(path);
        }
    }
}

/**
 * @brief How an asset path reads from a file in outputDir: relative to it, or absolute. Paths
 * that do not name a file are kept as they are, and so are a layer identifier's file format
 * arguments.
 **/
std::string
rewriteAssetPath(const std::string& assetPath,
                 const QString& outputDir,
                 USDWrite::PathModeEnum pathMode)
{
    std::string path;
    PXR_NS::SdfLayer::FileFormatArguments arguments;

    if (!PXR_NS::SdfLayer::SplitIdentifier(assetPath, &path, &arguments) || !isFilesystemPath(path)) {
        return assetPath;
    }
    std::string rewritten = absolutePath(path);
    if (pathMode == USDWrite::ePathModeRelative) {
        const QString relative = QDir(outputDir).relativeFilePath(QString::fromStdString(rewritten));
        // Across file systems with no common root a path cannot be made relative: it stays absolute.
        if (!relative.isEmpty() && QDir::isRelativePath(relative)) {
            // ./ anchors the path to the file; a bare name would also be looked up on the resolver's
            // search path.
            rewritten = (relative.startsWith(QString::fromUtf8("..")) ? relative : QString::fromUtf8("./") + relative).toStdString();
        }
    }

    return arguments.empty() ? rewritten : PXR_NS::SdfLayer::CreateIdentifier(rewritten, arguments);
}

/**
 * @brief One layer of the scene's layer stack laid out flat: either the content of an anonymous
 * layer, without its sublayers, or a file layer the scene brings in as a sublayer, which layered
 * mode keeps a sublayer of what it writes. toRoot maps the layer's times to the composed stage's
 * root.
 **/
struct StackItem {
    PXR_NS::SdfLayerRefPtr layer;
    PXR_NS::SdfLayerOffset toRoot;
    bool external = false;
};

/**
 * @brief Appends the layer stack of source to *items, strongest first, as composition orders it:
 * source's own content, then each sublayer's stack in turn. A file layer is one item; what it
 * sublayers in turn stays its own business. Anonymous layers are copied without their sublayers.
 **/
void
appendLayerStack(const PXR_NS::SdfLayerHandle& source,
                 const PXR_NS::SdfLayerOffset& toRoot,
                 std::vector<StackItem>* items)
{
    PXR_NS::SdfLayerRefPtr copy = PXR_NS::SdfLayer::CreateAnonymous("natron-usdwrite-copy");

    copy->TransferContent(source);
    copy->SetSubLayerPaths(std::vector<std::string>());
    items->push_back(StackItem { copy, toRoot, false });

    const std::vector<std::string> paths = source->GetSubLayerPaths();
    const PXR_NS::SdfLayerOffsetVector offsets = source->GetSubLayerOffsets();
    const double sourceTcps = source->GetTimeCodesPerSecond();
    for (std::size_t i = 0; i < paths.size(); ++i) {
        const PXR_NS::SdfLayerRefPtr sublayer = PXR_NS::SdfLayer::FindOrOpenRelativeToLayer(source, paths[i]);
        if (!sublayer) {
            continue;
        }
        const PXR_NS::SdfLayerOffset authored = (i < offsets.size()) ? offsets[i] : PXR_NS::SdfLayerOffset();
        // What composition does: the authored offset, then the scale between the two layers' time
        // codes per second.
        const PXR_NS::SdfLayerOffset toSource(authored.GetOffset(), authored.GetScale() * sourceTcps / sublayer->GetTimeCodesPerSecond());
        const PXR_NS::SdfLayerOffset sublayerToRoot = toRoot * toSource;
        if (sublayer->IsAnonymous()) {
            appendLayerStack(sublayer, sublayerToRoot, items);
        } else {
            items->push_back(StackItem { sublayer, sublayerToRoot, true });
        }
    }
}

/**
 * @brief The sublayer offset that, authored in a layer of time codes per second rootTcps, maps
 * layer's times as toRoot does: composition scales an authored offset by the ratio of the two
 * layers' time codes per second.
 **/
PXR_NS::SdfLayerOffset
authoredOffset(const PXR_NS::SdfLayerOffset& toRoot,
               const PXR_NS::SdfLayerRefPtr& layer,
               double rootTcps)
{
    return PXR_NS::SdfLayerOffset(toRoot.GetOffset(), toRoot.GetScale() * layer->GetTimeCodesPerSecond() / rootTcps);
}

/**
 * @brief The anonymous layers run, strongest first, flattened into one layer of time codes per
 * second rootTcps, their times mapped to the root's. Null when USD cannot.
 **/
PXR_NS::SdfLayerRefPtr
flattenRun(const std::vector<StackItem>& run,
           double rootTcps)
{
    PXR_NS::SdfLayerRefPtr root = PXR_NS::SdfLayer::CreateAnonymous("natron-usdwrite-root");

    root->SetTimeCodesPerSecond(rootTcps);
    std::vector<std::string> paths;
    for (const StackItem& item : run) {
        paths.push_back(item.layer->GetIdentifier());
    }
    root->SetSubLayerPaths(paths);
    for (std::size_t i = 0; i < run.size(); ++i) {
        root->SetSubLayerOffset(authoredOffset(run[i].toRoot, run[i].layer, rootTcps), (int)i);
    }
    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(root, PXR_NS::SdfLayerHandle(), PXR_NS::UsdStage::LoadNone);
    if (!stage) {
        return PXR_NS::SdfLayerRefPtr();
    }

    // The scene nodes' layers author anchored asset paths, kept as they are; a file's are resolved
    // against the file. The written layers make them relative afterwards when asked.
    return PXR_NS::UsdUtilsFlattenLayerStack(stage, [](const PXR_NS::SdfLayerHandle& sourceLayer, const std::string& assetPath) {
        if ( sourceLayer && !sourceLayer->IsAnonymous() ) {
            return PXR_NS::UsdUtilsFlattenLayerStackResolveAssetPath(sourceLayer, assetPath);
        }

        return assetPath; }, "natron-usdwrite");
}

/**
 * @brief Removes layer's time samples outside [startTime, endTime], first adding samples at the
 * bounds with the value the attribute had there, interpolated, so the clipped animation matches
 * the original over the range.
 **/
void
clipTimeSamples(const PXR_NS::SdfLayerRefPtr& layer,
                double startTime,
                double endTime)
{
    std::vector<PXR_NS::SdfPath> animated;

    layer->Traverse(PXR_NS::SdfPath::AbsoluteRootPath(), [&](const PXR_NS::SdfPath& path) {
        if (path.IsPropertyPath() && (layer->GetNumTimeSamplesForPath(path) > 0)) {
            const std::set<double> times = layer->ListTimeSamplesForPath(path);
            if ((*times.begin() < startTime) || (*times.rbegin() > endTime)) {
                animated.push_back(path);
            }
        }
    });
    if (animated.empty()) {
        return;
    }

    struct Bound {
        PXR_NS::SdfPath path;
        double time;
        PXR_NS::VtValue value;
    };
    std::vector<Bound> bounds;
    {
        // The layer composed on its own interpolates its samples as a stage reading the file will.
        PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(layer, PXR_NS::UsdStage::LoadNone);
        for (const PXR_NS::SdfPath& path : animated) {
            const std::set<double> times = layer->ListTimeSamplesForPath(path);
            const PXR_NS::UsdAttribute attribute = stage ? stage->GetAttributeAtPath(path) : PXR_NS::UsdAttribute();
            for (int i = 0; i < 2; ++i) {
                const double time = (i == 0) ? startTime : endTime;
                if ((i == 0) ? (*times.begin() >= startTime) : (*times.rbegin() <= endTime)) {
                    continue;
                }
                PXR_NS::VtValue value;
                if (!attribute || !attribute.Get(&value, PXR_NS::UsdTimeCode(time)) || value.IsEmpty()) {
                    // Not on the stage, as under an inactive prim: the sample in force at time
                    // holds.
                    double lower = time;
                    double upper = time;
                    if (!layer->GetBracketingTimeSamplesForPath(path, time, &lower, &upper) || !layer->QueryTimeSample(path, (lower <= time) ? lower : upper, &value)) {
                        continue;
                    }
                }
                bounds.push_back(Bound { path, time, value });
            }
        }
    }

    for (const Bound& bound : bounds) {
        layer->SetTimeSample(bound.path, bound.time, bound.value);
    }
    for (const PXR_NS::SdfPath& path : animated) {
        const std::set<double> times = layer->ListTimeSamplesForPath(path);
        for (double time : times) {
            if ((time < startTime) || (time > endTime)) {
                layer->EraseTimeSample(path, time);
            }
        }
    }
} // clipTimeSamples

void
authorStageMetadata(const PXR_NS::SdfLayerRefPtr& layer,
                    const SceneMetadata& metadata,
                    double startTime,
                    double endTime,
                    double framesPerSecond)
{
    const PXR_NS::SdfPath& rootPath = PXR_NS::SdfPath::AbsoluteRootPath();

    layer->SetField(rootPath, PXR_NS::UsdGeomTokens->upAxis,
                    (metadata.upAxis == SceneMetadata::eUpAxisZ) ? PXR_NS::UsdGeomTokens->z : PXR_NS::UsdGeomTokens->y);
    layer->SetField(rootPath, PXR_NS::UsdGeomTokens->metersPerUnit, metadata.metersPerUnit);
    if (metadata.defaultPrim.empty()) {
        layer->ClearDefaultPrim();
    } else {
        layer->SetDefaultPrim(PXR_NS::TfToken(metadata.defaultPrim));
    }
    layer->SetTimeCodesPerSecond(metadata.timeCodesPerSecond);
    layer->SetFramesPerSecond(framesPerSecond);
    layer->SetStartTimeCode(startTime);
    layer->SetEndTimeCode(endTime);
}
} // anonymous namespace

USDWrite::USDWrite(NodePtr node)
    : NativeEffectBase(node)
    , _writeCount(0)
{
}

USDWrite::~USDWrite()
{
}

const NodeSpec*
USDWrite::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

void
USDWrite::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    KnobOutputFilePtr filename = createKnob<KnobOutputFile>(tr("File"));
    filename->setName(kUSDWriteParamFile);
    filename->turnOffSequences();
    filename->setHintToolTip(tr("The USD file to write: .usda for text, .usdc for binary, .usd for binary under the name "
                                "every USD application opens. An existing file is overwritten."));
    page->addKnob(filename);
    _filename = filename;

    KnobChoicePtr exportMode = createKnob<KnobChoice>(tr("Export Mode"));
    exportMode->setName(kUSDWriteParamExportMode);
    exportMode->setHintToolTip(tr("What goes into the file."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("layered", tr("Layered").toStdString(),
                                       tr("Flattens what the nodes of this project authored into the file, and keeps the USD "
                                          "files the scene reads (USDRead's) as sublayers, so they are referred to rather "
                                          "than copied. Each file keeps its strength: what was authored below one goes to "
                                          "a file of its own beside the written one (name_layer1.usda, ...), sublayered "
                                          "below it. Files of those names that the write does not produce, left by an earlier write, are "
                                          "removed.")
                                           .toStdString()));
        options.push_back(ChoiceOption("flattened", tr("Flattened").toStdString(),
                                       tr("Bakes the whole composed scene, the files it reads included, into the file, which "
                                          "then stands on its own.")
                                           .toStdString()));
        exportMode->populateChoices(options);
    }
    exportMode->setDefaultValue(eExportModeLayered);
    exportMode->setAnimationEnabled(false);
    page->addKnob(exportMode);
    _exportMode = exportMode;

    KnobChoicePtr pathMode = createKnob<KnobChoice>(tr("Asset Paths"));
    pathMode->setName(kUSDWriteParamPathMode);
    pathMode->setHintToolTip(tr("How the file refers to other files: the sublayers it reads in Layered mode, and the "
                                "textures and other assets of the scene."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("relative", tr("Relative").toStdString(),
                                       tr("Relative to the written file's directory, so the files can move together. A path "
                                          "that cannot be made relative is written absolute.")
                                           .toStdString()));
        options.push_back(ChoiceOption("absolute", tr("Absolute").toStdString(),
                                       tr("Absolute, so the written file can move on its own.").toStdString()));
        pathMode->populateChoices(options);
    }
    pathMode->setDefaultValue(ePathModeRelative);
    pathMode->setAnimationEnabled(false);
    page->addKnob(pathMode);
    _pathMode = pathMode;

    KnobChoicePtr frameRange = createKnob<KnobChoice>(tr("Frame Range"));
    frameRange->setName(kUSDWriteParamFrameRange);
    frameRange->setHintToolTip(tr("The frames whose time samples are written. Samples outside it are dropped, with samples "
                                  "added at its ends so the animation over it is unchanged."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("project", tr("Project").toStdString(),
                                       tr("The project's frame range.").toStdString()));
        options.push_back(ChoiceOption("manual", tr("Manual").toStdString(),
                                       tr("First Frame to Last Frame.").toStdString()));
        frameRange->populateChoices(options);
    }
    frameRange->setDefaultValue(eFrameRangeProject);
    frameRange->setAnimationEnabled(false);
    page->addKnob(frameRange);
    _frameRange = frameRange;

    KnobIntPtr firstFrame = createKnob<KnobInt>(tr("First Frame"));
    firstFrame->setName(kUSDWriteParamFirstFrame);
    firstFrame->setHintToolTip(tr("The first frame to write, when the frame range is Manual."));
    firstFrame->setDefaultValue(0);
    firstFrame->setAnimationEnabled(false);
    firstFrame->disableSlider();
    firstFrame->setAddNewLine(false);
    page->addKnob(firstFrame);
    _firstFrame = firstFrame;

    KnobIntPtr lastFrame = createKnob<KnobInt>(tr("Last Frame"));
    lastFrame->setName(kUSDWriteParamLastFrame);
    lastFrame->setHintToolTip(tr("The last frame to write, when the frame range is Manual."));
    lastFrame->setDefaultValue(0);
    lastFrame->setAnimationEnabled(false);
    lastFrame->disableSlider();
    page->addKnob(lastFrame);
    _lastFrame = lastFrame;

    refreshFrameRangeKnobs();
} // USDWrite::initializeKnobs

void
USDWrite::onKnobsLoaded()
{
    refreshFrameRangeKnobs();
}

void
USDWrite::refreshFrameRangeKnobs()
{
    KnobChoicePtr frameRange = _frameRange.lock();
    const bool manual = frameRange && (frameRange->getValue() == eFrameRangeManual);

    setSecretAndDisabled(_firstFrame.lock(), !manual);
    setSecretAndDisabled(_lastFrame.lock(), !manual);
}

bool
USDWrite::knobChanged(KnobI* k,
                      ValueChangedReasonEnum reason,
                      ViewSpec /*view*/,
                      double /*time*/,
                      bool /*originatedFromMainThread*/)
{
    if (reason == eValueChangedReasonTimeChanged) {
        return false;
    }
    KnobChoicePtr frameRange = _frameRange.lock();
    if (!frameRange || (k != frameRange.get())) {
        return false;
    }
    KnobIntPtr firstFrame = _firstFrame.lock();
    KnobIntPtr lastFrame = _lastFrame.lock();
    // Switching to Manual starts from the project's range rather than from 0 to 0.
    if ((frameRange->getValue() == eFrameRangeManual) && firstFrame && lastFrame && (firstFrame->getValue() == 0) && (lastFrame->getValue() == 0)) {
        double first = 0.;
        double last = 0.;
        getApp()->getFrameRange(&first, &last);
        firstFrame->setValue((int)std::floor(first));
        lastFrame->setValue((int)std::floor(last));
    }
    refreshFrameRangeKnobs();

    return true;
}

void
USDWrite::getFrameRange(double* first,
                        double* last)
{
    KnobChoicePtr frameRange = _frameRange.lock();
    KnobIntPtr firstFrame = _firstFrame.lock();
    KnobIntPtr lastFrame = _lastFrame.lock();

    if (frameRange && (frameRange->getValue() == eFrameRangeManual) && firstFrame && lastFrame) {
        *first = firstFrame->getValue();
        *last = lastFrame->getValue();
    } else {
        getApp()->getFrameRange(first, last);
    }
}

bool
USDWrite::dependsOnProjectFrameRange() const
{
    KnobChoicePtr frameRange = _frameRange.lock();

    return !frameRange || (frameRange->getValue() != eFrameRangeManual);
}

StatusEnum
USDWrite::renderScene(const SceneRenderActionArgs& args,
                      ScenePayload* outputScene,
                      std::string* /*errorMessage*/)
{
    *outputScene = args.getInputScene(0);

    return eStatusOK;
}

StatusEnum
USDWrite::writeFrameRange(int firstFrame,
                          int lastFrame,
                          ViewIdx view)
{
    const auto fail = [this](const QString& message) {
        setPersistentMessage(eMessageTypeError, message.toStdString());

        return eStatusFailed;
    };

    std::string usdError;
    if (!SceneOps::checkUsdRuntime(&usdError)) {
        return fail(QString::fromStdString(usdError));
    }
    if (firstFrame > lastFrame) {
        return fail(tr("The frame range is empty: the first frame, %1, is after the last, %2.").arg(firstFrame).arg(lastFrame));
    }

    KnobOutputFilePtr filename = _filename.lock();
    const QString pattern = filename ? filename->generateFileNameAtTime(firstFrame, view) : QString();
    if (pattern.isEmpty()) {
        return fail(tr("No file to write: set File."));
    }
    const QFileInfo outputInfo(QDir::current().absoluteFilePath(pattern));
    const QString outputPath = QDir::cleanPath(outputInfo.absoluteFilePath());
    const QString extension = outputInfo.suffix().toLower();
    if ((extension != QString::fromUtf8("usd")) && (extension != QString::fromUtf8("usda")) && (extension != QString::fromUtf8("usdc"))) {
        return fail(tr("%1 is not a USD file name: it must end in .usd, .usda or .usdc.").arg(outputPath));
    }
    const QString outputDir = outputInfo.absolutePath();
    if (!QDir().mkpath(outputDir)) {
        return fail(tr("Cannot create the directory %1.").arg(outputDir));
    }

    ScenePayload scene;
    std::string sceneError;
    if (evaluateScene(EvaluateSceneArgs(firstFrame, view), &scene, &sceneError) != eRenderRoIRetCodeOk) {
        return fail(sceneError.empty() ? tr("The input scene could not be evaluated.") : QString::fromStdString(sceneError));
    }

    KnobChoicePtr exportModeKnob = _exportMode.lock();
    KnobChoicePtr pathModeKnob = _pathMode.lock();
    const ExportModeEnum exportMode = (exportModeKnob && (exportModeKnob->getValue() == (int)eExportModeFlattened)) ? eExportModeFlattened : eExportModeLayered;
    const PathModeEnum pathMode = (pathModeKnob && (pathModeKnob->getValue() == (int)ePathModeAbsolute)) ? ePathModeAbsolute : ePathModeRelative;
    AppInstancePtr app = getApp();
    const double fps = (app && app->getProject()) ? app->getProject()->getProjectFrameRate() : 24.;
    const SceneMetadata& metadata = scene.getMetadata();
    const double startTime = SceneOps::frameToTimeCode(scene, firstFrame, fps);
    const double endTime = SceneOps::frameToTimeCode(scene, lastFrame, fps);

    PXR_NS::TfErrorMark mark;
    // What is written, strongest first: the file itself, then, in layered mode, the files of the
    // runs of anonymous layers below a file the scene reads, written beside it.
    struct Written {
        QString path;
        PXR_NS::SdfLayerRefPtr layer;
    };
    std::vector<Written> written;
    {
        // The scene's layers are read while they are flattened: no write may edit them meanwhile.
        SceneWriteBarrier::ReadScope read;
        PXR_NS::SdfLayerRefPtr output;
        // In layered mode, the sublayers of output, strongest first: the files the scene reads, and
        // between them the runs of anonymous layers weaker than one, flattened in place so the
        // written file composes with the strength order the scene had.
        std::vector<StackItem> sublayers;
        if (exportMode == eExportModeFlattened) {
            PXR_NS::UsdStageRefPtr stage = SceneOps::compose(scene);
            output = stage ? stage->Flatten(false) : PXR_NS::SdfLayerRefPtr();
        } else {
            std::vector<StackItem> items;
            const std::vector<SceneLayerEntry> layers = SceneOps::getLayers(scene);
            for (std::vector<SceneLayerEntry>::const_reverse_iterator it = layers.rbegin(); it != layers.rend(); ++it) {
                // As composition maps a sublayer of the root: by the ratio of their time codes per second.
                const PXR_NS::SdfLayerOffset toRoot(0., metadata.timeCodesPerSecond / it->layer->GetTimeCodesPerSecond());
                appendLayerStack(it->layer, toRoot, &items);
            }
            // A file read twice the same way is one sublayer, where it is strongest.
            std::set<std::pair<std::string, std::pair<double, double>>> seen;
            std::vector<StackItem> run;
            bool flattenedOk = true;
            auto endRun = [&]() {
                if (run.empty()) {
                    return;
                }
                PXR_NS::SdfLayerRefPtr flattened = flattenRun(run, metadata.timeCodesPerSecond);
                run.clear();
                if (!flattened) {
                    flattenedOk = false;
                } else if (!output && sublayers.empty()) {
                    output = flattened;
                } else {
                    sublayers.push_back(StackItem { flattened, PXR_NS::SdfLayerOffset(), false });
                }
            };
            for (const StackItem& item : items) {
                if (!item.external) {
                    run.push_back(item);
                    continue;
                }
                if (!seen.insert(std::make_pair(item.layer->GetIdentifier(), std::make_pair(item.toRoot.GetOffset(), item.toRoot.GetScale()))).second) {
                    continue;
                }
                endRun();
                sublayers.push_back(item);
            }
            endRun();
            if (flattenedOk && !output) {
                // The strongest layer is a file: the written file holds nothing but its sublayers.
                output = PXR_NS::SdfLayer::CreateAnonymous("natron-usdwrite");
            }
            if (!flattenedOk) {
                output.Reset();
            }
        }
        if (!output) {
            const std::string errors = takeUsdErrors(mark);

            return fail(tr("The scene could not be flattened%1").arg(errors.empty() ? QString::fromUtf8(".") : QString::fromUtf8(": ") + QString::fromStdString(errors)));
        }

        // The files the write produces or references, which a removal of stale sidecars spares.
        std::set<QString> keep;
        const auto finish = [&](const PXR_NS::SdfLayerRefPtr& layer) {
            clipTimeSamples(layer, startTime, endTime);
            PXR_NS::UsdUtilsModifyAssetPaths(layer, [&outputDir, pathMode, &keep](const std::string& assetPath) {
                const QString referenced = referencedFilePath(assetPath);
                if (!referenced.isEmpty()) {
                    keep.insert(referenced);
                }

                return rewriteAssetPath(assetPath, outputDir, pathMode);
            });
        };
        finish(output);
        authorStageMetadata(output, metadata, startTime, endTime, fps);
        written.push_back(Written { outputPath, output });

        // After the asset paths are rewritten, which would drop the offsets of the sublayers.
        std::vector<std::string> subLayerPaths;
        PXR_NS::SdfLayerOffsetVector subLayerOffsets;
        int runIndex = 0;
        for (const StackItem& item : sublayers) {
            if (item.external) {
                subLayerPaths.push_back(rewriteAssetPath(item.layer->GetIdentifier(), outputDir, pathMode));
                // Authored so that composition, which scales by the ratio of the two layers' time
                // codes per second, arrives at the offset the scene had.
                subLayerOffsets.push_back(authoredOffset(item.toRoot, item.layer, metadata.timeCodesPerSecond));
                continue;
            }
            // A run below a file goes to a file of its own beside the written one.
            const QString runPath = QDir(outputDir).filePath(QString::fromUtf8("%1_layer%2.%3").arg(outputInfo.completeBaseName()).arg(++runIndex).arg(outputInfo.suffix()));
            finish(item.layer);
            item.layer->SetTimeCodesPerSecond(metadata.timeCodesPerSecond);
            written.push_back(Written { runPath, item.layer });
            subLayerPaths.push_back(rewriteAssetPath(runPath.toStdString(), outputDir, pathMode));
            subLayerOffsets.push_back(PXR_NS::SdfLayerOffset());
        }
        for (const StackItem& item : sublayers) {
            if (!item.external) {
                continue;
            }
            const QString realPath = QDir::cleanPath(QString::fromStdString(item.layer->GetRealPath()));
            keep.insert(realPath);
            for (const Written& w : written) {
                if (realPath == w.path) {
                    return fail(tr("%1 is read by the scene it would be overwritten with: write to another file, or use Flattened mode.").arg(w.path));
                }
            }
        }
        output->SetSubLayerPaths(subLayerPaths);
        for (std::size_t i = 0; i < subLayerOffsets.size(); ++i) {
            output->SetSubLayerOffset(subLayerOffsets[i], (int)i);
        }

        // The weakest first, so that a reader of the written file never finds a sublayer missing.
        for (std::vector<Written>::const_reverse_iterator it = written.rbegin(); it != written.rend(); ++it) {
            if (!it->layer->Export(it->path.toStdString())) {
                const std::string errors = takeUsdErrors(mark);

                return fail(tr("Cannot write %1%2").arg(it->path).arg(errors.empty() ? QString::fromUtf8(".") : QString::fromUtf8(": ") + QString::fromStdString(errors)));
            }
        }
        // Only once every file is written, so that a failed write leaves the earlier set whole.
        if (exportMode == eExportModeLayered) {
            for (const Written& w : written) {
                keep.insert(QDir::cleanPath(w.path));
            }
            removeStaleSidecars(outputInfo, keep);
        }
    }
    // Layers already open on the files, such as a USDRead's of them, would go on showing what
    // they held before: they are reloaded in place, through the barrier, so every stage composing
    // them sees the new content.
    std::vector<PXR_NS::SdfLayerRefPtr> open;
    for (const Written& w : written) {
        const PXR_NS::SdfLayerHandle layer = PXR_NS::SdfLayer::Find(w.path.toStdString());
        if (layer) {
            open.push_back(PXR_NS::SdfLayerRefPtr(layer));
        }
    }
    std::string reloadError;
    if (!open.empty() && !SceneOps::reloadFileLayers(open, &reloadError)) {
        return fail(tr("%1 was written, but the scene could not read it again: %2").arg(outputPath).arg(QString::fromStdString(reloadError)));
    }
    mark.Clear();

    clearPersistentMessage(false);
    ++_writeCount;

    return eStatusOK;
} // USDWrite::writeFrameRange

NATRON_NAMESPACE_EXIT
