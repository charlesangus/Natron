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

#ifndef NATRON_ENGINE_SCENEPAYLOAD_H
#define NATRON_ENGINE_SCENEPAYLOAD_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <memory>
#include <string>

#include "Global/GlobalDefines.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The stage-level metadata of a scene, resolved over its layer stack: for each field the
 * strongest layer that authors it on its pseudo-root wins, and USD's fallback applies when none
 * does. Composition writes these onto the stage's root layer, since USD reads stage metadata from
 * the root layer only.
 **/
struct SceneMetadata {
    enum UpAxisEnum {
        eUpAxisY = 0,
        eUpAxisZ
    };

    UpAxisEnum upAxis = eUpAxisY;
    double metersPerUnit = 0.01;
    std::string defaultPrim;
    double timeCodesPerSecond = 24.;

    bool operator==(const SceneMetadata& other) const
    {
        return upAxis == other.upAxis && metersPerUnit == other.metersPerUnit && defaultPrim == other.defaultPrim && timeCodesPerSecond == other.timeCodesPerSecond;
    }

    bool operator!=(const SceneMetadata& other) const
    {
        return !(*this == other);
    }
};

struct ScenePayloadImpl;

/**
 * @brief What a scene-kind edge carries: an ordered stack of USD layers, strongest last, each one
 * holding exactly one node's edits. It is time-independent (one payload per layer stack, not per
 * frame): consumers pick the time when they query the composed stage.
 *
 * Immutable and cheap to copy. A payload made by appending a layer (SceneOps) points at its
 * base's stack, so a chain of N scene nodes holds N layer entries in all, not N squared. The
 * layers themselves are never edited once they are in a payload.
 *
 * USD types never appear here. Code that needs the layers or the composed stage goes through
 * Engine/Nodes/Scene/ (SceneOps, StageCache), the only place pxr headers are included.
 **/
class ScenePayload {
public:
    // An empty stack: no layers, stack hash 0, fallback metadata.
    ScenePayload();

    explicit ScenePayload(const std::shared_ptr<const ScenePayloadImpl>& imp);

    ~ScenePayload();

    bool isEmpty() const;

    std::size_t getNumLayers() const;

    /**
     * @brief Combines the hashes of the layers in stack order, so equal stacks hash equal and
     * appending a layer moves it. 0 only for the empty stack.
     **/
    U64 getStackHash() const;

    const SceneMetadata& getMetadata() const;

    /**
     * @brief True if both hold the same layers, by identity, in the same order.
     **/
    bool isSameStack(const ScenePayload& other) const;

    // For Engine/Nodes/Scene/ only; null for the empty stack.
    const std::shared_ptr<const ScenePayloadImpl>& getImpl() const
    {
        return _imp;
    }

private:
    std::shared_ptr<const ScenePayloadImpl> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_SCENEPAYLOAD_H
