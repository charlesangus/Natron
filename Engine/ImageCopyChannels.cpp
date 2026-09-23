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

#include <cassert>
#include <stdexcept>

#include <QDebug>

#include "Engine/OSGLContext.h"
#include "Engine/GLShader.h"


// disable some warnings due to unused parameters
// clang-format off
GCC_DIAG_OFF(unused-parameter)
# if ( ( __GNUC__ * 100) + __GNUC_MINOR__) >= 406
GCC_DIAG_OFF(unused-but-set-variable) // only on gcc >= 4.6
#endif
// clang-format on

NATRON_NAMESPACE_ENTER

#define DOCHANNEL(c) dst_pixels[c] = (!src_pixels || c >= srcNComps) ? 0 : src_pixels[c];

template <typename PIX, int maxValue, int srcNComps, int dstNComps, bool doR, bool doG, bool doB, bool doA>
void
Image::copyUnProcessedChannelsForChannels(const std::bitset<4> processChannels,
                                          const RectI& roi,
                                          const ImagePtr& originalImage)
{
    Q_UNUSED(processChannels); // silence warnings in release version
    assert( ( (doR == !processChannels[0]) || !(dstNComps >= 2) ) &&
            ( (doG == !processChannels[1]) || !(dstNComps >= 2) ) &&
            ( (doB == !processChannels[2]) || !(dstNComps >= 3) ) &&
            ( (doA == !processChannels[3]) || !(dstNComps == 1 || dstNComps == 4) ) );
    ReadAccess acc( originalImage.get() );
    int dstRowElements = dstNComps * _bounds.width();
    PIX* dst_pixels = (PIX*)pixelAt(roi.x1, roi.y1);
    assert(dst_pixels);

    for ( int y = roi.y1; y < roi.y2; ++y, dst_pixels += (dstRowElements - (roi.x2 - roi.x1) * dstNComps) ) {
        for (int x = roi.x1; x < roi.x2; ++x, dst_pixels += dstNComps) {
            const PIX* src_pixels = originalImage ? (const PIX*)acc.pixelAt(x, y) : 0;
            PIX srcA = src_pixels ? maxValue : 0; /* alpha reads as 1 for anything that has no alpha channel */
            if ( ( (srcNComps == 1) || (srcNComps == 4) ) && src_pixels ) {
#             ifdef DEBUG_NAN
                assert( !std::isnan(src_pixels[srcNComps - 1]) ); // check for NaN
#             endif
                srcA = src_pixels[srcNComps - 1];
            }

            if ( (dstNComps == 1) || (dstNComps == 4) ) {
#             ifdef DEBUG_NAN
                assert(  !std::isnan(dst_pixels[dstNComps - 1]) ); // check for NaN
#endif
            }
            if (doR) {
#             ifdef DEBUG_NAN
                assert(!src_pixels ||  !std::isnan(src_pixels[0]) ); // check for NaN
                assert(  !std::isnan(dst_pixels[0]) ); // check for NaN
#             endif
                DOCHANNEL(0);
#             ifdef DEBUG_NAN
                assert(  !std::isnan(dst_pixels[0]) ); // check for NaN
#             endif
            }
            if (doG) {
#             ifdef DEBUG_NAN
                assert(!src_pixels ||  !std::isnan(src_pixels[1]) ); // check for NaN
                assert( !std::isnan(dst_pixels[1]) ); // check for NaN
#             endif
                DOCHANNEL(1);
#             ifdef DEBUG_NAN
                assert( !std::isnan(dst_pixels[1]) ); // check for NaN
#             endif
            }
            if (doB) {
#             ifdef DEBUG_NAN
                assert(!src_pixels ||  !std::isnan(src_pixels[2]) ); // check for NaN
                assert( !std::isnan(dst_pixels[2]) ); // check for NaN
#             endif
                DOCHANNEL(2);
#             ifdef DEBUG_NAN
                assert( !std::isnan(dst_pixels[2]) ); // check for NaN
#             endif
            }
            if (doA) {
                if ( (dstNComps == 1) || (dstNComps == 4) ) {
                    dst_pixels[dstNComps - 1] = srcA;
#                 ifdef DEBUG_NAN
                    assert( !std::isnan(dst_pixels[dstNComps - 1]) ); // check for NaN
#                 endif
                }
            }
        }
    }
} // Image::copyUnProcessedChannelsForChannels

