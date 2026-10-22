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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "GuiApplicationManager.h"
#include "GuiApplicationManagerPrivate.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QAction>
#include <QApplication>
#include <QColor>
#include <QDebug>
#include <QFile>
#include <QFontDatabase>
#include <QIcon>
#include <QKeySequence>
#include <QMenu>
#include <QPixmapCache>
#include <QSettings>
#include <QTimer>
#include <QtConcurrentRun> // QtCore on Qt4, QtConcurrent on Qt5
#include <QtGlobal> // for Q_OS_*
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#ifdef DEBUG
#include "Global/FloatingPointExceptions.h"
#endif

#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h" // PLUGINID_OFX_*
#include "Engine/Node.h"
#include "Engine/Plugin.h"
#include "Engine/PluginActionShortcut.h"
#include "Engine/Settings.h"
#include "Gui/DocumentationManager.h"
#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/GuiDefines.h"
#include "Gui/KnobGuiFactory.h"
#include "Gui/Menu.h"
#include "Gui/PreviewThread.h"
#include "Gui/QtEnumConvert.h"
#include "Gui/SplashScreen.h"
#include "Gui/ThemedIcon.h"

//All fixed sizes were calculated for a 96 dpi screen
#ifndef Q_OS_DARWIN
#define NATRON_PIXELS_FOR_DPI_DEFAULT 96.
#else
#define NATRON_PIXELS_FOR_DPI_DEFAULT 72.
#endif

NATRON_NAMESPACE_ENTER


GuiApplicationManager::GuiApplicationManager()
    : AppManager()
    , _imp( new GuiApplicationManagerPrivate(this) )
{
    // A colour knob reports each channel separately and a colour picker reports every drag
    // step, and reapplying the stylesheet repolishes every widget, so the changes are
    // coalesced into one reapply.
    _imp->themeReloadTimer.setSingleShot(true);
    _imp->themeReloadTimer.setInterval(150);
    QObject::connect(&_imp->themeReloadTimer, SIGNAL(timeout()), this, SLOT(onThemeReloadTimerTriggered()));
}

GuiApplicationManager::~GuiApplicationManager()
{
    for (AppShortcuts::iterator it = _imp->_actionShortcuts.begin(); it != _imp->_actionShortcuts.end(); ++it) {
        for (GroupShortcuts::iterator it2 = it->second.begin(); it2 != it->second.end(); ++it2) {
            delete it2->second;
        }
    }
    _imp->previewRenderThread.quitThread(false);
    delete _imp->nodesMenu;
}

