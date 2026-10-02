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

#include "Image.h"

#include <algorithm> // min, max
#include <cassert>
#include <stdexcept>

#include <QDebug>

#include "Engine/AppManager.h"
#include "Engine/Lut.h"

NATRON_NAMESPACE_ENTER

///explicit template instantiations

template <>
float
Image::convertPixelDepth(unsigned char pix)
{
    return Color::intToFloat<256>(pix);
}

template <>
unsigned short
Image::convertPixelDepth(unsigned char pix)
{
    // 0x01 -> 0x0101, 0x02 -> 0x0202, ..., 0xff -> 0xffff
    return (unsigned short)( (pix << 8) + pix );
}

template <>
unsigned char
Image::convertPixelDepth(unsigned char pix)
{
    return pix;
}

template <>
unsigned char
Image::convertPixelDepth(unsigned short pix)
{
    // the following is from ImageMagick's quantum.h
    return (unsigned char)( ( (pix + 128UL) - ( (pix + 128UL) >> 8 ) ) >> 8 );
}

template <>
float
Image::convertPixelDepth(unsigned short pix)
{
    return Color::intToFloat<65536>(pix);
}

template <>
unsigned short
Image::convertPixelDepth(unsigned short pix)
{
    return pix;
}

template <>
unsigned char
Image::convertPixelDepth(float pix)
{
    return (unsigned char)Color::floatToInt<256>(pix);
}

template <>
unsigned short
Image::convertPixelDepth(float pix)
{
    return (unsigned short)Color::floatToInt<65536>(pix);
}

template <>
float
Image::convertPixelDepth(float pix)
{
    return pix;
}

template <typename SRCPIX, typename DSTPIX, int srcMaxValue, int dstMaxValue>
void
Image::convertToFormatInternal_sameComps(const RectI& renderWindow,
                                         const Image& srcImg,
                                         Image& dstImg,
                                         ViewerColorSpaceEnum /*srcColorSpace*/,
                                         ViewerColorSpaceEnum /*dstColorSpace*/,
                                         bool copyBitmap)
{
    const RectI & r = srcImg._bounds;
    const RectI intersection = renderWindow.intersect(r);

    if ( intersection.isNull() ) {
        return;
    }

    const int nComp = (int)srcImg.getComponentsCount();
    const int rowElements = intersection.width() * nComp;

    for (int y = 0; y < intersection.height(); ++y) {
        const SRCPIX* srcPixels = (const SRCPIX*)srcImg.pixelAt(intersection.x1, intersection.y1 + y);
        DSTPIX* dstPixels = (DSTPIX*)dstImg.pixelAt(intersection.x1, intersection.y1 + y);

        for (int i = 0; i < rowElements; ++i) {
#ifdef DEBUG_NAN
            assert(!std::isnan(srcPixels[i])); // check for NaN
#endif
            dstPixels[i] = convertPixelDepth<SRCPIX, DSTPIX>(srcPixels[i]);
        }

        if (copyBitmap) {
            dstImg.copyBitmapRowPortion(intersection.x1, intersection.x2, intersection.y1 + y, srcImg);
        }
    }
} // convertToFormatInternal_sameComps

