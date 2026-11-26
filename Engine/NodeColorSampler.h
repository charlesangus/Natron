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

#include <array>
#include <optional>

#include "Engine/EngineFwd.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Renders layer of node's output over pixelRect, in pixel coordinates at mipmapLevel, synchronously on
 * the calling thread, into RAM. renderedRect, if not NULL, receives the part of pixelRect inside the region of
 * definition, which the returned image covers and may exceed.
 *
 * Returns NULL in the cases sampleNodeColor() returns false.
 *
 * abortInfo, if set, lets another thread stop the render with setAborted(); the call then returns
 * NULL. Without it the render cannot be aborted.
 **/
ImagePtr renderNodeImage(const NodePtr& node,
                         double time,
                         ViewIdx view,
                         unsigned int mipmapLevel,
                         const RectI& pixelRect,
                         const ImageLayerDesc& layer,
                         RectI* renderedRect,
                         const AbortableRenderInfoPtr& abortInfo = AbortableRenderInfoPtr());

/**
 * @brief The node whose image node renders at time: node itself, or, for a disabled node, the first
 * enabled node along its pass-through inputs, as the engine renders a disabled node. The render
 * functions here decline a disabled node, so a caller rendering an input resolves it first.
 *
 * Returns null for a null node, or a disabled one with no pass-through input to fall back on, which
 * the caller treats as a disconnected input.
 **/
NodePtr resolveDisabledNode(const NodePtr& node,
                            double time);

/**
 * @brief The part of node's image at time a texture of it holds, in full-scale pixel coordinates,
 * found without rendering: window receives its output format (its display window, the project
 * format when it has none) merged with its region of definition, whole. An infinite region is not merged,
 * which leaves the format. format receives the format.
 *
 * Returns false for a null or deactivated node, or one whose format cannot be told.
 **/
bool getNodeImageWindow(const NodePtr& node,
                        double time,
                        ViewIdx view,
                        RectI* format,
                        RectI* window);

/**
 * @brief As renderNodeImage(), at full scale over window: renderedRect receives the part of window
 * inside the region of definition, which image covers and may exceed. Pixels outside window are not
 * rendered.
 *
 * Returns false in the cases sampleNodeColor() returns false. Returns true with a null image when the
 * region of definition misses window: it is then all transparent black. abortInfo stops the render as
 * for renderNodeImage().
 **/
bool renderNodeImageOverWindow(const NodePtr& node,
                               double time,
                               ViewIdx view,
                               const ImageLayerDesc& layer,
                               const RectI& window,
                               ImagePtr* image,
                               RectI* renderedRect,
                               const AbortableRenderInfoPtr& abortInfo = AbortableRenderInfoPtr());

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
 * abortInfo stops the render as for renderNodeImage(), and the call then returns false.
 **/
bool sampleNodeColor(const NodePtr& node,
                     double time,
                     ViewIdx view,
                     unsigned int mipmapLevel,
                     const RectI& pixelRect,
                     const ImageLayerDesc& layer,
                     float rgba[4],
                     const AbortableRenderInfoPtr& abortInfo = AbortableRenderInfoPtr());

/**
 * @brief The channels of layer (RGBA unless given) of the pixel of node's input inputNb that holds
 * the canonical position (x, y) at time and view, rendered at full scale through sampleNodeColor(),
 * so a frame the input already has in the cache costs a lookup. A disabled input is looked through
 * to the node it passes on. The input's own pixel aspect ratio places (x, y) in its pixels. Channels
 * read as sampleNodeColor() reads them.
 *
 * None when node has no such input connected, (x, y) is outside the input's region of definition,
 * or in the other cases sampleNodeColor() returns false, an abort through abortInfo included. Main
 * thread or any thread not rendering node; an overlay pen event may call it.
 **/
std::optional<std::array<float, 4>> sampleNodeInputAt(const NodePtr& node,
                                                      int inputNb,
                                                      double time,
                                                      ViewIdx view,
                                                      double x,
                                                      double y,
                                                      const ImageLayerDesc& layer = ImageLayerDesc::getRGBAComponents(),
                                                      const AbortableRenderInfoPtr& abortInfo = AbortableRenderInfoPtr());

NATRON_NAMESPACE_EXIT

#endif // Engine_NodeColorSampler_h