template <typename PIX, int maxValue, int srcNComps, int dstNComps>
void
Image::copyUnProcessedChannelsForChannels(const std::bitset<4> processChannels,
                                          const RectI& roi,
                                          const ImagePtr& originalImage)
{
    ReadAccess acc( originalImage.get() );
    int dstRowElements = dstNComps * _bounds.width();
    PIX* dst_pixels = (PIX*)pixelAt(roi.x1, roi.y1);

    assert(dst_pixels);
    const bool doR = !processChannels[0] && (dstNComps >= 2);
    const bool doG = !processChannels[1] && (dstNComps >= 2);
    const bool doB = !processChannels[2] && (dstNComps >= 3);
    const bool doA = !processChannels[3] && (dstNComps == 1 || dstNComps == 4);

    for ( int y = roi.y1; y < roi.y2; ++y, dst_pixels += (dstRowElements - (roi.x2 - roi.x1) * dstNComps) ) {
        for (int x = roi.x1; x < roi.x2; ++x, dst_pixels += dstNComps) {
            const PIX* src_pixels = originalImage ? (const PIX*)acc.pixelAt(x, y) : 0;
            PIX srcA = src_pixels ? maxValue : 0; /* alpha reads as 1 for anything that has no alpha channel */
            if ( ( (srcNComps == 1) || (srcNComps == 4) ) && src_pixels ) {
#             ifdef DEBUG_NAN
                assert( !std::isnan(src_pixels[srcNComps - 1]) ); // check for NaN
#             endif
                srcA = src_pixels[srcNComps - 1];
            }
            if ( (dstNComps == 1) || (dstNComps == 4) ) {
#             ifdef DEBUG_NAN
                assert(!std::isnan(dst_pixels[dstNComps - 1])); // check for NaN
#             endif
            }
            if (doR) {
#             ifdef DEBUG_NAN
                assert(!src_pixels || !std::isnan(src_pixels[0]) ); // check for NaN
                assert( !std::isnan(dst_pixels[0]) ); // check for NaN
#             endif
                DOCHANNEL(0);
#             ifdef DEBUG_NAN
                assert( !std::isnan(dst_pixels[0]) ); // check for NaN
#             endif
            }
            if (doG) {
#             ifdef DEBUG_NAN
                assert(!src_pixels || !std::isnan(src_pixels[1]) ); // check for NaN
                assert( !std::isnan(dst_pixels[1]) ); // check for NaN
#             endif
                DOCHANNEL(1);
#             ifdef DEBUG_NAN
                assert( !std::isnan(dst_pixels[1]) ); // check for NaN
#             endif
            }
            if (doB) {
#             ifdef DEBUG_NAN
                assert(!src_pixels || !std::isnan(src_pixels[2]) ); // check for NaN
                assert( !std::isnan(dst_pixels[2]) ); // check for NaN
#             endif
                DOCHANNEL(2);
#             ifdef DEBUG_NAN
                assert( !std::isnan(dst_pixels[2]) ); // check for NaN
#             endif
            }
            if (doA) {
                // coverity[dead_error_line]
                dst_pixels[dstNComps - 1] = srcA;
#              ifdef DEBUG_NAN
                assert( !std::isnan(dst_pixels[dstNComps - 1]) ); // check for NaN
#              endif
            }
        }
    }
} // Image::copyUnProcessedChannelsForChannels

#undef DOCHANNEL

