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

#ifndef Engine_Nodes_Scene_USDRead_h
#define Engine_Nodes_Scene_USDRead_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <atomic>
#include <memory>
#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/NativeEffectBase.h"

// Free of USD: NativeRegistryAll.inc includes every node header into translation units that keep
// Py_LIMITED_API, which USD's headers cannot be compiled under.

#define PLUGINID_NATRON_USDREAD "USDRead"

#define kUSDReadParamFile "filename"
#define kUSDReadParamFrameOffset "frameOffset"
#define kUSDReadParamReload "reload"

NATRON_NAMESPACE_ENTER

struct SceneLayerEdit;

/**
 * @brief Reads a USD file (.usd, .usda, .usdc, .usdz) or, through USD's Alembic file-format
 * plug-in, an .abc file, as a scene.
 *
 * Its layer sublayers the file, so the file's prims keep their paths, and carries the file's
 * stage metadata (up axis, meters per unit, default prim, time codes per second). Time maps by
 * seconds: frame f of the project reads the file at timecode (f + frame offset) x the file's time
 * codes per second / the project frame rate, which a consumer gets by querying the composed stage
 * at SceneOps::frameToTimeCode(f).
 **/
class USDRead
    : public NativeEffectBase {
public:
    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_USDREAD,
        .label = "USDRead",
        .description = "Read a USD file (.usd, .usda, .usdc, .usdz) or an Alembic file (.abc) as a scene. "
                       "The file's prims keep their paths and the stage keeps the file's up axis and units. "
                       "Time maps by seconds: a frame reads the file at the timecode of the same instant, "
                       "shifted by the frame offset.",
        .grouping = PLUGIN_GROUP_3D_USD,
        .majorVersion = 1,
        .minorVersion = 0,
        .isWriter = false,
        .outputKind = eDataKindScene,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new USDRead(node);
    }

    explicit USDRead(NodePtr node);

    virtual ~USDRead();

    /**
     * @brief Drops the file's stamp and the memoized layer, so the next evaluation authors the
     * layer again and opens the file anew if it changed on disk. Edits no USD layer.
     **/
    virtual void purgeCaches() OVERRIDE FINAL;

private:
    struct Implementation;

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

    virtual void appendNodeContentHash(ContentHasher& h) const OVERRIDE FINAL;

    virtual StatusEnum renderScene(const SceneRenderActionArgs& args,
                                   ScenePayload* outputScene,
                                   std::string* errorMessage) OVERRIDE FINAL WARN_UNUSED_RETURN;

    bool authorLayer(const SceneLayerEdit& edit,
                     const std::string& path,
                     double frameOffset,
                     std::string* errorMessage);

    KnobFileWPtr _filename;
    KnobDoubleWPtr _frameOffset;
    KnobButtonWPtr _reload;

    // A process-wide unique token taken by each Reload, 0 before any: keys the node and the file's
    // layer, so a Reload reads the file again even when its stamp did not move.
    std::atomic<U64> _reloadGeneration;

    std::unique_ptr<Implementation> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_USDRead_h
