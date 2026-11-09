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
#include <atomic>
#include <cmath>
#include <random>

#include <QFileInfo>
#include <QString>

#include "Engine/ContentHasher.h"
#include "Engine/EffectInstance.h"
#include "Engine/FileStampRegistry.h"
#include "Engine/KnobFile.h"
#include "Engine/Nodes/Scene/SceneWriteBarrier.h"
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

// Random across sessions and never repeated within one. Until the walk knows which files the node
// reads, a digest must not name anything a persisted cache holds: those files may have changed
// since it was stored.
U64
makeUnwalkedDigest()
{
    static std::atomic<U64> next([]() {
        std::random_device random;

        return (U64(random()) << 32) | U64(random());
    }());
    ContentHasher h;
    h.appendU64(next.fetch_add(1));

    return h.finalize();
}
} // anonymous namespace

SceneFileSource::SceneFileSource()
    : _loadCount(0)
    , _walked(false)
    , _closureReloadCount(0)
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

SceneFileSource::Closure
SceneFileSource::computeClosure(const PXR_NS::SdfLayerRefPtr& layer)
{
    Closure closure;
    // Read before the walk, so a reload during it leaves the result marked stale.
    closure.reloadCount = SceneOps::getFileLayerReloadCount();
    std::vector<std::string> missing;
    std::vector<std::string> payloads;
    if (layer) {
        closure.layers = SceneOps::getFileLayerClosure(layer, &missing, &payloads);
    }
    closure.paths.reserve(closure.layers.size() + missing.size() + payloads.size());
    for (const PXR_NS::SdfLayerRefPtr& l : closure.layers) {
        closure.paths.push_back(l->GetRealPath());
    }
    closure.paths.insert(closure.paths.end(), missing.begin(), missing.end());
    closure.paths.insert(closure.paths.end(), payloads.begin(), payloads.end());
    closure.links = SceneOps::findLayerStackLinks(layer);

    return closure;
}

void
SceneFileSource::publishClosure(Closure* closure)
{
    _closure.swap(closure->layers);
    _closurePaths.swap(closure->paths);
    _links.swap(closure->links);
    _closureReloadCount = closure->reloadCount;
}

void
SceneFileSource::setClosure(const PXR_NS::SdfLayerRefPtr& layer)
{
    Closure closure = computeClosure(layer);
    std::lock_guard<std::mutex> lock(_mutex);

    publishClosure(&closure);
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

void
SceneFileSource::load(const std::string& path)
{
    // Destroying a large file's layers takes as long as reading them, so the walk that replaces
    // them lets go of them, off the main thread.
    std::lock_guard<std::mutex> lock(_mutex);

    if (path == _path) {
        return;
    }
    _path = path;
    ++_loadCount;
    _retired.insert(_retired.end(), _closure.begin(), _closure.end());
    _closure.clear();
    _closurePaths.clear();
    _links = SceneLinkIndex();
    _walked = false;
    _digest = path.empty() ? 0 : makeUnwalkedDigest();
}

bool
SceneFileSource::walk(const std::string& path)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        // File layers are never edited, so only a reload changes what they bring in.
        if (_walked && (_path == path) && !_closure.empty() && (_closureReloadCount == SceneOps::getFileLayerReloadCount())) {
            return false;
        }
    }
    if (!SceneOps::checkUsdRuntime(nullptr)) {
        return false;
    }
    // Opens files: no layer reload may run meanwhile. Taken before the mutex, so a thread waiting
    // on it holds no write off.
    SceneWriteBarrier::ReadScope read;
    std::lock_guard<std::mutex> walkLock(_walkMutex);
    std::uint64_t loadCount;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        loadCount = _loadCount;
        if (_walked && (_path == path) && !_closure.empty() && (_closureReloadCount == SceneOps::getFileLayerReloadCount())) {
            return false;
        }
    }
    Closure closure;
    {
        // Authoring reports a file USD cannot read.
        PXR_NS::TfErrorMark mark;
        closure = computeClosure(PXR_NS::SdfLayer::FindOrOpen(path));
        mark.Clear();
    }
    const U64 digest = stampPaths(closure.paths);
    // Declared before the lock, so the layers let go of are destroyed after it is released.
    std::vector<PXR_NS::SdfLayerRefPtr> retired;
    std::lock_guard<std::mutex> lock(_mutex);
    // The file changed during the walk: its result is dropped, for the next walk to redo, and
    // load() has already set the digest of the file now named.
    if (_loadCount != loadCount) {
        retired.swap(closure.layers);

        return false;
    }
    publishClosure(&closure);
    _path = path;
    _walked = true;
    retired.swap(_retired);

    return _digest.exchange(digest) != digest;
}

