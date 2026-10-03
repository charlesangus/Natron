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

#ifndef NATRON_ENGINE_LUT_H
#define NATRON_ENGINE_LUT_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cmath>

#include "Engine/EngineFwd.h"

#define NATRON_COLOR_HUE_CIRCLE 1. // if hue should be between 0 and 1
//#define NATRON_COLOR_HUE_CIRCLE 360. // if hue should be in degrees

NATRON_NAMESPACE_ENTER
namespace Color {

// r,g,b values are from 0 to 1
// h = [0,NATRON_COLOR_HUE_CIRCLE], s = [0,1], v = [0,1]
//		if s == 0, then h = 0 (undefined)
void rgb_to_hsv( float r, float g, float b, float *h, float *s, float *v );
void hsv_to_rgb(float h, float s, float v, float *r, float *g, float *b);

/// numvals should be 256 for byte, 65536 for 16-bits, etc.

/// maps 0-(numvals-1) to 0.-1.
template<int numvals>
float
intToFloat(int value)
{
    return value / (float)(numvals - 1);
}

/// maps 0.-1. to 0-(numvals-1)
template<int numvals>
int
floatToInt(float value)
{
    if (value <= 0) {
        return 0;
    } else if (value >= 1.) {
        return numvals - 1;
    }

    //return value * (numvals - 1) + 0.5; // gives a bad result with gcc 7.2.0 32 bits for value=intToFloat<0xff01>(0x8080)
    float v = value * (numvals - 1) + 0.5f;
    return int(v);
}

/// maps 0x0-0xffff to 0x0-0xff
inline unsigned char
uint16ToChar(unsigned short quantum)
{
    // see ScaleQuantumToChar() in ImageMagick's magick/quantum.h

    /* test:
       for(int i=0; i < 0x10000; ++i) {
        printf("%x -> %x,%x\n", i, uint16ToChar(i), floatToInt<256>(intToFloat<65536>(i)));
        assert(uint16ToChar(i) == floatToInt<256>(intToFloat<65536>(i)));
       }
     */
    return (unsigned char) ( ( (quantum + 128UL) - ( (quantum + 128UL) >> 8 ) ) >> 8 );
}

/// maps 0x0-0xff to 0x0-0xffff
inline unsigned short
charToUint16(unsigned char quantum)
{
    /* test:
       for(int i=0; i < 0x100; ++i) {
        printf("%x -> %x,%x\n", i, charToUint16(i), floatToInt<65536>(intToFloat<256>(i)));
       assert(charToUint16(i) == floatToInt<65536>(intToFloat<256>(i)));
       assert(i == uint16ToChar(charToUint16(i)));
       }
     */
    return (unsigned short) ( (quantum << 8) | quantum );
}

// maps 0x0-0xff00 to 0x0-0xff
inline unsigned char
uint8xxToChar(unsigned short quantum)
{
    /* test:
       for(int i=0; i < 0xff01; ++i) {
        printf("%x -> %x,%x, err=%d\n", i, uint8xxToChar(i), floatToInt<256>(intToFloat<0xff01>(i)),i - charToUint8xx(uint8xxToChar(i)));
        assert(uint8xxToChar(i) == floatToInt<256>(intToFloat<0xff01>(i)));
       }
     */
    return (unsigned char) ( (quantum + 0x80) >> 8 );
}

// maps 0x0-0xff to 0x0-0xff00
inline unsigned short
charToUint8xx(unsigned char quantum)
{
    /* test:
       for(int i=0; i < 0x100; ++i) {
        printf("%x -> %x,%x\n", i, charToUint8xx(i), floatToInt<0xff01>(intToFloat<256>(i)));
        assert(charToUint8xx(i) == floatToInt<0xff01>(intToFloat<256>(i)));
        assert(i == uint8xxToChar(charToUint8xx(i)));
       }
     */
    return (unsigned short) (quantum << 8);
}
} // namespace Color

NATRON_NAMESPACE_EXIT

#endif //NATRON_ENGINE_LUT_H
