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

#include "SceneFileSource.h"

#include <algorithm>
#include <cmath>

#include <QFileInfo>
#include <QString>

#include "Engine/ContentHasher.h"
#include "Engine/EffectInstance.h"
#include "Engine/FileStampRegistry.h"
#include "Engine/Project.h"

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
} // anonymous namespace

SceneFileSource::SceneFileSource()
    : _closureReloadCount(0)
    , _digest(0)
{
}

SceneFileSource::~SceneFileSource()
{
}

PXR_NS::SdfLayerRefPtr
SceneFileSource::openLayer(const std::string& path,
                           std::string* errorMessage)
{
    PXR_NS::SdfLayerRefPtr layer;
    std::string diagnostic;
    {
        PXR_NS::TfErrorMark mark;
        layer = PXR_NS::SdfLayer::FindOrOpen(path);
        diagnostic = takeUsdErrors(mark);
    }
    if (layer) {
        return layer;
    }
    // USD posts nothing for a file that is not there.
    if (!diagnostic.empty()) {
        *errorMessage = diagnostic;
    } else if (!QFileInfo::exists(QString::fromStdString(path))) {
        *errorMessage = EffectInstance::tr("%1 does not exist.").arg(QString::fromStdString(path)).toStdString();
    } else {
        *errorMessage = EffectInstance::tr("USD could not read %1.").arg(QString::fromStdString(path)).toStdString();
    }

    return PXR_NS::SdfLayerRefPtr();
}

double
SceneFileSource::getTimeCodesPerSecond(const PXR_NS::SdfLayerRefPtr& layer)
{
    if (layer->HasTimeCodesPerSecond()) {
        return layer->GetTimeCodesPerSecond();
    }
    if (layer->HasFramesPerSecond()) {
        return layer->GetFramesPerSecond();
    }

    return layer->GetTimeCodesPerSecond();
}

void
SceneFileSource::setClosure(const PXR_NS::SdfLayerRefPtr& layer)
{
    // Read before the walk, so a reload during it leaves the result marked stale.
    const std::uint64_t reloadCount = SceneOps::getFileLayerReloadCount();
    std::vector<PXR_NS::SdfLayerRefPtr> layers = layer ? SceneOps::getFileLayerClosure(layer) : std::vector<PXR_NS::SdfLayerRefPtr>();
    std::vector<std::string> paths;
    paths.reserve(layers.size());
    for (const PXR_NS::SdfLayerRefPtr& l : layers) {
        paths.push_back(l->GetRealPath());
    }
    SceneLinkIndex links = SceneOps::findLayerStackLinks(layer);
    std::lock_guard<std::mutex> lock(_mutex);
    _closure.swap(layers);
    _closurePaths.swap(paths);
    _links.swap(links);
    _closureReloadCount = reloadCount;
}

void
SceneFileSource::hold(const PXR_NS::SdfLayerRefPtr& layer)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _layer = layer;
        // File layers are never edited, so only a reload changes what they bring in.
        if (!_closure.empty() && (_closure.front() == layer) && (_closureReloadCount == SceneOps::getFileLayerReloadCount())) {
            return;
        }
    }
    setClosure(layer);
}

SceneLinkIndex
SceneFileSource::getLinks() const
{
    std::lock_guard<std::mutex> lock(_mutex);

    return _links;
}

PXR_NS::UsdStageRefPtr
SceneFileSource::getStage(const PXR_NS::SdfLayerRefPtr& layer)
{
    std::lock_guard<std::mutex> lock(_mutex);

    if (!_stage || (_stageLayer != layer)) {
        _stage = SceneOps::openStage(layer);
        _stageLayer = _stage ? layer : PXR_NS::SdfLayerRefPtr();
    }

    return _stage;
}

PXR_NS::UsdStageRefPtr
SceneFileSource::getLastStage() const
{
    std::lock_guard<std::mutex> lock(_mutex);

    return _stage;
}

bool
SceneFileSource::reload(const std::string& path,
                        std::string* errorMessage)
{
    std::vector<PXR_NS::SdfLayerRefPtr> layers;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        layers = _closure;
    }
    if (layers.empty() && !path.empty()) {
        PXR_NS::SdfLayerHandle open = PXR_NS::SdfLayer::Find(path);
        if (open) {
            layers = SceneOps::getFileLayerClosure(PXR_NS::SdfLayerRefPtr(open));
        }
    }
    const bool reloaded = layers.empty() || SceneOps::reloadFileLayers(layers, errorMessage);
    // A reload can change what the file brings in. The file's layer is the same object, reloaded
    // in place.
    setClosure(layers.empty() ? PXR_NS::SdfLayerRefPtr() : layers.front());

    return reloaded;
}

void
SceneFileSource::purgeStamps()
{
    std::vector<std::string> paths;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        paths = _closurePaths;
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
    _digest = h.finalize();
}

U64
SceneFileSource::getDigest() const
{
    return _digest.load();
}

std::string
getSceneUnitsWarning(const SceneMetadata& file,
                     const ProjectPtr& project)
{
    if (!project) {
        return std::string();
    }
    const SceneMetadata::UpAxisEnum projectUpAxis = project->isProjectUpAxisZ() ? SceneMetadata::eUpAxisZ : SceneMetadata::eUpAxisY;
    const double projectMetersPerUnit = project->getProjectMetersPerUnit();
    const bool sameMetersPerUnit = std::abs(file.metersPerUnit - projectMetersPerUnit) <= 1e-9 * std::max(std::abs(file.metersPerUnit), std::abs(projectMetersPerUnit));
    if ((file.upAxis == projectUpAxis) && sameMetersPerUnit) {
        return std::string();
    }

    return EffectInstance::tr("The file's up axis is %1 and meters per unit %2; the project's are %3 and %4. The scene is not converted.")
        .arg(upAxisName(file.upAxis))
        .arg(file.metersPerUnit)
        .arg(upAxisName(projectUpAxis))
        .arg(projectMetersPerUnit)
        .toStdString();
}

NATRON_NAMESPACE_EXIT
