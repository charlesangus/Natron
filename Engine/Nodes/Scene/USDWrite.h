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

#ifndef Engine_Nodes_Scene_USDWrite_h
#define Engine_Nodes_Scene_USDWrite_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <atomic>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/NativeEffectBase.h"

// Free of USD: NativeRegistryAll.inc includes every node header into translation units that keep
// Py_LIMITED_API, which USD's headers cannot be compiled under.

#define PLUGINID_NATRON_USDWRITE "USDWrite"

// The same name as Write's file knob, which `NatronRenderer -w <node> <file>` overrides.
#define kUSDWriteParamFile "filename"
#define kUSDWriteParamExportMode "exportMode"
#define kUSDWriteParamPathMode "pathMode"
#define kUSDWriteParamFrameRange "frameRange"
#define kUSDWriteParamFirstFrame "firstFrame"
#define kUSDWriteParamLastFrame "lastFrame"

NATRON_NAMESPACE_ENTER

/**
 * @brief Writes its input scene to one .usd, .usda or .usdc file per render, whatever the frame
 * range: Render, the render queue and `NatronRenderer -w` all run it as a single frame that writes
 * the time samples of the whole range. Its output is its input, unchanged.
 *
 * Layered mode flattens every layer Natron authored into the file and keeps the files those
 * layers bring in (USDRead's) as sublayers, as Solaris's USD ROP does with Flatten Implicit
 * Layers. Flattened mode bakes the whole composed stage, external files included, into the one
 * layer. Asset paths are written relative to the output file, or absolute.
 **/
class USDWrite
    : public NativeEffectBase {
public:
    enum ExportModeEnum {
        eExportModeLayered = 0,
        eExportModeFlattened
    };

    enum PathModeEnum {
        ePathModeRelative = 0,
        ePathModeAbsolute
    };

    enum FrameRangeEnum {
        eFrameRangeProject = 0,
        eFrameRangeManual
    };

    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Scene", .kind = eDataKindScene },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_USDWRITE,
        .label = "USDWrite",
        .description = "Write the input scene to a USD file (.usd, .usda, .usdc), once per render, with the time "
                       "samples of the frame range. Layered mode keeps the files the scene reads as sublayers and "
                       "flattens what Natron authored; Flattened mode bakes everything into one layer.",
        .grouping = PLUGIN_GROUP_3D_USD,
        .majorVersion = 1,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/USDWrite.png",
        .isWriter = true,
        .outputKind = eDataKindScene,
        .inputs = kInputs,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new USDWrite(node);
    }

    explicit USDWrite(NodePtr node);

    virtual ~USDWrite();

    virtual bool writesFrameRangeAtOnce() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual StatusEnum writeFrameRange(int firstFrame,
                                       int lastFrame,
                                       ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    // How many files this node has written.
    U64 getWriteCount() const WARN_UNUSED_RETURN
    {
        return _writeCount.load();
    }

private:
    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual void onKnobsLoaded() OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;
    virtual void getFrameRange(double* first,
                               double* last) OVERRIDE FINAL;
    virtual bool dependsOnProjectFrameRange() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum renderScene(const SceneRenderActionArgs& args,
                                   ScenePayload* outputScene,
                                   std::string* errorMessage) OVERRIDE FINAL WARN_UNUSED_RETURN;

    void refreshFrameRangeKnobs();

    KnobOutputFileWPtr _filename;
    KnobChoiceWPtr _exportMode;
    KnobChoiceWPtr _pathMode;
    KnobChoiceWPtr _frameRange;
    KnobIntWPtr _firstFrame;
    KnobIntWPtr _lastFrame;
    std::atomic<U64> _writeCount;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_USDWrite_h
