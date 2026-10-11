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

#ifndef Gui_GuiDensity_h
#define Gui_GuiDensity_h

#include "Global/Macros.h"

// Logical px. Callers wrap each value in TO_DPIX/TO_DPIY (Engine/AppManager.h)
// at the call site; this header never scales anything itself.
#define NATRON_DENSITY_ROW_GAP 4
#define NATRON_DENSITY_LABEL_GAP 8
#define NATRON_DENSITY_INLINE_GAP 12
#define NATRON_DENSITY_GROUP_INDENT 12
#define NATRON_DENSITY_SECTION_GAP 8
#define NATRON_DENSITY_PANEL_MARGIN 6
#define NATRON_DENSITY_HEADER_MARGIN 2
#define NATRON_DENSITY_HEADER_CLUSTER 8
#define NATRON_DENSITY_PANE_TOP_MARGIN 2
#define NATRON_DENSITY_TOOLBAR_SPACING 2
#define NATRON_DENSITY_TOOLBAR_GROUP_GAP 6

#endif // Gui_GuiDensity_h