template <typename PIX, int maxValue, int srcNComps, int dstNComps>
void
Image::copyUnProcessedChannelsForComponents(const RectI& roi,
                                            const std::bitset<4> processChannels,
                                            const ImagePtr& originalImage)
{
    const bool doR = !processChannels[0] && (dstNComps >= 2);
    const bool doG = !processChannels[1] && (dstNComps >= 2);
    const bool doB = !processChannels[2] && (dstNComps >= 3);
    const bool doA = !processChannels[3] && (dstNComps == 1 || dstNComps == 4);

    if (dstNComps == 1) {
        if (doA) {
            copyUnProcessedChannelsForChannels<PIX, maxValue, srcNComps, dstNComps, false, false, false, true>(processChannels, roi, originalImage); // RGB were processed, copy A
        } else {
            copyUnProcessedChannelsForChannels<PIX, maxValue, srcNComps, dstNComps, false, false, false, false>(processChannels, roi, originalImage); // RGBA were processed
        }
    } else {
        assert(2 <= dstNComps && dstNComps <= 4);
        if (doR) {
            if (doG) {
                if ( (dstNComps >= 3) && doB ) {
                    if ( (dstNComps >= 4) && doA ) {
                        copyUnProcessedChannelsForChannels<PIX, maxValue, srcNComps, dstNComps, true, true, true, true>(processChannels, roi, originalImage); // none were processed
                    } else {
                        copyUnProcessedChannelsForChannels<PIX, maxValue, srcNComps, dstNComps, true, true, true, false>(processChannels, roi, originalImage); // A was processed
                    }
                } else {
                    if ( (dstNComps >= 4) && doA ) {
                        copyUnProcessedChannelsForChannels<PIX, maxValue, srcNComps, dstNComps, true, true, false, true>(processChannels, roi, originalImage); // B was processed
                    } else {
                        copyUnProcessedChannelsForChannels<PIX, maxValue, srcNComps, dstNComps>(processChannels, roi, originalImage); // BA were processed (rare)
                    }
                }
            } else {
                if ( (dstNComps >= 3) && doB ) {
                    if ( (dstNComps >= 4) && doA ) {
                        copyUnProcessedChannelsForChannels<PIX, maxValue, srcNComps, dstNComps, true, false, true, true>(processChannels, roi, originalImage); // G was processed
                    } else {
                        copyUnProcessedChannelsForChannels<PIX, maxValue, srcNComps, dstNComps>(processChannels, roi, originalImage); // GA were processed (rare)
                    }
                } else {
                    copyUnProcessedChannelsForChannels<PIX, maxValue, srcNComps, dstNComps>(processChannels, roi, originalImage); // GB or GBA were processed (rare)
                }
            }
        } else {
            if (doG) {
                if ( (dstNComps >= 3) && doB ) {
                    if ( (dstNComps >= 4) && doA ) {
                        copyUnProcessedChannelsForChannels<PIX, maxValue, srcNComps, dstNComps, false, true, true, true>(processChannels, roi, originalImage); // R was processed
                    } else {
                        copyUnProcessedChannelsForChannels<PIX, maxValue, srcNComps, dstNComps>(processChannels, roi, originalImage); // RA were processed (rare)
                    }
                } else {
                    copyUnProcessedChannelsForChannels<PIX, maxValue, srcNComps, dstNComps>(processChannels, roi, originalImage); // RB or RBA were processed (rare)
                }
            } else {
                if ( (dstNComps >= 3) && doB ) {
                    copyUnProcessedChannelsForChannels<PIX, maxValue, srcNComps, dstNComps>(processChannels, roi, originalImage); // RG or RGA were processed (rare)
                } else {
                    if ( (dstNComps >= 4) && doA ) {
                        copyUnProcessedChannelsForChannels<PIX, maxValue, srcNComps, dstNComps, false, false, false, true>(processChannels, roi, originalImage); // RGB were processed
                    } else {
                        copyUnProcessedChannelsForChannels<PIX, maxValue, srcNComps, dstNComps, false, false, false, false>(processChannels, roi, originalImage); // RGBA were processed
                    }
                }
            }
        }
    }
} // Image::copyUnProcessedChannelsForComponents

