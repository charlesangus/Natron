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

#ifndef Engine_Nodes_IO_NativeRead_h
#define Engine_Nodes_IO_NativeRead_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>

#include "Engine/EffectInstance.h" // for PLUGINID_NATRON_READ
#include "Engine/EngineFwd.h"
#include "Engine/Nodes/NativeEffectBase.h"

#define PLUGIN_MAJOR_NATRON_READ 2

NATRON_NAMESPACE_ENTER

/**
 * @brief The native image reader, registered under the ID of the Read container
 * (PLUGINID_NATRON_READ) at major 2. The container stays registered at major 1.
 *
 * The planned knobs keep GenericReader's script names, so the host hooks and the DopeSheet
 * that look knobs up by name apply unchanged:
 *   - file and proxy: filename (kOfxImageEffectFileParamName, a metadata slave), proxy,
 *     proxyThreshold, originalProxyScale, customProxyScale
 *   - time: originalFrameRange, firstFrame, lastFrame, before, after, onMissingFrame,
 *     frameMode, startingTime, timeOffset, timeDomainUserEdited, frameRate, customFps
 *   - colour: ocioInputSpace, ocioInputSpaceIndex, ocioInputSpaceSet, and the hidden
 *     ocioConfigFile and ocioWorkingSpace
 *   - views: the hidden availableViews
 * Only filename exists so far; the node renders black.
 **/
class NativeRead
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new NativeRead(node);
    }

    explicit NativeRead(NodePtr node);

    virtual bool isReader() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual bool isGenerator() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual bool isMultiPlanar() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual bool supportsTiles() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual LayerKnobSpec getLayerKnobSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return LayerKnobSpec();
    }

    virtual StatusEnum getRegionOfDefinition(U64 hash,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

private:
    virtual NativePluginDescription getNativePluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual StatusEnum getPreferredMetadata(NodeMetadata& metadata) OVERRIDE FINAL WARN_UNUSED_RETURN;

    KnobFileWPtr _filename;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_IO_NativeRead_h