template <typename SRCPIX, typename DSTPIX, int srcMaxValue, int dstMaxValue, int srcNComps, int dstNComps>
void
Image::convertToFormatInternal(const RectI& renderWindow,
                               const Image& srcImg,
                               Image& dstImg,
                               ViewerColorSpaceEnum /*srcColorSpace*/,
                               ViewerColorSpaceEnum /*dstColorSpace*/,
                               int channelForAlpha,
                               bool zeroFillMissing,
                               bool copyBitmap)
{
    /*
     * If channelForAlpha is -1 the user wants to convert using the default
     * If channelForAlpha >= 0 then the user wants a specific channel to convert from. This is used mainly when converting
     * to Alpha images (masks) to know which channel to use for the mask.
     */
    if (channelForAlpha != -1) {
        switch (srcNComps) {
        case 3:
            //invalid value passed by the called
            if (channelForAlpha > 2) {
                channelForAlpha = -1;
            }
            break;
        case 2:
            //invalid value passed by the called
            if (channelForAlpha > 1) {
                channelForAlpha = -1;
            }
        default:
            break;
        }
    } else {
        switch (srcNComps) {
        case 4:
            channelForAlpha = 3;
            break;
        case 3:
        case 1:
        default:
            //no alpha anyway
            break;
        }
    }


    ///special case comp == alpha && channelForAlpha = -1 clear out the mask
    if ( (dstNComps == 1) && (channelForAlpha == -1) ) {
        DSTPIX* dstPixels = (DSTPIX*)dstImg.pixelAt(renderWindow.x1, renderWindow.y1);
        int dstRowSize = dstImg._bounds.width() * dstNComps;
        if (copyBitmap) {
            dstImg.copyBitmapPortion(renderWindow, srcImg);
        }
        for (int y = 0; y < renderWindow.height();
             ++y, dstPixels += dstRowSize) {
            std::fill(dstPixels, dstPixels + renderWindow.width() * dstNComps, 0.);
        }

        return;
    }

    for (int y = 0; y < renderWindow.height(); ++y) {
        const SRCPIX* srcPixels = (const SRCPIX*)srcImg.pixelAt(renderWindow.x1, renderWindow.y1 + y);
        DSTPIX* dstPixels = (DSTPIX*)dstImg.pixelAt(renderWindow.x1, renderWindow.y1 + y);

        for (int x = 0; x < renderWindow.width(); ++x, srcPixels += srcNComps, dstPixels += dstNComps) {
            if (dstNComps == 1) {
                /// If we're converting to alpha, we just have to handle pixel depth conversion
                DSTPIX pix;

                switch (srcNComps) {
                case 4:
                    // channel for alpha must be valid
                    assert(channelForAlpha > -1 && channelForAlpha <= 3);
                    pix = convertPixelDepth<SRCPIX, DSTPIX>(srcPixels[channelForAlpha]);
                    break;
                case 3:
                    // RGB has no alpha, unless channelForAlpha is 0-2
                    pix = convertPixelDepth<SRCPIX, DSTPIX>(channelForAlpha == -1 ? 0. : srcPixels[channelForAlpha]);
                    break;
                case 2:
                    // XY has no alpha, unless channelForAlpha is 0-1
                    pix = convertPixelDepth<SRCPIX, DSTPIX>(channelForAlpha == -1 ? 0. : srcPixels[channelForAlpha]);
                    break;
                case 1:
                    // just copy alpha disregarding channelForAlpha
                    pix = convertPixelDepth<SRCPIX, DSTPIX>(*srcPixels);
                    break;
                }

                dstPixels[0] = pix;
#ifdef DEBUG_NAN
                assert(!std::isnan(dstPixels[0])); // check for NaN
#endif
            } else if (srcNComps == 1) {
                DSTPIX pix = convertPixelDepth<SRCPIX, DSTPIX>(srcPixels[0]);
#ifdef DEBUG_NAN
                assert(!std::isnan(pix)); // check for NaN
#endif
                if (zeroFillMissing) {
                    // A one-channel colour source is the alpha layout, so only an alpha destination channel receives it.
                    for (int k = 0; k < dstNComps; ++k) {
                        dstPixels[k] = (k == 3) ? pix : DSTPIX(0);
                    }
                } else {
                    for (int k = 0; k < dstNComps; ++k) {
                        dstPixels[k] = pix;
                    }
                }
            } else {
                /// In this case we've XY, RGB or RGBA input and outputs
                assert(srcNComps != dstNComps);

                for (int k = 0; k < 3 && k < dstNComps; ++k) {
                    if (k >= srcNComps) { // e.g. srcNComps = 2 && dstNComps == 3 or 4
                        dstPixels[k] = 0;
                        continue;
                    }
                    dstPixels[k] = convertPixelDepth<SRCPIX, DSTPIX>(srcPixels[k]);
#ifdef DEBUG_NAN
                    assert((std::isnan)(srcPixels[k]) || !std::isnan(dstPixels[k])); // check for NaN
#endif
                }

                if (dstNComps == 4) {
                    // Only RGB-->RGBA or XY-->RGBA reach here, so the source has no alpha to copy.
                    dstPixels[3] = convertPixelDepth<float, DSTPIX>(zeroFillMissing ? 0.f : 1.f);
                }
            }
        }
    }

    if (copyBitmap) {
        dstImg.copyBitmapPortion(renderWindow, srcImg);
    }
} // Image::convertToFormatInternal

