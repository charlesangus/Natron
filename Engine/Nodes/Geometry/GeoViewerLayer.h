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
#include <vector>

#include "Engine/GeoDetail.h"
#include "Engine/ScenePayload.h"

NATRON_NAMESPACE_ENTER

class GeoUsdConvert;

/**
 * @brief A viewer's own copy of the geometry it shows: one natrongeo layer opened for the viewer
 * as its consumer, with no time in its key, so it is the same layer at every frame and for every
 * geometry node viewed.
 *
 * serve() only swaps the detail the layer is served from. The layer itself is edited when its
 * owner calls refreshLayers() on the payload, with the lock it holds over the stage composing the
 * layer taken exclusively. No other stage composes the layer, so that lock is all a refresh needs:
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
     * place. Any thread.
     **/
    bool serve(const GeoDetailConstPtr& detail,
               ScenePayload* payload,
               std::string* errorMessage);

    GeoDetailConstPtr getDetail() const;

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

private:
    const std::string _key;
    const std::string _consumer;
    const std::string _identifier;
    struct Slot;

    const std::shared_ptr<Slot> _slot;
    mutable std::mutex _mutex;
    ScenePayload _payload;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Geometry_GeoViewerLayer_h