bool
SceneFileSource::reload(const std::string& path,
                        std::string* errorMessage,
                        std::vector<std::string>* reloadedFiles)
{
    // The layers to read again: those the last walk found and those it left closed that a stage
    // has loaded since. Before any walk, those of the file's layer if another consumer has it
    // open. Nothing closed is opened here: the next evaluation walks again, reloadFileLayers()
    // having moved the count, and that finds files that were missing.
    std::vector<PXR_NS::SdfLayerRefPtr> layers;
    std::vector<std::string> before;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        layers = _closure;
        before = _closurePaths;
    }
    PXR_NS::SdfLayerRefPtr root;
    if (!layers.empty()) {
        root = layers.front();
    } else if (!path.empty()) {
        root = PXR_NS::SdfLayerRefPtr(PXR_NS::SdfLayer::Find(path));
    }
    const std::vector<PXR_NS::SdfLayerRefPtr> open = SceneOps::getOpenFileLayers(root);
    layers.insert(layers.end(), open.begin(), open.end());
    for (const PXR_NS::SdfLayerRefPtr& layer : layers) {
        before.push_back(layer->GetRealPath());
    }
    const bool reloaded = layers.empty() || SceneOps::reloadFileLayers(layers, errorMessage);
    FileStampRegistry& stamps = FileStampRegistry::instance();
    for (const std::string& file : before) {
        if (!file.empty()) {
            stamps.invalidate(file);
        }
    }
    reloadedFiles->insert(reloadedFiles->end(), before.begin(), before.end());
    takeDigest();

    return reloaded;
} // SceneFileSource::reload

bool
SceneFileSource::refresh(const std::vector<std::string>& paths)
{
    bool reads = false;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        // Before any walk the closure is empty, but the named file is known.
        reads = !_path.empty() && (std::find(paths.begin(), paths.end(), _path) != paths.end());
        for (const std::string& path : _closurePaths) {
            if (std::find(paths.begin(), paths.end(), path) != paths.end()) {
                reads = true;
                break;
            }
        }
    }
    if (!reads) {
        return false;
    }
    takeDigest();

    return true;
}

void
SceneFileSource::reloadOtherReaders(std::vector<std::string> reloadedFiles,
                                    const std::string& resolvedPath,
                                    const EffectInstance* reloader)
{
    reloadedFiles.push_back(resolvedPath);
    std::sort(reloadedFiles.begin(), reloadedFiles.end());
    reloadedFiles.erase(std::unique(reloadedFiles.begin(), reloadedFiles.end()), reloadedFiles.end());
    reloadedFiles.erase(std::remove(reloadedFiles.begin(), reloadedFiles.end(), std::string()), reloadedFiles.end());
    KnobFile::reloadNodesReadingFiles(reloadedFiles, reloader);
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
    takeDigest();
}

void
SceneFileSource::takeDigest()
{
    bool walked;
    bool named;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        walked = _walked;
        named = !_path.empty();
    }
    if (walked) {
        _digest = stampClosurePaths();
    } else if (named) {
        _digest = makeUnwalkedDigest();
    }
}

U64
SceneFileSource::stampClosurePaths() const
{
    std::vector<std::string> paths;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        paths = _closurePaths;
    }

    return stampPaths(paths);
}

U64
SceneFileSource::stampPaths(const std::vector<std::string>& paths)
{
    FileStampRegistry& stamps = FileStampRegistry::instance();
    ContentHasher h;
    h.appendU64(paths.size());
    for (const std::string& path : paths) {
        h.appendU64(path.empty() ? 0 : stamps.stamp(path));
    }

    return h.finalize();
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
