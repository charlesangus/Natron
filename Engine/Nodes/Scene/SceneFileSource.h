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

#ifndef Engine_Nodes_Scene_SceneFileSource_h
#define Engine_Nodes_Scene_SceneFileSource_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"
#include "Engine/ScenePayload.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The file a scene node reads: its layer, from USD's layer registry and never edited, and
 * every file layer it brings in, which Reload reads again in place behind SceneWriteBarrier so
 * every stage composing them follows. The stamps of those files, taken only when they are read,
 * key the node (appendNodeContentHash), so a file brought in that changed on disk, however deep,
 * re-keys the node at the next load or Reload, including while the application was closed.
 * Shared by USDRead and USDCamera's file camera.
 **/
class SceneFileSource {
public:
    SceneFileSource();
    ~SceneFileSource();

    /**
     * @brief The layer of the file at path, shared with every other consumer through USD's layer
     * registry. Null with *errorMessage set to USD's diagnostic, or to why there is none, when
     * the file cannot be read.
     **/
    static PXR_NS::SdfLayerRefPtr openLayer(const std::string& path,
                                            std::string* errorMessage);

    // What a USD stage would read as layer's time codes per second.
    static double getTimeCodesPerSecond(const PXR_NS::SdfLayerRefPtr& layer);

    /**
     * @brief Holds layer open until the next hold(), so a stage composed meanwhile finds it rather
     * than reading the file, and walks the file layers it brings in unless the last walk was of
     * layer and no reload has run since.
     **/
    void hold(const PXR_NS::SdfLayerRefPtr& layer);

    // The links out of their root the held layer's layer stack authors, as of the last walk.
    SceneLinkIndex getLinks() const;

    /**
     * @brief A stage on layer, every payload loaded, kept until a stage on another layer is asked
     * for. Reload reads its layers again in place, so it follows the files. Read it under a
     * SceneWriteBarrier::ReadScope.
     **/
    PXR_NS::UsdStageRefPtr getStage(const PXR_NS::SdfLayerRefPtr& layer);

    // The stage getStage() last returned, or null.
    PXR_NS::UsdStageRefPtr getLastStage() const;

    /**
     * @brief Opens the file at path, none when empty, walks every file it brings in and takes
     * their stamps into the digest. Outside any render and any SceneWriteBarrier::ReadScope: when
     * the file changes and when the node is created or loaded.
     **/
    void load(const std::string& path);

    /**
     * @brief Reads again every file layer the last walk found or, before any walk, those of the
     * file at path if another consumer has it open, then walks afresh, which also finds files
     * that were missing. Drops the stamps of every file that was in the closure before or is
     * after, and appends them to *reloadedFiles. Returns false with *errorMessage set when the
     * reload fails; the walk is taken either way.
     **/
    bool reload(const std::string& path,
                std::string* errorMessage,
                std::vector<std::string>* reloadedFiles);

    /**
     * @brief EffectInstance::refreshFileDependencies() for the node: when the closure holds one of
     * paths, walks it afresh, takes the stamps into the digest and returns true. Outside any
     * SceneWriteBarrier::ReadScope.
     **/
    bool refresh(const std::vector<std::string>& paths);

    /**
     * @brief The part of a Reload that reaches the other nodes, for reloadedFiles and the node's
     * own file at resolvedPath: KnobFile::reloadNodesReadingFiles(). Main thread only.
     **/
    static void reloadOtherReaders(std::vector<std::string> reloadedFiles,
                                   const std::string& resolvedPath,
                                   const EffectInstance* reloader);

    /**
     * @brief Drops the stamps of the files the last walk found and takes them again into the
     * digest getDigest() returns. Edits no layer.
     **/
    void purgeStamps();

    // The digest the last load(), purgeStamps() or refresh() took, 0 before any.
    U64 getDigest() const;

private:
    void setClosure(const PXR_NS::SdfLayerRefPtr& layer);

    // Hashes the registry's stamps of _closurePaths: those recorded when each file was first
    // looked at, until a Reload drops them.
    U64 stampClosurePaths() const;

    mutable std::mutex _mutex;
    PXR_NS::SdfLayerRefPtr _layer;
    // Every file layer _layer brings in, its own first, as of the last walk: what reload() reads
    // again.
    std::vector<PXR_NS::SdfLayerRefPtr> _closure;
    // Their files on disk, in the same order, then the files the closure names that could not be
    // opened: stamped as missing, so one appearing re-keys the node at the next load or Reload.
    std::vector<std::string> _closurePaths;
    SceneLinkIndex _links;
    // SceneOps::getFileLayerReloadCount() when _closure was walked: hold() skips the walk, which
    // visits every spec of every layer, while no reload has run since.
    std::uint64_t _closureReloadCount;
    PXR_NS::SdfLayerRefPtr _stageLayer;
    PXR_NS::UsdStageRefPtr _stage;
    // Taken only when the files are read, never by an evaluation, so an evaluation cannot re-key
    // the node while the layers still hold what was read before.
    std::atomic<U64> _digest;
};

/**
 * @brief The node warning for a file whose up axis or meters per unit differ from the project's,
 * which no scene node converts. Empty when they match or there is no project.
 **/
std::string getSceneUnitsWarning(const SceneMetadata& file,
                                 const ProjectPtr& project);

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_SceneFileSource_h