template <typename SRCPIX, typename DSTPIX, int srcMaxValue, int dstMaxValue>
void
Image::convertToFormatInternalForDepth(const RectI& renderWindow,
                                       const Image& srcImg,
                                       Image& dstImg,
                                       ViewerColorSpaceEnum srcColorSpace,
                                       ViewerColorSpaceEnum dstColorSpace,
                                       int channelForAlpha,
                                       bool zeroFillMissing,
                                       bool copyBitmap)
{
    int dstNComp = dstImg.getComponents().getNumComponents();
    int srcNComp = srcImg.getComponents().getNumComponents();

    switch (srcNComp) {
    case 1:
        switch (dstNComp) {
        case 2:
            convertToFormatInternal<SRCPIX, DSTPIX, srcMaxValue, dstMaxValue, 1, 2>(renderWindow, srcImg, dstImg,
                                                                                    srcColorSpace,
                                                                                    dstColorSpace,
                                                                                    channelForAlpha,
                                                                                    zeroFillMissing,
                                                                                    copyBitmap);
            break;
        case 3:
            convertToFormatInternal<SRCPIX, DSTPIX, srcMaxValue, dstMaxValue, 1, 3>(renderWindow, srcImg, dstImg,
                                                                                    srcColorSpace,
                                                                                    dstColorSpace,
                                                                                    channelForAlpha,
                                                                                    zeroFillMissing,
                                                                                    copyBitmap);
            break;
        case 4:
            convertToFormatInternal<SRCPIX, DSTPIX, srcMaxValue, dstMaxValue, 1, 4>(renderWindow, srcImg, dstImg,
                                                                                    srcColorSpace,
                                                                                    dstColorSpace,
                                                                                    channelForAlpha,
                                                                                    zeroFillMissing,
                                                                                    copyBitmap);
            break;
        default:
            assert(false);
            break;
        }
        break;
    case 2:
        switch (dstNComp) {
        case 1:
            convertToFormatInternal<SRCPIX, DSTPIX, srcMaxValue, dstMaxValue, 2, 1>(renderWindow, srcImg, dstImg,
                                                                                    srcColorSpace,
                                                                                    dstColorSpace,
                                                                                    channelForAlpha,
                                                                                    zeroFillMissing,
                                                                                    copyBitmap);
            break;
        case 3:
            convertToFormatInternal<SRCPIX, DSTPIX, srcMaxValue, dstMaxValue, 2, 3>(renderWindow, srcImg, dstImg,
                                                                                    srcColorSpace,
                                                                                    dstColorSpace,
                                                                                    channelForAlpha,
                                                                                    zeroFillMissing,
                                                                                    copyBitmap);
            break;
        case 4:
            convertToFormatInternal<SRCPIX, DSTPIX, srcMaxValue, dstMaxValue, 2, 4>(renderWindow, srcImg, dstImg,
                                                                                    srcColorSpace,
                                                                                    dstColorSpace,
                                                                                    channelForAlpha,
                                                                                    zeroFillMissing,
                                                                                    copyBitmap);
            break;
        default:
            assert(false);
            break;
        }
        break;
    case 3:
        switch (dstNComp) {
        case 1:
            convertToFormatInternal<SRCPIX, DSTPIX, srcMaxValue, dstMaxValue, 3, 1>(renderWindow, srcImg, dstImg,
                                                                                    srcColorSpace,
                                                                                    dstColorSpace,
                                                                                    channelForAlpha,
                                                                                    zeroFillMissing,
                                                                                    copyBitmap);
            break;
        case 2:
            convertToFormatInternal<SRCPIX, DSTPIX, srcMaxValue, dstMaxValue, 3, 2>(renderWindow, srcImg, dstImg,
                                                                                    srcColorSpace,
                                                                                    dstColorSpace,
                                                                                    channelForAlpha,
                                                                                    zeroFillMissing,
                                                                                    copyBitmap);
            break;
        case 4:
            convertToFormatInternal<SRCPIX, DSTPIX, srcMaxValue, dstMaxValue, 3, 4>(renderWindow, srcImg, dstImg,
                                                                                    srcColorSpace,
                                                                                    dstColorSpace,
                                                                                    channelForAlpha,
                                                                                    zeroFillMissing,
                                                                                    copyBitmap);
            break;
        default:
            assert(false);
            break;
        }
        break;
    case 4:
        switch (dstNComp) {
        case 1:
            convertToFormatInternal<SRCPIX, DSTPIX, srcMaxValue, dstMaxValue, 4, 1>(renderWindow, srcImg, dstImg,
                                                                                    srcColorSpace,
                                                                                    dstColorSpace,
                                                                                    channelForAlpha,
                                                                                    zeroFillMissing,
                                                                                    copyBitmap);
            break;
        case 2:
            convertToFormatInternal<SRCPIX, DSTPIX, srcMaxValue, dstMaxValue, 4, 2>(renderWindow, srcImg, dstImg,
                                                                                    srcColorSpace,
                                                                                    dstColorSpace,
                                                                                    channelForAlpha,
                                                                                    zeroFillMissing,
                                                                                    copyBitmap);
            break;
        case 3:
            convertToFormatInternal<SRCPIX, DSTPIX, srcMaxValue, dstMaxValue, 4, 3>(renderWindow, srcImg, dstImg,
                                                                                    srcColorSpace,
                                                                                    dstColorSpace,
                                                                                    channelForAlpha,
                                                                                    zeroFillMissing,
                                                                                    copyBitmap);
            break;
        default:
            assert(false);
            break;
        }
        break;
    default:
        break;
    } // switch
} // Image::convertToFormatInternalForDepth

