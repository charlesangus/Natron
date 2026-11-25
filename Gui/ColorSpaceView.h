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

#ifndef Gui_ColorSpaceView_h
#define Gui_ColorSpaceView_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <memory>

#include <QtCore/QVariant>

#include "Engine/EngineFwd.h"
#include "Engine/ViewIdx.h"
#include "Gui/GuiFwd.h"

class QColor;
class QFont;
class QMouseEvent;
class QWheelEvent;
class QWidget;

NATRON_NAMESPACE_ENTER

class TextRenderer;

/**
 * @brief A 3D view of colour space drawn in a viewer's GL context in place of the image: a
 * ColorWarp's input or output pixels as a point cloud in the node's warp space, its pins as
 * source-to-target arrows, and the selected pin's target draggable in 3D. It owns an orbit camera
 * navigated with the 3D viewer's mouse bindings.
 **/
class ColorSpaceView {
public:
    /**
     * @brief Converts count packed RGBA working-space pixels in place to display values in [0, 1].
     * Called on a worker thread.
     **/
    typedef std::function<void(float* rgba, int count)> DisplayConverter;

    /**
     * @brief widget is the viewer the view draws into and asks to repaint. makeDisplayConverter is
     * called on the main thread for each fetch of the cloud; it may return an empty converter.
     * currentView gives the view the cloud is fetched for.
     **/
    ColorSpaceView(QWidget* widget,
                   const std::function<DisplayConverter()>& makeDisplayConverter,
                   const std::function<ViewIdx()>& currentView);

    ~ColorSpaceView();

    /**
     * @brief The ColorWarp node shown, or null for none. A new node re-fetches the cloud and frames it.
     **/
    void setNode(const NodePtr& node);
    NodePtr getNode() const WARN_UNUSED_RETURN;

    /**
     * @brief Whether the view is on screen: while it is not, nothing is fetched.
     **/
    void setActive(bool active);
    bool isActive() const WARN_UNUSED_RETURN;

    /**
     * @brief Whether the cloud shows the ColorWarp's output rather than its input.
     **/
    void setShowOutput(bool output);
    bool isShowingOutput() const WARN_UNUSED_RETURN;

    /**
     * @brief Points the camera at the cloud and the pins so that all of them are on screen.
     **/
    void frameAll();

    /**
     * @brief Draws the view over the whole viewport; the caller's GL context is current. The text
     * is drawn with font through text.
     **/
    void paint(const QColor& background,
               const QFont& font,
               const TextRenderer& text);

    /**
     * @brief Deletes the GL objects the view holds in the current context, which the caller makes
     * current before its context goes away.
     **/
    void releaseGL();

    bool mousePress(QMouseEvent* e);
    bool mouseMove(QMouseEvent* e);
    bool mouseRelease(QMouseEvent* e);
    bool wheel(QWheelEvent* e);

    /**
     * @brief A read-only summary for scripts: node, output, pointCount, arrowCount, holdCount,
     * selectedPin, fetching, space, axes, a camera key, and per pin its id and the widget
     * positions (logical pixels) of its source, target and, when selected, its axis handle ends.
     **/
    QVariantMap state() const WARN_UNUSED_RETURN;

    /**
     * @brief Where working-space colour (r, g, b) sits: its warp-space position, its widget
     * position, and the warp-space distance from it to the nearest point of the cloud.
     **/
    QVariantMap probe(double r,
                      double g,
                      double b) const WARN_UNUSED_RETURN;

private:
    struct Implementation;
    std::unique_ptr<Implementation> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_ColorSpaceView_h
