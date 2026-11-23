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

#ifndef Engine_Nodes_Geometry_GeoViewerLayer_h
#define Engine_Nodes_Geometry_GeoViewerLayer_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/GeoDetail.h"
#include "Engine/ScenePayload.h"

NATRON_NAMESPACE_ENTER

class GeoUsdConvert;

/** @brief What a click in a 3D view showing geometry picks. */
enum GeoComponentEnum {
    eGeoComponentObjects = 0,
    eGeoComponentPoints,
    eGeoComponentEdges,
    eGeoComponentPrimitives,
};

/**
 * @brief Picked components of the geometry a 3D view shows, in its numbering. The type says
 * which list is used: point or primitive numbers in elements, ascending and unique, or for edges
 * point-number pairs in edges, each with p0 <= p1, ascending and unique.
 **/
struct GeoComponentSelection {
    GeoComponentEnum type = eGeoComponentObjects;
    std::vector<GeoIndex> elements;
    std::vector<GeoEdge> edges;

    bool empty() const
    {
        return elements.empty() && edges.empty();
    }

    // Sorts and removes duplicates.
    void normalize();

    bool operator==(const GeoComponentSelection&) const = default;
};

/**
 * @brief A viewer's own copy of the geometry it shows: one natrongeo layer opened for the viewer
 * as its consumer, with no time in its key, so it is the same layer at every frame and for every
 * geometry node viewed.
 *
 * serve() and showFrame() only swap the detail the layer is served from. The layer itself is
 * edited when its owner calls refreshLayers() on the payload, with the lock it holds over the
 * stage composing the layer taken exclusively. No other stage composes the layer, so that lock is all a refresh needs:
 * the layers GeoToUSD serves to scene nodes are separate layers and never see these edits.
 **/
class GeoViewerLayer {
public:
    /**
     * @brief Serves the layer under a key of its own to @p consumer, which only tells the
     * consumers of one key apart.
     **/
    explicit GeoViewerLayer(const std::string& consumer);
    ~GeoViewerLayer();

    GeoViewerLayer(const GeoViewerLayer&) = delete;
    GeoViewerLayer& operator=(const GeoViewerLayer&) = delete;

    const std::string& getIdentifier() const;

    /**
     * @brief Makes @p detail the one the layer is served from, and returns in *payload the stack
     * holding the layer alone. The first call opens the layer on @p detail; later calls return the
     * same stack, so syncing a stage to it edits nothing and the refresh applies the new detail in
     * place. Drops the frames serveFrame() holds. Any thread.
     **/
    bool serve(const GeoDetailConstPtr& detail,
               ScenePayload* payload,
               std::string* errorMessage);

    /**
     * @brief Holds @p detail, evaluated at @p frame, until showFrame() picks it, and returns the
     * stack as serve() does. Playback evaluates frames ahead of the one shown, so a frame waits here
     * until the timeline reaches it. An evaluation outside playback (@p sequential false) drops
     * the frames held before it, which an edit may have outdated. Any thread.
     **/
    bool serveFrame(const GeoDetailConstPtr& detail,
                    int frame,
                    bool sequential,
                    ScenePayload* payload,
                    std::string* errorMessage,
                    const NodeWPtr& sourceNode = NodeWPtr(),
                    U64 sourceHash = 0);

    /**
     * @brief Serves the detail held for @p frame from now on, letting go of it. True when that
     * changed the detail served, so the layer needs a refresh; false when no detail is held for
     * @p frame. Any thread.
     **/
    bool showFrame(int frame);

    /**
     * @brief The detail the layer is served from, and in *sourceNode and *sourceHash the node
     * serveFrame() was told it was evaluated from and that node's hash then. Either pointer may be
     * null.
     **/
    GeoDetailConstPtr getDetail(NodeWPtr* sourceNode = nullptr,
                                U64* sourceHash = nullptr) const;

    /**
     * @brief The frame the detail last read from the viewer geometry layer @p identifier was
     * evaluated at, by the opening of the layer or a refresh. False before showFrame() picked any,
     * or for another layer.
     **/
    static bool getServedFrame(const std::string& identifier,
                               int* frame);

    /**
     * @brief Brings every viewer geometry layer of @p payload to the detail it is now served from.
     * Must hold off every reader of the stage composing them. Sets *edited when any layer was
     * edited.
     **/
    static bool refreshLayers(const ScenePayload& payload,
                              bool* edited,
                              std::string* errorMessage);

    // The identifiers of the viewer geometry layers in @p payload, weakest first.
    static std::vector<std::string> findLayers(const ScenePayload& payload);

    struct Pick {
        // The prim made of the primitives.
        std::string primPath;
        // Their primitive numbers, ascending.
        std::vector<GeoIndex> primitives;
    };

    /**
     * @brief A hit of a pick in a 3D view: the prim drawn and the point, edge or face of it the
     * pick landed on, numbered as the renderer numbers them, or -1 for the prim as a whole, with
     * the instancers drawing it, outermost first, and the instance of each.
     **/
    struct ComponentHit {
        std::string primPath;
        int index = -1;
        std::vector<std::pair<std::string, int>> instancers;
    };

    /**
     * @brief The conversion the open layer @p identifier holds, null for another layer. The layer
     * must not be refreshed meanwhile; the conversion returned stays valid after a refresh.
     **/
    static std::shared_ptr<const GeoUsdConvert> layerConvert(const std::string& identifier);

    /**
     * @brief Maps a prim @p convert authors, or a prim under it, back to the primitives it was
     * made from. False when the prim is none of its partitions'. Any thread.
     **/
    static bool pickPrimitives(const GeoUsdConvert& convert,
                               const std::string& primPath,
                               Pick* pick);

    /**
     * @brief Adds to @p selection the edges or primitives of @p convert's detail its type picks
     * that @p hits land on: an edge of a mesh prim, a face of a mesh prim or a curve of a curves
     * prim, or a whole packed primitive drawn by a point instancer, as a native instance or, for
     * packed USD, by its source prim. With
     * @p firstOnly, only the first hit that maps to anything counts. Packed primitives are not
     * edges. Any thread; @p selection is left normalized.
     **/
    static void mapComponentHits(const GeoUsdConvert& convert,
                                 const std::vector<ComponentHit>& hits,
                                 bool firstOnly,
                                 GeoComponentSelection* selection);

    /**
     * @brief Per mesh or curves prim of @p convert, the faces or edges of it that the primitives
     * or edges of @p selection are, numbered as the renderer numbers them, for it to highlight. A
     * mesh edge has one number per face using it. Packed primitives and points have none. Any
     * thread.
     **/
    static std::vector<std::pair<std::string, std::vector<int>>> componentHighlights(const GeoUsdConvert& convert,
                                                                                     const GeoComponentSelection& selection);

    // What the layer is served from, shared with its provider.
    struct Slot;

private:
    const std::string _key;
    const std::string _consumer;
    const std::string _identifier;

    bool openPayload(ScenePayload* payload,
                     std::string* errorMessage);

    const std::shared_ptr<Slot> _slot;
    mutable std::mutex _mutex;
    ScenePayload _payload;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Geometry_GeoViewerLayer_h
