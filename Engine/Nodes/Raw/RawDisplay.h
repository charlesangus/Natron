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

#ifndef Engine_Nodes_Raw_RawDisplay_h
#define Engine_Nodes_Raw_RawDisplay_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief How raw data is shown as an image, shared by the Viewer and RawToImage: the values are
 * kept as they are, with no demosaic, scaling or colour. A mosaic, one value per photosite,
 * becomes grey (red, green and blue all the photosite's value); linear raw keeps its channels,
 * a single one again as grey. Alpha is 1.
 **/
namespace RawDisplay {
enum ModeEnum {
    eModeMosaic,
    eModeLinear
};

/**
 * @brief A mosaic when raw/cfa_pattern names a colour filter array, linear when it is "none".
 * Without the key, which a node that passes no metadata on leaves out, one channel is taken for
 * a mosaic and more for linear data.
 **/
ModeEnum modeOf(const ImageMetadata& metadata,
                int nComps);

/**
 * @brief "RAW mosaic" or "RAW linear", the Viewer's info line for raw input.
 **/
const char* label(ModeEnum mode);

/**
 * @brief The red, green, blue and alpha of a raw pixel of rawComps channels.
 **/
inline void
toRgba(const float* raw,
       int rawComps,
       float* rgba)
{
    if (rawComps >= 3) {
        rgba[0] = raw[0];
        rgba[1] = raw[1];
        rgba[2] = raw[2];
    } else if (rawComps >= 1) {
        rgba[0] = rgba[1] = rgba[2] = raw[0];
    } else {
        rgba[0] = rgba[1] = rgba[2] = 0.f;
    }
    rgba[3] = 1.f;
}

/**
 * @brief Writes window of dst, a float image of 1 to 4 channels, from the raw float image src:
 * channel c of each pixel is channel c of toRgba() (a one-channel dst gets the alpha, 1, as
 * Natron's one-channel images are alpha). A pixel src does not cover reads as zero, alpha
 * included.
 **/
void convert(const Image& src,
             Image* dst,
             const RectI& window);

/**
 * @brief An RGBA float image over src's bounds, converted from src by convert().
 **/
ImagePtr toDisplayImage(const ImagePtr& src);
} // namespace RawDisplay

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Raw_RawDisplay_h
