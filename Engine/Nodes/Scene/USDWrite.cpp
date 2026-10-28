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

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <thread>
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
#include "Engine/Nodes/Scene/LiveTextureStore.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/ScenePayloadImpl.h"
#include "Engine/Nodes/Scene/SceneWriteBarrier.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"
#include "Engine/Project.h"

#include <OpenImageIO/imageio.h>

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
 * layered write names them, except those in keep: what an earlier layered write left, which
 * nothing written references. A file that cannot be removed stays, as the written files do not
 * depend on it. Returns the names of the files removed.
 **/
QStringList
removeStaleSidecars(const QFileInfo& outputInfo,
                    const std::set<QString>& keep)
{
    QStringList removed;
    const QDir dir(outputInfo.absolutePath());
    const QRegularExpression sidecar(QString::fromUtf8("^") + QRegularExpression::escape(outputInfo.completeBaseName()) + QString::fromUtf8("_layer[1-9][0-9]*\\.") + QRegularExpression::escape(outputInfo.suffix()) + QString::fromUtf8("$"));
    const QStringList names = dir.entryList(QDir::Files | QDir::Hidden | QDir::System);

    // A symlinked output folder spells the same file two ways, so keep entries also match by
    // canonical path.
    std::set<QString> keepCanonical;
    for (const QString& k : keep) {
        const QString canonical = QFileInfo(k).canonicalFilePath();
        if (!canonical.isEmpty()) {
            keepCanonical.insert(canonical);
        }
    }

    for (const QString& name : names) {
        if (!sidecar.match(name).hasMatch()) {
            continue;
        }
        const QString path = QDir::cleanPath(dir.absoluteFilePath(name));
        if (keepCanonical.count(QFileInfo(path).canonicalFilePath()) != 0) {
            continue;
        }
        if ((keep.count(path) == 0) && QFile::remove(path)) {
            removed.push_back(name);
        }
    }

    return removed;
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

// The paths of layer's prim specs, variant ones included.
std::vector<PXR_NS::SdfPath>
primSpecPaths(const PXR_NS::SdfLayerRefPtr& layer)
{
    std::vector<PXR_NS::SdfPath> paths;

    layer->Traverse(PXR_NS::SdfPath::AbsoluteRootPath(), [&paths](const PXR_NS::SdfPath& path) {
        if (path.IsPrimOrPrimVariantSelectionPath()) {
            paths.push_back(path);
        }
    });

    return paths;
}

/**
 * @brief Composes scale after the layer offset of each of layer's references and payloads to
 * another layer. UsdUtilsFlattenLayerStack folds the offset a layer has in the stack, its time
 * codes per second against the root's included, into these arcs, while composition of the
 * flattened layer scales them again by the root's rate against the target's: scaling them by the
 * layer's rate against the root's first leaves the arc mapping the target as it did in the layer.
 **/
void
scaleArcOffsets(const PXR_NS::SdfLayerRefPtr& layer,
                double scale)
{
    const PXR_NS::SdfLayerOffset scaling(0., scale);

    for (const PXR_NS::SdfPath& path : primSpecPaths(layer)) {
        PXR_NS::SdfReferenceListOp references;
        if (layer->HasField(path, PXR_NS::SdfFieldKeys->References, &references)) {
            references.ModifyOperations([&scaling](const PXR_NS::SdfReference& reference) -> std::optional<PXR_NS::SdfReference> {
                PXR_NS::SdfReference scaled(reference);
                if (!reference.GetAssetPath().empty()) {
                    scaled.SetLayerOffset(reference.GetLayerOffset() * scaling);
                }

                return scaled;
            });
            layer->SetField(path, PXR_NS::SdfFieldKeys->References, references);
        }
        PXR_NS::SdfPayloadListOp payloads;
        if (layer->HasField(path, PXR_NS::SdfFieldKeys->Payload, &payloads)) {
            payloads.ModifyOperations([&scaling](const PXR_NS::SdfPayload& payload) -> std::optional<PXR_NS::SdfPayload> {
                PXR_NS::SdfPayload scaled(payload);
                if (!payload.GetAssetPath().empty()) {
                    scaled.SetLayerOffset(payload.GetLayerOffset() * scaling);
                }

                return scaled;
            });
            layer->SetField(path, PXR_NS::SdfFieldKeys->Payload, payloads);
        }
    }
}

// A reference or payload to an anonymous layer, and the layer offset it maps the layer's times by.
struct AnonymousArc {
    std::string identifier;
    PXR_NS::SdfLayerOffset offset;
};

// The references and payloads layer authors to anonymous layers, in traversal order.
std::vector<AnonymousArc>
anonymousArcs(const PXR_NS::SdfLayerRefPtr& layer)
{
    std::vector<AnonymousArc> arcs;

    for (const PXR_NS::SdfPath& path : primSpecPaths(layer)) {
        PXR_NS::SdfReferenceListOp references;
        if (layer->HasField(path, PXR_NS::SdfFieldKeys->References, &references)) {
            for (const PXR_NS::SdfReference& reference : references.GetAppliedItems()) {
                if (PXR_NS::SdfLayer::IsAnonymousLayerIdentifier(reference.GetAssetPath())) {
                    arcs.push_back(AnonymousArc { reference.GetAssetPath(), reference.GetLayerOffset() });
                }
            }
        }
        PXR_NS::SdfPayloadListOp payloads;
        if (layer->HasField(path, PXR_NS::SdfFieldKeys->Payload, &payloads)) {
            for (const PXR_NS::SdfPayload& payload : payloads.GetAppliedItems()) {
                if (PXR_NS::SdfLayer::IsAnonymousLayerIdentifier(payload.GetAssetPath())) {
                    arcs.push_back(AnonymousArc { payload.GetAssetPath(), payload.GetLayerOffset() });
                }
            }
        }
    }

    return arcs;
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
        // The run's layers are copies, which are ours to edit.
        const double tcps = item.layer->GetTimeCodesPerSecond();
        if (tcps != rootTcps) {
            scaleArcOffsets(item.layer, tcps / rootTcps);
        }
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
 * @brief A layer stack as layered mode writes it: its strongest run of anonymous layers flattened
 * into output, and output's sublayers, strongest first: the files the stack reads, and between them
 * the runs of anonymous layers weaker than one, flattened in place so the written stack composes
 * with the strength order it had.
 **/
struct StackLayout {
    PXR_NS::SdfLayerRefPtr output;
    std::vector<StackItem> sublayers;
};

// Lays out items, strongest first, in layers of time codes per second rootTcps. False when USD
// cannot flatten a run.
bool
layOutStack(const std::vector<StackItem>& items,
            double rootTcps,
            StackLayout* layout)
{
    // A file read twice the same way is one sublayer, where it is strongest.
    std::set<std::pair<std::string, std::pair<double, double>>> seen;
    std::vector<StackItem> run;
    bool flattenedOk = true;
    auto endRun = [&]() {
        if (run.empty()) {
            return;
        }
        PXR_NS::SdfLayerRefPtr flattened = flattenRun(run, rootTcps);
        run.clear();
        if (!flattened) {
            flattenedOk = false;
        } else if (!layout->output && layout->sublayers.empty()) {
            layout->output = flattened;
        } else {
            layout->sublayers.push_back(StackItem { flattened, PXR_NS::SdfLayerOffset(), false });
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
        layout->sublayers.push_back(item);
    }
    endRun();
    if (!flattenedOk) {
        return false;
    }
    if (!layout->output) {
        // The strongest layer is a file: the written layer holds nothing but its sublayers.
        layout->output = PXR_NS::SdfLayer::CreateAnonymous("natron-usdwrite");
    }

    return true;
}

/**
 * @brief A layer stack written to files: the scene's, or, in layered mode, that of an anonymous
 * layer a written layer references, as a re-parenting node references the layer it grafts its
 * input from. Such a stack goes to a sidecar, which the reference is pointed at. source is the
 * referenced layer, null for the scene's stack. Its written layers carry time codes per second
 * tcps, and [startTime, endTime] is the range of its times the frame range reaches.
 **/
struct WrittenStack {
    PXR_NS::SdfLayerRefPtr source;
    QString path;
    double tcps = 24.;
    StackLayout layout;
    // Where each run among layout.sublayers is written; empty for a file.
    std::vector<QString> runPaths;
    bool hasRange = false;
    double startTime = 0.;
    double endTime = 0.;
};

// A written stack's reference to another's: toFrom maps the times of stack to those of stack from.
struct StackArc {
    std::size_t from;
    std::size_t to;
    PXR_NS::SdfLayerOffset toFrom;
};

/**
 * @brief Widens each referenced stack's range to hold every time its references map the
 * referencing stacks' ranges to, until no range grows: references between written stacks follow
 * the scene's node graph, which has no cycles.
 **/
void
propagateRanges(const std::vector<StackArc>& arcs,
                std::vector<WrittenStack>* stacks)
{
    for (bool grew = true; grew;) {
        grew = false;
        for (const StackArc& arc : arcs) {
            const WrittenStack& from = (*stacks)[arc.from];
            if (!from.hasRange || !arc.toFrom.IsValid() || (arc.toFrom.GetScale() == 0.)) {
                continue;
            }
            const PXR_NS::SdfLayerOffset fromTo = arc.toFrom.GetInverse();
            const double a = fromTo * from.startTime;
            const double b = fromTo * from.endTime;
            const double start = std::min(a, b);
            const double end = std::max(a, b);
            WrittenStack& to = (*stacks)[arc.to];
            if (!to.hasRange) {
                to.hasRange = true;
                to.startTime = start;
                to.endTime = end;
                grew = true;
            } else if ((start < to.startTime) || (end > to.endTime)) {
                to.startTime = std::min(start, to.startTime);
                to.endTime = std::max(end, to.endTime);
                grew = true;
            }
        }
    }
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

// A live texture the written file refers to, and the file it is exported to.
struct LiveTextureExport {
    LiveTextureSource source;
    // A frame whose URI it is.
    double frame = 0.;
    QString path;
    bool needed = false;
};

// Letters, digits, '_', '-' and '.' kept, anything else '_': a node's name as a file name.
QString
fileNameOf(const std::string& name)
{
    QString result = QString::fromStdString(name);

    for (QChar& c : result) {
        if (!c.isLetterOrNumber() && (c != QLatin1Char('_')) && (c != QLatin1Char('-')) && (c != QLatin1Char('.'))) {
            c = QLatin1Char('_');
        }
    }

    return result;
}

/**
 * @brief Every URI sources author over their frames, keyed to the file in directory it is
 * exported to: <node>_<layer>.exr for a source that does not vary, else <node>_<layer>.<frame>.exr,
 * the frame padded to 4 digits.
 **/
std::map<std::string, LiveTextureExport>
planLiveTextureExports(const LiveTextureSourceList& sources,
                       const QString& directory)
{
    std::map<std::string, LiveTextureExport> exports;

    for (const LiveTextureSource& source : sources) {
        const QString stem = QDir(directory).filePath(fileNameOf(source.nodeName) + QLatin1Char('_') + fileNameOf(source.layer));
        const double first = std::ceil(source.firstFrame);
        const double last = source.varying ? std::max(first, std::floor(source.lastFrame)) : first;
        for (double frame = first; frame <= last; frame += 1.) {
            LiveTextureExport e;
            e.source = source;
            e.frame = frame;
            e.path = source.varying ? QString::fromUtf8("%1.%2.exr").arg(stem).arg((int)source.getSourceFrame(frame), 4, 10, QLatin1Char('0')) : stem + QString::fromUtf8(".exr");
            exports.insert(std::make_pair(source.getUri(frame), e));
        }
    }

    return exports;
}

// Writes buffer to path as half-float RGBA OpenEXR, top row first.
bool
writeLiveTextureFile(const LiveTextureBufferConstPtr& buffer,
                     const QString& path,
                     std::string* error)
{
    const std::string file = path.toStdString();
    std::unique_ptr<OIIO::ImageOutput> out = OIIO::ImageOutput::create(file);

    if (!out) {
        *error = OIIO::geterror();

        return false;
    }
    OIIO::ImageSpec spec(buffer->getWidth(), buffer->getHeight(), 4, OIIO::TypeDesc::HALF);
    spec.attribute("compression", "zip");
    if (!out->open(file, spec)) {
        *error = out->geterror();

        return false;
    }
    bool ok = true;
    for (int y = 0; ok && (y < buffer->getHeight()); ++y) {
        ok = out->write_scanline(y, 0, OIIO::TypeDesc::FLOAT, buffer->getRow(buffer->getHeight() - 1 - y));
    }
    ok = out->close() && ok;
    if (!ok) {
        *error = out->geterror();
    }

    return ok;
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
                                          "below it, and so does what a node places under one of its prims, as USDXformPrim "
                                          "does its inputs, which the written file references. Files of those names that the write does not produce, left by an earlier write, are "
                                          "removed.")
                                           .toStdString()));
        options.push_back(ChoiceOption("flattened", tr("Flattened").toStdString(),
                                       tr("Bakes the whole composed scene, the files it reads included, into the file, which "
                                          "then stands on its own. Sidecar files a layered write to the same name left beside it "
                                          "(name_layer1.usda, ...) are removed, with a warning naming them, unless the scene reads them.")
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

    KnobBoolPtr exportLiveTextures = createKnob<KnobBool>(tr("Export Live Textures"));
    exportLiveTextures->setName(kUSDWriteParamExportLiveTextures);
    exportLiveTextures->setHintToolTip(tr("Renders the images a USDMaterial's image inputs feed the scene to OpenEXR files in "
                                          "<file name>_textures beside the file, one per frame for an image that changes over "
                                          "time, and points the materials at them. Off, the file names the images as only this "
                                          "Natron session can read them."));
    exportLiveTextures->setDefaultValue(true);
    exportLiveTextures->setAnimationEnabled(false);
    page->addKnob(exportLiveTextures);
    _exportLiveTextures = exportLiveTextures;

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

    KnobBoolPtr exportLiveTexturesKnob = _exportLiveTextures.lock();
    const bool exportLiveTextures = exportLiveTexturesKnob && exportLiveTexturesKnob->getValue();
    const QString texturesDir = QDir(outputDir).filePath(outputInfo.completeBaseName() + QString::fromUtf8("_textures"));
    std::map<std::string, LiveTextureExport> liveTextures;
    if (exportLiveTextures) {
        liveTextures = planLiveTextureExports(SceneOps::getLiveTextures(scene), texturesDir);
    }
    // natron:// URIs written as they are, which only this session resolves.
    std::set<std::string> unexportedLiveTextures;

    PXR_NS::TfErrorMark mark;
    // What is written, the file itself first, then, in layered mode, the sidecars beside it: the
    // runs of anonymous layers below a file the scene reads, and the stacks of the anonymous layers
    // the written layers reference.
    struct Written {
        QString path;
        PXR_NS::SdfLayerRefPtr layer;
    };
    std::vector<Written> written;
    QStringList removedSidecars;
    {
        // The scene's layers are read while they are flattened: no write may edit them meanwhile.
        SceneWriteBarrier::ReadScope read;
        // The written stacks, the scene's first. Each anonymous layer a written layer references
        // is one stack, numbered among the sidecars in the order the written layers reference it,
        // so the same scene gets the same names.
        std::vector<WrittenStack> stacks(1);
        stacks[0].path = outputPath;
        stacks[0].tcps = metadata.timeCodesPerSecond;
        stacks[0].hasRange = true;
        stacks[0].startTime = startTime;
        stacks[0].endTime = endTime;
        std::map<std::string, std::size_t> stackOfLayer;
        std::vector<StackArc> arcs;
        int sidecarIndex = 0;
        const auto nextSidecarPath = [&]() {
            return QDir(outputDir).filePath(QString::fromUtf8("%1_layer%2.%3").arg(outputInfo.completeBaseName()).arg(++sidecarIndex).arg(outputInfo.suffix()));
        };
        const auto flattenFailure = [&]() {
            const std::string errors = takeUsdErrors(mark);

            return fail(tr("The scene could not be flattened%1").arg(errors.empty() ? QString::fromUtf8(".") : QString::fromUtf8(": ") + QString::fromStdString(errors)));
        };
        // The files the write produces or references, which a removal of stale sidecars spares.
        std::set<QString> keep;
        if (exportMode == eExportModeFlattened) {
            PXR_NS::UsdStageRefPtr stage = SceneOps::compose(scene);
            stacks[0].layout.output = stage ? stage->Flatten(false) : PXR_NS::SdfLayerRefPtr();
            if (!stacks[0].layout.output) {
                return flattenFailure();
            }
            // The flattened file holds what these files author, but the nodes reading them, a
            // USDRead of one, still need them.
            for (const PXR_NS::SdfLayerHandle& layer : stage->GetUsedLayers()) {
                const std::string realPath = layer ? layer->GetRealPath() : std::string();
                if (!realPath.empty()) {
                    keep.insert(QDir::cleanPath(QString::fromStdString(realPath)));
                }
            }
        } else {
            for (std::size_t i = 0; i < stacks.size(); ++i) {
                std::vector<StackItem> items;
                if (i == 0) {
                    const std::vector<SceneLayerEntry> layers = SceneOps::getLayers(scene);
                    for (std::vector<SceneLayerEntry>::const_reverse_iterator it = layers.rbegin(); it != layers.rend(); ++it) {
                        // As composition maps a sublayer of the root: by the ratio of their time codes per second.
                        const PXR_NS::SdfLayerOffset toRoot(0., metadata.timeCodesPerSecond / it->layer->GetTimeCodesPerSecond());
                        appendLayerStack(it->layer, toRoot, &items);
                    }
                } else {
                    appendLayerStack(stacks[i].source, PXR_NS::SdfLayerOffset(), &items);
                }
                StackLayout layout;
                if (!layOutStack(items, stacks[i].tcps, &layout)) {
                    return flattenFailure();
                }
                std::vector<PXR_NS::SdfLayerRefPtr> layers(1, layout.output);
                std::vector<QString> runPaths;
                for (const StackItem& item : layout.sublayers) {
                    runPaths.push_back(item.external ? QString() : nextSidecarPath());
                    if (!item.external) {
                        layers.push_back(item.layer);
                    }
                }
                stacks[i].layout = layout;
                stacks[i].runPaths = runPaths;

                for (const PXR_NS::SdfLayerRefPtr& layer : layers) {
                    for (const AnonymousArc& arc : anonymousArcs(layer)) {
                        std::map<std::string, std::size_t>::const_iterator found = stackOfLayer.find(arc.identifier);
                        std::size_t to = 0;
                        if (found != stackOfLayer.end()) {
                            to = found->second;
                        } else {
                            const PXR_NS::SdfLayerHandle referenced = PXR_NS::SdfLayer::Find(arc.identifier);
                            if (!referenced) {
                                return fail(tr("The scene refers to the layer %1, which no longer exists.").arg(QString::fromStdString(arc.identifier)));
                            }
                            to = stacks.size();
                            stackOfLayer[arc.identifier] = to;
                            WrittenStack stack;
                            stack.source = PXR_NS::SdfLayerRefPtr(referenced);
                            stack.path = nextSidecarPath();
                            stack.tcps = referenced->GetTimeCodesPerSecond();
                            stacks.push_back(stack);
                        }
                        // As composition maps a reference: its offset, then the scale between the
                        // two layers' time codes per second.
                        arcs.push_back(StackArc { i, to, arc.offset * PXR_NS::SdfLayerOffset(0., stacks[i].tcps / stacks[to].tcps) });
                    }
                }
            }
            propagateRanges(arcs, &stacks);
        }

        const auto finish = [&](const PXR_NS::SdfLayerRefPtr& layer, const WrittenStack& stack) {
            if (stack.hasRange) {
                clipTimeSamples(layer, stack.startTime, stack.endTime);
            }
            PXR_NS::UsdUtilsModifyAssetPaths(layer, [&](const std::string& assetPath) {
                const std::map<std::string, std::size_t>::const_iterator sidecar = stackOfLayer.find(assetPath);
                if (sidecar != stackOfLayer.end()) {
                    return rewriteAssetPath(stacks[sidecar->second].path.toStdString(), outputDir, pathMode);
                }
                if (assetPath.compare(0, std::string(kLiveTextureScheme "://").size(), kLiveTextureScheme "://") == 0) {
                    const std::map<std::string, LiveTextureExport>::iterator live = liveTextures.find(assetPath);
                    if (live == liveTextures.end()) {
                        unexportedLiveTextures.insert(assetPath);

                        return assetPath;
                    }
                    live->second.needed = true;

                    return rewriteAssetPath(live->second.path.toStdString(), outputDir, pathMode);
                }
                const QString referenced = referencedFilePath(assetPath);
                if (!referenced.isEmpty()) {
                    keep.insert(referenced);
                }

                return rewriteAssetPath(assetPath, outputDir, pathMode);
            });
        };
        for (const WrittenStack& stack : stacks) {
            const PXR_NS::SdfLayerRefPtr& output = stack.layout.output;
            finish(output, stack);
            if (!stack.source) {
                authorStageMetadata(output, metadata, startTime, endTime, fps);
            } else {
                // Only a stage's root layer's metadata takes effect, but a sidecar opened on its
                // own reads as the layer it stands for.
                const PXR_NS::SdfPath& rootPath = PXR_NS::SdfPath::AbsoluteRootPath();
                for (const PXR_NS::TfToken& field : { PXR_NS::UsdGeomTokens->upAxis, PXR_NS::UsdGeomTokens->metersPerUnit }) {
                    PXR_NS::VtValue value;
                    if (stack.source->HasField(rootPath, field, &value)) {
                        output->SetField(rootPath, field, value);
                    }
                }
                if (stack.source->HasDefaultPrim()) {
                    output->SetDefaultPrim(stack.source->GetDefaultPrim());
                }
                output->SetTimeCodesPerSecond(stack.tcps);
            }
            written.push_back(Written { stack.path, output });

            // After the asset paths are rewritten, which would drop the offsets of the sublayers.
            std::vector<std::string> subLayerPaths;
            PXR_NS::SdfLayerOffsetVector subLayerOffsets;
            for (std::size_t i = 0; i < stack.layout.sublayers.size(); ++i) {
                const StackItem& item = stack.layout.sublayers[i];
                if (item.external) {
                    subLayerPaths.push_back(rewriteAssetPath(item.layer->GetIdentifier(), outputDir, pathMode));
                    // Authored so that composition, which scales by the ratio of the two layers' time
                    // codes per second, arrives at the offset the stack had.
                    subLayerOffsets.push_back(authoredOffset(item.toRoot, item.layer, stack.tcps));
                    continue;
                }
                // A run below a file goes to a file of its own beside the written one.
                finish(item.layer, stack);
                item.layer->SetTimeCodesPerSecond(stack.tcps);
                written.push_back(Written { stack.runPaths[i], item.layer });
                subLayerPaths.push_back(rewriteAssetPath(stack.runPaths[i].toStdString(), outputDir, pathMode));
                subLayerOffsets.push_back(PXR_NS::SdfLayerOffset());
            }
            output->SetSubLayerPaths(subLayerPaths);
            for (std::size_t i = 0; i < subLayerOffsets.size(); ++i) {
                output->SetSubLayerOffset(subLayerOffsets[i], (int)i);
            }
        }
        for (const WrittenStack& stack : stacks) {
            for (const StackItem& item : stack.layout.sublayers) {
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
        }

        // The weakest first, so that a reader of the written file never finds a sublayer missing.
        for (std::vector<Written>::const_reverse_iterator it = written.rbegin(); it != written.rend(); ++it) {
            if (!it->layer->Export(it->path.toStdString())) {
                const std::string errors = takeUsdErrors(mark);

                return fail(tr("Cannot write %1%2").arg(it->path).arg(errors.empty() ? QString::fromUtf8(".") : QString::fromUtf8(": ") + QString::fromStdString(errors)));
            }
        }
        // Only once every file is written, so that a failed write leaves the earlier set whole.
        for (const Written& w : written) {
            keep.insert(QDir::cleanPath(w.path));
        }
        removedSidecars = removeStaleSidecars(outputInfo, keep);
    }
    // After the scene's layers are let go: a render may need to write to a scene of its own.
    for (const std::pair<const std::string, LiveTextureExport>& live : liveTextures) {
        if (!live.second.needed) {
            continue;
        }
        if (!QDir().mkpath(texturesDir)) {
            return fail(tr("Cannot create the directory %1.").arg(texturesDir));
        }
        LiveTextureStore& store = LiveTextureStore::instance();
        LiveTextureBufferConstPtr buffer = store.find(live.first);
        if (!buffer) {
            // On a thread of its own, which owns no render state: this one may be a render's.
            std::thread renderer([&]() { store.prepare(LiveTextureSourceList(1, live.second.source), live.second.frame, view); });
            renderer.join();
            buffer = store.find(live.first);
        }
        if (!buffer) {
            const std::string failure = store.getFailure(live.first);

            return fail(tr("The live texture %1 could not be rendered%2").arg(QString::fromStdString(live.first)).arg(failure.empty() ? QString::fromUtf8(".") : QString::fromUtf8(": ") + QString::fromStdString(failure)));
        }
        std::string writeError;
        if (!writeLiveTextureFile(buffer, live.second.path, &writeError)) {
            return fail(tr("Cannot write %1: %2").arg(live.second.path).arg(QString::fromStdString(writeError)));
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
    QStringList warnings;
    if (!unexportedLiveTextures.empty()) {
        warnings.push_back(tr("%1 names %2 live textures, such as %3, which only this Natron session can read: "
                              "turn on Export Live Textures to write them beside it.")
                               .arg(outputInfo.fileName())
                               .arg((int)unexportedLiveTextures.size())
                               .arg(QString::fromStdString(*unexportedLiveTextures.begin())));
    }
    // A layered write replaces its own sidecars, as expected; a flattened one removes files the user
    // may not know the export mode made, so it says which.
    if ((exportMode == eExportModeFlattened) && !removedSidecars.isEmpty()) {
        const int kMaxNamed = 5;
        const QString what = (removedSidecars.size() <= kMaxNamed)
            ? removedSidecars.join(QString::fromUtf8(", "))
            : tr("%1 files named %2_layerN.%3").arg(removedSidecars.size()).arg(outputInfo.completeBaseName()).arg(outputInfo.suffix());
        warnings.push_back(tr("Removed %1 from %2: sidecar files of a layered write to %3, which the "
                              "flattened file does not use.")
                               .arg(what)
                               .arg(outputDir)
                               .arg(outputInfo.fileName()));
    }
    if (!warnings.isEmpty()) {
        setPersistentMessage(eMessageTypeWarning, warnings.join(QString::fromUtf8("\n")).toStdString());
    }
    ++_writeCount;

    return eStatusOK;
} // USDWrite::writeFrameRange

NATRON_NAMESPACE_EXIT