namespace {
/**
 * @brief Maps a PixmapEnum id to its resource path.
 **/
std::string
pixmapEnumToPath(PixmapEnum e)
{
    std::string path;

    switch (e) {
    case NATRON_PIXMAP_PLAYER_PREVIOUS:
        path = NATRON_ICONS_PATH "lucide/step-back.svg";
        break;
    case NATRON_PIXMAP_PLAYER_FIRST_FRAME:
        path = NATRON_ICONS_PATH "lucide/skip-back.svg";
        break;
    case NATRON_PIXMAP_PLAYER_NEXT:
        path = NATRON_ICONS_PATH "lucide/step-forward.svg";
        break;
    case NATRON_PIXMAP_PLAYER_LAST_FRAME:
        path = NATRON_ICONS_PATH "lucide/skip-forward.svg";
        break;
    case NATRON_PIXMAP_PLAYER_NEXT_INCR:
        path = NATRON_ICONS_PATH "natron/incr-next.svg";
        break;
    case NATRON_PIXMAP_PLAYER_NEXT_KEY:
        path = NATRON_ICONS_PATH "natron/key-next.svg";
        break;
    case NATRON_PIXMAP_PLAYER_PREVIOUS_INCR:
        path = NATRON_ICONS_PATH "natron/incr-prev.svg";
        break;
    case NATRON_PIXMAP_PLAYER_PREVIOUS_KEY:
        path = NATRON_ICONS_PATH "natron/key-prev.svg";
        break;
    case NATRON_PIXMAP_ADD_KEYFRAME:
        path = NATRON_ICONS_PATH "lucide/diamond-plus.svg";
        break;
    case NATRON_PIXMAP_REMOVE_KEYFRAME:
        path = NATRON_ICONS_PATH "lucide/diamond-minus.svg";
        break;
    case NATRON_PIXMAP_PLAYER_REWIND_ENABLED:
        path = NATRON_IMAGES_PATH "rewind_enabled.png";
        break;
    case NATRON_PIXMAP_PLAYER_REWIND_DISABLED:
        path = NATRON_ICONS_PATH "natron/play-reverse.svg";
        break;
    case NATRON_PIXMAP_PLAYER_PLAY_ENABLED:
        path = NATRON_ICONS_PATH "lucide/play.svg";
        break;
    case NATRON_PIXMAP_PLAYER_PLAY_DISABLED:
        path = NATRON_ICONS_PATH "lucide/play.svg";
        break;
    case NATRON_PIXMAP_PLAYER_STOP_ENABLED:
        path = NATRON_ICONS_PATH "lucide/square.svg";
        break;
    case NATRON_PIXMAP_PLAYER_STOP_DISABLED:
        path = NATRON_ICONS_PATH "lucide/square.svg";
        break;
    case NATRON_PIXMAP_PLAYER_PAUSE_ENABLED:
        path = NATRON_ICONS_PATH "lucide/pause.svg";
        break;
    case NATRON_PIXMAP_PLAYER_PAUSE_DISABLED:
        path = NATRON_ICONS_PATH "lucide/pause.svg";
        break;

    case NATRON_PIXMAP_PLAYER_LOOP_MODE:
        path = NATRON_ICONS_PATH "lucide/repeat.svg";
        break;
    case NATRON_PIXMAP_PLAYER_BOUNCE:
        path = NATRON_ICONS_PATH "lucide/arrow-left-right.svg";
        break;
    case NATRON_PIXMAP_PLAYER_PLAY_ONCE:
        path = NATRON_ICONS_PATH "lucide/arrow-right-to-line.svg";
        break;
    case NATRON_PIXMAP_PLAYER_TIMELINE_IN:
        path = NATRON_ICONS_PATH "natron/in-point.svg";
        break;
    case NATRON_PIXMAP_PLAYER_TIMELINE_OUT:
        path = NATRON_ICONS_PATH "natron/out-point.svg";
        break;
    case NATRON_PIXMAP_MAXIMIZE_WIDGET:
        path = NATRON_ICONS_PATH "lucide/square-arrow-out-up-right.svg";
        break;
    case NATRON_PIXMAP_MINIMIZE_WIDGET:
        path = NATRON_ICONS_PATH "lucide/minus.svg";
        break;
    case NATRON_PIXMAP_CLOSE_WIDGET:
        path = NATRON_ICONS_PATH "lucide/x.svg";
        break;
    case NATRON_PIXMAP_CLOSE_PANEL:
        path = NATRON_ICONS_PATH "lucide/list-x.svg";
        break;
    case NATRON_PIXMAP_HIDE_UNMODIFIED:
        path = NATRON_ICONS_PATH "lucide/list-filter.svg";
        break;
    case NATRON_PIXMAP_UNHIDE_UNMODIFIED:
        path = NATRON_ICONS_PATH "lucide/list-filter.svg";
        break;
    case NATRON_PIXMAP_HELP_WIDGET:
        path = NATRON_ICONS_PATH "lucide/circle-question-mark.svg";
        break;
    case NATRON_PIXMAP_GROUPBOX_FOLDED:
        path = NATRON_ICONS_PATH "lucide/chevron-right.svg";
        break;
    case NATRON_PIXMAP_GROUPBOX_UNFOLDED:
        path = NATRON_ICONS_PATH "lucide/chevron-down.svg";
        break;
    case NATRON_PIXMAP_UNDO:
        path = NATRON_ICONS_PATH "lucide/undo-2.svg";
        break;
    case NATRON_PIXMAP_REDO:
        path = NATRON_ICONS_PATH "lucide/redo-2.svg";
        break;
    case NATRON_PIXMAP_UNDO_GRAYSCALE:
        path = NATRON_ICONS_PATH "lucide/undo-2.svg";
        break;
    case NATRON_PIXMAP_REDO_GRAYSCALE:
        path = NATRON_ICONS_PATH "lucide/redo-2.svg";
        break;
    case NATRON_PIXMAP_RESTORE_DEFAULTS_ENABLED:
        path = NATRON_ICONS_PATH "lucide/rotate-ccw.svg";
        break;
    case NATRON_PIXMAP_RESTORE_DEFAULTS_DISABLED:
        path = NATRON_IMAGES_PATH "restoreDefaultDisabled.png";
        break;
    case NATRON_PIXMAP_TAB_WIDGET_LAYOUT_BUTTON:
        path = NATRON_ICONS_PATH "lucide/layout-grid.svg";
        break;
    case NATRON_PIXMAP_TAB_WIDGET_LAYOUT_BUTTON_ANCHOR:
        path = NATRON_ICONS_PATH "natron/layout-anchor.svg";
        break;
    case NATRON_PIXMAP_TAB_WIDGET_SPLIT_HORIZONTALLY:
        path = NATRON_ICONS_PATH "lucide/columns-2.svg";
        break;
    case NATRON_PIXMAP_TAB_WIDGET_SPLIT_VERTICALLY:
        path = NATRON_ICONS_PATH "lucide/rows-2.svg";
        break;
    case NATRON_PIXMAP_VIEWER_CENTER:
        path = NATRON_ICONS_PATH "lucide/focus.svg";
        break;
    case NATRON_PIXMAP_VIEWER_FULL_FRAME_OFF:
        path = NATRON_ICONS_PATH "lucide/fullscreen.svg";
        break;
    case NATRON_PIXMAP_VIEWER_FULL_FRAME_ON:
        path = NATRON_ICONS_PATH "lucide/fullscreen.svg";
        break;
    case NATRON_PIXMAP_VIEWER_CLIP_TO_PROJECT_ENABLED:
        path = NATRON_ICONS_PATH "lucide/square-dashed.svg";
        break;
    case NATRON_PIXMAP_VIEWER_CLIP_TO_PROJECT_DISABLED:
        path = NATRON_ICONS_PATH "lucide/square-dashed.svg";
        break;
    case NATRON_PIXMAP_VIEWER_REFRESH:
        path = NATRON_ICONS_PATH "lucide/refresh-cw.svg";
        break;
    case NATRON_PIXMAP_VIEWER_REFRESH_ACTIVE:
        path = NATRON_ICONS_PATH "lucide/refresh-cw.svg";
        break;
    case NATRON_PIXMAP_VIEWER_ROI_ENABLED:
        path = NATRON_ICONS_PATH "natron/roi.svg";
        break;
    case NATRON_PIXMAP_VIEWER_ROI_DISABLED:
        path = NATRON_ICONS_PATH "natron/roi.svg";
        break;
    case NATRON_PIXMAP_VIEWER_RENDER_SCALE:
        path = NATRON_ICONS_PATH "natron/render-scale.svg";
        break;
    case NATRON_PIXMAP_VIEWER_RENDER_SCALE_CHECKED:
        path = NATRON_ICONS_PATH "natron/render-scale.svg";
        break;
    case NATRON_PIXMAP_VIEWER_AUTOCONTRAST_ENABLED:
        path = NATRON_ICONS_PATH "lucide/contrast.svg";
        break;
    case NATRON_PIXMAP_VIEWER_AUTOCONTRAST_DISABLED:
        path = NATRON_ICONS_PATH "lucide/contrast.svg";
        break;
    case NATRON_PIXMAP_OPEN_FILE:
        path = NATRON_ICONS_PATH "lucide/folder-open.svg";
        break;
    case NATRON_PIXMAP_COLORWHEEL:
        path = NATRON_ICONS_PATH "lucide/swatch-book.svg";
        break;
    case NATRON_PIXMAP_COLOR_ADJUST_DROPDOWN:
        path = NATRON_ICONS_PATH "lucide/chevrons-down.svg";
        break;
    case NATRON_PIXMAP_COLOR_GLOBAL_TRACKBALL:
        path = NATRON_ICONS_PATH "lucide/expand.svg";
        break;
    case NATRON_PIXMAP_OVERLAY:
        path = NATRON_ICONS_PATH "lucide/paint-bucket.svg";
        break;
    case NATRON_PIXMAP_ROTO_MERGE:
        path = NATRON_ICONS_PATH "lucide/layers-2.svg";
        break;
    case NATRON_PIXMAP_COLOR_PICKER:
        path = NATRON_ICONS_PATH "lucide/pipette.svg";
        break;

    case NATRON_PIXMAP_IO_GROUPING:
        path = NATRON_ICONS_PATH "lucide/image.svg";
        break;
    case NATRON_PIXMAP_3D_GROUPING:
        path = NATRON_ICONS_PATH "lucide/box.svg";
        break;
    case NATRON_PIXMAP_CHANNEL_GROUPING:
        path = NATRON_ICONS_PATH "lucide/menu.svg";
        break;
    case NATRON_PIXMAP_MERGE_GROUPING:
        path = NATRON_ICONS_PATH "lucide/layers-2.svg";
        break;
    case NATRON_PIXMAP_COLOR_GROUPING:
        path = NATRON_ICONS_PATH "lucide/palette.svg";
        break;
    case NATRON_PIXMAP_TRANSFORM_GROUPING:
        path = NATRON_ICONS_PATH "lucide/move.svg";
        break;
    case NATRON_PIXMAP_DEEP_GROUPING:
        path = NATRON_ICONS_PATH "natron/deep.svg";
        break;
    case NATRON_PIXMAP_METADATA_GROUPING:
        path = NATRON_ICONS_PATH "lucide/tags.svg";
        break;
    case NATRON_PIXMAP_FILTER_GROUPING:
        path = NATRON_ICONS_PATH "natron/filter.svg";
        break;
    case NATRON_PIXMAP_MULTIVIEW_GROUPING:
        path = NATRON_ICONS_PATH "lucide/glasses.svg";
        break;
    case NATRON_PIXMAP_MISC_GROUPING:
        path = NATRON_ICONS_PATH "lucide/shapes.svg";
        break;
    case NATRON_PIXMAP_OTHER_PLUGINS:
        path = NATRON_ICONS_PATH "lucide/puzzle.svg";
        break;
    case NATRON_PIXMAP_TOOLSETS_GROUPING:
        path = NATRON_ICONS_PATH "lucide/wrench.svg";
        break;
    case NATRON_PIXMAP_KEYER_GROUPING:
        path = NATRON_ICONS_PATH "lucide/pipette.svg";
        break;
    case NATRON_PIXMAP_TIME_GROUPING:
        path = NATRON_ICONS_PATH "lucide/clock.svg";
        break;
    case NATRON_PIXMAP_PAINT_GROUPING:
        path = NATRON_ICONS_PATH "lucide/brush.svg";
        break;

    case NATRON_PIXMAP_OPEN_EFFECTS_GROUPING:
        path = NATRON_IMAGES_PATH "openeffects.png";
        break;
    case NATRON_PIXMAP_COMBOBOX:
        path = NATRON_ICONS_PATH "lucide/chevron-down.svg";
        break;
    case NATRON_PIXMAP_COMBOBOX_PRESSED:
        path = NATRON_ICONS_PATH "lucide/chevron-down.svg";
        break;
    case NATRON_PIXMAP_READ_IMAGE:
        path = NATRON_IMAGES_PATH "readImage.png";
        break;
    case NATRON_PIXMAP_WRITE_IMAGE:
        path = NATRON_IMAGES_PATH "writeImage.png";
        break;

    case NATRON_PIXMAP_APP_ICON:
        path = NATRON_APPLICATION_ICON_PATH;
        break;
    case NATRON_PIXMAP_INVERTED:
        path = NATRON_ICONS_PATH "natron/inverted.svg";
        break;
    case NATRON_PIXMAP_UNINVERTED:
        path = NATRON_ICONS_PATH "natron/inverted.svg";
        break;
    case NATRON_PIXMAP_VISIBLE:
        path = NATRON_ICONS_PATH "lucide/eye.svg";
        break;
    case NATRON_PIXMAP_UNVISIBLE:
        path = NATRON_ICONS_PATH "lucide/eye-off.svg";
        break;
    case NATRON_PIXMAP_LOCKED:
        path = NATRON_ICONS_PATH "lucide/lock.svg";
        break;
    case NATRON_PIXMAP_UNLOCKED:
        path = NATRON_ICONS_PATH "lucide/lock-open.svg";
        break;
    case NATRON_PIXMAP_LAYER:
        path = NATRON_ICONS_PATH "lucide/layers.svg";
        break;
    case NATRON_PIXMAP_BEZIER:
        path = NATRON_ICONS_PATH "lucide/spline.svg";
        break;
    case NATRON_PIXMAP_CURVE:
        path = NATRON_IMAGES_PATH "curve.png";
        break;
    case NATRON_PIXMAP_BEZIER_32:
        path = NATRON_ICONS_PATH "lucide/pen-tool.svg";
        break;
    case NATRON_PIXMAP_PENCIL:
        path = NATRON_ICONS_PATH "lucide/pencil.svg";
        break;
    case NATRON_PIXMAP_ELLIPSE:
        path = NATRON_ICONS_PATH "natron/ellipse-handles.svg";
        break;
    case NATRON_PIXMAP_RECTANGLE:
        path = NATRON_ICONS_PATH "natron/rectangle-handles.svg";
        break;
    case NATRON_PIXMAP_ADD_POINTS:
        path = NATRON_ICONS_PATH "natron/add-points.svg";
        break;
    case NATRON_PIXMAP_REMOVE_POINTS:
        path = NATRON_ICONS_PATH "natron/remove-points.svg";
        break;
    case NATRON_PIXMAP_REMOVE_FEATHER:
        path = NATRON_ICONS_PATH "natron/remove-feather.svg";
        break;
    case NATRON_PIXMAP_CUSP_POINTS:
        path = NATRON_ICONS_PATH "natron/cusp-point.svg";
        break;
    case NATRON_PIXMAP_SMOOTH_POINTS:
        path = NATRON_ICONS_PATH "lucide/tangent.svg";
        break;
    case NATRON_PIXMAP_OPEN_CLOSE_CURVE:
        path = NATRON_ICONS_PATH "natron/open-close-curve.svg";
        break;
    case NATRON_PIXMAP_SELECT_ALL:
        path = NATRON_ICONS_PATH "lucide/list-checks.svg";
        break;
    case NATRON_PIXMAP_SELECT_POINTS:
        path = NATRON_ICONS_PATH "natron/select-points.svg";
        break;
    case NATRON_PIXMAP_SELECT_FEATHER:
        path = NATRON_ICONS_PATH "natron/select-feather.svg";
        break;
    case NATRON_PIXMAP_SELECT_CURVES:
        path = NATRON_ICONS_PATH "natron/select-curves.svg";
        break;
    case NATRON_PIXMAP_AUTO_KEYING_ENABLED:
        path = NATRON_ICONS_PATH "lucide/timer.svg";
        break;
    case NATRON_PIXMAP_AUTO_KEYING_DISABLED:
        path = NATRON_ICONS_PATH "lucide/timer.svg";
        break;
    case NATRON_PIXMAP_STICKY_SELECTION_ENABLED:
        path = NATRON_ICONS_PATH "lucide/pin.svg";
        break;
    case NATRON_PIXMAP_STICKY_SELECTION_DISABLED:
        path = NATRON_ICONS_PATH "lucide/pin.svg";
        break;
    case NATRON_PIXMAP_FEATHER_LINK_ENABLED:
        path = NATRON_ICONS_PATH "lucide/link.svg";
        break;
    case NATRON_PIXMAP_FEATHER_LINK_DISABLED:
        path = NATRON_ICONS_PATH "lucide/link.svg";
        break;
    case NATRON_PIXMAP_FEATHER_VISIBLE:
        path = NATRON_ICONS_PATH "lucide/feather.svg";
        break;
    case NATRON_PIXMAP_FEATHER_UNVISIBLE:
        path = NATRON_ICONS_PATH "lucide/feather.svg";
        break;
    case NATRON_PIXMAP_RIPPLE_EDIT_ENABLED:
        path = NATRON_ICONS_PATH "lucide/waves-horizontal.svg";
        break;
    case NATRON_PIXMAP_RIPPLE_EDIT_DISABLED:
        path = NATRON_ICONS_PATH "lucide/waves-horizontal.svg";
        break;
    case NATRON_PIXMAP_BOLD_CHECKED:
        path = NATRON_ICONS_PATH "lucide/bold.svg";
        break;
    case NATRON_PIXMAP_BOLD_UNCHECKED:
        path = NATRON_ICONS_PATH "lucide/bold.svg";
        break;
    case NATRON_PIXMAP_ITALIC_UNCHECKED:
        path = NATRON_ICONS_PATH "lucide/italic.svg";
        break;
    case NATRON_PIXMAP_ITALIC_CHECKED:
        path = NATRON_ICONS_PATH "lucide/italic.svg";
        break;
    case NATRON_PIXMAP_CLEAR_ALL_ANIMATION:
        path = NATRON_ICONS_PATH "lucide/trash.svg";
        break;
    case NATRON_PIXMAP_CLEAR_BACKWARD_ANIMATION:
        path = NATRON_ICONS_PATH "natron/clear-anim-bw.svg";
        break;
    case NATRON_PIXMAP_CLEAR_FORWARD_ANIMATION:
        path = NATRON_ICONS_PATH "natron/clear-anim-fw.svg";
        break;
    case NATRON_PIXMAP_UPDATE_VIEWER_ENABLED:
        path = NATRON_IMAGES_PATH "updateViewerEnabled.png";
        break;
    case NATRON_PIXMAP_UPDATE_VIEWER_DISABLED:
        path = NATRON_IMAGES_PATH "updateViewerDisabled.png";
        break;
    case NATRON_PIXMAP_SETTINGS:
        path = NATRON_ICONS_PATH "lucide/settings.svg";
        break;
    case NATRON_PIXMAP_FREEZE_ENABLED:
        path = NATRON_ICONS_PATH "lucide/snowflake.svg";
        break;
    case NATRON_PIXMAP_FREEZE_DISABLED:
        path = NATRON_ICONS_PATH "lucide/snowflake.svg";
        break;
    case NATRON_PIXMAP_VIEWER_ICON:
        path = NATRON_IMAGES_PATH "viewer_icon.png";
        break;
    case NATRON_PIXMAP_VIEWER_CHECKERBOARD_ENABLED:
        path = NATRON_ICONS_PATH "natron/checkerboard.svg";
        break;
    case NATRON_PIXMAP_VIEWER_CHECKERBOARD_DISABLED:
        path = NATRON_ICONS_PATH "natron/checkerboard.svg";
        break;
    case NATRON_PIXMAP_VIEWER_ZEBRA_ENABLED:
        path = NATRON_IMAGES_PATH "zebra_on.png";
        break;
    case NATRON_PIXMAP_VIEWER_ZEBRA_DISABLED:
        path = NATRON_IMAGES_PATH "zebra_off.png";
        break;
    case NATRON_PIXMAP_VIEWER_GAMMA_ENABLED:
        path = NATRON_ICONS_PATH "natron/gamma.svg";
        break;
    case NATRON_PIXMAP_VIEWER_GAMMA_DISABLED:
        path = NATRON_ICONS_PATH "natron/gamma.svg";
        break;
    case NATRON_PIXMAP_VIEWER_GAIN_ENABLED:
        path = NATRON_ICONS_PATH "lucide/aperture.svg";
        break;
    case NATRON_PIXMAP_VIEWER_GAIN_DISABLED:
        path = NATRON_ICONS_PATH "lucide/aperture.svg";
        break;
    case NATRON_PIXMAP_ADD_TRACK:
        path = NATRON_ICONS_PATH "lucide/crosshair.svg";
        break;
    case NATRON_PIXMAP_ADD_USER_KEY:
        path = NATRON_ICONS_PATH "lucide/diamond-plus.svg";
        break;
    case NATRON_PIXMAP_REMOVE_USER_KEY:
        path = NATRON_ICONS_PATH "lucide/diamond-minus.svg";
        break;
    case NATRON_PIXMAP_PREV_USER_KEY:
        path = NATRON_ICONS_PATH "natron/key-prev.svg";
        break;
    case NATRON_PIXMAP_NEXT_USER_KEY:
        path = NATRON_ICONS_PATH "natron/key-next.svg";
        break;
    case NATRON_PIXMAP_TRACK_RANGE:
        path = NATRON_ICONS_PATH "natron/track-range.svg";
        break;
    case NATRON_PIXMAP_TRACK_ALL_KEYS:
        path = NATRON_ICONS_PATH "natron/track-all-keys.svg";
        break;
    case NATRON_PIXMAP_TRACK_CURRENT_KEY:
        path = NATRON_ICONS_PATH "natron/track-current-key.svg";
        break;
    case NATRON_PIXMAP_TRACK_BACKWARD_OFF:
        path = NATRON_ICONS_PATH "natron/play-reverse.svg";
        break;
    case NATRON_PIXMAP_TRACK_BACKWARD_ON:
        path = NATRON_ICONS_PATH "natron/play-reverse.svg";
        break;
    case NATRON_PIXMAP_TRACK_FORWARD_OFF:
        path = NATRON_ICONS_PATH "lucide/play.svg";
        break;
    case NATRON_PIXMAP_TRACK_FORWARD_ON:
        path = NATRON_ICONS_PATH "lucide/play.svg";
        break;
    case NATRON_PIXMAP_TRACK_PREVIOUS:
        path = NATRON_ICONS_PATH "lucide/step-back.svg";
        break;
    case NATRON_PIXMAP_TRACK_NEXT:
        path = NATRON_ICONS_PATH "lucide/step-forward.svg";
        break;
    case NATRON_PIXMAP_RESET_TRACK_OFFSET:
        path = NATRON_ICONS_PATH "lucide/undo-dot.svg";
        break;
    case NATRON_PIXMAP_HIDE_TRACK_ERROR:
        path = NATRON_ICONS_PATH "lucide/triangle-alert.svg";
        break;
    case NATRON_PIXMAP_SHOW_TRACK_ERROR:
        path = NATRON_ICONS_PATH "lucide/triangle-alert.svg";
        break;
    case NATRON_PIXMAP_CREATE_USER_KEY_ON_MOVE_ON:
        path = NATRON_ICONS_PATH "natron/key-on-move.svg";
        break;
    case NATRON_PIXMAP_CREATE_USER_KEY_ON_MOVE_OFF:
        path = NATRON_ICONS_PATH "natron/key-on-move.svg";
        break;
    case NATRON_PIXMAP_RESET_USER_KEYS:
        path = NATRON_IMAGES_PATH "removeUserKeys.png";
        break;
    case NATRON_PIXMAP_CENTER_VIEWER_ON_TRACK:
        path = NATRON_ICONS_PATH "lucide/locate-fixed.svg";
        break;
    case NATRON_PIXMAP_SCRIPT_CLEAR_OUTPUT:
        path = NATRON_ICONS_PATH "lucide/brush-cleaning.svg";
        break;
    case NATRON_PIXMAP_SCRIPT_EXEC_SCRIPT:
        path = NATRON_ICONS_PATH "lucide/circle-play.svg";
        break;
    case NATRON_PIXMAP_SCRIPT_LOAD_EXEC_SCRIPT:
        path = NATRON_ICONS_PATH "lucide/file-play.svg";
        break;
    case NATRON_PIXMAP_SCRIPT_LOAD_SCRIPT:
        path = NATRON_ICONS_PATH "lucide/file-code.svg";
        break;
    case NATRON_PIXMAP_SCRIPT_NEXT_SCRIPT:
        path = NATRON_ICONS_PATH "lucide/arrow-right.svg";
        break;
    case NATRON_PIXMAP_SCRIPT_OUTPUT_PANE_ACTIVATED:
        path = NATRON_ICONS_PATH "lucide/panel-top.svg";
        break;
    case NATRON_PIXMAP_SCRIPT_OUTPUT_PANE_DEACTIVATED:
        path = NATRON_ICONS_PATH "lucide/panel-top.svg";
        break;
    case NATRON_PIXMAP_SCRIPT_PREVIOUS_SCRIPT:
        path = NATRON_ICONS_PATH "lucide/arrow-left.svg";
        break;
    case NATRON_PIXMAP_SCRIPT_SAVE_SCRIPT:
        path = NATRON_ICONS_PATH "lucide/save.svg";
        break;

    case NATRON_PIXMAP_MERGE_ATOP:
        path = NATRON_IMAGES_PATH "merge_atop.png";
        break;
    case NATRON_PIXMAP_MERGE_AVERAGE:
        path = NATRON_IMAGES_PATH "merge_average.png";
        break;
    case NATRON_PIXMAP_MERGE_COLOR:
        path = NATRON_IMAGES_PATH "merge_color.png";
        break;
    case NATRON_PIXMAP_MERGE_COLOR_BURN:
        path = NATRON_IMAGES_PATH "merge_color_burn.png";
        break;
    case NATRON_PIXMAP_MERGE_COLOR_DODGE:
        path = NATRON_IMAGES_PATH "merge_color_dodge.png";
        break;
    case NATRON_PIXMAP_MERGE_CONJOINT_OVER:
        path = NATRON_IMAGES_PATH "merge_conjoint_over.png";
        break;
    case NATRON_PIXMAP_MERGE_COPY:
        path = NATRON_IMAGES_PATH "merge_copy.png";
        break;
    case NATRON_PIXMAP_MERGE_DIFFERENCE:
        path = NATRON_IMAGES_PATH "merge_difference.png";
        break;
    case NATRON_PIXMAP_MERGE_DISJOINT_OVER:
        path = NATRON_IMAGES_PATH "merge_disjoint_over.png";
        break;
    case NATRON_PIXMAP_MERGE_DIVIDE:
        path = NATRON_IMAGES_PATH "merge_divide.png";
        break;
    case NATRON_PIXMAP_MERGE_EXCLUSION:
        path = NATRON_IMAGES_PATH "merge_exclusion.png";
        break;
    case NATRON_PIXMAP_MERGE_FREEZE:
        path = NATRON_IMAGES_PATH "merge_freeze.png";
        break;
    case NATRON_PIXMAP_MERGE_FROM:
        path = NATRON_IMAGES_PATH "merge_from.png";
        break;
    case NATRON_PIXMAP_MERGE_GEOMETRIC:
        path = NATRON_IMAGES_PATH "merge_geometric.png";
        break;
    case NATRON_PIXMAP_MERGE_GRAIN_EXTRACT:
        path = NATRON_IMAGES_PATH "merge_grain_extract.png";
        break;
    case NATRON_PIXMAP_MERGE_GRAIN_MERGE:
        path = NATRON_IMAGES_PATH "merge_grain_merge.png";
        break;
    case NATRON_PIXMAP_MERGE_HARD_LIGHT:
        path = NATRON_IMAGES_PATH "merge_hardlight.png";
        break;
    case NATRON_PIXMAP_MERGE_HUE:
        path = NATRON_IMAGES_PATH "merge_hue.png";
        break;
    case NATRON_PIXMAP_MERGE_HYPOT:
        path = NATRON_IMAGES_PATH "merge_hypot.png";
        break;
    case NATRON_PIXMAP_MERGE_IN:
        path = NATRON_IMAGES_PATH "merge_in.png";
        break;
    case NATRON_PIXMAP_MERGE_LUMINOSITY:
        path = NATRON_IMAGES_PATH "merge_luminosity.png";
        break;
    case NATRON_PIXMAP_MERGE_MASK:
        path = NATRON_IMAGES_PATH "merge_mask.png";
        break;
    case NATRON_PIXMAP_MERGE_MATTE:
        path = NATRON_IMAGES_PATH "merge_matte.png";
        break;
    case NATRON_PIXMAP_MERGE_MAX:
        path = NATRON_IMAGES_PATH "merge_max.png";
        break;
    case NATRON_PIXMAP_MERGE_MIN:
        path = NATRON_IMAGES_PATH "merge_min.png";
        break;
    case NATRON_PIXMAP_MERGE_MINUS:
        path = NATRON_IMAGES_PATH "merge_minus.png";
        break;
    case NATRON_PIXMAP_MERGE_MULTIPLY:
        path = NATRON_IMAGES_PATH "merge_multiply.png";
        break;
    case NATRON_PIXMAP_MERGE_OUT:
        path = NATRON_IMAGES_PATH "merge_out.png";
        break;
    case NATRON_PIXMAP_MERGE_OVER:
        path = NATRON_IMAGES_PATH "merge_over.png";
        break;
    case NATRON_PIXMAP_MERGE_OVERLAY:
        path = NATRON_IMAGES_PATH "merge_overlay.png";
        break;
    case NATRON_PIXMAP_MERGE_PINLIGHT:
        path = NATRON_IMAGES_PATH "merge_pinlight.png";
        break;
    case NATRON_PIXMAP_MERGE_PLUS:
        path = NATRON_IMAGES_PATH "merge_plus.png";
        break;
    case NATRON_PIXMAP_MERGE_REFLECT:
        path = NATRON_IMAGES_PATH "merge_reflect.png";
        break;
    case NATRON_PIXMAP_MERGE_SATURATION:
        path = NATRON_IMAGES_PATH "merge_saturation.png";
        break;
    case NATRON_PIXMAP_MERGE_SCREEN:
        path = NATRON_IMAGES_PATH "merge_screen.png";
        break;
    case NATRON_PIXMAP_MERGE_SOFT_LIGHT:
        path = NATRON_IMAGES_PATH "merge_softlight.png";
        break;
    case NATRON_PIXMAP_MERGE_STENCIL:
        path = NATRON_IMAGES_PATH "merge_stencil.png";
        break;
    case NATRON_PIXMAP_MERGE_UNDER:
        path = NATRON_IMAGES_PATH "merge_under.png";
        break;
    case NATRON_PIXMAP_MERGE_XOR:
        path = NATRON_IMAGES_PATH "merge_xor.png";
        break;

    case NATRON_PIXMAP_LINK_CURSOR:
        path = NATRON_ICONS_PATH "natron/link-cursor.svg";
        break;
    case NATRON_PIXMAP_LINK_MULT_CURSOR:
        path = NATRON_ICONS_PATH "natron/link-mult-cursor.svg";
        break;
    case NATRON_PIXMAP_ENTER_GROUP:
        path = NATRON_ICONS_PATH "lucide/square-arrow-right-enter.svg";
        break;
    case NATRON_PIXMAP_ROTOPAINT_BUILDUP_ENABLED:
        path = NATRON_ICONS_PATH "natron/buildup.svg";
        break;
    case NATRON_PIXMAP_ROTOPAINT_BUILDUP_DISABLED:
        path = NATRON_ICONS_PATH "natron/buildup.svg";
        break;
    case NATRON_PIXMAP_ROTOPAINT_BLUR:
        path = NATRON_ICONS_PATH "lucide/droplet.svg";
        break;
    case NATRON_PIXMAP_ROTOPAINT_BURN:
        path = NATRON_ICONS_PATH "lucide/flame.svg";
        break;
    case NATRON_PIXMAP_ROTOPAINT_CLONE:
        path = NATRON_ICONS_PATH "natron/clone-stamp.svg";
        break;
    case NATRON_PIXMAP_ROTOPAINT_DODGE:
        path = NATRON_ICONS_PATH "lucide/sun.svg";
        break;
    case NATRON_PIXMAP_ROTOPAINT_ERASER:
        path = NATRON_ICONS_PATH "lucide/eraser.svg";
        break;
    case NATRON_PIXMAP_ROTOPAINT_PRESSURE_ENABLED:
        path = NATRON_ICONS_PATH "natron/pressure.svg";
        break;
    case NATRON_PIXMAP_ROTOPAINT_PRESSURE_DISABLED:
        path = NATRON_ICONS_PATH "natron/pressure.svg";
        break;
    case NATRON_PIXMAP_ROTOPAINT_REVEAL:
        path = NATRON_ICONS_PATH "lucide/stamp.svg";
        break;
    case NATRON_PIXMAP_ROTOPAINT_SHARPEN:
        path = NATRON_ICONS_PATH "lucide/triangle.svg";
        break;
    case NATRON_PIXMAP_ROTOPAINT_SMEAR:
        path = NATRON_ICONS_PATH "lucide/pointer.svg";
        break;
    case NATRON_PIXMAP_ROTOPAINT_SOLID:
        path = NATRON_ICONS_PATH "lucide/brush.svg";
        break;
    case NATRON_PIXMAP_ROTO_NODE_ICON:
        path = NATRON_IMAGES_PATH "rotoNodeIcon.png";
        break;
    case NATRON_PIXMAP_INTERP_LINEAR:
        path = NATRON_ICONS_PATH "natron/interp-linear.svg";
        break;
    case NATRON_PIXMAP_INTERP_CURVE:
        path = NATRON_ICONS_PATH "natron/interp-free.svg";
        break;
    case NATRON_PIXMAP_INTERP_CONSTANT:
        path = NATRON_ICONS_PATH "natron/interp-constant.svg";
        break;
    case NATRON_PIXMAP_INTERP_BREAK:
        path = NATRON_ICONS_PATH "natron/interp-break.svg";
        break;
    case NATRON_PIXMAP_INTERP_CURVE_C:
        path = NATRON_ICONS_PATH "natron/interp-cubic.svg";
        break;
    case NATRON_PIXMAP_INTERP_CURVE_H:
        path = NATRON_ICONS_PATH "natron/interp-horizontal.svg";
        break;
    case NATRON_PIXMAP_INTERP_CURVE_R:
        path = NATRON_ICONS_PATH "natron/interp-catmull.svg";
        break;
    case NATRON_PIXMAP_INTERP_CURVE_Z:
        path = NATRON_ICONS_PATH "natron/interp-smooth.svg";
        break;
        // DON'T add a default: case here
    } // switch

    return path;
} // pixmapEnumToPath

} // anon namespace

