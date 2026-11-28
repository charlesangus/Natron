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

#ifndef Engine_Nodes_Scene_TexelSplat_h
#define Engine_Nodes_Scene_TexelSplat_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Global/Enums.h"

#include "Engine/Nodes/Scene/PaintMeshIndex.h"
#include "Engine/Nodes/Scene/SceneRaster.h"
#include "Engine/Nodes/Scene/TexelPages.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"
#include "Engine/RectI.h"
#include "Engine/RotoStrokeDabs.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Paints strokes straight into the UDIM texels of the meshes under them. Each dab is found
 * on the surface through the session camera, and its footprint is drawn over the triangles it
 * touches in their own UV layout, so paint that crosses a UV seam or a tile boundary lands on both
 * sides with falloff measured on the mesh, and the texels between UV islands stay untouched.
 *
 * The brushes are RotoPaint's, with its formulas: the dabs of a stroke build its mask, by the
 * stroke's build-up rule, and the brush then blends what it makes into the texels under that mask.
 * Smear is the exception, as in RotoPaint: each of its dabs copies what lies behind it in turn.
 *
 * Texel lists are built in parallel and laid in dab order page by page, so the result does not
 * depend on the number of threads.
 **/
namespace TexelSplat {
enum FootprintEnum {
    // Within the dab's radius, in world units at the depth of the surface under its centre, of that point.
    eFootprintSurface = 0,
    // What the dab's disc covers in the view.
    eFootprintView
};

/**
 * @brief Which target mesh's UV layout covers each texel, as USDTexturePaintCache::composeTile()
 * lays the meshes out: the last mesh covering a texel owns it.
 **/
class Coverage {
public:
    // Over index's targets as they are now; index must outlive the coverage and not be rebuilt.
    Coverage(const PaintMeshIndex& index, int tileSize);
    ~Coverage();

    Coverage(const Coverage&) = delete;
    Coverage& operator=(const Coverage&) = delete;

    int tileSize() const;

    // owner[x - rect.x1 + (y - rect.y1) * rect.width()]: the owning target's index, or -1.
    void owners(int tile, const RectI& rect, int* owner) const;

    // The tiles some target triangle covers, in order.
    std::vector<int> tiles() const;

private:
    struct Implementation;
    std::unique_ptr<Implementation> _imp;
};

/**
 * @brief The base a mesh's own look gives its texels when there is no base file: colours[m], alpha
 * 1, on the texels target m owns, transparent black elsewhere.
 **/
TexelPages::BaseFill meshColourBase(const std::shared_ptr<const Coverage>& coverage,
                                    const std::vector<PXR_NS::GfVec3f>& colours);

struct Context {
    const PaintMeshIndex* index = nullptr;
    // For the seam gutter: texels no target covers take the paint laid beside them.
    const Coverage* coverage = nullptr;
    SceneRaster::ExplicitCamera camera;
    FootprintEnum footprint = eFootprintSurface;
    TexelPages* pages = nullptr;
    int plane = 0;
    // What Eraser restores, Reveal reveals and Clone copies from when it copies the base; transparent
    // black when null. Never written.
    TexelPages* base = nullptr;
    // As TextureProjection::Args::depthBias.
    double depthBias = 0.01;
    // 0 for the effect's share of the machine.
    int threads = 0;
    /**
     * Called with pages before the first write to any of them, so that they can be journalled; may
     * be called more than once per stroke, with pages not passed before.
     **/
    std::function<bool(const std::vector<TexelPages::PageKey>& pages, std::string* error)> beforeWrite;
    // When set and true, painting stops and paint() returns false with an empty error.
    std::function<bool()> cancelled;
};

struct Stroke {
    RotoStrokeType type = eRotoStrokeTypeSolid;
    // In paint space (USDTexturePaintView::paintSpaceBounds() of the camera's aspect) at mipmap level 0.
    RotoStrokeDabs::Strokes points;
    RotoStrokeDabs::BrushParams brush;
    bool buildUp = false;
    double opacity = 1.;
    double color[3] = { 1., 1., 1. };
    // R, G, B, A: the channels the stroke writes.
    bool channels[4] = { true, true, true, true };
    // Blur's and Sharpen's size, in paint-space pixels.
    double effectStrength = 15.;
    /**
     * Clone: the paint-space point a point of the stroke copies from, sourceTransform times (x, y, 1),
     * row-major.
     **/
    double sourceTransform[9] = { 1., 0., 0., 0., 1., 0., 0., 0., 1. };
    // Clone copies the base rather than the paint under the stroke.
    bool cloneFromBase = false;
};

/**
 * @brief Lays stroke into context.pages, which are written only once the whole stroke is worked
 * out. False with *error set when a page cannot be read or written or the camera is degenerate;
 * false with *error empty and the pages untouched when context.cancelled returns true first.
 **/
bool paint(const Context& context, const Stroke& stroke, std::string* error) WARN_UNUSED_RETURN;

/**
 * @brief One stroke laid into context.pages as it grows. Each paint() lays only the dabs added since
 * the one before, and leaves the pages exactly as paint() of the whole stroke would. When an earlier
 * dab, a setting or the context but for its callbacks changed, the pages the stroke wrote are put
 * back and it is laid again from its start.
 *
 * While the stroke lives, nothing else may write context.pages, which must outlive it.
 **/
class LiveStroke {
public:
    // Without a journal, the stroke can be laid only once.
    explicit LiveStroke(bool keepsJournal = true);
    ~LiveStroke();

    LiveStroke(const LiveStroke&) = delete;
    LiveStroke& operator=(const LiveStroke&) = delete;

    /**
     * As TexelSplat::paint(). Cancelled, or failing before it writes, it leaves the pages as the
     * last paint() did; failing as it writes, it leaves the stroke to be laid again by the next.
     **/
    bool paint(const Context& context, const Stroke& stroke, std::string* error) WARN_UNUSED_RETURN;

    // The pages the stroke wrote, as they were before it; the stroke cannot be laid after this.
    std::vector<TexelPages::Snapshot> takeJournal();

    // How many times the stroke was laid again from its start.
    int restarts() const;

    std::size_t dabCount() const;

private:
    struct Implementation;
    std::unique_ptr<Implementation> _imp;
};
} // namespace TexelSplat

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_TexelSplat_h