template <typename PIX, int maxValue>
void
Image::copyUnProcessedChannelsForDepth(const RectI& roi,
                                       const std::bitset<4> processChannels,
                                       const ImagePtr& originalImage)
{
    int dstNComps = getComponents().getNumComponents();
    int srcNComps = originalImage ? originalImage->getComponents().getNumComponents() : 0;

    switch (dstNComps) {
    case 1:
        switch (srcNComps) {
        case 0:
            copyUnProcessedChannelsForComponents<PIX, maxValue, 0, 1>(roi, processChannels, originalImage);
            break;
        case 1:
            copyUnProcessedChannelsForComponents<PIX, maxValue, 1, 1>(roi, processChannels, originalImage);
            break;
        case 2:
            copyUnProcessedChannelsForComponents<PIX, maxValue, 2, 1>(roi, processChannels, originalImage);
            break;
        case 3:
            copyUnProcessedChannelsForComponents<PIX, maxValue, 3, 1>(roi, processChannels, originalImage);
            break;
        case 4:
            copyUnProcessedChannelsForComponents<PIX, maxValue, 4, 1>(roi, processChannels, originalImage);
            break;
        default:
            assert(false);
            break;
        }
        break;
    case 2:
        switch (srcNComps) {
        case 0:
            copyUnProcessedChannelsForComponents<PIX, maxValue, 0, 2>(roi, processChannels, originalImage);
            break;
        case 1:
            copyUnProcessedChannelsForComponents<PIX, maxValue, 1, 2>(roi, processChannels, originalImage);
            break;
        case 2:
            copyUnProcessedChannelsForComponents<PIX, maxValue, 2, 2>(roi, processChannels, originalImage);
            break;
        case 3:
            copyUnProcessedChannelsForComponents<PIX, maxValue, 3, 2>(roi, processChannels, originalImage);
            break;
        case 4:
            copyUnProcessedChannelsForComponents<PIX, maxValue, 4, 2>(roi, processChannels, originalImage);
            break;
        default:
            assert(false);
            break;
        }
        break;
    case 3:
        switch (srcNComps) {
        case 0:
            copyUnProcessedChannelsForComponents<PIX, maxValue, 0, 3>(roi, processChannels, originalImage);
            break;
        case 1:
            copyUnProcessedChannelsForComponents<PIX, maxValue, 1, 3>(roi, processChannels, originalImage);
            break;
        case 2:
            copyUnProcessedChannelsForComponents<PIX, maxValue, 2, 3>(roi, processChannels, originalImage);
            break;
        case 3:
            copyUnProcessedChannelsForComponents<PIX, maxValue, 3, 3>(roi, processChannels, originalImage);
            break;
        case 4:
            copyUnProcessedChannelsForComponents<PIX, maxValue, 4, 3>(roi, processChannels, originalImage);
            break;
        default:
            assert(false);
            break;
        }
        break;
    case 4:
        switch (srcNComps) {
        case 0:
            copyUnProcessedChannelsForComponents<PIX, maxValue, 0, 4>(roi, processChannels, originalImage);
            break;
        case 1:
            copyUnProcessedChannelsForComponents<PIX, maxValue, 1, 4>(roi, processChannels, originalImage);
            break;
        case 2:
            copyUnProcessedChannelsForComponents<PIX, maxValue, 2, 4>(roi, processChannels, originalImage);
            break;
        case 3:
            copyUnProcessedChannelsForComponents<PIX, maxValue, 3, 4>(roi, processChannels, originalImage);
            break;
        case 4:
            copyUnProcessedChannelsForComponents<PIX, maxValue, 4, 4>(roi, processChannels, originalImage);
            break;
        default:
            assert(false);
            break;
        }
        break;

    default:
        assert(false);
        break;
    } // switch
} // Image::copyUnProcessedChannelsForDepth

bool
Image::canCallCopyUnProcessedChannels(const std::bitset<4> processChannels) const
{
    int numComp = getComponents().getNumComponents();

    if (numComp == 0) {
        return false;
    }
    if ( (numComp == 1) && processChannels[3] ) { // 1 component is alpha
        return false;
    } else if ( (numComp == 2) && processChannels[0] && processChannels[1] ) {
        return false;
    } else if ( (numComp == 3) && processChannels[0] && processChannels[1] && processChannels[2] ) {
        return false;
    } else if ( (numComp == 4) && processChannels[0] && processChannels[1] && processChannels[2] && processChannels[3] ) {
        return false;
    }

    return true;
}

