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

#ifndef Tests_OfxTestPlugins_NatronTestPluginIDs_h
#define Tests_OfxTestPlugins_NatronTestPluginIDs_h

// Identifiers, versions and parameter names of the natronTest bundle's plug-ins. Plain macros
// with no OFX include, so the host-side tests can name them too.

// Routes input "0" or "1" to the output, like Switch: by identity when passThrough is on, by
// rendering a copy of the chosen input when it is off.
#define kNatronTestTwoInputCopyPluginID "fr.natron.test.TwoInputCopy"
#define kNatronTestTwoInputCopyMajor 1
#define kNatronTestTwoInputCopyClipA "0"
#define kNatronTestTwoInputCopyClipB "1"
#define kNatronTestTwoInputCopyParamWhich "which"
#define kNatronTestTwoInputCopyParamPassThrough "passThrough"

// Fills its colour over the project or a chosen format, with an optional Source it writes over.
#define kNatronTestGeneratorPluginID "fr.natron.test.Generator"
#define kNatronTestGeneratorMajor 1
#define kNatronTestGeneratorParamColor "color"
#define kNatronTestGeneratorParamExtent "extent"
#define kNatronTestGeneratorExtentProject "project"
#define kNatronTestGeneratorExtentFormat "format"
#define kNatronTestGeneratorParamOutputComponents "outputComponents"

// Multiplies RGBA by a 4x4 matrix whose rows are the four output* colour params, with
// ColorMatrix's quad, (un)premult pair, mix and Mask input.
#define kNatronTestPointMatrixPluginID "fr.natron.test.PointMatrix"
#define kNatronTestPointMatrixMajor 2
#define kNatronTestPointMatrixParamOutputRed "outputRed"
#define kNatronTestPointMatrixParamOutputGreen "outputGreen"
#define kNatronTestPointMatrixParamOutputBlue "outputBlue"
#define kNatronTestPointMatrixParamOutputAlpha "outputAlpha"
#define kNatronTestPointMatrixParamMix "mix"
#define kNatronTestPointMatrixParamMaskInvert "maskInvert"
#define kNatronTestPointMatrixClipMask "Mask"

// The previous major of PointMatrix under its own ID, as HueCorrect1 is to HueCorrect.
#define kNatronTestPointMatrix1PluginID kNatronTestPointMatrixPluginID "1"
#define kNatronTestPointMatrix1Major 1

// The Generator and PointMatrix again, with the process quad's alpha off by default as
// Quantize's is, so the host seeds the channel set's R, G and B from it.
#define kNatronTestRGBGeneratorPluginID "fr.natron.test.RGBGenerator"
#define kNatronTestRGBGeneratorMajor 1
#define kNatronTestRGBPointPluginID "fr.natron.test.RGBPoint"
#define kNatronTestRGBPointMajor 1

// Multiplanar, like the openfx-misc Premult: multiplies the quad's channels of inputPlane (or of every plane, with
// processAllPlanes) by the unPremultByChannel channel of Source. Other planes pass through.
#define kNatronTestMultiPlanePluginID "fr.natron.test.MultiPlane"
#define kNatronTestMultiPlaneMajor 1
#define kNatronTestMultiPlaneParamInputPlane "inputPlane"
#define kNatronTestMultiPlaneParamProcessAllPlanes "processAllPlanes"
#define kNatronTestMultiPlaneParamByChannel "unPremultByChannel"

// Warps Source by the 3x3 matrix in transform_row1..3 (canonical coordinates, as CornerPin's
// extra matrix), offset per pixel by UV's channelU/channelV and scaled by its channelA, as
// IDistort. With UV neutral it is a pure transform the host can concatenate.
#define kNatronTestMatrixWarpPluginID "fr.natron.test.MatrixWarp"
#define kNatronTestMatrixWarpMajor 1
#define kNatronTestMatrixWarpClipUV "UV"
#define kNatronTestMatrixWarpParamRow1 "transform_row1"
#define kNatronTestMatrixWarpParamRow2 "transform_row2"
#define kNatronTestMatrixWarpParamRow3 "transform_row3"
#define kNatronTestMatrixWarpParamChannelU "channelU"
#define kNatronTestMatrixWarpParamChannelV "channelV"
#define kNatronTestMatrixWarpParamChannelA "channelA"

// Averages Source over frameRange, relative to the time minus timeOffset unless absolute is set,
// like FrameBlend; a one-frame range is an identity at that frame, like TimeOffset.
#define kNatronTestFrameWindowPluginID "fr.natron.test.FrameWindow"
#define kNatronTestFrameWindowMajor 1
#define kNatronTestFrameWindowParamFrameRange "frameRange"
#define kNatronTestFrameWindowParamAbsolute "absolute"
#define kNatronTestFrameWindowParamTimeOffset "timeOffset"

// An identity filter declaring the OCIO params the project pushes into every node.
#define kNatronTestOCIOConfigPluginID "fr.natron.test.OCIOConfig"
#define kNatronTestOCIOConfigMajor 1
#define kNatronTestOCIOConfigParamConfigFile "ocioConfigFile"
#define kNatronTestOCIOConfigParamWorkingSpace "ocioWorkingSpace"
#define kNatronTestOCIOConfigParamInputSpace "ocioInputSpace"
#define kNatronTestOCIOConfigParamOutputSpace "ocioOutputSpace"

#endif // Tests_OfxTestPlugins_NatronTestPluginIDs_h