void
Image::convertToFormat(const RectI& renderWindow,
                       ViewerColorSpaceEnum srcColorSpace,
                       ViewerColorSpaceEnum dstColorSpace,
                       int channelForAlpha,
                       bool copyBitmap,
                       Image* dstImg) const
{
    // OpenGL textures are always RGBA anyway
    assert(getStorageMode() != eStorageModeGLTex);

    // A colour channel the source layout lacks reads zero. Any other plane keeps the historical
    // fill, because a one-channel plane handed to a plug-in through its colour clip is read back
    // from channel 0, and a scratch alpha of 1 keeps premultiplication a no-op on such a plane.
    const bool zeroFillMissing = getComponents().isColorLayer();

    QWriteLocker k(&dstImg->_entryLock);
    QReadLocker k2(&_entryLock);

    assert( _bounds.contains(renderWindow) &&  dstImg->_bounds.contains(renderWindow) );

    if ( dstImg->getComponents().getNumComponents() == getComponents().getNumComponents() ) {
        switch ( dstImg->getBitDepth() ) {
        case eImageBitDepthByte: {
            switch ( getBitDepth() ) {
            case eImageBitDepthByte:
                ///Same as a copy
                convertToFormatInternal_sameComps<unsigned char, unsigned char, 255, 255>(renderWindow, *this, *dstImg,
                                                                                          srcColorSpace,
                                                                                          dstColorSpace, copyBitmap);
                break;
            case eImageBitDepthShort:
                convertToFormatInternal_sameComps<unsigned short, unsigned char, 65535, 255>(renderWindow, *this, *dstImg,
                                                                                             srcColorSpace,
                                                                                             dstColorSpace, copyBitmap);
                break;
            case eImageBitDepthHalf:
                break;
            case eImageBitDepthFloat:
                convertToFormatInternal_sameComps<float, unsigned char, 1, 255>(renderWindow, *this, *dstImg,
                                                                                srcColorSpace,
                                                                                dstColorSpace, copyBitmap);
                break;
            case eImageBitDepthNone:
                break;
            }
            break;
        }

        case eImageBitDepthShort: {
            switch ( getBitDepth() ) {
            case eImageBitDepthByte:
                convertToFormatInternal_sameComps<unsigned char, unsigned short, 255, 65535>(renderWindow, *this, *dstImg,
                                                                                             srcColorSpace,
                                                                                             dstColorSpace, copyBitmap);
                break;
            case eImageBitDepthShort:
                ///Same as a copy
                convertToFormatInternal_sameComps<unsigned short, unsigned short, 65535, 65535>(renderWindow, *this, *dstImg,
                                                                                                srcColorSpace,
                                                                                                dstColorSpace, copyBitmap);
                break;
            case eImageBitDepthHalf:
                break;
            case eImageBitDepthFloat:
                convertToFormatInternal_sameComps<float, unsigned short, 1, 65535>(renderWindow, *this, *dstImg,
                                                                                   srcColorSpace,
                                                                                   dstColorSpace, copyBitmap);
                break;
            case eImageBitDepthNone:
                break;
            }
            break;
        }

        case eImageBitDepthHalf:
            break;

        case eImageBitDepthFloat: {
            switch ( getBitDepth() ) {
            case eImageBitDepthByte:
                convertToFormatInternal_sameComps<unsigned char, float, 255, 1>(renderWindow, *this, *dstImg,
                                                                                srcColorSpace,
                                                                                dstColorSpace, copyBitmap);
                break;
            case eImageBitDepthShort:
                convertToFormatInternal_sameComps<unsigned short, float, 65535, 1>(renderWindow, *this, *dstImg,
                                                                                   srcColorSpace,
                                                                                   dstColorSpace, copyBitmap);
                break;
            case eImageBitDepthHalf:
                break;
            case eImageBitDepthFloat:
                ///Same as a copy
                convertToFormatInternal_sameComps<float, float, 1, 1>(renderWindow, *this, *dstImg,
                                                                      srcColorSpace,
                                                                      dstColorSpace, copyBitmap);
                break;
            case eImageBitDepthNone:
                break;
            }
            break;
        }

        case eImageBitDepthNone:
            break;
        } // switch
    } else {
        switch ( dstImg->getBitDepth() ) {
        case eImageBitDepthByte: {
            switch ( getBitDepth() ) {
            case eImageBitDepthByte:
                convertToFormatInternalForDepth<unsigned char, unsigned char, 255, 255>(renderWindow, *this, *dstImg,
                                                                                        srcColorSpace,
                                                                                        dstColorSpace,
                                                                                        channelForAlpha,
                                                                                        zeroFillMissing,
                                                                                        copyBitmap);
                break;
            case eImageBitDepthShort:
                convertToFormatInternalForDepth<unsigned short, unsigned char, 65535, 255>(renderWindow, *this, *dstImg,
                                                                                           srcColorSpace,
                                                                                           dstColorSpace,
                                                                                           channelForAlpha,
                                                                                           zeroFillMissing,
                                                                                           copyBitmap);
                break;
            case eImageBitDepthHalf:
                break;
            case eImageBitDepthFloat:
                convertToFormatInternalForDepth<float, unsigned char, 1, 255>(renderWindow, *this, *dstImg,
                                                                              srcColorSpace,
                                                                              dstColorSpace,
                                                                              channelForAlpha,
                                                                              zeroFillMissing,
                                                                              copyBitmap);

                break;
            case eImageBitDepthNone:
                break;
            }
            break;
        }
        case eImageBitDepthShort: {
            switch ( getBitDepth() ) {
            case eImageBitDepthByte:
                convertToFormatInternalForDepth<unsigned char, unsigned short, 255, 65535>(renderWindow, *this, *dstImg,
                                                                                           srcColorSpace,
                                                                                           dstColorSpace,
                                                                                           channelForAlpha,
                                                                                           zeroFillMissing,
                                                                                           copyBitmap);

                break;
            case eImageBitDepthShort:
                convertToFormatInternalForDepth<unsigned short, unsigned short, 65535, 65535>(renderWindow, *this, *dstImg,
                                                                                              srcColorSpace,
                                                                                              dstColorSpace,
                                                                                              channelForAlpha,
                                                                                              zeroFillMissing,
                                                                                              copyBitmap);

                break;
            case eImageBitDepthHalf:
                break;
            case eImageBitDepthFloat:
                convertToFormatInternalForDepth<float, unsigned short, 1, 65535>(renderWindow, *this, *dstImg,
                                                                                 srcColorSpace,
                                                                                 dstColorSpace,
                                                                                 channelForAlpha,
                                                                                 zeroFillMissing,
                                                                                 copyBitmap);
                break;
            case eImageBitDepthNone:
                break;
            }
            break;
        }
        case eImageBitDepthHalf:
            break;
        case eImageBitDepthFloat: {
            switch ( getBitDepth() ) {
            case eImageBitDepthByte:
                convertToFormatInternalForDepth<unsigned char, float, 255, 1>(renderWindow, *this, *dstImg,
                                                                              srcColorSpace,
                                                                              dstColorSpace,
                                                                              channelForAlpha,
                                                                              zeroFillMissing,
                                                                              copyBitmap);
                break;
            case eImageBitDepthShort:
                convertToFormatInternalForDepth<unsigned short, float, 65535, 1>(renderWindow, *this, *dstImg,
                                                                                 srcColorSpace,
                                                                                 dstColorSpace,
                                                                                 channelForAlpha,
                                                                                 zeroFillMissing,
                                                                                 copyBitmap);

                break;
            case eImageBitDepthHalf:
                break;
            case eImageBitDepthFloat:
                convertToFormatInternalForDepth<float, float, 1, 1>(renderWindow, *this, *dstImg,
                                                                    srcColorSpace,
                                                                    dstColorSpace,
                                                                    channelForAlpha,
                                                                    zeroFillMissing,
                                                                    copyBitmap);
                break;
            case eImageBitDepthNone:
                break;
            }
            break;
        }

        default:
            break;
        } // switch
    }
} // Image::convertToFormat

NATRON_NAMESPACE_EXIT
