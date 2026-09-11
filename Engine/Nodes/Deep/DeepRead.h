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

#ifndef Engine_Nodes_Deep_DeepRead_h
#define Engine_Nodes_Deep_DeepRead_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/NativeEffectBase.h"

#define PLUGINID_NATRON_DEEPREAD "fr.natron.DeepRead"

NATRON_NAMESPACE_ENTER

/**
 * @brief Reads a deep EXR file -- scanline or tiled part -- into a DeepImage.
 *
 * The file's per-pixel sample lists are transported verbatim: samples keep the order the
 * file stores them in, and every channel the file carries becomes a channel of the same
 * name on the output, so deep AOVs need nothing of their own here. The output is therefore
 * never declared tidy, whatever the file happens to contain.
 **/
class DeepRead
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new DeepRead(node);
    }

    explicit DeepRead(NodePtr node)
        : NativeEffectBase(node)
        , _filename()
    {
    }

    virtual bool supportsTiles() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual StatusEnum getRegionOfDefinition(U64 hash,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;

private:
    virtual NativePluginDescription getNativePluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual StatusEnum renderDeep(const DeepRenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    std::string getFilenameAtTime(double time) const WARN_UNUSED_RETURN;

    KnobFileWPtr _filename;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Deep_DeepRead_h