void
Image::copyUnProcessedChannels(const RectI& roi,
                               const std::bitset<4> processChannels,
                               const ImagePtr& originalImage,
                               const OSGLContextPtr& glContext)
{
    int numComp = getComponents().getNumComponents();

    if (numComp == 0) {
        return;
    }
    if ( (numComp == 1) && processChannels[3] ) { // 1 component is alpha
        return;
    } else if ( (numComp == 2) && processChannels[0] && processChannels[1] ) {
        return;
    } else if ( (numComp == 3) && processChannels[0] && processChannels[1] && processChannels[2] ) {
        return;
    } else if ( (numComp == 4) && processChannels[0] && processChannels[1] && processChannels[2] && processChannels[3] ) {
        return;
    }


    if ( originalImage && ( getMipmapLevel() != originalImage->getMipmapLevel() ) ) {
        qDebug() << "WARNING: attempting to call copyUnProcessedChannels on images with different mipmapLevel";

        return;
    }

    QWriteLocker k(&_entryLock);
    assert( !originalImage || getBitDepth() == originalImage->getBitDepth() );


    const RectI srcRoi = roi.intersect(_bounds);

    if (getStorageMode() == eStorageModeGLTex) {
        assert(glContext);
        assert(originalImage->getStorageMode() == eStorageModeGLTex);
        GLShaderPtr shader = glContext->getOrCreateCopyUnprocessedChannelsShader(processChannels[0], processChannels[1], processChannels[2], processChannels[3]);
        assert(shader);
        GLuint fboID = glContext->getFBOId();

        glBindFramebuffer(GL_FRAMEBUFFER, fboID);
        int target = getGLTextureTarget();
        glEnable(target);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture( target, getGLTextureID() );

        glTexParameteri (target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri (target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        glTexParameteri (target, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri (target, GL_TEXTURE_WRAP_T, GL_REPEAT);

        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, target, getGLTextureID(), 0 /*LoD*/);
        glCheckFramebufferError();
        glCheckError();
        glActiveTexture(GL_TEXTURE1);
        glBindTexture( target, originalImage->getGLTextureID() );

        glTexParameteri (target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri (target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        glTexParameteri (target, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri (target, GL_TEXTURE_WRAP_T, GL_REPEAT);


        shader->bind();
        shader->setUniform("originalImageTex", 1);
        shader->setUniform("outputImageTex", 0);
        OfxRGBAColourF procChannelsV = {
            processChannels[0] ? 1.f : 0.f,
            processChannels[1] ? 1.f : 0.f,
            processChannels[2] ? 1.f : 0.f,
            processChannels[3] ? 1.f : 0.f
        };
        shader->setUniform("processChannels", procChannelsV);
        applyTextureMapping(_bounds, srcRoi);
        shader->unbind();

        glCheckError();
        glBindTexture(target, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(target, 0);
        glCheckError();

        return;
    }

    switch ( getBitDepth() ) {
    case eImageBitDepthByte:
        copyUnProcessedChannelsForDepth<unsigned char, 255>(roi, processChannels, originalImage);
        break;
    case eImageBitDepthShort:
        copyUnProcessedChannelsForDepth<unsigned short, 65535>(roi, processChannels, originalImage);
        break;
    case eImageBitDepthFloat:
        copyUnProcessedChannelsForDepth<float, 1>(roi, processChannels, originalImage);
        break;
    default:

        return;
    }
} // copyUnProcessedChannels

template <typename PIX>
void
Image::extractChannelsForDepth(const std::vector<int>& channelIndices,
                               Image* output) const
{
    const int srcNComps = getComponents().getNumComponents();
    const int dstNComps = (int)channelIndices.size();

    for (int y = _bounds.y1; y < _bounds.y2; ++y) {
        const PIX* src_pixels = (const PIX*)pixelAt(_bounds.x1, y);
        PIX* dst_pixels = (PIX*)output->pixelAt(_bounds.x1, y);
        assert(src_pixels && dst_pixels);
        for (int x = _bounds.x1; x < _bounds.x2; ++x, src_pixels += srcNComps, dst_pixels += dstNComps) {
            for (int c = 0; c < dstNComps; ++c) {
                dst_pixels[c] = src_pixels[channelIndices[c]];
            }
        }
    }
} // Image::extractChannelsForDepth

ImagePtr
Image::extractChannels(const std::vector<int>& channelIndices) const
{
    if (getStorageMode() == eStorageModeGLTex) {
        return ImagePtr();
    }

    const ImageLayerDesc& layer = getComponents();
    std::vector<std::string> channels;
    for (std::size_t i = 0; i < channelIndices.size(); ++i) {
        if (channelIndices[i] < 0 || channelIndices[i] >= layer.getNumComponents()) {
            return ImagePtr();
        }
        channels.push_back(layer.getChannels()[channelIndices[i]]);
    }
    if (channels.empty()) {
        return ImagePtr();
    }
    ImageLayerDesc subset(layer.getLayerID(), layer.getLayerLabel(), std::string(), channels);

    ReadAccess acc(this);
    ImagePtr output = std::make_shared<Image>(subset,
                                              getRoD(),
                                              _bounds,
                                              getMipmapLevel(),
                                              getPixelAspectRatio(),
                                              getBitDepth(),
                                              getFieldingOrder(),
                                              false);
    output->setKey(getKey());

    switch (getBitDepth()) {
    case eImageBitDepthByte:
        extractChannelsForDepth<unsigned char>(channelIndices, output.get());
        break;
    case eImageBitDepthShort:
        extractChannelsForDepth<unsigned short>(channelIndices, output.get());
        break;
    case eImageBitDepthFloat:
        extractChannelsForDepth<float>(channelIndices, output.get());
        break;
    default:

        return ImagePtr();
    }

    return output;
} // Image::extractChannels

NATRON_NAMESPACE_EXIT
