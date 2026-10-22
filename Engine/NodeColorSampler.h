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

#ifndef Engine_NodeColorSampler_h
#define Engine_NodeColorSampler_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Renders layer of node's output over pixelRect, in pixel coordinates at mipmapLevel, synchronously on
 * the calling thread, into RAM. renderedRect, if not NULL, receives the part of pixelRect inside the region of
 * definition, which the returned image covers and may exceed.
 *
 * Returns NULL in the cases sampleNodeColor() returns false.
 **/
ImagePtr renderNodeImage(const NodePtr& node,
                         double time,
                         ViewIdx view,
                         unsigned int mipmapLevel,
                         const RectI& pixelRect,
                         const ImageLayerDesc& layer,
                         RectI* renderedRect);

/**
 * @brief Renders node's output over pixelRect, in pixel coordinates at mipmapLevel, synchronously on
 * the calling thread, and writes the mean of every rendered pixel of layer to rgba. Asking for the
 * time, view, scale and layer the viewer last rendered makes this a cache lookup in the usual case.
 *
 * A channel the rendered layer does not have reads 0, and a missing alpha reads 1. A one-channel
 * alpha layer lands in rgba[3]. The part of pixelRect outside the node's region of definition is
 * left out of the mean.
 *
 * Returns false, leaving rgba untouched, for a null, deactivated, disabled or deep node, a node with
 * a mandatory input disconnected, a pixelRect that misses the region of definition, or a failed render.
 **/
bool sampleNodeColor(const NodePtr& node,
                     double time,
                     ViewIdx view,
                     unsigned int mipmapLevel,
                     const RectI& pixelRect,
                     const ImageLayerDesc& layer,
                     float rgba[4]);

NATRON_NAMESPACE_EXIT

#endif // Engine_NodeColorSampler_h