void
GuiApplicationManager::getIcon(PixmapEnum e,
                               QPixmap* pix) const
{
    std::string path = pixmapEnumToPath(e);

    if (path.empty()) {
        assert(!"Missing image.");
        return;
    }

    const QString qpath = QString::fromUtf8(path.c_str());

    if (ThemedIcon::isSvgPath(qpath)) {
        // The vendored glyphs are drawn on Lucide's 24-unit grid; this is their own
        // natural size, not a UI constant, so a direct request scales from here.
        const int naturalSize = 24;

        *pix = ThemedIcon::cachedPixmap(qpath, naturalSize, ThemedIcon::themeTextColor(), qApp->devicePixelRatio());

        return;
    }

    // put a breakpoint in png_chunk_report to catch the error "libpng warning: iCCP: known incorrect sRGB profile"
    if (!QPixmapCache::find(QString::number(e), pix)) {
        pix->load(qpath);
        QPixmapCache::insert(QString::number(e), *pix);
    }
} // getIcon

void
GuiApplicationManager::getIcon(PixmapEnum e,
                               int size,
                               QPixmap* pix) const
{
    std::string path = pixmapEnumToPath(e);
    const QString qpath = QString::fromUtf8(path.c_str());

    if (!path.empty() && ThemedIcon::isSvgPath(qpath)) {
        *pix = ThemedIcon::cachedPixmap(qpath, size, ThemedIcon::themeTextColor(), qApp->devicePixelRatio());

        return;
    }

    const QString key = QString::number(e) + QLatin1Char('@') + QString::number(size);

    if (!QPixmapCache::find(key, pix)) {
        getIcon(e, pix);
        if (std::max( pix->width(), pix->height() ) != size) {
            *pix = pix->scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        QPixmapCache::insert(key, *pix);
    }
}

void
GuiApplicationManager::getIcon(PixmapEnum e,
                               int size,
                               QIcon* icon) const
{
    std::string path = pixmapEnumToPath(e);
    const QString qpath = QString::fromUtf8(path.c_str());

    if (!path.empty() && ThemedIcon::isSvgPath(qpath)) {
        // Gives toolbox and menu grouping icons the same themed Normal/On/Disabled
        // states as any other themed glyph, instead of Qt's generic auto-disabled look.
        *icon = ThemedIcon::icon(qpath, size);
        return;
    }

    QPixmap pix;
    getIcon(e, size, &pix);
    icon->addPixmap(pix);
}

void
GuiApplicationManager::getIcon(PixmapEnum off,
                               PixmapEnum on,
                               int size,
                               QIcon* icon) const
{
    const QString offPath = QString::fromUtf8(pixmapEnumToPath(off).c_str());
    const QString onPath = QString::fromUtf8(pixmapEnumToPath(on).c_str());

    if (ThemedIcon::isSvgPath(offPath) && ThemedIcon::isSvgPath(onPath)) {
        *icon = ThemedIcon::icon(offPath, onPath, size);

        return;
    }

    QPixmap offPix, onPix;
    getIcon(off, size, &offPix);
    getIcon(on, size, &onPix);
    *icon = QIcon();
    icon->addPixmap(offPix, QIcon::Normal, QIcon::Off);
    icon->addPixmap(onPix, QIcon::Normal, QIcon::On);
}

const std::list<PluginGroupNodePtr>&
GuiApplicationManager::getTopLevelPluginsToolButtons() const
{
    return _imp->_topLevelToolButtons;
}

const QCursor &
GuiApplicationManager::getColorPickerCursor() const
{
    return _imp->_colorPickerCursor;
}

const QCursor &
GuiApplicationManager::getLinkToCursor() const
{
    return _imp->_linkToCursor;
}

const QCursor &
GuiApplicationManager::getLinkToMultCursor() const
{
    return _imp->_linkMultCursor;
}

bool
GuiApplicationManager::initGui(const CLArgs& args)
{
    QSettings settings( QString::fromUtf8(NATRON_ORGANIZATION_NAME), QString::fromUtf8(NATRON_APPLICATION_NAME) );

    //load custom fonts
    QString fontResource = QString::fromUtf8(":/Resources/Fonts/%1.ttf");
    QStringList fontFilenames;

    fontFilenames << fontResource.arg( QString::fromUtf8("DroidSans") );
    fontFilenames << fontResource.arg( QString::fromUtf8("DroidSans-Bold") );
    Q_FOREACH (const QString &fontFilename, fontFilenames) {
        int fontID = QFontDatabase::addApplicationFont(fontFilename);

        qDebug() << "fontID=" << fontID << "families=" << QFontDatabase::applicationFontFamilies(fontID);
    }
    QString fontFamily = QString::fromUtf8(NATRON_FONT);
    int fontSize = NATRON_FONT_SIZE_11;


    ///Do not load old font stored in the setting "systemFont" on Natron < 2 because it might contain a crappy font.
    if ( settings.contains( QString::fromUtf8(kQSettingsSoftwareMajorVersionSettingName) ) && (settings.value( QString::fromUtf8(kQSettingsSoftwareMajorVersionSettingName) ).toInt() >= 2) ) {
        if ( settings.contains( QString::fromUtf8("systemFont") ) ) {
            fontFamily = settings.value( QString::fromUtf8("systemFont") ).toString();
        }
    }


    if ( settings.contains( QString::fromUtf8("fontSize") ) ) {
        fontSize = settings.value( QString::fromUtf8("fontSize") ).toInt();
    }
    //fontFamily = "Courier"; fontSize = 24; // for debugging purposes
    qDebug() << "Setting application font to " << fontFamily << fontSize;
    {
        QFontDatabase database;
        Q_FOREACH ( const QString &family, database.families() ) {
            if (family == fontFamily) {
                qDebug() << "... found" << fontFamily << "available styles:";
                Q_FOREACH ( const QString &style, database.styles(family) ) {
                    qDebug() << family << style;
                }
            }
        }
        QFont font(fontFamily, fontSize);
        if ( !font.exactMatch() ) {
            QFontInfo fi(font);
            qDebug() << "Not an exact match, got: " << fi.family() << fi.pointSize();
        }
        QApplication::setFont(font);
#ifdef Q_OS_DARWIN
        // https://bugreports.qt.io/browse/QTBUG-32789
        QFont::insertSubstitution(QString::fromUtf8(".Lucida Grande UI"), fontFamily /*"Lucida Grande"*/);
        // https://bugreports.qt.io/browse/QTBUG-40833
        QFont::insertSubstitution(QString::fromUtf8(".Helvetica Neue DeskInterface"), fontFamily /*"Helvetica Neue"*/);
        // there are lots of remaining bugs on Yosemite in 4.8.6, to be fixed in 4.8.7&
#endif
    }
    _imp->_fontFamily = fontFamily;
    _imp->_fontSize = fontSize;

    /*Display a splashscreen while we wait for the engine to load*/
    QString filename = QString::fromUtf8(NATRON_IMAGES_PATH "splashscreen.png");

    _imp->_splashScreen = new SplashScreen(filename);
    _imp->_splashScreen->setAttribute(Qt::WA_DeleteOnClose, 0);

    {
#ifdef DEBUG
        boost_adaptbx::floating_point::exception_trapping trap(0);
#endif
        QCoreApplication::processEvents();
    }
    QPixmap appIcPixmap;
    appPTR->getIcon(NATRON_PIXMAP_APP_ICON, &appIcPixmap);
    QIcon appIc(appIcPixmap);
    qApp->setWindowIcon(appIc);

    QFontDatabase db;
    QStringList families = db.families(QFontDatabase::Latin); // We need a Latin font for the UI
    std::vector<std::string> systemFonts( families.size() );
    for (int i = 0; i < families.size(); ++i) {
        systemFonts[i] = families[i].toStdString();
    }
    getCurrentSettings()->populateSystemFonts(settings, systemFonts);

    _imp->createColorPickerCursor();
    _imp->createLinkToCursor();
    _imp->createLinkMultCursor();

    ///Copy the args that will be used when fontconfig cache is updated
    _imp->startupArgs = args;

    _imp->fontconfigUpdateWatcher.reset(new QFutureWatcher<void>);
    QObject::connect( _imp->fontconfigUpdateWatcher.get(), SIGNAL(finished()), this, SLOT(onFontconfigCacheUpdateFinished()) );
    setLoadingStatus( tr("Updating fontconfig cache...") );

    QObject::connect( &_imp->updateSplashscreenTimer, SIGNAL(timeout()), this, SLOT(onFontconfigTimerTriggered()) );
    _imp->updateSplashscreenTimer.start(1000);

    _imp->fontconfigUpdateWatcher->setFuture( QtConcurrent::run(&GuiApplicationManagerPrivate::updateFontConfigCache, _imp.get()) );

    Gui::loadStyleSheet();

    // Init documentation manager
    _imp->documentation.reset(new DocumentationManager);
    _imp->documentation->startServer();

    return exec();
} // GuiApplicationManager::initGui

void
GuiApplicationManager::onFontconfigCacheUpdateFinished()
{
    _imp->updateSplashscreenTimer.stop();

    ///Ignore return value here, the user will have an error dialog if loading failed anyway.
    bool ret = loadInternalAfterInitGui(_imp->startupArgs);
    Q_UNUSED(ret);
}

void
GuiApplicationManager::onFontconfigTimerTriggered()
{
    _imp->fontconfigMessageDots = (_imp->fontconfigMessageDots + 1) % 4;

    QString message = tr("Updating fontconfig cache");
    for (int i = 0; i < _imp->fontconfigMessageDots; ++i) {
        message.append( QLatin1Char('.') );
    }
    setLoadingStatus(message);
}

void
GuiApplicationManager::onPluginLoaded(Plugin* plugin)
{
    QString shortcutGrouping = QString::fromUtf8(kShortcutGroupNodes);
    const QStringList & groups = plugin->getGrouping();
    const QString & pluginID = plugin->getPluginID();
    const QString pluginLabel = plugin->getLabelWithoutSuffix();
    const QString & pluginIconPath = plugin->getIconFilePath();
    const QStringList & groupIconPath = plugin->getGroupIconFilePath();
    QStringList groupingWithID = groups;

    groupingWithID.push_back(pluginID);
    PluginGroupNodePtr child = findPluginToolButtonOrCreate( groupingWithID,
                                                                             pluginLabel,
                                                                             groupIconPath,
                                                                             pluginIconPath,
                                                                             plugin->getMajorVersion(),
                                                                             plugin->getMinorVersion(),
                                                                             plugin->getIsUserCreatable() );
    for (int i = 0; i < groups.size(); ++i) {
        shortcutGrouping.push_back( QLatin1Char('/') );
        shortcutGrouping.push_back(groups[i]);
    }

    Qt::Key symbol = (Qt::Key)0;
    bool hasShortcut = true;

    /*These are the plug-ins which have a default shortcut. Other plug-ins can have a user-assigned shortcut.*/
    if ( pluginID == QString::fromUtf8(PLUGINID_OFX_TRANSFORM) ) {
        symbol = Qt::Key_T;
    } else if ( pluginID == QString::fromUtf8(PLUGINID_NATRON_ROTO) ) {
        symbol = Qt::Key_O;
    } else if ( pluginID == QString::fromUtf8(PLUGINID_NATRON_ROTOPAINT) ) {
        symbol = Qt::Key_P;
    } else if ( pluginID == QString::fromUtf8(PLUGINID_OFX_MERGE) ) {
        symbol = Qt::Key_M;
    } else if ( pluginID == QString::fromUtf8(PLUGINID_OFX_GRADE) ) {
        symbol = Qt::Key_G;
    } else if ( pluginID == QString::fromUtf8(PLUGINID_OFX_COLORCORRECT) ) {
        symbol = Qt::Key_C;
    } else if ( pluginID == QString::fromUtf8(PLUGINID_OFX_BLURCIMG) ) {
        symbol = Qt::Key_B;
    } else if ( pluginID == QString::fromUtf8(PLUGINID_NATRON_DOT) ) {
        symbol = Qt::Key_Period;
    } else if (pluginID == QString::fromUtf8(PLUGINID_NATRON_READ)) {
        symbol = Qt::Key_R;
    } else if (pluginID == QString::fromUtf8(PLUGINID_NATRON_WRITE)) {
        symbol = Qt::Key_W;
    } else {
        hasShortcut = false;
    }
    plugin->setHasShortcut(hasShortcut);

    if ( hasShortcut && plugin->getIsUserCreatable() ) {
        _imp->addKeybind(shortcutGrouping.toStdString(), pluginID.toStdString(), pluginLabel.toStdString(), Qt::NoModifier, symbol, /*modifiersMask=*/Qt::ShiftModifier);
    }

    const std::list<PluginActionShortcut>& shortcuts =  plugin->getShortcuts();
    std::string pluginShortcutGroup =  plugin->getPluginShortcutGroup().toStdString();
    for (std::list<PluginActionShortcut>::const_iterator it = shortcuts.begin(); it != shortcuts.end(); ++it) {
        _imp->addKeybind( pluginShortcutGroup, it->actionID, it->actionLabel, QtEnumConvert::toQtModifiers(it->modifiers), QtEnumConvert::toQtKey(it->key) );
    }
} // GuiApplicationManager::onPluginLoaded

void
GuiApplicationManager::ignorePlugin(Plugin* plugin)
{
    const QStringList& groups = plugin->getGrouping();
    QStringList groupingWithID = groups;
    groupingWithID.push_back(plugin->getPluginID());
    _imp->removePluginToolButton(groupingWithID);

    QString shortcutGrouping = QString::fromUtf8(kShortcutGroupNodes);
    for (int i = 0; i < groups.size(); ++i) {
        shortcutGrouping.push_back(QLatin1Char('/'));
        shortcutGrouping.push_back(groups[i]);
    }
    _imp->removeKeybind(shortcutGrouping, plugin->getPluginID());

    if (_imp->nodesMenu) {
        rebuildNodesMenu();
    }
}

NATRON_NAMESPACE_ANONYMOUS_ENTER

#define PLUGIN_GROUP_DEFAULT_ICON_PATH NATRON_ICONS_PATH "lucide/puzzle.svg"

// Group ordering is set at every place in the code where GROUP_ORDER appears in the comments
const char* const namedGroupsOrdered[] = {
    PLUGIN_GROUP_IMAGE,
    PLUGIN_GROUP_PAINT,
    PLUGIN_GROUP_TIME,
    PLUGIN_GROUP_CHANNEL,
    PLUGIN_GROUP_COLOR,
    PLUGIN_GROUP_FILTER,
    PLUGIN_GROUP_KEYER,
    PLUGIN_GROUP_MERGE,
    PLUGIN_GROUP_TRANSFORM,
    PLUGIN_GROUP_3D,
    PLUGIN_GROUP_DEEP,
    PLUGIN_GROUP_METADATA,
    PLUGIN_GROUP_MULTIVIEW,
    PLUGIN_GROUP_TOOLSETS,
    PLUGIN_GROUP_OTHER,
    PLUGIN_GROUP_DEFAULT
};
const int namedGroupsCount = sizeof(namedGroupsOrdered) / sizeof(namedGroupsOrdered[0]);

void
getIconForGrouping(QIcon* icon,
                   int size,
                   const QString& grouping)
{
    PixmapEnum e = NATRON_PIXMAP_OTHER_PLUGINS;

    // Group ordering is set at every place in the code where GROUP_ORDER appears in the comments
    if (grouping == QString::fromUtf8(PLUGIN_GROUP_IMAGE)) {
        e = NATRON_PIXMAP_IO_GROUPING;
    } else if (grouping == QString::fromUtf8(PLUGIN_GROUP_PAINT)) {
        e = NATRON_PIXMAP_PAINT_GROUPING;
    } else if (grouping == QString::fromUtf8(PLUGIN_GROUP_TIME)) {
        e = NATRON_PIXMAP_TIME_GROUPING;
    } else if (grouping == QString::fromUtf8(PLUGIN_GROUP_CHANNEL)) {
        e = NATRON_PIXMAP_CHANNEL_GROUPING;
    } else if (grouping == QString::fromUtf8(PLUGIN_GROUP_COLOR)) {
        e = NATRON_PIXMAP_COLOR_GROUPING;
    } else if (grouping == QString::fromUtf8(PLUGIN_GROUP_FILTER)) {
        e = NATRON_PIXMAP_FILTER_GROUPING;
    } else if (grouping == QString::fromUtf8(PLUGIN_GROUP_KEYER)) {
        e = NATRON_PIXMAP_KEYER_GROUPING;
    } else if (grouping == QString::fromUtf8(PLUGIN_GROUP_MERGE)) {
        e = NATRON_PIXMAP_MERGE_GROUPING;
    } else if (grouping == QString::fromUtf8(PLUGIN_GROUP_TRANSFORM)) {
        e = NATRON_PIXMAP_TRANSFORM_GROUPING;
    } else if (grouping == QString::fromUtf8(PLUGIN_GROUP_3D)) {
        e = NATRON_PIXMAP_3D_GROUPING;
    } else if (grouping == QString::fromUtf8(PLUGIN_GROUP_DEEP)) {
        e = NATRON_PIXMAP_DEEP_GROUPING;
    } else if (grouping == QString::fromUtf8(PLUGIN_GROUP_METADATA)) {
        e = NATRON_PIXMAP_METADATA_GROUPING;
    } else if (grouping == QString::fromUtf8(PLUGIN_GROUP_MULTIVIEW)) {
        e = NATRON_PIXMAP_MULTIVIEW_GROUPING;
    } else if (grouping == QString::fromUtf8(PLUGIN_GROUP_TOOLSETS)) {
        e = NATRON_PIXMAP_TOOLSETS_GROUPING;
    } else if (grouping == QString::fromUtf8(PLUGIN_GROUP_OTHER)) {
        e = NATRON_PIXMAP_MISC_GROUPING;
    }
    appPTR->getIcon(e, size, icon);
}

void
makePluginGroupIcons(const PluginGroupNode& node,
                     QIcon* menuIcon,
                     QIcon* toolButtonIcon)
{
    const QString& iconPath = node.getIconPath();

    if (!iconPath.isEmpty() && QFile::exists(iconPath)) {
        int menuSize = TO_DPIX(NATRON_MEDIUM_BUTTON_ICON_SIZE);
        int toolButtonSize = !node.hasParent() ? TO_DPIX(NATRON_TOOL_BUTTON_ICON_SIZE) : TO_DPIX(NATRON_MEDIUM_BUTTON_ICON_SIZE);
        if (ThemedIcon::isSvgPath(iconPath)) {
            // A built-in plugin (e.g. RotoPaint) pointed at a vendored glyph gets the
            // same themed Normal/On/Disabled states as the toolbox grouping icons.
            // An OFX plugin's own icon is always a PNG, so it never reaches this branch.
            *menuIcon = ThemedIcon::icon(iconPath, menuSize);
            *toolButtonIcon = ThemedIcon::icon(iconPath, toolButtonSize);
        } else if (!node.hasParent() && !node.getChildren().empty()) {
            // A plugin bundle's own top-level grouping (e.g. openfx-arena's "Extra") brings a
            // colour PNG into the otherwise monochrome toolbox. Its silhouette is kept and drawn
            // in the theme colour; plugins' own icons, which may be colour-coded, keep theirs.
            *menuIcon = ThemedIcon::tintedIcon(iconPath, menuSize);
            *toolButtonIcon = ThemedIcon::tintedIcon(iconPath, toolButtonSize);
        } else {
            QPixmap pix(iconPath);
            QPixmap menuPix = pix, toolbuttonPix = pix;
            if ((std::max(menuPix.width(), menuPix.height()) != menuSize) && !menuPix.isNull()) {
                menuPix = menuPix.scaled(menuSize, menuSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            }
            if ((std::max(toolbuttonPix.width(), toolbuttonPix.height()) != toolButtonSize) && !toolbuttonPix.isNull()) {
                toolbuttonPix = toolbuttonPix.scaled(toolButtonSize, toolButtonSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            }
            menuIcon->addPixmap(menuPix);
            toolButtonIcon->addPixmap(toolbuttonPix);
        }
    } else if (!node.hasParent()) {
        getIconForGrouping(toolButtonIcon, TO_DPIX(NATRON_TOOL_BUTTON_ICON_SIZE), node.getLabel());
        getIconForGrouping(menuIcon, TO_DPIX(NATRON_TOOL_BUTTON_ICON_SIZE), node.getLabel());
    }
}

// QMenu treats a single '&' as a mnemonic marker.
QString
escapedMenuText(const QString& text)
{
    QString ret = text;

    ret.replace(QString::fromUtf8("&"), QString::fromUtf8("&&"));

    return ret;
}

bool
isPluginOwned(const QAction* action)
{
    return action->property(kNodesMenuPropPluginOwned).toBool();
}

const Plugin*
findPlugin(const QString& pluginID,
           int major)
{
    const PluginsMap& plugins = appPTR->getPluginsList();
    PluginsMap::const_iterator found = plugins.find(pluginID.toStdString());

    if (found == plugins.end()) {
        return 0;
    }
    for (PluginVersionsOrdered::const_reverse_iterator it = found->second.rbegin(); it != found->second.rend(); ++it) {
        if ((*it)->getMajorVersion() == major) {
            return *it;
        }
    }

    return 0;
}

int
highestUserCreatableMajor(const QString& pluginID)
{
    const PluginsMap& plugins = appPTR->getPluginsList();
    PluginsMap::const_iterator found = plugins.find(pluginID.toStdString());
    int highest = std::numeric_limits<int>::min();

    if (found == plugins.end()) {
        return highest;
    }
    for (PluginVersionsOrdered::const_iterator it = found->second.begin(); it != found->second.end(); ++it) {
        if ((*it)->getIsUserCreatable()) {
            highest = std::max(highest, (*it)->getMajorVersion());
        }
    }

    return highest;
}

QString
categoryColorName(const Plugin& plugin)
{
    float r, g, b;

    appPTR->getCurrentSettings()->getNodeCategoryColor(Node::categoryForPlugin(plugin), &r, &g, &b);

    return QColor::fromRgbF(std::clamp(r, 0.f, 1.f),
                            std::clamp(g, 0.f, 1.f),
                            std::clamp(b, 0.f, 1.f))
        .name();
}

QString
shortcutText(const Plugin& plugin)
{
    QString shortcutGroup = QString::fromUtf8(kShortcutGroupNodes);
    const QStringList& grouping = plugin.getGrouping();

    for (int i = 0; i < grouping.size(); ++i) {
        shortcutGroup += QLatin1Char('/') + grouping[i];
    }
    std::list<QKeySequence> sequences = getKeybind(shortcutGroup, plugin.getPluginID());

    return sequences.empty() ? QString() : sequences.front().toString(QKeySequence::NativeText);
}

void
refreshPluginActionProperties(QAction* action,
                              const Plugin* plugin)
{
    action->setProperty(kNodesMenuPropCategoryColor, plugin ? categoryColorName(*plugin) : QString());
    action->setProperty(kNodesMenuPropShortcut, plugin ? shortcutText(*plugin) : QString());
}

void
refreshPluginActionsProperties(QMenu* menu,
                               const QString& pluginID)
{
    QList<QAction*> actions = menu->actions();

    for (int i = 0; i < actions.size(); ++i) {
        if (!isPluginOwned(actions[i])) {
            continue;
        }
        if (actions[i]->menu()) {
            refreshPluginActionsProperties(actions[i]->menu(), pluginID);
            continue;
        }
        const QString id = actions[i]->property(kNodesMenuPropPluginID).toString();
        if (pluginID.isEmpty() || (id == pluginID)) {
            refreshPluginActionProperties(actions[i], findPlugin(id, actions[i]->property(kNodesMenuPropPluginMajor).toInt()));
        }
    }
}

GuiAppInstancePtr
activeWindowApp()
{
    QWidget* active = QApplication::activeWindow();

    if (!active) {
        return GuiAppInstancePtr();
    }
    const AppInstanceVec& instances = appPTR->getAppInstances();
    for (AppInstanceVec::const_iterator it = instances.begin(); it != instances.end(); ++it) {
        GuiAppInstancePtr app = std::dynamic_pointer_cast<GuiAppInstance>(*it);
        Gui* gui = app ? app->getGui() : 0;
        if (!gui) {
            continue;
        }
        // A floating pane is a window of its own, parented to its Gui.
        for (QWidget* w = active; w; w = w->parentWidget()) {
            if (w == gui) {
                return app;
            }
        }
    }

    return GuiAppInstancePtr();
}

bool
hasPluginGroupNode(const std::list<PluginGroupNodePtr>& level,
                   const QStringList& path,
                   int depth,
                   int major)
{
    for (std::list<PluginGroupNodePtr>::const_iterator it = level.begin(); it != level.end(); ++it) {
        if ((*it)->getID() != path[depth]) {
            continue;
        }
        if (depth + 1 < path.size()) {
            if (hasPluginGroupNode((*it)->getChildren(), path, depth + 1, major)) {
                return true;
            }
        } else if ((*it)->getMajorVersion() == major) {
            return true;
        }
    }

    return false;
}

QMenu*
findPluginOwnedSubmenu(QMenu* parent,
                       const QString& grouping)
{
    QList<QAction*> actions = parent->actions();

    for (int i = 0; i < actions.size(); ++i) {
        if (actions[i]->menu() && isPluginOwned(actions[i]) && (actions[i]->property(kNodesMenuPropGrouping).toString() == grouping)) {
            return actions[i]->menu();
        }
    }

    return 0;
}

// Deletion is deferred because a rebuild can be requested from a slot of an action that is
// still being triggered.
void
removePluginOwnedLeaves(QMenu* menu)
{
    QList<QAction*> actions = menu->actions();

    for (int i = 0; i < actions.size(); ++i) {
        if (!isPluginOwned(actions[i])) {
            continue;
        }
        if (actions[i]->menu()) {
            removePluginOwnedLeaves(actions[i]->menu());
        } else {
            menu->removeAction(actions[i]);
            actions[i]->deleteLater();
        }
    }
}

void
removeEmptyPluginOwnedSubmenus(QMenu* menu)
{
    QList<QAction*> actions = menu->actions();

    for (int i = 0; i < actions.size(); ++i) {
        QMenu* submenu = actions[i]->menu();
        if (!submenu || !isPluginOwned(actions[i])) {
            continue;
        }
        removeEmptyPluginOwnedSubmenus(submenu);
        if (submenu->actions().isEmpty()) {
            menu->removeAction(actions[i]);
            submenu->deleteLater();
        }
    }
}

int
topLevelRank(const QAction* action,
             const QStringList& treeOrder)
{
    const QString grouping = action->property(kNodesMenuPropGrouping).toString();

    for (int i = 0; i < namedGroupsCount; ++i) {
        if (grouping == QString::fromUtf8(namedGroupsOrdered[i])) {
            return i;
        }
    }
    int index = treeOrder.indexOf(grouping);

    return namedGroupsCount + (index < 0 ? treeOrder.size() : index);
}

// Plug-in-owned actions come first, in the given order, and every other action keeps its
// relative order after them.
template <typename LessThan>
void
orderPluginOwnedActions(QMenu* menu,
                        LessThan lessThan)
{
    QList<QAction*> actions = menu->actions();
    QList<QAction*> owned, others;

    for (int i = 0; i < actions.size(); ++i) {
        (isPluginOwned(actions[i]) ? owned : others).push_back(actions[i]);
    }
    std::stable_sort(owned.begin(), owned.end(), lessThan);
    QList<QAction*> ordered = owned + others;
    if (ordered == actions) {
        return;
    }
    for (int i = 0; i < actions.size(); ++i) {
        menu->removeAction(actions[i]);
    }
    menu->addActions(ordered);
}

bool
actionTextLessThan(const QAction* lhs,
                   const QAction* rhs)
{
    return lhs->text() < rhs->text();
}

void
sortPluginOwnedSubmenus(QMenu* menu)
{
    QList<QAction*> actions = menu->actions();

    for (int i = 0; i < actions.size(); ++i) {
        if (actions[i]->menu() && isPluginOwned(actions[i])) {
            orderPluginOwnedActions(actions[i]->menu(), actionTextLessThan);
            sortPluginOwnedSubmenus(actions[i]->menu());
        }
    }
}

NATRON_NAMESPACE_ANONYMOUS_EXIT

QMenu*
GuiApplicationManager::getNodesMenu() const
{
    return _imp->nodesMenu;
}

void
GuiApplicationManager::setNodesMenuTarget(Gui* gui,
                                          QMenu* popup)
{
    _imp->nodesMenuTarget = popup ? gui : 0;
    const quint64 generation = ++_imp->nodesMenuTargetGeneration;
    if (!popup) {
        return;
    }
    // A QMenu hides before it emits the chosen action's triggered(), so the target is cleared only
    // once the event that hid the popup has been handled. A later popup's target is left alone.
    QObject::connect(popup, &QMenu::aboutToHide, this, [this, generation]() { QTimer::singleShot(0, this, [this, generation]() {
                                                                                  if (_imp->nodesMenuTargetGeneration == generation) {
                                                                                      _imp->nodesMenuTarget.clear();
                                                                                  }
                                                                              }); }, Qt::SingleShotConnection);
}

void
GuiApplicationManager::refreshNodesMenuPluginProperties(const QString& pluginID)
{
    if (_imp->nodesMenu) {
        refreshPluginActionsProperties(_imp->nodesMenu, pluginID);
    }
}

void
GuiApplicationManagerPrivate::createNodeFromNodesMenu(const QString& pluginID,
                                                      int major,
                                                      int minor)
{
    GuiAppInstancePtr app;
    // A window with a node creation context set, such as the tab menu's, decides where the node goes.
    const AppInstanceVec& instances = _publicInterface->getAppInstances();
    for (AppInstanceVec::const_iterator it = instances.begin(); it != instances.end(); ++it) {
        if ((*it)->getNodeCreationContextCollection()) {
            app = std::dynamic_pointer_cast<GuiAppInstance>(*it);
            break;
        }
    }
    NodeCollectionPtr group;
    if (app) {
        group = app->getNodeCreationContextCollection();
    } else {
        if (nodesMenuTarget) {
            app = nodesMenuTarget->getApp();
        }
        if (!app) {
            app = activeWindowApp();
        }
        if (!app) {
            app = std::dynamic_pointer_cast<GuiAppInstance>(_publicInterface->getTopLevelInstance());
        }
        if (!app || !app->getGui()) {
            return;
        }
        group = app->getGui()->getLastSelectedNodeCollection();
    }
    if (!app || !group) {
        return;
    }
    CreateNodeArgs args(pluginID.toStdString(), group);
    args.setProperty<int>(kCreateNodeArgsPropPluginVersion, major, 0);
    args.setProperty<int>(kCreateNodeArgsPropPluginVersion, minor, 1);
    app->createNode(args);
}

void
GuiApplicationManagerPrivate::populateNodesMenuGroup(QMenu* parent,
                                                     const PluginGroupNode& group,
                                                     const std::list<PluginGroupNodePtr>& children)
{
    QMenu* menu = findPluginOwnedSubmenu(parent, group.getLabel());

    if (!menu) {
        QIcon menuIcon, toolButtonIcon;
        makePluginGroupIcons(group, &menuIcon, &toolButtonIcon);
        menu = new Menu(parent);
        menu->setTitle(escapedMenuText(group.getLabel()));
        menu->setIcon(menuIcon);
        QAction* menuAction = menu->menuAction();
        menuAction->setProperty(kNodesMenuPropPluginOwned, true);
        menuAction->setProperty(kNodesMenuPropGrouping, group.getLabel());
        menuAction->setProperty(kNodesMenuPropToolButtonIcon, QVariant::fromValue(toolButtonIcon));
        parent->addAction(menuAction);
    }

    for (std::list<PluginGroupNodePtr>::const_iterator it = children.begin(); it != children.end(); ++it) {
        if ((*it)->getChildren().empty()) {
            addNodesMenuPluginAction(menu, **it);
        } else {
            populateNodesMenuGroup(menu, **it, (*it)->getChildren());
        }
    }
}

void
GuiApplicationManagerPrivate::addNodesMenuPluginAction(QMenu* menu,
                                                       const PluginGroupNode& leaf)
{
    const Plugin* plugin = findPlugin(leaf.getID(), leaf.getMajorVersion());
    bool isUserCreatable = plugin ? plugin->getIsUserCreatable() : leaf.getIsUserCreatable();

    if (!isUserCreatable) {
        return;
    }

    // Only an older major is told apart by its version: the newest one keeps the plain label.
    const bool olderMajor = plugin && (leaf.getMajorVersion() < highestUserCreatableMajor(leaf.getID()));
    QString label = olderMajor ? leaf.getLabelVersionMajorEncoded() : leaf.getLabel();
    QIcon menuIcon, toolButtonIcon;
    makePluginGroupIcons(leaf, &menuIcon, &toolButtonIcon);

    QAction* action = new QAction(menu);
    action->setText(escapedMenuText(label));
    action->setIcon(menuIcon);
    action->setProperty(kNodesMenuPropPluginOwned, true);
    action->setProperty(kNodesMenuPropPluginID, leaf.getID());
    action->setProperty(kNodesMenuPropPluginMajor, leaf.getMajorVersion());
    refreshPluginActionProperties(action, plugin);

    const QString pluginID = leaf.getID();
    const int major = leaf.getMajorVersion();
    const int minor = leaf.getMinorVersion();
    QObject::connect(action, &QAction::triggered, _publicInterface, [this, pluginID, major, minor]() {
        createNodeFromNodesMenu(pluginID, major, minor);
    });
    menu->addAction(action);
}

void
GuiApplicationManager::rebuildNodesMenu()
{
    if (!qobject_cast<QApplication*>(QCoreApplication::instance())) {
        return;
    }
    if (!_imp->nodesMenu) {
        _imp->nodesMenu = new Menu(NULL);
        _imp->nodesMenu->setObjectName(QString::fromUtf8(kNodesMenuObjectName));
        // As in the node graph, colours follow saved settings rather than settingChanged(), so a
        // discarded Preferences edit never shows.
        QObject::connect(getCurrentSettings().get(), &Settings::settingsSaved, this, [this]() {
            refreshNodesMenuPluginProperties(QString());
        });
    }
    QMenu* root = _imp->nodesMenu;

    removePluginOwnedLeaves(root);

    // A plug-in without a grouping sits at the top of the tree; it is shown under "Misc".
    std::list<PluginGroupNodePtr> ungrouped;
    PluginGroupNodePtr defaultGroup;
    QStringList treeOrder;
    for (std::list<PluginGroupNodePtr>::const_iterator it = _imp->_topLevelToolButtons.begin(); it != _imp->_topLevelToolButtons.end(); ++it) {
        if ((*it)->getChildren().empty()) {
            ungrouped.push_back(*it);
        } else {
            treeOrder.push_back((*it)->getLabel());
            if ((*it)->getID() == QString::fromUtf8(PLUGIN_GROUP_DEFAULT)) {
                defaultGroup = *it;
            }
        }
    }
    for (std::list<PluginGroupNodePtr>::const_iterator it = _imp->_topLevelToolButtons.begin(); it != _imp->_topLevelToolButtons.end(); ++it) {
        if ((*it)->getChildren().empty()) {
            continue;
        }
        if (*it == defaultGroup) {
            std::list<PluginGroupNodePtr> children = (*it)->getChildren();
            children.insert(children.end(), ungrouped.begin(), ungrouped.end());
            _imp->populateNodesMenuGroup(root, **it, children);
        } else {
            _imp->populateNodesMenuGroup(root, **it, (*it)->getChildren());
        }
    }
    if (!defaultGroup && !ungrouped.empty()) {
        PluginGroupNode miscGroup(QString::fromUtf8(PLUGIN_GROUP_DEFAULT), QString::fromUtf8(PLUGIN_GROUP_DEFAULT),
                                  QString::fromUtf8(PLUGIN_GROUP_DEFAULT_ICON_PATH), 1, 0, true);
        _imp->populateNodesMenuGroup(root, miscGroup, ungrouped);
    }

    removeEmptyPluginOwnedSubmenus(root);
    orderPluginOwnedActions(root, [&treeOrder](const QAction* lhs, const QAction* rhs) {
        return topLevelRank(lhs, treeOrder) < topLevelRank(rhs, treeOrder);
    });
    sortPluginOwnedSubmenus(root);

    Q_EMIT nodesMenuRebuilt();
} // GuiApplicationManager::rebuildNodesMenu

void
GuiApplicationManager::onPluginsActivationChanged()
{
    const PluginsMap& plugins = getPluginsList();

    for (PluginsMap::const_iterator it = plugins.begin(); it != plugins.end(); ++it) {
        for (PluginVersionsOrdered::const_reverse_iterator itver = it->second.rbegin(); itver != it->second.rend(); ++itver) {
            Plugin* plugin = *itver;
            if (!plugin->getIsUserCreatable()) {
                continue;
            }
            QStringList groupingWithID = plugin->getGrouping();
            groupingWithID.push_back(plugin->getPluginID());
            if (hasPluginGroupNode(_imp->_topLevelToolButtons, groupingWithID, 0, plugin->getMajorVersion())) {
                continue;
            }
            // Plug-ins disabled at startup were never given a short label.
            plugin->setLabelWithoutSuffix(Plugin::makeLabelWithoutSuffix(plugin->getPluginLabel()));
            onPluginLoaded(plugin);
        }
    }
    rebuildNodesMenu();
}

PluginGroupNodePtr
GuiApplicationManager::findPluginToolButtonOrCreate(const QStringList & grouping,
                                                    const QString & name,
                                                    const QStringList& groupIconPath,
                                                    const QString & iconPath,
                                                    int major,
                                                    int minor,
                                                    bool isUserCreatable)
{
    assert(grouping.size() > 0);

    return _imp->findPluginToolButtonInternal(_imp->_topLevelToolButtons, PluginGroupNodePtr(), grouping, name, groupIconPath, iconPath, major, minor, isUserCreatable);
}

bool
GuiApplicationManager::isSplashcreenVisible() const
{
    return _imp->_splashScreen ? true : false;
}

void
GuiApplicationManager::hideSplashScreen()
{
#ifdef DEBUG
    boost_adaptbx::floating_point::exception_trapping trap(0);
#endif
    if (_imp->_splashScreen) {
        _imp->_splashScreen->hide();
        _imp->_splashScreen->close();
        _imp->_splashScreen->deleteLater();
        _imp->_splashScreen = 0;
    }
}

void
GuiApplicationManager::setKnobClipBoard(KnobClipBoardType type,
                                        const KnobIPtr& serialization,
                                        int dimension)
{
    _imp->_knobsClipBoard->serialization = serialization;
    _imp->_knobsClipBoard->type = type;
    _imp->_knobsClipBoard->dimension = dimension;
}

void
GuiApplicationManager::getKnobClipBoard(KnobClipBoardType *type,
                                        KnobIPtr *serialization,
                                        int* dimension) const
{
    *serialization = _imp->_knobsClipBoard->serialization;
    *type = _imp->_knobsClipBoard->type;
    *dimension = _imp->_knobsClipBoard->dimension;
}

void
GuiApplicationManager::appendTaskToPreviewThread(const NodeGuiPtr& node,
                                                 double time)
{
    _imp->previewRenderThread.appendToQueue(node, time);
}

int
GuiApplicationManager::getDocumentationServerPort()
{
    return _imp->documentation->serverPort();
}

double
GuiApplicationManager::getLogicalDPIXRATIO() const
{
    return _imp->dpiX / NATRON_PIXELS_FOR_DPI_DEFAULT;
}

double
GuiApplicationManager::getLogicalDPIYRATIO() const
{
    return _imp->dpiY / NATRON_PIXELS_FOR_DPI_DEFAULT;
}

void
GuiApplicationManager::setCurrentLogicalDPI(double dpiX,
                                            double dpiY)
{
    _imp->dpiX = dpiX;
    _imp->dpiY = dpiY;
}

void
GuiApplicationManager::updateAboutWindowLibrariesVersion()
{
    const AppInstanceVec& instances = getAppInstances();

    for (AppInstanceVec::const_iterator it = instances.begin(); it != instances.end(); ++it) {
        GuiAppInstance* isGuiInstance = dynamic_cast<GuiAppInstance*>( it->get() );
        if (isGuiInstance) {
            Gui* gui = isGuiInstance->getGui();
            if (gui) {
                gui->updateAboutWindowLibrariesVersion();
            }
        }
    }
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_GuiApplicationManager.cpp"
