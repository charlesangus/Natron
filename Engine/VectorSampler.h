/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
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

#ifndef Engine_VectorSampler_h
#define Engine_VectorSampler_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>
#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/VectorFollow.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Reads a node's motion layers, in canonical coordinates, at scale 1 over its region of
 * definition. A few frames stay in memory: a follow only moves one frame at a time, in one direction.
 *
 * A point outside the region gets the vector at the border, of confidence 0. With no confidence
 * layer every vector inside the region has confidence 1.
 *
 * Renders on the calling thread. abort() may be called from any thread, and makes a render in
 * progress return early.
 **/
class VectorSampler
    : public VectorField {
public:
    explicit VectorSampler(const NodePtr& source, int maxFrames = 6);

    virtual ~VectorSampler();

    VectorSampler(const VectorSampler&) = delete;
    VectorSampler& operator=(const VectorSampler&) = delete;

    virtual bool prepareFrame(int frame) OVERRIDE FINAL;

    // A frame that cannot be read gives zero vectors of confidence 0.
    virtual VectorSample sample(int frame, double x, double y) OVERRIDE FINAL;

    void abort();

    bool isAborted() const;

    /**
     * @brief Why the last prepareFrame() failed.
     **/
    std::string lastError() const;

    /**
     * @brief Both motion layers are among the node's outputs. Layers offered by the project alone
     * do not count: there would be nothing in them.
     **/
    static bool hasMotionLayers(const NodePtr& node, double time);

    /**
     * @brief Whether host's shapes may follow source: it must have the layers and must not depend
     * on host, whose shapes the follow writes. On false, error says why. Main thread.
     **/
    static bool checkSource(const NodePtr& source, const NodePtr& host, double time, std::string* error);

private:
    struct Imp;
    std::unique_ptr<Imp> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_VectorSampler_h
