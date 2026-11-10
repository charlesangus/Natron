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

#ifndef Gui_GeoDisplay_h
#define Gui_GeoDisplay_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>
#include <string>

#include "Engine/Nodes/Geometry/GeoViewerLayer.h"
#include "Gui/SceneStageEditor.h"

class QPainter;

NATRON_NAMESPACE_ENTER

struct GeoDisplayOptions {
    bool pointMarkers = false;
    bool pointNumbers = false;
    bool primitiveNumbers = false;
    bool normals = false;
    // The length, in scene units, a normal of length 1 is drawn at.
    double normalScale = 0.1;
    // A point group, else a primitive group, of that name is highlighted. Empty for none.
    std::string highlightGroup;

    bool operator==(const GeoDisplayOptions&) const = default;
};

/**
 * @brief What a 3D view shows of the geometry it views besides the geometry: point markers,
 * normals, a highlighted group and the bounds of packed USD primitives, as guide-purpose prims of
 * a layer of its own stacked over the payload, and point and primitive numbers painted over the
 * frame.
 *
 * The stage editing runs in the view's syncs; the options, labels and picks are for the main
 * thread.
 **/
class GeoDisplay
    : public SceneStageEditor {
public:
    GeoDisplay();
    ~GeoDisplay() override;

    GeoDisplay(const GeoDisplay&) = delete;
    GeoDisplay& operator=(const GeoDisplay&) = delete;

    // True if the options differ from those in force.
    bool setOptions(const GeoDisplayOptions& options);

    GeoDisplayOptions getOptions() const;

    ScenePayload stagedPayload(const ScenePayload& payload) override;

    bool editStage(SceneStage& stage) override;

    bool showsGuidePurpose() const override;

    /**
     * @brief Paints the point and primitive numbers turned on that land in view, over a widget of
     * @p width by @p height logical pixels drawn with @p viewProjection, a row-major matrix taking
     * a world-space row vector to clip space. At most a fixed number of labels per paint; nothing
     * hides a label behind geometry. Returns how many it painted.
     **/
    int paintLabels(QPainter& painter,
                    const double viewProjection[16],
                    int width,
                    int height) const;

    // How many syncs brought the viewed geometry's layers to a new detail in place.
    U64 getRefreshCount() const;

    // The prims those in-place edits resynced in the stage, which a point edit never does.
    U64 getRefreshResyncCount() const;

    // The primitives the prim at @p primPath of the geometry last synced was made from.
    bool pickPrimitives(const std::string& primPath,
                        GeoViewerLayer::Pick* pick) const;

private:
    struct Implementation;

    const std::unique_ptr<Implementation> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_GeoDisplay_h
