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

#ifndef Tests_OfxTestPlugins_NatronTestMetadataPluginIDs_h
#define Tests_OfxTestPlugins_NatronTestMetadataPluginIDs_h

// Identifiers, versions and parameter names of the natronTestMetadata bundle's plug-ins. Plain
// macros with no OFX include, so the host-side tests can name them too. All three pass Source's
// pixels through.

// Contributes six keys under kNatronTestMetadataContributeKeyPrefix, one through each of the
// metadata suite's setters, plus ofx/framerate = 30, and inherits from Source by mode.
#define kNatronTestMetadataContributePluginID "fr.natron.test.metadataContribute"
#define kNatronTestMetadataContributeMajor 1
#define kNatronTestMetadataContributeKeyPrefix "org.openfx.examples.metadataContribute."
#define kNatronTestMetadataContributeParamNote "note"
#define kNatronTestMetadataContributeParamMode "mode"
#define kNatronTestMetadataContributeParamDropKey "dropKey"
#define kNatronTestMetadataContributeModeInheritAll 0
#define kNatronTestMetadataContributeModeDropOneKey 1
#define kNatronTestMetadataContributeModeInheritNothing 2
#define kNatronTestMetadataContributeFrameRate 30.

// Reads Source's metadata at render time and writes every key to the host log.
#define kNatronTestMetadataPrintPluginID "fr.natron.test.metadataPrint"
#define kNatronTestMetadataPrintMajor 1

// Publishes ofx/timecode, counted non-drop-frame from startTimecode at the origin frame, and the
// ofx/framerate it counted at.
#define kNatronTestMetadataTimeCodePluginID "fr.natron.test.metadataTimeCode"
#define kNatronTestMetadataTimeCodeMajor 1
#define kNatronTestMetadataTimeCodeParamStartTimecode "startTimecode"
#define kNatronTestMetadataTimeCodeParamRate "rate"
#define kNatronTestMetadataTimeCodeParamRateFromMetadata "rateFromMetadata"
#define kNatronTestMetadataTimeCodeParamStartFrame "startFrame"
#define kNatronTestMetadataTimeCodeParamUseStartFrame "useStartFrame"

#endif // Tests_OfxTestPlugins_NatronTestMetadataPluginIDs_h
