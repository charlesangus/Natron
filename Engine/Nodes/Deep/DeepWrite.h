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

#ifndef Engine_Nodes_Deep_DeepWrite_h
#define Engine_Nodes_Deep_DeepWrite_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/NativeEffectBase.h"

#define PLUGINID_NATRON_DEEPWRITE "fr.natron.DeepWrite"

NATRON_NAMESPACE_ENTER

/**
 * @brief Writes its deep input to a deep EXR file -- a scanline part, or a tiled one -- and
 * passes that same deep data through to its own output, so it can sit anywhere in a chain.
 *
 * Every channel of the incoming DeepImage is written under its own name, deep AOVs included,
 * and every sample keeps the order it arrived in: nothing is sorted, merged or dropped on the
 * way out. The file is written as the node renders, so it covers the region that render asked
 * for, and a second render served from the deep cache does not write it again.
 **/
class DeepWrite
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new DeepWrite(node);
    }

    explicit DeepWrite(NodePtr node)
        : NativeEffectBase(node)
        , _filename()
        , _tiled()
    {
    }

    virtual bool supportsTiles() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

private:
    virtual NativePluginDescription getNativePluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual StatusEnum renderDeep(const DeepRenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    std::string getFilenameAtTime(double time) const WARN_UNUSED_RETURN;

    StatusEnum writeDeepImage(const std::string& filename,
                              const DeepImage& image) WARN_UNUSED_RETURN;

    KnobOutputFileWPtr _filename;
    KnobBoolWPtr _tiled;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Deep_DeepWrite_h
