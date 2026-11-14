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

#ifndef Engine_Nodes_Scene_TexturePaintInteract_h
#define Engine_Nodes_Scene_TexturePaintInteract_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"

NATRON_NAMESPACE_ENTER

class TexturePaint;

namespace SceneRaster {
struct ExplicitCamera;
}

/**
 * @brief Paints a TexturePaint node from a 3D view: pen events in normalized view coordinates,
 * with the camera the view looks through, become strokes of the node's shared stroke builder in
 * the current session.
 *
 * The first pen-down after the session was closed, or through another camera than the session's,
 * starts a session through the pen's camera. A session is closed, and its projection into the
 * tiles queued on the node's baker, when the view's camera changes (closeSession() from the view),
 * when the paint tool changes, or on request (the Bake Paint key); the next stroke then starts
 * another. A session frozen by Bake Output, or deleted, takes no more strokes: the next one starts
 * a session through the same camera.
 *
 * Brush sizes are in pixels of the view, as the toolbar shows them: they are scaled into the paint
 * space, which spans TexturePaintView::kPaintSpaceSize on the view's longer side.
 *
 * Owned by the node. Main thread; never waits on the baker.
 **/
class TexturePaintInteract {
public:
    explicit TexturePaintInteract(TexturePaint* node);

    ~TexturePaintInteract();

    /**
     * @brief The factor from pixels of a viewWidth x viewHeight view to paint-space units:
     * kPaintSpaceSize over the view's longer side.
     **/
    static double brushSizeScale(int viewWidth, int viewHeight) WARN_UNUSED_RETURN;

    /**
     * @brief Begins a stroke at normalized view coordinates (u, v), (0, 0) the bottom-left corner
     * and (1, 1) the top-right one, of a viewWidth x viewHeight pixel view looking through camera,
     * at frame time. Starts a session first when needed, closing the current one if camera is not
     * its camera or the tool changed. False, with nothing begun, when the selected tool is not a
     * brush; a pen-down while a stroke is open ends that stroke first.
     **/
    bool penDown(const SceneRaster::ExplicitCamera& camera,
                 int viewWidth,
                 int viewHeight,
                 double u,
                 double v,
                 double pressure,
                 double timestamp,
                 double time);

    // Adds a point to the open stroke, in the coordinates penDown() takes. False when none is open.
    bool penMotion(double u, double v, double pressure, double timestamp);

    // Ends the open stroke and pushes its undo step, which may wait for the previous stroke's render.
    bool penUp();

    bool isPenDown() const WARN_UNUSED_RETURN
    {
        return _penDown;
    }

    /**
     * @brief Closes the current session, ending any open stroke: unless it has been frozen or
     * deleted, or took no stroke, its bake is queued at time. Returns whether a bake was queued.
     **/
    bool closeSession(double time);

    // Closes the current session when the selected tool is not the one it was painted with.
    void onToolChanged(double time);

    // The session the next stroke goes into, null when it will start one.
    RotoLayerPtr getCurrentSession() const WARN_UNUSED_RETURN;

    // The session last painted into and not closed, whether or not it still takes strokes.
    RotoLayerPtr getOpenSession() const WARN_UNUSED_RETURN
    {
        return _session.lock();
    }

    // Whether a stroke is open, or begun and still to land in its session.
    bool isPainting() const WARN_UNUSED_RETURN;

private:
    // Whether session is still one of the node's sessions, live.
    bool acceptsStrokes(const RotoLayerPtr& session) const WARN_UNUSED_RETURN;

    void toPaintSpace(double u, double v, double* x, double* y) const;

    TexturePaint* _node;
    RotoLayerWPtr _session;
    // The RotoToolEnum the current session is painted with.
    int _sessionTool;
    bool _sessionPainted;
    bool _penDown;
    // The aspect of the camera the open stroke is painted through.
    double _strokeAspect;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_TexturePaintInteract_h
