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

#ifndef Engine_Nodes_Transform_CornerPinKnobs_h
#define Engine_Nodes_Transform_CornerPinKnobs_h

/*
 * The plug-in ID and knob script names of the native CornerPin, shared by the node and by
 * everything that drives one by name (the tracker's internal CornerPin and its export).
 * Free of engine includes so any translation unit can take it.
 */

#define PLUGINID_NATRON_CORNERPIN "CornerPin"
#define PLUGIN_MAJOR_NATRON_CORNERPIN 1

// Tabs holding the "to" points (with their enable flags) and the "from" points.
#define kCornerPinNodeParamToGroup "to"
#define kCornerPinNodeParamFromGroup "from"

// Points, in canonical coordinates. enableN covers both toN and fromN.
#define kCornerPinNodeParamTo1 "to1"
#define kCornerPinNodeParamTo2 "to2"
#define kCornerPinNodeParamTo3 "to3"
#define kCornerPinNodeParamTo4 "to4"
#define kCornerPinNodeParamFrom1 "from1"
#define kCornerPinNodeParamFrom2 "from2"
#define kCornerPinNodeParamFrom3 "from3"
#define kCornerPinNodeParamFrom4 "from4"
#define kCornerPinNodeParamEnable1 "enable1"
#define kCornerPinNodeParamEnable2 "enable2"
#define kCornerPinNodeParamEnable3 "enable3"
#define kCornerPinNodeParamEnable4 "enable4"

// Buttons.
#define kCornerPinNodeParamCopyFrom "copyFrom"
#define kCornerPinNodeParamCopyFromSingle "copyFromSingle"
#define kCornerPinNodeParamCopyTo "copyTo"
#define kCornerPinNodeParamCopyToSingle "copyToSingle"
#define kCornerPinNodeParamSetToInput "setToInput"

#define kCornerPinNodeParamOverlayPoints "overlayPoints"
#define kCornerPinNodeParamAmount "amount"
// One 9-dimension double knob, the row-major 3x3 matrix composed with the pin.
#define kCornerPinNodeParamExtraMatrix "extraMatrix"
#define kCornerPinNodeParamInvert "invert"
#define kCornerPinNodeParamInteractive "interactive"

#define kCornerPinNodeParamFilter "filter"
#define kCornerPinNodeParamClamp "clamp"
#define kCornerPinNodeParamBlackOutside "blackOutside"

#define kCornerPinNodeParamMotionBlur "motionBlur"
#define kCornerPinNodeParamShutter "shutter"
#define kCornerPinNodeParamShutterOffset "shutterOffset"
#define kCornerPinNodeParamShutterCustomOffset "shutterCustomOffset"

// Hidden: set once the user edits the points, so connecting a source no longer resets them.
#define kCornerPinNodeParamSrcClipChanged "srcClipChanged"

#endif // Engine_Nodes_Transform_CornerPinKnobs_h
