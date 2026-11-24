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
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
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
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/LiveTextureStore.h"
#include "Engine/Nodes/Scene/SceneNodeAuthoring.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/ScenePayloadImpl.h"
#include "Engine/Nodes/Scene/SceneProjectionCameras.h"
#include "Engine/Nodes/Scene/SceneWriteBarrier.h"
#include "Engine/Nodes/Scene/USDProject3D.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"
#include "Engine/Project.h"

#include <SequenceParsing.h>

#include <pxr/usd/sdf/copyUtils.h>
#include <pxr/usd/sdf/listOp.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdUtils/stitch.h>

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

// The names a layered write to outputInfo gives its sidecars: name_layerN.ext.
QRegularExpression
sidecarNamePattern(const QFileInfo& outputInfo)
{
    return QRegularExpression(QString::fromUtf8("^") + QRegularExpression::escape(outputInfo.completeBaseName()) + QString::fromUtf8("_layer[1-9][0-9]*\\.") + QRegularExpression::escape(outputInfo.suffix()) + QString::fromUtf8("$"));
}

/**
 * @brief The path two spellings of one file, through symbolic links, share: the canonical path of
 * the deepest part of path that exists, then the rest as written. For comparing paths only; files
 * are written where their own spelling says.
 **/
QString
comparisonPath(const QString& path)
{
    const QString clean = QDir::cleanPath(path);
    QString existing = clean;
    QStringList parts;
    for (;;) {
        const QString canonical = QFileInfo(existing).canonicalFilePath();
        if (!canonical.isEmpty()) {
            parts.prepend(canonical);

            return QDir::cleanPath(parts.join(QLatin1Char('/')));
        }
        const QFileInfo info(existing);
        const QString parent = info.path();
        if (parent.isEmpty() || (parent == existing)) {
            return clean;
        }
        parts.prepend(info.fileName());
        existing = parent;
    }
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
    const QRegularExpression sidecar = sidecarNamePattern(outputInfo);
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
    // What an anonymous item's layer is a copy of.
    std::string source;
    // Of the scene's own stack: the innermost layer region of the entry the item comes from, and
    // that entry's index, strongest first.
    const SceneLayerRegion* region = nullptr;
    std::size_t entry = 0;
    // A region file sublayered in the item's place, by its index among the written stacks; it has
    // no layer.
    int file = -1;
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
    items->push_back(StackItem { copy, toRoot, false, source->GetIdentifier() });

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
            items->push_back(StackItem { sublayer, sublayerToRoot, true, std::string() });
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

// Adds to *files the real paths of the files stage composes.
void
collectUsedFiles(const PXR_NS::UsdStageRefPtr& stage,
                 std::set<QString>* files)
{
    for (const PXR_NS::SdfLayerHandle& layer : stage->GetUsedLayers()) {
        const std::string realPath = layer ? layer->GetRealPath() : std::string();
        if (!realPath.empty()) {
            files->insert(QDir::cleanPath(QString::fromStdString(realPath)));
        }
    }
}

/**
 * @brief The layers run, strongest first, flattened into one layer of time codes per second
 * rootTcps, their times mapped to the root's: the anonymous ones, and with them, in flattened
 * mode, the files the scene reads. Adds the files it composes to *usedFiles when given. Null when
 * USD cannot.
 **/
PXR_NS::SdfLayerRefPtr
flattenRun(const std::vector<StackItem>& run,
           double rootTcps,
           std::set<QString>* usedFiles = nullptr)
{
    PXR_NS::SdfLayerRefPtr root = PXR_NS::SdfLayer::CreateAnonymous("natron-usdwrite-root");

    root->SetTimeCodesPerSecond(rootTcps);
    std::vector<std::string> paths;
    for (const StackItem& item : run) {
        // The anonymous layers are copies, which are ours to edit; a file is not, and the scene's
        // nodes author no arcs in one.
        const double tcps = item.layer->GetTimeCodesPerSecond();
        if (!item.external && (tcps != rootTcps)) {
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
    if (usedFiles) {
        collectUsedFiles(stage, usedFiles);
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
 * @brief Bakes into layer the content of the anonymous layers its references and payloads name,
 * as UsdStage::Flatten would, beneath layer's own opinions on the prims holding them: the scene's
 * nodes author those layers, and nothing else writes them. Arcs to files or within the scene stay
 * arcs. Adds the files it composes to *usedFiles. False when USD cannot compose them.
 **/
bool
bakeAnonymousArcs(const PXR_NS::SdfLayerRefPtr& layer,
                  std::set<QString>* usedFiles)
{
    const auto anonymousOnly = [](bool anonymous) {
        return [anonymous](const auto& arc) -> std::optional<std::decay_t<decltype(arc)>> {
            if (PXR_NS::SdfLayer::IsAnonymousLayerIdentifier(arc.GetAssetPath()) == anonymous) {
                return arc;
            }

            return std::nullopt;
        };
    };
    // A list op left with no operation is no opinion, while an explicit empty one still clears
    // the weaker layers' arcs, as the baked arcs it named did.
    const auto setOrErase = [&layer](const PXR_NS::SdfPath& path,
                                     const PXR_NS::TfToken& field,
                                     const auto& listOp) {
        if (!listOp.HasKeys()) {
            layer->EraseField(path, field);
        } else {
            layer->SetField(path, field, listOp);
        }
    };

    PXR_NS::SdfLayerRefPtr arcs = PXR_NS::SdfLayer::CreateAnonymous("natron-usdwrite-arcs");
    arcs->SetTimeCodesPerSecond(layer->GetTimeCodesPerSecond());
    std::vector<PXR_NS::SdfPath> baked;
    for (const PXR_NS::SdfPath& path : primSpecPaths(layer)) {
        if (!path.IsPrimPath()) {
            continue;
        }
        PXR_NS::SdfReferenceListOp references;
        PXR_NS::SdfReferenceListOp anonymousReferences;
        if (layer->HasField(path, PXR_NS::SdfFieldKeys->References, &references)) {
            anonymousReferences = references;
            anonymousReferences.ModifyOperations(anonymousOnly(true));
        }
        PXR_NS::SdfPayloadListOp payloads;
        PXR_NS::SdfPayloadListOp anonymousPayloads;
        if (layer->HasField(path, PXR_NS::SdfFieldKeys->Payload, &payloads)) {
            anonymousPayloads = payloads;
            anonymousPayloads.ModifyOperations(anonymousOnly(true));
        }
        const bool bakesReferences = !anonymousReferences.GetAppliedItems().empty();
        const bool bakesPayloads = !anonymousPayloads.GetAppliedItems().empty();
        if (!bakesReferences && !bakesPayloads) {
            continue;
        }
        if (!PXR_NS::SdfCreatePrimInLayer(arcs, path)) {
            return false;
        }
        if (bakesReferences) {
            arcs->SetField(path, PXR_NS::SdfFieldKeys->References, anonymousReferences);
            references.ModifyOperations(anonymousOnly(false));
            setOrErase(path, PXR_NS::SdfFieldKeys->References, references);
        }
        if (bakesPayloads) {
            arcs->SetField(path, PXR_NS::SdfFieldKeys->Payload, anonymousPayloads);
            payloads.ModifyOperations(anonymousOnly(false));
            setOrErase(path, PXR_NS::SdfFieldKeys->Payload, payloads);
        }
        baked.push_back(path);
    }
    if (baked.empty()) {
        return true;
    }
    // The variant the written layer selects inside what an arc brings in is the one baked.
    for (const PXR_NS::SdfPath& path : primSpecPaths(layer)) {
        PXR_NS::VtValue selection;
        if (!path.IsPrimPath() || !layer->HasField(path, PXR_NS::SdfFieldKeys->VariantSelection, &selection)) {
            continue;
        }
        const bool underArc = std::any_of(baked.begin(), baked.end(), [&path](const PXR_NS::SdfPath& arc) {
            return path.HasPrefix(arc);
        });
        if (underArc && PXR_NS::SdfCreatePrimInLayer(arcs, path)) {
            arcs->SetField(path, PXR_NS::SdfFieldKeys->VariantSelection, selection);
        }
    }

    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(arcs, PXR_NS::SdfLayerHandle(), PXR_NS::UsdStage::LoadAll);
    if (!stage) {
        return false;
    }
    collectUsedFiles(stage, usedFiles);
    const PXR_NS::SdfLayerRefPtr flattened = stage->Flatten(false);
    if (!flattened) {
        return false;
    }
    // The prims holding the arcs are overs where the written layers only edit them, and are as
    // defined as what the arcs bring in.
    PXR_NS::UsdUtilsStitchLayers(layer, flattened, [](const PXR_NS::TfToken& field, const PXR_NS::SdfPath& path, const PXR_NS::SdfLayerHandle& strongLayer, bool fieldInStrongLayer, const PXR_NS::SdfLayerHandle& weakLayer, bool fieldInWeakLayer, PXR_NS::VtValue* stitchedValue) {
        if ((field == PXR_NS::SdfFieldKeys->Specifier) && fieldInStrongLayer && fieldInWeakLayer
            && (strongLayer->GetFieldAs<PXR_NS::SdfSpecifier>(path, field) == PXR_NS::SdfSpecifierOver)) {
            *stitchedValue = weakLayer->GetField(path, field);

            return PXR_NS::UsdUtilsStitchValueStatus::UseSuppliedValue;
        }

        return PXR_NS::UsdUtilsStitchValueStatus::UseDefaultValue;
    });

    return true;
} // bakeAnonymousArcs

/**
 * @brief A layer stack as layered mode writes it: its strongest run of anonymous layers flattened
 * into output, and output's sublayers, strongest first: the files the stack reads and the layer
 * region files written in place of their layers, and between them the runs of anonymous layers
 * weaker than one, flattened in place so the written stack composes with the strength order it had.
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
            layout->sublayers.push_back(StackItem { flattened, PXR_NS::SdfLayerOffset(), false, std::string() });
        }
    };

    for (const StackItem& item : items) {
        if (item.file >= 0) {
            endRun();
            layout->sublayers.push_back(item);
            continue;
        }
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
    // A layer region's file, whose items are laid out with the scene's stack.
    bool region = false;
    std::vector<StackItem> items;
    // The region metadata the written layer gets.
    SceneLayerRegionSettings settings;
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
        PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(layer, PXR_NS::SdfLayerHandle(), PXR_NS::UsdStage::LoadNone);
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
    if (metadata.renderSettingsPrimPath.empty()) {
        layer->EraseField(rootPath, PXR_NS::UsdRenderTokens->renderSettingsPrimPath);
    } else {
        layer->SetField(rootPath, PXR_NS::UsdRenderTokens->renderSettingsPrimPath, metadata.renderSettingsPrimPath);
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

// Whether value is an asset path one of uris names, which it returns in *uri.
bool
namesOneOf(const PXR_NS::VtValue& value,
           const std::set<std::string>& uris,
           std::string* uri)
{
    if (!value.IsHolding<PXR_NS::SdfAssetPath>()) {
        return false;
    }
    const std::string& path = value.UncheckedGet<PXR_NS::SdfAssetPath>().GetAssetPath();
    if (uris.find(path) == uris.end()) {
        return false;
    }
    *uri = path;

    return true;
}

/**
 * @brief Removes from layer every texture shader whose file is one of uris, and every connection to
 * it, so the inputs it fed take the values authored on them. Returns the URIs it found, each with
 * the path of a prim whose texture read it.
 **/
std::map<std::string, PXR_NS::SdfPath>
removeTexturesReading(const PXR_NS::SdfLayerHandle& layer,
                      const std::set<std::string>& uris)
{
    std::map<std::string, PXR_NS::SdfPath> found;

    if (uris.empty()) {
        return found;
    }
    const PXR_NS::TfToken fileInput("inputs:file");
    std::set<PXR_NS::SdfPath> textures;
    layer->Traverse(PXR_NS::SdfPath::AbsoluteRootPath(), [&](const PXR_NS::SdfPath& path) {
        if (!path.IsPrimPropertyPath() || (path.GetNameToken() != fileInput)) {
            return;
        }
        std::string uri;
        PXR_NS::VtValue value;
        bool reads = layer->HasField(path, PXR_NS::SdfFieldKeys->Default, &value) && namesOneOf(value, uris, &uri);
        for (double time : layer->ListTimeSamplesForPath(path)) {
            if (reads) {
                break;
            }
            reads = layer->QueryTimeSample(path, time, &value) && namesOneOf(value, uris, &uri);
        }
        if (reads) {
            found[uri] = path.GetPrimPath().GetParentPath();
            textures.insert(path.GetPrimPath());
        }
    });
    if (textures.empty()) {
        return found;
    }
    const auto intoTexture = [&textures](const PXR_NS::SdfPath& target) {
        for (const PXR_NS::SdfPath& texture : textures) {
            if (target.HasPrefix(texture)) {
                return true;
            }
        }

        return false;
    };
    std::vector<PXR_NS::SdfAttributeSpecHandle> connected;
    layer->Traverse(PXR_NS::SdfPath::AbsoluteRootPath(), [&](const PXR_NS::SdfPath& path) {
        if (!path.IsPrimPropertyPath()) {
            return;
        }
        const PXR_NS::SdfAttributeSpecHandle attribute = layer->GetAttributeAtPath(path);
        if (attribute && attribute->HasConnectionPaths()) {
            connected.push_back(attribute);
        }
    });

    PXR_NS::SdfChangeBlock block;
    for (const PXR_NS::SdfAttributeSpecHandle& attribute : connected) {
        if (textures.count(attribute->GetPath().GetPrimPath())) {
            continue;
        }
        bool changed = false;
        for (const PXR_NS::SdfPath& target : attribute->GetConnectionPathList().GetAppliedItems()) {
            if (intoTexture(target)) {
                attribute->GetConnectionPathList().RemoveItemEdits(target);
                changed = true;
            }
        }
        if (changed && attribute->GetConnectionPathList().GetAppliedItems().empty()) {
            attribute->ClearConnectionPaths();
        }
    }
    for (const PXR_NS::SdfPath& texture : textures) {
        const PXR_NS::SdfPrimSpecHandle prim = layer->GetPrimAtPath(texture);
        const PXR_NS::SdfPrimSpecHandle parent = layer->GetPrimAtPath(texture.GetParentPath());
        if (prim && parent) {
            parent->RemoveNameChild(prim);
        }
    }

    return found;
}

bool
meshMightVary(const PXR_NS::UsdGeomMesh& mesh)
{
    return mesh.GetPointsAttr().ValueMightBeTimeVarying() || mesh.GetFaceVertexIndicesAttr().ValueMightBeTimeVarying() || SceneProjectionCameras::transformMightVary(mesh.GetPrim());
}

// A projection material left with its Storm shader, and why.
struct UnbakedProjection {
    enum ReasonEnum {
        eReasonNotBound,
        eReasonNotAMesh,
        eReasonInstanced
    };

    std::string material;
    ReasonEnum reason;
    // The prim that stopped the bake; empty when the material is bound to nothing.
    std::string prim;
};

// Whether prim is a node of the Cycles or MoonRay network USDProject3D authors.
bool
isProjectionNetworkNode(const PXR_NS::UsdPrim& prim)
{
    const PXR_NS::VtValue mark = prim ? prim.GetCustomDataByKey(PXR_NS::TfToken(kSceneProjectionNetworkMark)) : PXR_NS::VtValue();

    return mark.IsHolding<bool>() && mark.UncheckedGet<bool>();
}

/**
 * @brief Copies a deferred camera's values (SceneProjectionCameras::projectorValues()) from the
 * composed scene onto the Storm surface of shader's material in out: the delegate networks are
 * stripped from every export. A layered export writes the
 * scene's own layers, which name the camera but hold no values for it: compose() resolves those
 * into a layer that is not exported.
 **/
bool
authorResolvedProjector(const PXR_NS::UsdShadeShader& shader,
                        const PXR_NS::UsdStageRefPtr& out)
{
    const PXR_NS::UsdAttribute camera = shader.GetPrim().GetAttribute(PXR_NS::TfToken(kUSDProject3DAttrCamera));
    std::string cameraPath;
    if (!camera || !camera.Get(&cameraPath) || cameraPath.empty()) {
        return true;
    }
    const PXR_NS::SdfPath material = shader.GetPath().GetParentPath();
    const PXR_NS::UsdStageRefPtr scene = shader.GetPrim().GetStage();
    for (const SceneProjectionCameras::ShaderValue& projected : SceneProjectionCameras::projectorValues(PXR_NS::GfMatrix4d(0.), PXR_NS::GfVec4f(0.f))) {
        const PXR_NS::UsdShadeShader owner(scene->GetPrimAtPath(material.AppendChild(projected.shader)));
        const PXR_NS::UsdShadeInput input = (owner && !isProjectionNetworkNode(owner.GetPrim())) ? owner.GetInput(projected.input) : PXR_NS::UsdShadeInput();
        if (!input) {
            continue;
        }
        const PXR_NS::UsdPrim over = out->OverridePrim(owner.GetPath());
        if (!over) {
            return false;
        }
        const PXR_NS::UsdAttribute source = input.GetAttr();
        PXR_NS::UsdAttribute target = over.GetAttribute(source.GetName());
        // Over the layer's own values, whose time samples would win over a default.
        if (target) {
            target.Clear();
        }
        target = over.CreateAttribute(source.GetName(), source.GetTypeName());
        if (!target) {
            return false;
        }
        std::vector<double> times;
        source.GetTimeSamples(&times);
        PXR_NS::VtValue value;
        if (times.empty()) {
            if (source.Get(&value) && !target.Set(value)) {
                return false;
            }
        }
        for (const double time : times) {
            if (source.Get(&value, time) && !target.Set(value, time)) {
                return false;
            }
        }
    }

    return true;
}

// A spec the export writes: its written stack, and its path in that stack's own namespace.
typedef std::pair<std::size_t, PXR_NS::SdfPath> WrittenSpec;
typedef std::set<WrittenSpec> WrittenSpecs;

// Whether the export writes something of the composed prim at a path.
typedef std::function<bool(const PXR_NS::SdfPath&)> WrittenPrimFunc;

// Appends layer and, recursively, the layers its sublayers bring in to *layers, each once.
void
appendSublayerClosure(const PXR_NS::SdfLayerRefPtr& layer,
                      std::set<std::string>* seen,
                      std::vector<PXR_NS::SdfLayerRefPtr>* layers)
{
    if (!layer || !seen->insert(layer->GetIdentifier()).second) {
        return;
    }
    layers->push_back(layer);
    for (const std::string& path : layer->GetSubLayerPaths()) {
        appendSublayerClosure(PXR_NS::SdfLayer::FindOrOpenRelativeToLayer(layer, path), seen, layers);
    }
}

// Whether layers hold a spec of the prim at path, or an arc on an ancestor of it, which brings
// it in.
bool
writtenLayersReach(const std::vector<PXR_NS::SdfLayerRefPtr>& layers,
                   const PXR_NS::SdfPath& path)
{
    for (const PXR_NS::SdfLayerRefPtr& layer : layers) {
        if (layer->GetPrimAtPath(path)) {
            return true;
        }
        for (PXR_NS::SdfPath ancestor = path.GetParentPath(); ancestor.IsPrimPath(); ancestor = ancestor.GetParentPath()) {
            if (layer->HasField(ancestor, PXR_NS::SdfFieldKeys->References) || layer->HasField(ancestor, PXR_NS::SdfFieldKeys->Payload)) {
                return true;
            }
        }
    }

    return false;
}

/**
 * @brief Where the specs of the composed scene land among the written stacks. A file is written as it
 * is, so its specs land nowhere. A referenced layer's prims sit at other paths once composed, and one
 * referenced twice holds a single spec for both, so a layered export has to be told apart by spec, not
 * by composed path. A flattened export is one stack in the composed namespace.
 **/
struct WrittenSpecLocator {
    bool flattened = false;
    std::map<std::string, std::vector<std::size_t>> stacksOfLayer;

    template <typename SPEC_HANDLES>
    void
    add(const PXR_NS::SdfPath& composedPath,
        const SPEC_HANDLES& specs,
        WrittenSpecs* into) const
    {
        if (flattened) {
            into->insert(WrittenSpec(0, composedPath));

            return;
        }
        for (const auto& spec : specs) {
            const std::map<std::string, std::vector<std::size_t>>::const_iterator found = stacksOfLayer.find(spec->GetLayer()->GetIdentifier());
            if (found != stacksOfLayer.end()) {
                for (const std::size_t stack : found->second) {
                    into->insert(WrittenSpec(stack, spec->GetPath()));
                }
            }
        }
    }

    void
    addAttribute(const PXR_NS::UsdAttribute& attribute,
                 WrittenSpecs* into) const
    {
        if (attribute) {
            add(attribute.GetPath(), attribute.GetPropertyStack(), into);
        }
    }

    void
    addPrim(const PXR_NS::UsdPrim& prim,
            WrittenSpecs* into) const
    {
        if (prim) {
            add(prim.GetPath(), prim.GetPrimStack(), into);
        }
    }
};

// What only a live projection reads, recorded per material and gprim: the Storm surface's
// placement and the Cycles and MoonRay networks.
struct ProjectionSpecs {
    WrittenSpecs properties;
    WrittenSpecs prims;
};

struct ProjectionBake {
    std::vector<UnbakedProjection> unbaked;
    std::set<std::string> behind;
    // Materials whose cropped projection shows the plate's data beyond its format once baked.
    std::set<std::string> overscan;
    // Materials setting controls baked UVs cannot honour, and those controls.
    std::map<std::string, QStringList> ignoredControls;
    // The prim whose authoring failed, if one did.
    std::string failed;
    // The specs the Geometry projections that were baked and those that keep their Storm shader
    // reach. A baked one's specs are stripped unless a kept one also reaches them, since one
    // referenced layer can serve both.
    ProjectionSpecs baked;
    ProjectionSpecs kept;
    // The Cycles and MoonRay networks and the primvars only they read, stripped from every
    // projection: a renderer reading the export takes the baked surface, or the Storm shader of a
    // kept one, which it ignores, rather than a network nothing outside Natron maintains.
    ProjectionSpecs networks;
};

/**
 * @brief Bakes the USDProject3D materials of scene for renderers other than Storm, as overs in
 * output: each mesh bound to one gets its projection as faceVarying st (sampled at times when the
 * camera or the mesh moves) and the material's surface becomes an unlit UsdPreviewSurface reading
 * the plate's texture through st. A material bound to nothing, inside an instance, or also bound to
 * a gprim that is not a mesh or lies inside an instance keeps its Storm shader, with a deferred
 * camera's values written out for it: overs cannot reach below an instance. When writes is set,
 * a projection whose material and bound gprims it writes none of is left alone.
 **/
void
bakeProjections(const PXR_NS::UsdStageRefPtr& scene,
                const PXR_NS::SdfLayerRefPtr& output,
                const std::vector<double>& times,
                const WrittenSpecLocator& locator,
                const WrittenPrimFunc& writes,
                ProjectionBake* result)
{
    if (!scene || !output || times.empty()) {
        return;
    }
    const auto everyPrim = PXR_NS::UsdTraverseInstanceProxies();
    std::map<PXR_NS::SdfPath, PXR_NS::UsdShadeShader> projections;
    for (const PXR_NS::UsdPrim& prim : scene->Traverse(everyPrim)) {
        if (prim.IsA<PXR_NS::UsdShadeMaterial>()) {
            const PXR_NS::UsdShadeShader surface = PXR_NS::UsdShadeMaterial(prim).ComputeSurfaceSource();
            if (SceneNodeAuthoring::isProjectionShader(surface)) {
                projections[prim.GetPath()] = surface;
            }
        }
    }
    if (projections.empty()) {
        return;
    }
    std::map<PXR_NS::SdfPath, std::vector<PXR_NS::UsdPrim>> bound;
    // Every material a gprim shows under any purpose, where bound holds the one it bakes from.
    std::map<PXR_NS::SdfPath, std::vector<PXR_NS::UsdPrim>> users;
    for (const PXR_NS::UsdPrim& prim : scene->Traverse(everyPrim)) {
        if (!prim.IsA<PXR_NS::UsdGeomGprim>()) {
            continue;
        }
        // A material can be bound for one purpose only, and a mesh baked under any of them keeps
        // its projection in the export.
        const PXR_NS::UsdShadeMaterialBindingAPI binding(prim);
        bool baking = false;
        PXR_NS::SdfPathSet used;
        for (const PXR_NS::TfToken& purpose : { PXR_NS::UsdShadeTokens->allPurpose, PXR_NS::UsdShadeTokens->full, PXR_NS::UsdShadeTokens->preview }) {
            const PXR_NS::UsdShadeMaterial material = binding.ComputeBoundMaterial(purpose);
            if (material && projections.count(material.GetPath())) {
                if (!baking) {
                    bound[material.GetPath()].push_back(prim);
                    baking = true;
                }
                if (used.insert(material.GetPath()).second) {
                    users[material.GetPath()].push_back(prim);
                }
            }
        }
    }

    const PXR_NS::UsdStageRefPtr out = PXR_NS::UsdStage::Open(output, PXR_NS::UsdStage::LoadNone);
    if (!out) {
        result->failed = output->GetIdentifier();

        return;
    }
    const PXR_NS::TfToken st("st");
    for (const std::pair<const PXR_NS::SdfPath, PXR_NS::UsdShadeShader>& entry : projections) {
        const PXR_NS::SdfPath& materialPath = entry.first;
        const PXR_NS::UsdShadeShader& shader = entry.second;
        const std::vector<PXR_NS::UsdPrim>& gprims = bound[materialPath];
        if (writes && !writes(materialPath) && std::none_of(gprims.begin(), gprims.end(), [&writes](const PXR_NS::UsdPrim& prim) {
                return writes(prim.GetPath());
            })) {
            continue;
        }
        SceneGeometryProjection geometryProjection;
        SceneNodeAuthoring::readGeometryProjection(scene->GetPrimAtPath(materialPath), &geometryProjection);
        const std::vector<PXR_NS::UsdPrim>::const_iterator instanced = std::find_if(gprims.begin(), gprims.end(), [](const PXR_NS::UsdPrim& prim) {
            return prim.IsInstanceProxy();
        });
        // HasValue() on a primvar with no attribute throws instead of answering false, which on the write thread terminates the process.
        const auto hasValue = [](const PXR_NS::UsdGeomPrimvar& primvar) {
            return primvar.GetAttr() && primvar.HasValue();
        };
        const auto referenceOf = [&geometryProjection](const PXR_NS::UsdPrim& prim) {
            return geometryProjection.referencePrimvar.empty()
                ? PXR_NS::UsdGeomPrimvar(PXR_NS::UsdAttribute())
                : PXR_NS::UsdGeomPrimvarsAPI(prim).GetPrimvar(PXR_NS::TfToken(geometryProjection.referencePrimvar));
        };
        const auto frameOf = [&geometryProjection](const PXR_NS::UsdPrim& prim) {
            return geometryProjection.framePrimvar.empty()
                ? PXR_NS::UsdGeomPrimvar(PXR_NS::UsdAttribute())
                : PXR_NS::UsdGeomPrimvarsAPI(prim).FindPrimvarWithInheritance(PXR_NS::TfToken(geometryProjection.framePrimvar));
        };
        // By the mark on their nodes, since a network holds nodes no surface reads, such as the
        // facing test under Project On Both, and the user may author shaders of their own under
        // the material.
        const auto recordNetworks = [&](const std::vector<PXR_NS::UsdPrim>& prims) {
            ProjectionSpecs* specs = &result->networks;
            const PXR_NS::UsdPrim materialPrim = scene->GetPrimAtPath(materialPath);
            const PXR_NS::UsdShadeMaterial material(materialPrim);
            if (materialPrim) {
                for (const PXR_NS::UsdPrim& child : materialPrim.GetAllChildren()) {
                    if (isProjectionNetworkNode(child)) {
                        locator.addPrim(child, &specs->prims);
                    }
                }
            }
            for (const char* context : { "cycles", "moonray" }) {
                const PXR_NS::UsdShadeOutput output = material ? material.GetSurfaceOutput(PXR_NS::TfToken(context)) : PXR_NS::UsdShadeOutput();
                if (output) {
                    locator.addAttribute(output.GetAttr(), &specs->properties);
                }
            }
            if (geometryProjection.framePrimvar.empty()) {
                return;
            }
            std::vector<std::string> names;
            for (int column = 0; column < 3; ++column) {
                names.push_back(SceneProjectionCameras::frameColumnPrimvar(geometryProjection.framePrimvar, column));
            }
            for (int part = 0; part < 4; ++part) {
                names.push_back(SceneProjectionCameras::moonRayFramePrimvar(geometryProjection.framePrimvar, part));
            }
            if (!geometryProjection.referencePrimvar.empty()) {
                names.push_back(SceneProjectionCameras::cyclesReferencePresentPrimvar(geometryProjection.referencePrimvar));
            }
            for (const PXR_NS::UsdPrim& prim : prims) {
                for (const std::string& name : names) {
                    locator.addAttribute(PXR_NS::UsdGeomPrimvarsAPI(prim).FindPrimvarWithInheritance(PXR_NS::TfToken(name)).GetAttr(), &specs->properties);
                }
            }
        };
        const auto recordSpecs = [&](const std::vector<PXR_NS::UsdPrim>& prims, ProjectionSpecs* specs) {
            recordNetworks(prims);
            if (geometryProjection.framePrimvar.empty()) {
                return;
            }
            for (const PXR_NS::UsdPrim& prim : prims) {
                locator.addAttribute(frameOf(prim).GetAttr(), &specs->properties);
                locator.addAttribute(referenceOf(prim).GetAttr(), &specs->properties);
            }
            for (const char* name : { kUSDProject3DInputGeometryFrame, kUSDProject3DInputReferencePoints }) {
                const PXR_NS::UsdShadeInput input = shader.GetInput(PXR_NS::TfToken(name));
                if (!input) {
                    continue;
                }
                PXR_NS::UsdShadeConnectableAPI reader;
                PXR_NS::TfToken readerOutput;
                PXR_NS::UsdShadeAttributeType readerOutputType;
                if (input.GetConnectedSource(&reader, &readerOutput, &readerOutputType)) {
                    locator.addPrim(reader.GetPrim(), &specs->prims);
                }
                locator.addAttribute(input.GetAttr(), &specs->properties);
            }
            locator.addAttribute(shader.GetPrim().GetAttribute(PXR_NS::TfToken(kUSDProject3DAttrFrameHold)), &specs->properties);
        };
        const std::vector<PXR_NS::UsdPrim>::const_iterator other = std::find_if(gprims.begin(), gprims.end(), [&referenceOf, &hasValue](const PXR_NS::UsdPrim& prim) {
            return !prim.IsA<PXR_NS::UsdGeomMesh>() && !hasValue(referenceOf(prim));
        });
        const bool materialInstanced = shader.GetPrim().IsInstanceProxy();
        if (materialInstanced || gprims.empty() || (instanced != gprims.end()) || (other != gprims.end())) {
            recordSpecs(users[materialPath], &result->kept);
            if (materialInstanced) {
                result->unbaked.push_back(UnbakedProjection { materialPath.GetString(), UnbakedProjection::eReasonInstanced, materialPath.GetString() });
            } else if (gprims.empty()) {
                result->unbaked.push_back(UnbakedProjection { materialPath.GetString(), UnbakedProjection::eReasonNotBound, std::string() });
            } else if (instanced != gprims.end()) {
                result->unbaked.push_back(UnbakedProjection { materialPath.GetString(), UnbakedProjection::eReasonInstanced, instanced->GetPath().GetString() });
            } else {
                result->unbaked.push_back(UnbakedProjection { materialPath.GetString(), UnbakedProjection::eReasonNotAMesh, other->GetPath().GetString() });
            }
            if (!materialInstanced && !authorResolvedProjector(shader, out)) {
                result->failed = materialPath.GetString();

                return;
            }
            continue;
        }

        QStringList ignored;
        const auto differs = [&shader](const char* name, int defaultValue) {
            const PXR_NS::UsdShadeInput input = shader.GetInput(PXR_NS::TfToken(name));
            int value = defaultValue;

            return input && input.Get(&value) && (value != defaultValue);
        };
        if (differs("projectOn", (int)USDProject3D::eProjectOnBoth)) {
            ignored.push_back(QString::fromUtf8("Project On"));
        }
        if (differs("clip", 0)) {
            ignored.push_back(QString::fromUtf8("Clip Near Far"));
        }
        if (differs("outside", (int)USDProject3D::eOutsideBlack)) {
            ignored.push_back(QString::fromUtf8("Outside"));
        }
        if (!ignored.isEmpty()) {
            result->ignoredControls[materialPath.GetString()] = ignored;
        }

        const PXR_NS::UsdShadeInput matrixInput = shader.GetInput(PXR_NS::TfToken(kUSDProject3DInputWorldToProjector));
        const PXR_NS::UsdShadeInput windowInput = shader.GetInput(PXR_NS::TfToken(kUSDProject3DInputPlateTransform));
        const PXR_NS::UsdShadeInput spaceInput = shader.GetInput(PXR_NS::TfToken(kUSDProject3DInputProjectionSpace));
        int space = (int)USDProject3D::eProjectionSpaceWorld;
        if (spaceInput) {
            spaceInput.Get(&space);
        }
        const bool geometrySpace = (space == (int)USDProject3D::eProjectionSpaceGeometry);
        int crop = 0;
        const PXR_NS::UsdShadeInput cropInput = shader.GetInput(PXR_NS::TfToken("crop"));
        if (cropInput) {
            cropInput.Get(&crop);
        }
        const auto mightVary = [](const PXR_NS::UsdShadeInput& input) {
            return input && input.GetAttr().ValueMightBeTimeVarying();
        };
        const bool cameraVaries = mightVary(matrixInput) || mightVary(windowInput);
        for (const PXR_NS::UsdPrim& prim : gprims) {
            const PXR_NS::UsdGeomMesh mesh(prim);
            const PXR_NS::UsdGeomPrimvar framePrimvar = frameOf(prim);
            const bool hasFrame = hasValue(framePrimvar);
            const PXR_NS::UsdGeomPrimvar referencePrimvar = referenceOf(prim);
            const bool hasReference = hasValue(referencePrimvar);
            const bool hasFaces = prim.IsA<PXR_NS::UsdGeomMesh>();
            const bool varies = cameraVaries || (!hasReference && (meshMightVary(mesh) || (hasFrame && framePrimvar.ValueMightBeTimeVarying())));
            const PXR_NS::UsdPrim over = out->OverridePrim(prim.GetPath());
            if (!over) {
                result->failed = prim.GetPath().GetString();

                return;
            }
            // Over any st the layer already holds, whose time samples would win over a default.
            for (const char* name : { "primvars:st", "primvars:st:indices" }) {
                const PXR_NS::UsdAttribute attribute = over.GetAttribute(PXR_NS::TfToken(name));
                if (attribute) {
                    attribute.Clear();
                }
            }
            const PXR_NS::UsdGeomPrimvar primvar = PXR_NS::UsdGeomPrimvarsAPI(over).CreatePrimvar(st, PXR_NS::SdfValueTypeNames->TexCoord2fArray, hasFaces ? PXR_NS::UsdGeomTokens->faceVarying : PXR_NS::UsdGeomTokens->vertex);
            if (!primvar) {
                result->failed = prim.GetPath().GetString();

                return;
            }
            const std::size_t sampleCount = varies ? times.size() : 1;
            for (std::size_t i = 0; i < sampleCount; ++i) {
                const PXR_NS::UsdTimeCode time(times[i]);
                const PXR_NS::UsdTimeCode authored = varies ? time : PXR_NS::UsdTimeCode::Default();
                PXR_NS::GfMatrix4d worldToProjector(1.);
                PXR_NS::GfMatrix4d geometryFrame(1.);
                PXR_NS::GfVec4f window(1.f, 1.f, 0.f, 0.f);
                PXR_NS::VtVec3fArray points;
                PXR_NS::VtIntArray counts;
                PXR_NS::VtIntArray indices;
                if (matrixInput) {
                    matrixInput.Get(&worldToProjector, time);
                }
                if (hasFrame) {
                    framePrimvar.Get(&geometryFrame, time);
                }
                if (windowInput) {
                    windowInput.Get(&window, time);
                }
                if (crop && ((window[0] < 1.f) || (window[1] < 1.f))) {
                    result->overscan.insert(materialPath.GetString());
                }
                if (hasReference) {
                    referencePrimvar.Get(&points);
                } else {
                    mesh.GetPointsAttr().Get(&points, time);
                }
                if (hasFaces) {
                    mesh.GetFaceVertexCountsAttr().Get(&counts, time);
                    mesh.GetFaceVertexIndicesAttr().Get(&indices, time);
                }
                // In Geometry space the projection was set up on the geometry as its own node placed
                // it, wherever it has been moved since. Reference points are already in the world.
                const PXR_NS::GfMatrix4d toReference = hasReference ? PXR_NS::GfMatrix4d(1.) : geometrySpace ? geometryFrame
                                                                                                             : PXR_NS::UsdGeomXformCache(time).GetLocalToWorldTransform(prim);
                const PXR_NS::GfMatrix4d toProjector = toReference * worldToProjector;

                PXR_NS::VtVec2fArray uvs(points.size());
                std::vector<double> depths(points.size());
                for (std::size_t p = 0; p < points.size(); ++p) {
                    const PXR_NS::GfVec4d c = PXR_NS::GfVec4d(points[p][0], points[p][1], points[p][2], 1.) * toProjector;
                    // Behind the camera the frame is mirrored through it: the UV is where the
                    // point would be in front, the face it is on wrong either way.
                    const double w = std::max(std::abs(c[3]), 1e-12);
                    depths[p] = c[3];
                    // The camera's frame is the plate's format; the texture spans its data window.
                    uvs[p] = PXR_NS::GfVec2f((float)(c[0] / w * window[0] + window[2]), (float)(c[1] / w * window[1] + window[3]));
                }
                std::size_t offset = 0;
                for (const int count : counts) {
                    double nearest = HUGE_VAL;
                    for (int v = 0; (v < count) && (offset + v < indices.size()); ++v) {
                        const int index = indices[offset + v];
                        if ((index >= 0) && ((std::size_t)index < depths.size())) {
                            nearest = std::min(nearest, depths[index]);
                        }
                    }
                    offset += (std::size_t)std::max(count, 0);
                    if ((nearest <= 0.) && (nearest != HUGE_VAL)) {
                        result->behind.insert(prim.GetPath().GetString());
                    }
                }
                if (!hasFaces && !depths.empty() && (*std::min_element(depths.begin(), depths.end()) <= 0.)) {
                    result->behind.insert(prim.GetPath().GetString());
                }
                if (!primvar.Set(uvs, authored) || (hasFaces && !primvar.SetIndices(indices, authored))) {
                    result->failed = prim.GetPath().GetString();

                    return;
                }
            }
        }

        // The plate's texture, already reading st, feeds an unlit preview surface in place of the
        // projection shader. The plate holds straight colour, which the presence opacity mode
        // multiplies by the plate's alpha as the live projection does.
        const PXR_NS::UsdShadeMaterial material(out->OverridePrim(materialPath));
        PXR_NS::UsdShadeShader surface = PXR_NS::UsdShadeShader::Define(out, materialPath.AppendChild(PXR_NS::TfToken("BakedSurface")));
        PXR_NS::UsdShadeInput emissive;
        bool ok = material && surface && surface.CreateIdAttr(PXR_NS::VtValue(PXR_NS::TfToken("UsdPreviewSurface")))
            && SceneNodeAuthoring::authorUnlitSurfaceInputs(surface, &emissive);
        PXR_NS::UsdShadeConnectableAPI plateSource;
        PXR_NS::TfToken plateOutput;
        PXR_NS::UsdShadeAttributeType plateType;
        const PXR_NS::UsdShadeInput plate = shader.GetInput(PXR_NS::TfToken("plate"));
        if (ok && plate && plate.GetConnectedSource(&plateSource, &plateOutput, &plateType)) {
            PXR_NS::UsdShadeShader texture(out->OverridePrim(plateSource.GetPath()));
            const PXR_NS::UsdShadeOutput rgb = texture ? texture.CreateOutput(PXR_NS::TfToken("rgb"), PXR_NS::SdfValueTypeNames->Float3) : PXR_NS::UsdShadeOutput();
            ok = rgb && emissive.ConnectToSource(rgb) && SceneNodeAuthoring::connectOpacityToAlpha(surface, texture);
        } else if (ok) {
            ok = emissive.Set(PXR_NS::GfVec3f(0.f));
        }
        const PXR_NS::UsdShadeOutput surfaceOutput = ok ? surface.CreateOutput(PXR_NS::TfToken("surface"), PXR_NS::SdfValueTypeNames->Token) : PXR_NS::UsdShadeOutput();
        ok = surfaceOutput && material.CreateSurfaceOutput().ConnectToSource(surfaceOutput);
        const PXR_NS::UsdPrim projection = ok ? out->OverridePrim(shader.GetPath()) : PXR_NS::UsdPrim();
        if (!projection || !projection.SetActive(false)) {
            result->failed = materialPath.GetString();

            return;
        }
        recordSpecs(gprims, &result->baked);
    }
} // bakeProjections

// Of baked, what kept does not reach, and all of always, by written stack.
std::map<std::size_t, std::vector<PXR_NS::SdfPath>>
getStrippableSpecs(const WrittenSpecs& baked,
                   const WrittenSpecs& kept,
                   const WrittenSpecs& always)
{
    WrittenSpecs strippable = always;

    for (const WrittenSpec& spec : baked) {
        if (!kept.count(spec)) {
            strippable.insert(spec);
        }
    }
    std::map<std::size_t, std::vector<PXR_NS::SdfPath>> byStack;
    for (const WrittenSpec& spec : strippable) {
        byStack[spec.first].push_back(spec.second);
    }

    return byStack;
}

void
stripProjectionSpecs(const PXR_NS::SdfLayerRefPtr& layer,
                     const std::vector<PXR_NS::SdfPath>& properties,
                     const std::vector<PXR_NS::SdfPath>& prims)
{
    for (const PXR_NS::SdfPath& path : properties) {
        const PXR_NS::SdfPropertySpecHandle property = layer->GetPropertyAtPath(path);
        if (property) {
            layer->GetPrimAtPath(path.GetPrimPath())->RemoveProperty(property);
        }
    }
    for (const PXR_NS::SdfPath& path : prims) {
        const PXR_NS::SdfPrimSpecHandle prim = layer->GetPrimAtPath(path);
        const PXR_NS::SdfPrimSpecHandle parent = prim ? layer->GetPrimAtPath(path.GetParentPath()) : PXR_NS::SdfPrimSpecHandle();
        if (parent) {
            parent->RemoveNameChild(prim);
        }
    }
}

// Why a layout of the layer regions into files fails, and the regions' nodes it names.
struct RegionLayoutFailure {
    enum KindEnum {
        eKindNone = 0,
        // Two regions name one Save Path and collisions are errors.
        eKindCollision,
        // A region's layers reach both inside and outside another region's.
        eKindCrossing,
        // One file would sit inside itself, or at two places of the layer stack.
        eKindNested
    };
    KindEnum kind = eKindNone;
    QString path;
    QString first;
    QString second;
};

// The file of a layer region a layered write writes: what it holds, strongest first, and the
// metadata it gets.
struct RegionFile {
    QString path;
    std::vector<StackItem> items;
    SceneLayerRegionSettings settings;
};

QString
regionNodeName(const SceneLayerRegion* region)
{
    return QString::fromStdString(region->settings.node.empty() ? region->node : region->settings.node);
}

bool
isSameRegion(const SceneLayerRegion* a,
             const SceneLayerRegion* b)
{
    return (a == b) || (a && b && (a->id == b->id));
}

/**
 * @brief Lays the scene's stack, items strongest first, out into files: the output, first, then
 * one per Save Path the regions name (savePaths, by region id), each holding its regions' items
 * and, in place of the regions nested in it, an item naming their file. fileEntries tells, by
 * entry, those that read a file.
 *
 * Layers keep their strength order. A region gathering the layers before it takes the file read
 * right below them, which a file it sublayers stays weaker than. Layers of no region or of a
 * region nested in it that lie between a region's layers, as a merge can leave them, go into its
 * file with them; a region reaching both inside and outside another region's layers cannot be
 * written that way. With merge, regions naming one Save Path are one file, where the strongest of
 * them sits, and the layers between them go into it too.
 *
 * A file's metadata is that of the regions with no Save Path whose layers it holds, then that of
 * the regions naming it, each the weaker first, so that the file's own regions win.
 **/
bool
layOutRegionFiles(std::vector<StackItem> items,
                  const std::vector<bool>& fileEntries,
                  const std::map<U64, QString>& savePaths,
                  bool merge,
                  std::vector<RegionFile>* files,
                  RegionLayoutFailure* failure)
{
    const auto isNamed = [&savePaths](const SceneLayerRegion* region) {
        return savePaths.count(region->id) != 0;
    };

    std::map<const SceneLayerRegion*, std::size_t> weakestEntry;
    for (const StackItem& item : items) {
        if (item.region && item.region->gathers) {
            std::size_t& weakest = weakestEntry[item.region];
            weakest = std::max(weakest, item.entry);
        }
    }
    for (const std::pair<const SceneLayerRegion* const, std::size_t>& gathering : weakestEntry) {
        const std::size_t below = gathering.second + 1;
        if ((below >= fileEntries.size()) || !fileEntries[below]) {
            continue;
        }
        for (StackItem& item : items) {
            if ((item.entry == below) && isSameRegion(item.region, gathering.first->parent.get())) {
                item.region = gathering.first;
            }
        }
    }

    // The named regions around each item, outermost first, as the indices of their files' groups.
    std::map<QString, std::size_t> groupOfPath;
    std::vector<QString> groupPaths;
    std::vector<std::vector<const SceneLayerRegion*>> members;
    std::vector<std::vector<std::size_t>> chains(items.size());
    for (std::size_t i = 0; i < items.size(); ++i) {
        for (const SceneLayerRegion* region = items[i].region; region; region = region->parent.get()) {
            if (!isNamed(region)) {
                continue;
            }
            const QString& path = savePaths.find(region->id)->second;
            std::map<QString, std::size_t>::const_iterator found = groupOfPath.find(path);
            std::size_t group = groupPaths.size();
            if (found == groupOfPath.end()) {
                groupOfPath[path] = group;
                groupPaths.push_back(path);
                members.push_back(std::vector<const SceneLayerRegion*>());
            } else {
                group = found->second;
            }
            std::vector<const SceneLayerRegion*>& regions = members[group];
            if (std::none_of(regions.begin(), regions.end(), [region](const SceneLayerRegion* other) {
                    return isSameRegion(other, region);
                })) {
                if (!regions.empty() && !merge) {
                    failure->kind = RegionLayoutFailure::eKindCollision;
                    failure->path = path;
                    failure->first = regionNodeName(regions.front());
                    failure->second = regionNodeName(region);

                    return false;
                }
                regions.push_back(region);
            }
            // With merge, a region inside one naming the same file is that file, not a file in it.
            if (merge && !chains[i].empty() && (chains[i].back() == group)) {
                continue;
            }
            if (std::find(chains[i].begin(), chains[i].end(), group) != chains[i].end()) {
                failure->kind = RegionLayoutFailure::eKindNested;
                failure->path = path;
                failure->first = regionNodeName(region);

                return false;
            }
            chains[i].push_back(group);
        }
        std::reverse(chains[i].begin(), chains[i].end());
    }

    files->assign(1, RegionFile());
    std::vector<int> fileOfGroup(groupPaths.size(), -1);
    std::vector<std::size_t> landing(items.size(), 0);
    const auto nested = [&](std::size_t group) {
        failure->kind = RegionLayoutFailure::eKindNested;
        failure->path = groupPaths[group];
        failure->first = regionNodeName(members[group].front());

        return false;
    };
    std::function<bool(const std::vector<std::size_t>&, std::size_t, std::size_t)> place = [&](const std::vector<std::size_t>& elements, std::size_t depth, std::size_t file) -> bool {
        const auto head = [&](std::size_t i) {
            return (chains[i].size() > depth) ? (std::ptrdiff_t)chains[i][depth] : (std::ptrdiff_t)-1;
        };
        std::map<std::size_t, std::pair<std::size_t, std::size_t>> spans;
        for (std::size_t p = 0; p < elements.size(); ++p) {
            const std::ptrdiff_t group = head(elements[p]);
            if (group < 0) {
                continue;
            }
            if (spans.count((std::size_t)group) == 0) {
                spans[(std::size_t)group] = std::make_pair(p, p);
            } else {
                spans[(std::size_t)group].second = p;
            }
        }
        std::vector<std::size_t> order;
        for (const std::pair<const std::size_t, std::pair<std::size_t, std::size_t>>& span : spans) {
            order.push_back(span.first);
        }
        std::stable_sort(order.begin(), order.end(), [&spans](std::size_t a, std::size_t b) {
            return (spans[a].second - spans[a].first) > (spans[b].second - spans[b].first);
        });
        // The widest span first, so that a region lying inside another's span is found inside it.
        std::set<std::size_t> processed;
        std::set<std::size_t> absorbed;
        for (std::size_t group : order) {
            if (absorbed.count(group) != 0) {
                continue;
            }
            const std::pair<std::size_t, std::size_t> span = spans[group];
            for (std::size_t p = span.first; p <= span.second; ++p) {
                const std::size_t i = elements[p];
                const std::ptrdiff_t other = head(i);
                if (other == (std::ptrdiff_t)group) {
                    continue;
                }
                if (other >= 0) {
                    const std::pair<std::size_t, std::size_t> otherSpan = spans[(std::size_t)other];
                    if ((processed.count((std::size_t)other) != 0) || (otherSpan.first < span.first) || (otherSpan.second > span.second)) {
                        failure->kind = RegionLayoutFailure::eKindCrossing;
                        failure->path = groupPaths[group];
                        failure->first = regionNodeName(members[group].front());
                        failure->second = regionNodeName(members[(std::size_t)other].front());

                        return false;
                    }
                    absorbed.insert((std::size_t)other);
                }
                chains[i].insert(chains[i].begin() + depth, group);
                if (std::find(chains[i].begin() + depth + 1, chains[i].end(), group) != chains[i].end()) {
                    return nested(group);
                }
            }
            processed.insert(group);
        }

        for (std::size_t p = 0; p < elements.size();) {
            const std::ptrdiff_t group = head(elements[p]);
            if (group < 0) {
                (*files)[file].items.push_back(items[elements[p]]);
                landing[elements[p]] = file;
                ++p;
                continue;
            }
            std::vector<std::size_t> inner;
            for (; (p < elements.size()) && (head(elements[p]) == group); ++p) {
                inner.push_back(elements[p]);
            }
            if (fileOfGroup[(std::size_t)group] >= 0) {
                return nested((std::size_t)group);
            }
            const std::size_t innerFile = files->size();
            fileOfGroup[(std::size_t)group] = (int)innerFile;
            files->push_back(RegionFile());
            (*files)[innerFile].path = groupPaths[(std::size_t)group];
            StackItem sublayer;
            sublayer.file = (int)innerFile;
            (*files)[file].items.push_back(sublayer);
            if (!place(inner, depth + 1, innerFile)) {
                return false;
            }
        }

        return true;
    };
    std::vector<std::size_t> all(items.size());
    for (std::size_t i = 0; i < items.size(); ++i) {
        all[i] = i;
    }
    if (!place(all, 0, 0)) {
        return false;
    }

    std::vector<std::vector<const SceneLayerRegion*>> unnamedOf(files->size());
    std::set<U64> seenUnnamed;
    for (std::size_t i = items.size(); i-- > 0;) {
        for (const SceneLayerRegion* region = items[i].region; region && !isNamed(region); region = region->parent.get()) {
            if (seenUnnamed.insert(region->id).second) {
                unnamedOf[landing[i]].push_back(region);
            }
        }
    }
    for (std::size_t file = 0; file < files->size(); ++file) {
        SceneLayerRegionSettings settings;
        for (const SceneLayerRegion* region : unnamedOf[file]) {
            settings.mergeFrom(region->settings);
        }
        for (std::size_t group = 0; group < fileOfGroup.size(); ++group) {
            if (fileOfGroup[group] == (int)file) {
                for (std::vector<const SceneLayerRegion*>::const_reverse_iterator it = members[group].rbegin(); it != members[group].rend(); ++it) {
                    settings.mergeFrom((*it)->settings);
                }
            }
        }
        (*files)[file].settings = settings;
    }

    return true;
} // layOutRegionFiles

/**
 * @brief Authors on layer the metadata a layer region sets, over what it had. A colour
 * configuration naming a file is written as the file's other asset paths are, from dir.
 **/
void
authorRegionMetadata(const PXR_NS::SdfLayerRefPtr& layer,
                     const SceneLayerRegionSettings& settings,
                     const QString& dir,
                     USDWrite::PathModeEnum pathMode)
{
    const PXR_NS::SdfPath& rootPath = PXR_NS::SdfPath::AbsoluteRootPath();

    if (settings.defaultPrim) {
        // A root prim's path names it as well as its name does.
        const std::string& name = *settings.defaultPrim;
        layer->SetDefaultPrim(PXR_NS::TfToken((!name.empty() && (name[0] == '/') && (name.find('/', 1) == std::string::npos)) ? name.substr(1) : name));
    }
    if (settings.comment) {
        layer->SetComment(*settings.comment);
    }
    if (settings.startTime) {
        layer->SetStartTimeCode(*settings.startTime);
    }
    if (settings.endTime) {
        layer->SetEndTimeCode(*settings.endTime);
    }
    if (settings.timeCodesPerSecond) {
        layer->SetTimeCodesPerSecond(*settings.timeCodesPerSecond);
    }
    if (settings.framesPerSecond) {
        layer->SetFramesPerSecond(*settings.framesPerSecond);
    }
    if (settings.upAxis) {
        layer->SetField(rootPath, PXR_NS::UsdGeomTokens->upAxis, (*settings.upAxis == SceneMetadata::eUpAxisZ) ? PXR_NS::UsdGeomTokens->z : PXR_NS::UsdGeomTokens->y);
    }
    if (settings.metersPerUnit) {
        layer->SetField(rootPath, PXR_NS::UsdGeomTokens->metersPerUnit, *settings.metersPerUnit);
    }
    if (settings.colorConfiguration) {
        const std::string& path = *settings.colorConfiguration;
        layer->SetColorConfiguration(PXR_NS::SdfAssetPath(QDir::isAbsolutePath(QString::fromStdString(path)) ? rewriteAssetPath(path, dir, pathMode) : path));
    }
    if (settings.colorManagementSystem) {
        layer->SetColorManagementSystem(PXR_NS::TfToken(*settings.colorManagementSystem));
    }
    if (settings.renderSettingsPrimPath) {
        layer->SetField(rootPath, PXR_NS::UsdRenderTokens->renderSettingsPrimPath, *settings.renderSettingsPrimPath);
    }
    if (!settings.customData.empty()) {
        PXR_NS::VtDictionary customData = layer->GetCustomLayerData();
        for (const SceneLayerCustomDatum& datum : settings.customData) {
            customData[datum.name] = datum.value;
        }
        layer->SetCustomLayerData(customData);
    }
} // authorRegionMetadata

// path, absolute in a layer whose root prims are copied under prefix, as it reads there.
PXR_NS::SdfPath
rerootedPath(const PXR_NS::SdfPath& path,
             const PXR_NS::SdfPath& prefix)
{
    if (!path.IsAbsolutePath() || (path == PXR_NS::SdfPath::AbsoluteRootPath())) {
        return path;
    }

    return prefix.AppendPath(path.MakeRelativePath(PXR_NS::SdfPath::AbsoluteRootPath()));
}

/**
 * @brief Re-roots under prefix the paths layer's specs author into its own namespace, before its
 * root prims are copied there: relationship targets, attribute connections, inherits,
 * specializes, and the prim paths of internal references and payloads.
 **/
void
rerootLayerPaths(const PXR_NS::SdfLayerRefPtr& layer,
                 const PXR_NS::SdfPath& prefix)
{
    std::vector<PXR_NS::SdfPath> specs;
    layer->Traverse(PXR_NS::SdfPath::AbsoluteRootPath(), [&specs](const PXR_NS::SdfPath& path) {
        specs.push_back(path);
    });
    const auto reroot = [&prefix](const PXR_NS::SdfPath& path) {
        return std::optional<PXR_NS::SdfPath>(rerootedPath(path, prefix));
    };
    for (const PXR_NS::SdfPath& path : specs) {
        for (const PXR_NS::TfToken& field : { PXR_NS::SdfFieldKeys->TargetPaths, PXR_NS::SdfFieldKeys->ConnectionPaths, PXR_NS::SdfFieldKeys->InheritPaths, PXR_NS::SdfFieldKeys->Specializes }) {
            PXR_NS::SdfPathListOp paths;
            if (layer->HasField(path, field, &paths)) {
                paths.ModifyOperations(reroot);
                layer->SetField(path, field, paths);
            }
        }
        PXR_NS::SdfReferenceListOp references;
        if (layer->HasField(path, PXR_NS::SdfFieldKeys->References, &references)) {
            references.ModifyOperations([&prefix](const PXR_NS::SdfReference& reference) {
                PXR_NS::SdfReference result = reference;
                if (reference.GetAssetPath().empty()) {
                    result.SetPrimPath(rerootedPath(reference.GetPrimPath(), prefix));
                }

                return std::optional<PXR_NS::SdfReference>(result);
            });
            layer->SetField(path, PXR_NS::SdfFieldKeys->References, references);
        }
        PXR_NS::SdfPayloadListOp payloads;
        if (layer->HasField(path, PXR_NS::SdfFieldKeys->Payload, &payloads)) {
            payloads.ModifyOperations([&prefix](const PXR_NS::SdfPayload& payload) {
                PXR_NS::SdfPayload result = payload;
                if (payload.GetAssetPath().empty()) {
                    result.SetPrimPath(rerootedPath(payload.GetPrimPath(), prefix));
                }

                return std::optional<PXR_NS::SdfPayload>(result);
            });
            layer->SetField(path, PXR_NS::SdfFieldKeys->Payload, payloads);
        }
    }
} // rerootLayerPaths

// Where Flatten Layers copies the stack of an anonymous layer the scene references: under prefix,
// in the written layer, whose times are its own.
struct InlinedStack {
    PXR_NS::SdfPath prefix;
    PXR_NS::TfToken defaultPrim;
    double tcps = 24.;
};

/**
 * @brief Points the references and payloads layer, of time codes per second layerTcps, authors to
 * the anonymous layers inlined names at their copies in the same layer. Composition scales an arc
 * to another layer by the ratio of the two layers' time codes per second, and an internal one by
 * nothing, so the offset takes that ratio. An arc naming no prim, to a stack with no default prim,
 * composes nothing and is dropped.
 **/
void
inlineAnonymousArcs(const PXR_NS::SdfLayerRefPtr& layer,
                    double layerTcps,
                    const std::map<std::string, InlinedStack>& inlined)
{
    const auto target = [&inlined, layerTcps](const std::string& assetPath,
                                              const PXR_NS::SdfPath& primPath,
                                              const PXR_NS::SdfLayerOffset& offset,
                                              PXR_NS::SdfPath* path,
                                              PXR_NS::SdfLayerOffset* inlinedOffset) {
        const std::map<std::string, InlinedStack>::const_iterator found = inlined.find(assetPath);
        if (found == inlined.end()) {
            return false;
        }
        const InlinedStack& stack = found->second;
        const PXR_NS::SdfPath named = !primPath.IsEmpty() ? primPath : (stack.defaultPrim.IsEmpty() ? PXR_NS::SdfPath() : PXR_NS::SdfPath::AbsoluteRootPath().AppendChild(stack.defaultPrim));
        *path = named.IsEmpty() ? named : rerootedPath(named, stack.prefix);
        *inlinedOffset = offset * PXR_NS::SdfLayerOffset(0., layerTcps / stack.tcps);

        return true;
    };
    for (const PXR_NS::SdfPath& path : primSpecPaths(layer)) {
        PXR_NS::SdfReferenceListOp references;
        if (layer->HasField(path, PXR_NS::SdfFieldKeys->References, &references)) {
            references.ModifyOperations([&target](const PXR_NS::SdfReference& reference) -> std::optional<PXR_NS::SdfReference> {
                PXR_NS::SdfPath primPath;
                PXR_NS::SdfLayerOffset offset;
                if (!target(reference.GetAssetPath(), reference.GetPrimPath(), reference.GetLayerOffset(), &primPath, &offset)) {
                    return reference;
                }
                if (primPath.IsEmpty()) {
                    return std::nullopt;
                }

                return PXR_NS::SdfReference(std::string(), primPath, offset, reference.GetCustomData());
            });
            layer->SetField(path, PXR_NS::SdfFieldKeys->References, references);
        }
        PXR_NS::SdfPayloadListOp payloads;
        if (layer->HasField(path, PXR_NS::SdfFieldKeys->Payload, &payloads)) {
            payloads.ModifyOperations([&target](const PXR_NS::SdfPayload& payload) -> std::optional<PXR_NS::SdfPayload> {
                PXR_NS::SdfPath primPath;
                PXR_NS::SdfLayerOffset offset;
                if (!target(payload.GetAssetPath(), payload.GetPrimPath(), payload.GetLayerOffset(), &primPath, &offset)) {
                    return payload;
                }
                if (primPath.IsEmpty()) {
                    return std::nullopt;
                }

                return PXR_NS::SdfPayload(std::string(), primPath, offset);
            });
            layer->SetField(path, PXR_NS::SdfFieldKeys->Payload, payloads);
        }
    }
} // inlineAnonymousArcs

/**
 * @brief Copies the root prims of source under prefix in layer, which prefix's ancestors are
 * created in as classes: a class composes no prim of its own on the stage, so only the arcs to
 * the copies show them. False when USD cannot copy one.
 **/
bool
copyUnderClass(const PXR_NS::SdfLayerRefPtr& source,
               const PXR_NS::SdfLayerRefPtr& layer,
               const PXR_NS::SdfPath& prefix)
{
    PXR_NS::SdfPrimSpecHandle parent = layer->GetPseudoRoot();
    for (const PXR_NS::SdfPath& path : prefix.GetPrefixes()) {
        PXR_NS::SdfPrimSpecHandle prim = layer->GetPrimAtPath(path);
        if (!prim) {
            prim = PXR_NS::SdfPrimSpec::New(parent, path.GetNameToken(), PXR_NS::SdfSpecifierClass);
        }
        if (!prim) {
            return false;
        }
        parent = prim;
    }
    for (const PXR_NS::SdfPrimSpecHandle& root : source->GetRootPrims()) {
        if (!PXR_NS::SdfCopySpec(source, root->GetPath(), layer, prefix.AppendChild(root->GetNameToken()))) {
            return false;
        }
    }

    return true;
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
                                          "removed. The layer regions a USDConfigureLayer names a Save Path for go to those files, "
                                          "which the file of the region around them sublayers in their place.")
                                           .toStdString()));
        options.push_back(ChoiceOption("flattenLayers", tr("Flatten Layers").toStdString(),
                                       tr("Flattens the whole layer stack, the files the scene reads included, into the file, "
                                          "with no sublayers and no files beside it, but keeps its references, payloads, "
                                          "variants and list edits as they are authored, where Flattened bakes them in. A "
                                          "reference to a file on disk stays a reference to it; the scenes nodes reference "
                                          "from other branches, as USDReference and USDXformPrim do, are copied into the "
                                          "file under /_references and referenced there. Save Paths are ignored. Sidecar "
                                          "files a layered write to the same name left beside it are removed, with a "
                                          "warning naming them, unless the scene reads them.")
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

    KnobBoolPtr stripLayersAboveBreaks = createKnob<KnobBool>(tr("Strip Layers Above Breaks"));
    stripLayersAboveBreaks->setName(kUSDWriteParamStripLayersAboveBreaks);
    stripLayersAboveBreaks->setHintToolTip(tr("Leaves out of the file the context layers above a USDLayerBreak, and the files they "
                                              "read, in either export mode, so the file holds only the edits made after the break. "
                                              "Prims those edits change but only the context defines are written as overs. Off, "
                                              "the whole scene is written as if there were no break."));
    stripLayersAboveBreaks->setDefaultValue(true);
    stripLayersAboveBreaks->setAnimationEnabled(false);
    page->addKnob(stripLayersAboveBreaks);
    _stripLayersAboveBreaks = stripLayersAboveBreaks;

    KnobChoicePtr saveCollisions = createKnob<KnobChoice>(tr("Save Collisions"));
    saveCollisions->setName(kUSDWriteParamSaveCollisions);
    saveCollisions->setHintToolTip(tr("What a Layered write does when two layer regions name the same Save Path. A Save "
                                      "Path naming this node's File, or a file the scene reads, is an error either way."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("error", tr("Error").toStdString(),
                                       tr("Writes nothing and fails, naming the two USDConfigureLayer nodes.").toStdString()));
        options.push_back(ChoiceOption("merge", tr("Merge Into One File").toStdString(),
                                       tr("Writes the regions into the one file, in their order in the layer stack, where "
                                          "the strongest of them sits. Where their metadata differs, the stronger region's "
                                          "wins.")
                                           .toStdString()));
        saveCollisions->populateChoices(options);
    }
    saveCollisions->setDefaultValue(eSaveCollisionsError);
    saveCollisions->setAnimationEnabled(false);
    page->addKnob(saveCollisions);
    _saveCollisions = saveCollisions;

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
    exportLiveTextures->setHintToolTip(tr("Renders the images a USDPreviewSurface's image inputs feed the scene to OpenEXR files in "
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
    const int exportModeValue = exportModeKnob ? exportModeKnob->getValue() : (int)eExportModeLayered;
    ExportModeEnum exportMode = eExportModeLayered;
    if (exportModeValue == (int)eExportModeFlattened) {
        exportMode = eExportModeFlattened;
    } else if (exportModeValue == (int)eExportModeFlattenLayers) {
        exportMode = eExportModeFlattenLayers;
    }
    KnobChoicePtr saveCollisionsKnob = _saveCollisions.lock();
    const bool mergeCollisions = saveCollisionsKnob && (saveCollisionsKnob->getValue() == (int)eSaveCollisionsMerge);
    const PathModeEnum pathMode = (pathModeKnob && (pathModeKnob->getValue() == (int)ePathModeAbsolute)) ? ePathModeAbsolute : ePathModeRelative;
    KnobBoolPtr stripLayersAboveBreaksKnob = _stripLayersAboveBreaks.lock();
    const bool stripContext = stripLayersAboveBreaksKnob && stripLayersAboveBreaksKnob->getValue() && SceneOps::hasContext(scene);
    AppInstancePtr app = getApp();
    const double fps = (app && app->getProject()) ? app->getProject()->getProjectFrameRate() : 24.;
    const SceneMetadata& metadata = scene.getMetadata();
    const double startTime = SceneOps::frameToTimeCode(scene, firstFrame, fps);
    const double endTime = SceneOps::frameToTimeCode(scene, lastFrame, fps);

    KnobBoolPtr exportLiveTexturesKnob = _exportLiveTextures.lock();
    const bool exportLiveTextures = exportLiveTexturesKnob && exportLiveTexturesKnob->getValue();
    const QString texturesDir = QDir(outputDir).filePath(outputInfo.completeBaseName() + QString::fromUtf8("_textures"));
    std::map<std::string, LiveTextureExport> liveTextures = planLiveTextureExports(SceneOps::getLiveTextures(scene), texturesDir);
    // natron:// URIs written as they are, which only this session resolves.
    std::set<std::string> unexportedLiveTextures;
    // natron:// URIs no live texture of the scene authors, such as those of a file written earlier.
    std::set<std::string> staleLiveTextures;
    // The textures of image inputs whose node was deleted, which nothing can render, and the nodes
    // that author them. Written without them, their materials show their own colour.
    std::set<std::string> deletedSourceUris;
    std::map<std::string, std::string> deletedSourceConsumers;
    for (const std::pair<const std::string, LiveTextureExport>& live : liveTextures) {
        const NodePtr source = live.second.source.node.lock();
        if (source && source->isActivated()) {
            continue;
        }
        const NodePtr consumer = live.second.source.consumer.lock();
        deletedSourceUris.insert(live.first);
        deletedSourceConsumers[live.first] = consumer ? consumer->getFullyQualifiedName() : std::string();
    }
    std::set<std::string> materialsWithoutDeletedSources;

    PXR_NS::TfErrorMark mark;
    // What is written, the file itself first, then, in layered mode, the sidecars beside it: the
    // runs of anonymous layers below a file the scene reads, and the stacks of the anonymous layers
    // the written layers reference.
    struct Written {
        QString path;
        PXR_NS::SdfLayerRefPtr layer;
    };
    std::vector<Written> written;
    ProjectionBake projectionBake;
    // The files the write produces or references, which a removal of stale sidecars spares.
    std::set<QString> keep;
    {
        // The scene's layers are read while they are flattened: no write may edit them meanwhile.
        SceneWriteBarrier::ReadScope read;
        // The written stacks, the scene's first. Each anonymous layer a written layer references
        // is one stack, numbered among the sidecars in the order the written layers reference it,
        // so the same scene gets the same names.
        std::vector<WrittenStack> stacks(1);
        // The files the context layers left out read, which the nodes reading them still need and
        // which the write must not replace with the edits after them, and the layer stack of the
        // layers written.
        std::set<QString> contextFiles;
        std::vector<PXR_NS::SdfLayerRefPtr> writtenLayers;
        if (stripContext) {
            const std::vector<SceneLayerEntry> layers = SceneOps::getLayers(scene);
            std::set<std::string> seenWritten;
            for (std::vector<SceneLayerEntry>::const_reverse_iterator it = layers.rbegin(); it != layers.rend(); ++it) {
                if (!it->context) {
                    appendSublayerClosure(it->layer, &seenWritten, &writtenLayers);
                    continue;
                }
                for (const PXR_NS::SdfLayerRefPtr& file : SceneOps::getFileLayerClosure(it->layer)) {
                    const std::string realPath = file->GetRealPath();
                    if (!realPath.empty()) {
                        contextFiles.insert(QDir::cleanPath(QString::fromStdString(realPath)));
                    }
                }
            }
            keep.insert(contextFiles.begin(), contextFiles.end());
        }
        stacks[0].path = outputPath;
        stacks[0].tcps = metadata.timeCodesPerSecond;
        stacks[0].hasRange = true;
        stacks[0].startTime = startTime;
        stacks[0].endTime = endTime;
        std::map<std::string, std::size_t> stackOfLayer;
        std::vector<StackArc> arcs;
        WrittenSpecLocator locator;
        locator.flattened = (exportMode == eExportModeFlattened);
        int sidecarIndex = 0;
        const auto nextSidecarPath = [&]() {
            return QDir(outputDir).filePath(QString::fromUtf8("%1_layer%2.%3").arg(outputInfo.completeBaseName()).arg(++sidecarIndex).arg(outputInfo.suffix()));
        };
        const auto flattenFailure = [&]() {
            const std::string errors = takeUsdErrors(mark);

            return fail(tr("The scene could not be flattened%1").arg(errors.empty() ? QString::fromUtf8(".") : QString::fromUtf8(": ") + QString::fromStdString(errors)));
        };
        // The scene's own stack, strongest first, each item tagged with its entry and region, and
        // by entry, whether it reads a file. The entries hold the regions the items point to.
        const std::vector<SceneLayerEntry> sceneLayers = SceneOps::getLayers(scene);
        std::vector<StackItem> sceneItems;
        std::vector<bool> fileEntries;
        for (std::vector<SceneLayerEntry>::const_reverse_iterator it = sceneLayers.rbegin(); it != sceneLayers.rend(); ++it) {
            const std::size_t entry = fileEntries.size();
            fileEntries.push_back(SceneOps::isFileEntry(*it));
            if (stripContext && it->context) {
                continue;
            }
            // As composition maps a sublayer of the root: by the ratio of their time codes per second.
            const PXR_NS::SdfLayerOffset toRoot(0., metadata.timeCodesPerSecond / it->layer->GetTimeCodesPerSecond());
            std::vector<StackItem> entryItems;
            appendLayerStack(it->layer, toRoot, &entryItems);
            for (StackItem& item : entryItems) {
                item.region = it->region.get();
                item.entry = entry;
                sceneItems.push_back(item);
            }
        }
        const auto regionFailure = [&](const RegionLayoutFailure& failure) {
            switch (failure.kind) {
            case RegionLayoutFailure::eKindCollision:
                return fail(tr("%1 and %2 both save their layers to %3: give them different Save Paths, or set Save Collisions "
                               "to Merge Into One File.")
                                .arg(failure.first)
                                .arg(failure.second)
                                .arg(failure.path));
            case RegionLayoutFailure::eKindCrossing:
                return fail(tr("The layers %1 saves to %2 and those of %3 interleave in the layer stack, so neither file can hold "
                               "its layers without changing their strength: give them the same Save Path and set Save Collisions "
                               "to Merge Into One File, or configure the layers before they meet.")
                                .arg(failure.first)
                                .arg(failure.path)
                                .arg(failure.second));
            case RegionLayoutFailure::eKindNested:
            case RegionLayoutFailure::eKindNone:
                break;
            }

            return fail(tr("The layers %1 saves to %2 would be written at two places of the layer stack, one inside the other: "
                           "give the regions different Save Paths.")
                            .arg(failure.first)
                            .arg(failure.path));
        };
        // Adds to stacks, at the end, each anonymous layer that layer of stack i references and is
        // not yet written, with a sidecar path when sidecar is set, and to arcs, the references.
        // Returns the identifier of a referenced layer that no longer exists, if one does not.
        const auto addReferencedStacks = [&](const PXR_NS::SdfLayerRefPtr& layer,
                                             std::size_t i,
                                             bool sidecar) -> std::string {
            for (const AnonymousArc& arc : anonymousArcs(layer)) {
                std::map<std::string, std::size_t>::const_iterator found = stackOfLayer.find(arc.identifier);
                std::size_t to = 0;
                if (found != stackOfLayer.end()) {
                    to = found->second;
                } else {
                    const PXR_NS::SdfLayerHandle referenced = PXR_NS::SdfLayer::Find(arc.identifier);
                    if (!referenced) {
                        return arc.identifier;
                    }
                    to = stacks.size();
                    stackOfLayer[arc.identifier] = to;
                    WrittenStack stack;
                    stack.source = PXR_NS::SdfLayerRefPtr(referenced);
                    if (sidecar) {
                        stack.path = nextSidecarPath();
                    }
                    stack.tcps = referenced->GetTimeCodesPerSecond();
                    stacks.push_back(stack);
                }
                // As composition maps a reference: its offset, then the scale between the two
                // layers' time codes per second.
                arcs.push_back(StackArc { i, to, arc.offset * PXR_NS::SdfLayerOffset(0., stacks[i].tcps / stacks[to].tcps) });
            }

            return std::string();
        };
        const auto missingLayer = [&](const std::string& identifier) {
            return fail(tr("The scene refers to the layer %1, which no longer exists.").arg(QString::fromStdString(identifier)));
        };
        if ((exportMode == eExportModeFlattened) && stripContext) {
            // Composed without the context, the written layers' list ops, variant selections and
            // arcs to context prims would compose to nothing: their layer stack is flattened, which
            // keeps them as authored, and only the stacks they reference are baked in.
            std::vector<StackItem> items;
            std::set<std::pair<std::string, std::pair<double, double>>> seenFiles;
            const std::vector<SceneLayerEntry> layers = SceneOps::getLayers(scene);
            for (std::vector<SceneLayerEntry>::const_reverse_iterator it = layers.rbegin(); it != layers.rend(); ++it) {
                if (it->context) {
                    continue;
                }
                const PXR_NS::SdfLayerOffset toRoot(0., metadata.timeCodesPerSecond / it->layer->GetTimeCodesPerSecond());
                std::vector<StackItem> entryItems;
                appendLayerStack(it->layer, toRoot, &entryItems);
                for (const StackItem& item : entryItems) {
                    // A file read twice the same way composes where it is strongest.
                    if (item.external && !seenFiles.insert(std::make_pair(item.layer->GetIdentifier(), std::make_pair(item.toRoot.GetOffset(), item.toRoot.GetScale()))).second) {
                        continue;
                    }
                    items.push_back(item);
                }
            }
            std::set<QString> usedFiles;
            stacks[0].layout.output = items.empty() ? PXR_NS::SdfLayer::CreateAnonymous("natron-usdwrite") : flattenRun(items, metadata.timeCodesPerSecond, &usedFiles);
            if (!stacks[0].layout.output || !bakeAnonymousArcs(stacks[0].layout.output, &usedFiles)) {
                return flattenFailure();
            }
            // The flattened file holds what these files author, but the nodes reading them, a
            // USDRead of one, still need them.
            keep.insert(usedFiles.begin(), usedFiles.end());
        } else if (exportMode == eExportModeFlattened) {
            PXR_NS::UsdStageRefPtr stage = SceneOps::compose(scene);
            stacks[0].layout.output = stage ? stage->Flatten(false) : PXR_NS::SdfLayerRefPtr();
            if (!stacks[0].layout.output) {
                return flattenFailure();
            }
            // The flattened file holds what these files author, but the nodes reading them, a
            // USDRead of one, still need them.
            std::set<QString> usedFiles;
            collectUsedFiles(stage, &usedFiles);
            keep.insert(usedFiles.begin(), usedFiles.end());
        } else if (exportMode == eExportModeFlattenLayers) {
            // Regions are not written, but their metadata goes to the one file, as that of a
            // region with no Save Path does.
            std::vector<RegionFile> files;
            RegionLayoutFailure failure;
            if (!layOutRegionFiles(sceneItems, fileEntries, std::map<U64, QString>(), false, &files, &failure)) {
                return regionFailure(failure);
            }
            stacks[0].settings = files[0].settings;
            for (std::size_t i = 0; i < stacks.size(); ++i) {
                std::vector<StackItem> stackItems;
                if (i == 0) {
                    stackItems = sceneItems;
                } else {
                    appendLayerStack(stacks[i].source, PXR_NS::SdfLayerOffset(), &stackItems);
                }
                std::vector<StackItem> items;
                std::set<std::pair<std::string, std::pair<double, double>>> seenFiles;
                std::set<std::string> seenFileLayers;
                for (const StackItem& item : stackItems) {
                    if (!item.external) {
                        locator.stacksOfLayer[item.source].push_back(i);
                        items.push_back(item);
                        continue;
                    }
                    // A file read twice the same way composes where it is strongest.
                    if (!seenFiles.insert(std::make_pair(item.layer->GetIdentifier(), std::make_pair(item.toRoot.GetOffset(), item.toRoot.GetScale()))).second) {
                        continue;
                    }
                    std::vector<PXR_NS::SdfLayerRefPtr> closure;
                    appendSublayerClosure(item.layer, &seenFileLayers, &closure);
                    for (const PXR_NS::SdfLayerRefPtr& file : closure) {
                        locator.stacksOfLayer[file->GetIdentifier()].push_back(i);
                    }
                    items.push_back(item);
                }
                std::set<QString> usedFiles;
                stacks[i].layout.output = items.empty() ? PXR_NS::SdfLayer::CreateAnonymous("natron-usdwrite") : flattenRun(items, stacks[i].tcps, &usedFiles);
                if (!stacks[i].layout.output) {
                    return flattenFailure();
                }
                // The file holds what these files author, but the nodes reading them, a USDRead of
                // one, still need them.
                keep.insert(usedFiles.begin(), usedFiles.end());
                const std::string missing = addReferencedStacks(stacks[i].layout.output, i, false);
                if (!missing.empty()) {
                    return missingLayer(missing);
                }
            }
            propagateRanges(arcs, &stacks);
        } else {
            std::map<U64, QString> savePaths;
            // By comparison path, the spelling of the first region saving there, which aliases of
            // it share so that they are one file.
            std::map<QString, QString> savePathOfKey;
            const QString outputKey = comparisonPath(outputPath);
            const QString outputDirKey = comparisonPath(outputDir);
            const QRegularExpression sidecarName = sidecarNamePattern(outputInfo);
            std::set<QString> sceneFiles;
            bool sceneFilesKnown = false;
            std::vector<std::string> views;
            std::map<std::string, std::string> env;
            if (app && app->getProject()) {
                views = app->getProject()->getProjectViewNames();
                app->getProject()->getEnvironmentVariables(env);
            }
            for (const StackItem& item : sceneItems) {
                for (const SceneLayerRegion* region = item.region; region; region = region->parent.get()) {
                    if (region->settings.savePath.empty() || (savePaths.count(region->id) != 0)) {
                        continue;
                    }
                    // As File is: the project's path variables, then the view and frame patterns,
                    // of the first frame.
                    std::string pattern = region->settings.savePath;
                    Project::expandVariable(env, pattern);
                    const QString name = QString::fromStdString(SequenceParsing::generateFileNameFromPattern(pattern, views, firstFrame, (int)view));
                    const QString path = QDir::cleanPath(QDir(outputDir).absoluteFilePath(name));
                    const QString node = regionNodeName(region);
                    const QString suffix = QFileInfo(path).suffix().toLower();
                    if ((suffix != QString::fromUtf8("usd")) && (suffix != QString::fromUtf8("usda")) && (suffix != QString::fromUtf8("usdc"))) {
                        return fail(tr("The Save Path of %1, %2, is not a USD file name: it must end in .usd, .usda or .usdc.").arg(node).arg(path));
                    }
                    const QString key = comparisonPath(path);
                    if (key == outputKey) {
                        return fail(tr("The Save Path of %1 is %2, which this node writes the root layer to: save the region to another file.").arg(node).arg(path));
                    }
                    // The sweep of stale sidecars would remove the file once the region is gone.
                    if ((comparisonPath(QFileInfo(path).absolutePath()) == outputDirKey) && sidecarName.match(QFileInfo(path).fileName()).hasMatch()) {
                        return fail(tr("The Save Path of %1, %2, has the name this node gives its sidecar files: save the region to another file.").arg(node).arg(path));
                    }
                    if (!sceneFilesKnown) {
                        const PXR_NS::UsdStageRefPtr composed = SceneOps::compose(scene);
                        if (composed) {
                            std::set<QString> used;
                            collectUsedFiles(composed, &used);
                            for (const QString& file : used) {
                                sceneFiles.insert(comparisonPath(file));
                            }
                        }
                        sceneFilesKnown = true;
                    }
                    if (sceneFiles.count(key) != 0) {
                        return fail(tr("The Save Path of %1, %2, is read by the scene it would be overwritten with: save the region to another file.").arg(node).arg(path));
                    }
                    savePaths[region->id] = savePathOfKey.insert(std::make_pair(key, path)).first->second;
                }
            }
            std::vector<RegionFile> files;
            RegionLayoutFailure failure;
            if (!layOutRegionFiles(sceneItems, fileEntries, savePaths, mergeCollisions, &files, &failure)) {
                return regionFailure(failure);
            }
            stacks[0].items = files[0].items;
            stacks[0].settings = files[0].settings;
            for (std::size_t f = 1; f < files.size(); ++f) {
                WrittenStack stack;
                stack.path = files[f].path;
                stack.tcps = metadata.timeCodesPerSecond;
                stack.hasRange = true;
                stack.startTime = startTime;
                stack.endTime = endTime;
                stack.region = true;
                stack.items = files[f].items;
                stack.settings = files[f].settings;
                stacks.push_back(stack);
            }
            for (std::size_t i = 0; i < stacks.size(); ++i) {
                std::vector<StackItem> items;
                if ((i == 0) || stacks[i].region) {
                    items = stacks[i].items;
                } else {
                    appendLayerStack(stacks[i].source, PXR_NS::SdfLayerOffset(), &items);
                }
                for (const StackItem& item : items) {
                    if (!item.external && (item.file < 0)) {
                        locator.stacksOfLayer[item.source].push_back(i);
                    }
                }
                StackLayout layout;
                if (!layOutStack(items, stacks[i].tcps, &layout)) {
                    return flattenFailure();
                }
                std::vector<PXR_NS::SdfLayerRefPtr> layers(1, layout.output);
                std::vector<QString> runPaths;
                for (const StackItem& item : layout.sublayers) {
                    const bool run = !item.external && (item.file < 0);
                    runPaths.push_back(run ? nextSidecarPath() : QString());
                    if (run) {
                        layers.push_back(item.layer);
                    }
                }
                stacks[i].layout = layout;
                stacks[i].runPaths = runPaths;

                for (const PXR_NS::SdfLayerRefPtr& layer : layers) {
                    const std::string missing = addReferencedStacks(layer, i, true);
                    if (!missing.empty()) {
                        return missingLayer(missing);
                    }
                }
            }
            propagateRanges(arcs, &stacks);
        }

        // Before the asset paths are rewritten: the baked surfaces read the plates' live textures.
        {
            std::vector<double> times;
            for (int frame = firstFrame; frame <= lastFrame; ++frame) {
                times.push_back(SceneOps::frameToTimeCode(scene, frame, fps));
            }
            // The bake's overs go through schemas, which see a prim as a material or a shader only
            // where its definition composes: a layered stack whose strongest layers come from
            // another node, a USDRead above the geometry, holds those definitions below the output.
            // Stripped, the context's definitions go back in, and the written layers' too, where
            // the scene stacks them, which is not always weaker than every written layer.
            std::vector<std::string> definitions;
            if (stripContext) {
                const std::vector<SceneLayerEntry> layers = SceneOps::getLayers(scene);
                for (std::vector<SceneLayerEntry>::const_reverse_iterator it = layers.rbegin(); it != layers.rend(); ++it) {
                    definitions.push_back(it->layer->GetIdentifier());
                }
            } else {
                std::function<void(std::size_t)> appendDefinitions = [&](std::size_t stack) {
                    for (const StackItem& item : stacks[stack].layout.sublayers) {
                        if (item.file < 0) {
                            definitions.push_back(item.layer->GetIdentifier());
                            continue;
                        }
                        definitions.push_back(stacks[(std::size_t)item.file].layout.output->GetIdentifier());
                        appendDefinitions((std::size_t)item.file);
                    }
                };
                appendDefinitions(0);
            }
            stacks[0].layout.output->SetSubLayerPaths(definitions);
            // Stripped, a projection the context alone authors, material and gprims, is no edit.
            WrittenPrimFunc writes;
            if (stripContext) {
                writes = [&writtenLayers](const PXR_NS::SdfPath& path) {
                    return writtenLayersReach(writtenLayers, path);
                };
            }
            bakeProjections(SceneOps::compose(scene), stacks[0].layout.output, times, locator, writes, &projectionBake);
            stacks[0].layout.output->SetSubLayerPaths(std::vector<std::string>());
            if (projectionBake.failed.empty()) {
                const std::map<std::size_t, std::vector<PXR_NS::SdfPath>> properties = getStrippableSpecs(projectionBake.baked.properties, projectionBake.kept.properties, projectionBake.networks.properties);
                const std::map<std::size_t, std::vector<PXR_NS::SdfPath>> prims = getStrippableSpecs(projectionBake.baked.prims, projectionBake.kept.prims, projectionBake.networks.prims);
                const std::vector<PXR_NS::SdfPath> none;
                for (std::size_t i = 0; i < stacks.size(); ++i) {
                    const std::map<std::size_t, std::vector<PXR_NS::SdfPath>>::const_iterator stackProperties = properties.find(i);
                    const std::map<std::size_t, std::vector<PXR_NS::SdfPath>>::const_iterator stackPrims = prims.find(i);
                    const std::vector<PXR_NS::SdfPath>& propertyPaths = (stackProperties != properties.end()) ? stackProperties->second : none;
                    const std::vector<PXR_NS::SdfPath>& primPaths = (stackPrims != prims.end()) ? stackPrims->second : none;
                    stripProjectionSpecs(stacks[i].layout.output, propertyPaths, primPaths);
                    for (const StackItem& item : stacks[i].layout.sublayers) {
                        if (!item.external && (item.file < 0)) {
                            stripProjectionSpecs(item.layer, propertyPaths, primPaths);
                        }
                    }
                }
            }
            if (!projectionBake.failed.empty()) {
                const std::string errors = takeUsdErrors(mark);

                return fail(tr("The projection on %1 could not be baked for export%2").arg(QString::fromStdString(projectionBake.failed)).arg(errors.empty() ? QString::fromUtf8(".") : QString::fromUtf8(": ") + QString::fromStdString(errors)));
            }
        }

        // Asset paths are written relative to dir, the directory of the file holding layer.
        const auto finish = [&](const PXR_NS::SdfLayerRefPtr& layer, const WrittenStack& stack, const QString& dir, bool clip) {
            if (clip && stack.hasRange) {
                clipTimeSamples(layer, stack.startTime, stack.endTime);
            }
            for (const std::pair<const std::string, PXR_NS::SdfPath>& removed : removeTexturesReading(layer, deletedSourceUris)) {
                const std::string& consumer = deletedSourceConsumers[removed.first];
                materialsWithoutDeletedSources.insert(consumer.empty() ? removed.second.GetString() : consumer);
            }
            PXR_NS::UsdUtilsModifyAssetPaths(layer, [&](const std::string& assetPath) {
                const std::map<std::string, std::size_t>::const_iterator sidecar = stackOfLayer.find(assetPath);
                if (sidecar != stackOfLayer.end()) {
                    return rewriteAssetPath(stacks[sidecar->second].path.toStdString(), dir, pathMode);
                }
                if (assetPath.compare(0, std::string(kLiveTextureScheme "://").size(), kLiveTextureScheme "://") == 0) {
                    const std::map<std::string, LiveTextureExport>::iterator live = liveTextures.find(assetPath);
                    if (live == liveTextures.end()) {
                        staleLiveTextures.insert(assetPath);

                        return assetPath;
                    }
                    if (!exportLiveTextures) {
                        unexportedLiveTextures.insert(assetPath);

                        return assetPath;
                    }
                    live->second.needed = true;

                    return rewriteAssetPath(live->second.path.toStdString(), dir, pathMode);
                }
                const QString referenced = referencedFilePath(assetPath);
                if (!referenced.isEmpty()) {
                    keep.insert(referenced);
                }

                return rewriteAssetPath(assetPath, dir, pathMode);
            });
        };
        if (exportMode == eExportModeFlattenLayers) {
            // One file: each referenced stack is copied into it under a class, which the arcs to
            // the stack point to in place of the sidecar layered mode writes.
            std::map<std::string, InlinedStack> inlined;
            std::vector<PXR_NS::SdfPath> prefixes(stacks.size());
            for (const std::pair<const std::string, std::size_t>& stack : stackOfLayer) {
                const std::size_t i = stack.second;
                prefixes[i] = PXR_NS::SdfPath("/_references").AppendChild(PXR_NS::TfToken("stack" + std::to_string(i)));
                InlinedStack target;
                target.prefix = prefixes[i];
                if (stacks[i].source->HasDefaultPrim()) {
                    target.defaultPrim = stacks[i].source->GetDefaultPrim();
                }
                target.tcps = stacks[i].tcps;
                inlined[stack.first] = target;
            }
            for (std::size_t i = 0; i < stacks.size(); ++i) {
                const PXR_NS::SdfLayerRefPtr& output = stacks[i].layout.output;
                if (stacks[i].hasRange) {
                    clipTimeSamples(output, stacks[i].startTime, stacks[i].endTime);
                }
                if (i > 0) {
                    rerootLayerPaths(output, prefixes[i]);
                }
                inlineAnonymousArcs(output, stacks[i].tcps, inlined);
            }
            const PXR_NS::SdfLayerRefPtr& root = stacks[0].layout.output;
            for (std::size_t i = 1; i < stacks.size(); ++i) {
                if (!copyUnderClass(stacks[i].layout.output, root, prefixes[i])) {
                    return flattenFailure();
                }
            }
            finish(root, stacks[0], outputDir, false);
            authorStageMetadata(root, metadata, startTime, endTime, fps);
            authorRegionMetadata(root, stacks[0].settings, outputDir, pathMode);
            written.push_back(Written { stacks[0].path, root });
        }
        // The time codes per second a written stack's file declares: its regions' when they set
        // one, which composition scales its sublayers by and it by in its parent.
        const auto declaredTcps = [](const WrittenStack& stack) {
            return stack.settings.timeCodesPerSecond ? *stack.settings.timeCodesPerSecond : stack.tcps;
        };
        for (const WrittenStack& stack : stacks) {
            if (exportMode == eExportModeFlattenLayers) {
                break;
            }
            const PXR_NS::SdfLayerRefPtr& output = stack.layout.output;
            const double fileTcps = declaredTcps(stack);
            const QString dir = QFileInfo(stack.path).absolutePath();
            finish(output, stack, dir, true);
            if (stack.region) {
                // Only a stage's root layer's metadata takes effect, but a region's file opened on
                // its own reads as the scene it holds.
                output->SetTimeCodesPerSecond(stack.tcps);
                const PXR_NS::SdfPath& rootPath = PXR_NS::SdfPath::AbsoluteRootPath();
                output->SetField(rootPath, PXR_NS::UsdGeomTokens->upAxis,
                                 (metadata.upAxis == SceneMetadata::eUpAxisZ) ? PXR_NS::UsdGeomTokens->z : PXR_NS::UsdGeomTokens->y);
                output->SetField(rootPath, PXR_NS::UsdGeomTokens->metersPerUnit, metadata.metersPerUnit);
                authorRegionMetadata(output, stack.settings, dir, pathMode);
            } else if (!stack.source) {
                authorStageMetadata(output, metadata, startTime, endTime, fps);
                authorRegionMetadata(output, stack.settings, dir, pathMode);
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
                if (item.file >= 0) {
                    // Its layers keep their own offsets to the root, which the region's file shares:
                    // a region's time codes per second are metadata, so the scale composition gives
                    // the two files' ratio is undone.
                    const WrittenStack& region = stacks[(std::size_t)item.file];
                    subLayerPaths.push_back(rewriteAssetPath(region.path.toStdString(), dir, pathMode));
                    subLayerOffsets.push_back(PXR_NS::SdfLayerOffset(0., declaredTcps(region) / fileTcps));
                    continue;
                }
                if (item.external) {
                    subLayerPaths.push_back(rewriteAssetPath(item.layer->GetIdentifier(), dir, pathMode));
                    // Authored so that composition, which scales by the ratio of the two layers' time
                    // codes per second, arrives at the offset the stack had.
                    subLayerOffsets.push_back(authoredOffset(item.toRoot, item.layer, fileTcps));
                    continue;
                }
                // A run below a file goes to a file of its own beside the written one, at the same
                // rate, so that composition leaves its times as they are.
                finish(item.layer, stack, outputDir, true);
                item.layer->SetTimeCodesPerSecond(fileTcps);
                written.push_back(Written { stack.runPaths[i], item.layer });
                subLayerPaths.push_back(rewriteAssetPath(stack.runPaths[i].toStdString(), dir, pathMode));
                subLayerOffsets.push_back(PXR_NS::SdfLayerOffset());
            }
            output->SetSubLayerPaths(subLayerPaths);
            for (std::size_t i = 0; i < subLayerOffsets.size(); ++i) {
                output->SetSubLayerOffset(subLayerOffsets[i], (int)i);
            }
        }
        for (const WrittenStack& stack : stacks) {
            for (const StackItem& item : stack.layout.sublayers) {
                if (!item.external || (item.file >= 0)) {
                    continue;
                }
                const QString realPath = QDir::cleanPath(QString::fromStdString(item.layer->GetRealPath()));
                keep.insert(realPath);
                const QString realKey = comparisonPath(realPath);
                for (const Written& w : written) {
                    if (realKey == comparisonPath(w.path)) {
                        return fail(tr("%1 is read by the scene it would be overwritten with: write to another file, or use Flattened mode.").arg(w.path));
                    }
                }
            }
        }
        std::set<QString> writtenPaths;
        std::set<QString> contextKeys;
        for (const QString& file : contextFiles) {
            contextKeys.insert(comparisonPath(file));
        }
        for (const Written& w : written) {
            const QString key = comparisonPath(w.path);
            if (!writtenPaths.insert(key).second) {
                return fail(tr("%1 would be written twice by this node: give the layer regions saving to it different Save Paths.").arg(w.path));
            }
            if (contextKeys.count(key) != 0) {
                return fail(tr("%1 is read above a USDLayerBreak, so it would be overwritten with only the edits after the break: write to another file.").arg(w.path));
            }
        }
    }
    // Before the files that name them, so that a failure leaves the earlier files whole, and after
    // the scene's layers are let go: a render may need to write to a scene of its own.
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
            store.prepare(LiveTextureSourceList(1, live.second.source), live.second.frame);
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
    const auto reloadOpenLayers = [](const QStringList& paths, std::string* error) {
        std::vector<PXR_NS::SdfLayerRefPtr> open;
        for (const QString& path : paths) {
            const PXR_NS::SdfLayerHandle layer = PXR_NS::SdfLayer::Find(path.toStdString());
            if (layer) {
                open.push_back(PXR_NS::SdfLayerRefPtr(layer));
            }
        }

        return open.empty() || SceneOps::reloadFileLayers(open, error);
    };
    QStringList replaced;
    // The weakest first, so that a reader of the written file never finds a sublayer missing.
    for (std::vector<Written>::const_reverse_iterator it = written.rbegin(); it != written.rend(); ++it) {
        const QString dir = QFileInfo(it->path).absolutePath();
        if (!QDir().mkpath(dir)) {
            return fail(tr("Cannot create the directory %1.").arg(dir));
        }
        if (!it->layer->Export(it->path.toStdString())) {
            const std::string errors = takeUsdErrors(mark);
            QString message = tr("Cannot write %1%2").arg(it->path).arg(errors.empty() ? QString::fromUtf8(".") : QString::fromUtf8(": ") + QString::fromStdString(errors));
            std::string reloadError;
            if (!reloadOpenLayers(replaced, &reloadError)) {
                message += QLatin1Char('\n') + tr("The scene could not read the files already written again: %1").arg(QString::fromStdString(reloadError));
            }
            mark.Clear();

            return fail(message);
        }
        replaced.push_back(it->path);
    }
    // Only once every file is written, so that a failed write leaves the earlier set whole.
    for (const Written& w : written) {
        keep.insert(QDir::cleanPath(w.path));
    }
    const QStringList removedSidecars = removeStaleSidecars(outputInfo, keep);
    std::string reloadError;
    if (!reloadOpenLayers(replaced, &reloadError)) {
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
    for (const std::string& material : materialsWithoutDeletedSources) {
        warnings.push_back(tr("The material %1 is written with its own colour in place of a texture whose node was deleted.")
                               .arg(QString::fromStdString(material)));
    }
    if (!staleLiveTextures.empty()) {
        warnings.push_back(tr("%1 names %2 live textures, such as %3, whose source is no longer available: "
                              "they cannot be exported, and show the renderer's fallback.")
                               .arg(outputInfo.fileName())
                               .arg((int)staleLiveTextures.size())
                               .arg(QString::fromStdString(*staleLiveTextures.begin())));
    }
    for (const UnbakedProjection& unbaked : projectionBake.unbaked) {
        const QString material = QString::fromStdString(unbaked.material);
        const QString prim = QString::fromStdString(unbaked.prim);
        switch (unbaked.reason) {
        case UnbakedProjection::eReasonNotBound:
            warnings.push_back(tr("The projection material %1 is written for Storm only, which other renderers do not draw: "
                                  "it is bound to no geometry here, which its projection could be baked onto as UVs.")
                                   .arg(material));
            break;
        case UnbakedProjection::eReasonNotAMesh:
            warnings.push_back(tr("The projection material %1 is written for Storm only, which other renderers do not draw: "
                                  "%2, bound to it, is not a mesh, which its projection could be baked onto as UVs.")
                                   .arg(material)
                                   .arg(prim));
            break;
        case UnbakedProjection::eReasonInstanced:
            warnings.push_back(tr("The projection material %1 is written for Storm only, which other renderers do not draw: "
                                  "%2 lies inside an instance, which an export cannot author baked UVs or surfaces into.")
                                   .arg(material)
                                   .arg(prim));
            break;
        }
    }
    const auto joinPaths = [](const std::set<std::string>& paths) {
        QStringList names;
        for (const std::string& path : paths) {
            names.push_back(QString::fromStdString(path));
        }

        return names.join(QString::fromUtf8(", "));
    };
    if (!projectionBake.behind.empty()) {
        warnings.push_back(tr("Faces of %1 reach behind the projecting camera, where their baked UVs are wrong.")
                               .arg(joinPaths(projectionBake.behind)));
    }
    if (!projectionBake.overscan.empty()) {
        warnings.push_back(tr("The projections %1 crop their image to its format, which baked UVs cannot: the export shows "
                              "the image's data beyond its format there.")
                               .arg(joinPaths(projectionBake.overscan)));
    }
    for (const std::pair<const std::string, QStringList>& ignored : projectionBake.ignoredControls) {
        warnings.push_back(tr("The projection %1 sets %2, which baked UVs cannot honour: the export ignores them.")
                               .arg(QString::fromStdString(ignored.first))
                               .arg(ignored.second.join(QString::fromUtf8(", "))));
    }
    // A layered write replaces its own sidecars, as expected; a flattened one removes files the user
    // may not know the export mode made, so it says which.
    if ((exportMode != eExportModeLayered) && !removedSidecars.isEmpty()) {
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
