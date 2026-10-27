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

#ifndef Engine_Nodes_Scene_SceneLayerMemo_h
#define Engine_Nodes_Scene_SceneLayerMemo_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <memory>
#include <string>

#include "Engine/ScenePayload.h"

NATRON_NAMESPACE_ENTER

struct SceneLayerEdit;

/**
 * @brief The one layer a scene node owns, memoized. produce() re-uses the last layer while the key
 * and the upstream stack are unchanged, so an edit at the end of a chain re-authors one layer,
 * not one per node. Otherwise it authors a new layer: a layer already handed out is never
 * edited again, since consumers' stages may still hold it.
 *
 * Authoring is serialized per memo, so per node. Free of pxr types so NativeEffectBase can hold
 * one; implemented in SceneOps.cpp.
 **/
class SceneLayerMemo {
public:
    /**
     * @brief Everything the node's layer depends on besides the upstream stack: nodeDigest covers
     * the node's identity and knobs, the rest the project time settings animated knobs are
     * sampled over.
     **/
    struct Key {
        // What the node's authoring reads of its upstream stack.
        enum UpstreamReadEnum {
            // Anything: the whole stack's hash keys the layer.
            eUpstreamReadWholeStack = 0,
            // Nothing: the layer is authored blind to upstream, which is not passed to the
            // author, and an upstream edit only re-stacks the memoized layer on the new stack.
            eUpstreamReadNothing,
            // Only the facts factsDigest hashes (matched prim paths, values read), which the node
            // gathers before emitting: the layer is re-authored when they change.
            eUpstreamReadFacts
        };

        U64 nodeDigest = 0;
        double firstFrame = 0.;
        double lastFrame = 0.;
        double framesPerSecond = 0.;
        UpstreamReadEnum upstreamRead = eUpstreamReadWholeStack;
        U64 factsDigest = 0;
        // Whether the layer samples anything over [firstFrame, lastFrame], animated knobs above
        // all; a node that does not leaves the range out of its key.
        bool readsFrameRange = true;
    };

    // Authors the node's edits into edit.layer. Returns false and sets the message on failure.
    typedef std::function<bool(const SceneLayerEdit& edit, std::string* errorMessage)> AuthorFunc;

    SceneLayerMemo();

    ~SceneLayerMemo();

    /**
     * @brief Sets *output to upstream with the node's layer on top, authoring the layer through
     * author unless the memoized one was made for the same key and, as far as key.upstreamRead
     * says the node reads it, the same upstream. On failure
     * returns false with *errorMessage set and keeps the previous layer memoized. layerTag names
     * a newly authored layer, for debugging.
     **/
    bool produce(const Key& key,
                 const ScenePayload& upstream,
                 const std::string& layerTag,
                 const AuthorFunc& author,
                 ScenePayload* output,
                 std::string* errorMessage);

    /**
     * @brief Forgets the memoized layer, so the next produce() authors a new one. For a change the
     * key does not see.
     **/
    void invalidate();

    // How many layers produce() has authored.
    U64 getAuthorCount() const;

private:
    struct Implementation;
    std::unique_ptr<Implementation> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_SceneLayerMemo_h
