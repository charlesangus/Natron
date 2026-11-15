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

#include "ImageKey.h"

#include <cassert>
#include <stdexcept>

#include "Engine/ImageLayerDesc.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

namespace {

// ImageLayerDesc::operator== ignores channel names, and a flattened deep layer can be rgb or xyz under
// one layer ID, so the channels are part of the identity. A colour view holds the same pixels as the
// storage plane with its channels, hence the one ID.
std::string
makeLayerIdentity(const ImageLayerDesc& layer)
{
    std::string identity = layer.isColorLayer() ? std::string(kNatronColorLayerID) : layer.getLayerID();

    for (const std::string& channel : layer.getChannels()) {
        identity.push_back('\n');
        identity += channel;
    }

    return identity;
}

} // anonymous namespace

ImageKey::ImageKey()
    : KeyHelper<U64>()
    , _nodeHashKey(0)
    , _time(0)
    , _pixelAspect(1)
    , _view(0)
    , _draftMode(false)
    , _frameVaryingOrAnimated(false)
    , _fullScaleWithDownscaleInputs(false)
    , _mipmapLevel(0)
    , _layer()
    , _bitDepth(eImageBitDepthNone)
{
}

ImageKey::ImageKey(const CacheEntryHolder* holder,
                   U64 nodeHashKey,
                   bool frameVaryingOrAnimated,
                   double time,
                   ViewIdx view,
                   double pixelAspect,
                   bool draftMode,
                   bool fullScaleWithDownscaleInputs)
    : KeyHelper<U64>(holder)
    , _nodeHashKey(nodeHashKey)
    , _time(time)
    , _pixelAspect(pixelAspect)
    , _view(view)
    , _draftMode(draftMode)
    , _frameVaryingOrAnimated(frameVaryingOrAnimated)
    , _fullScaleWithDownscaleInputs(fullScaleWithDownscaleInputs)
    , _mipmapLevel(0)
    , _layer()
    , _bitDepth(eImageBitDepthNone)
{
}

ImageKey
ImageKey::forEntry(unsigned int mipmapLevel,
                   const ImageLayerDesc& layer,
                   ImageBitDepthEnum bitdepth) const
{
    ImageKey key(*this);

    key._mipmapLevel = mipmapLevel;
    key._layer = makeLayerIdentity(layer);
    key._bitDepth = (int)bitdepth;
    key.resetHash();

    return key;
}

ImageKey
ImageKey::nonDraft() const
{
    ImageKey key(*this);

    key._draftMode = false;
    key.resetHash();

    return key;
}

const ImageLayerDesc*
ImageKey::cachedLayerForRequest(const ImageLayerDesc& requested,
                                const std::list<ImageLayerDesc>& outputComponents)
{
    if (!requested.isColorLayer()) {
        return &requested;
    }
    for (const ImageLayerDesc& output : outputComponents) {
        if (output.isColorLayer()) {
            return &output;
        }
    }

    return nullptr;
}

bool
ImageKey::forEachServingKey(unsigned int coarsestMipmapLevel,
                            unsigned int finestMipmapLevel,
                            const ImageLayerDesc& layer,
                            ImageBitDepthEnum bitdepth,
                            const std::function<bool(const ImageKey&)>& visitor) const
{
    // Short comes before Half because a Half image holds a Short's range and precision well enough
    // for a Short request, whereas a Short image has clamped what a Half request may expect.
    static const ImageBitDepthEnum depths[] = { eImageBitDepthByte, eImageBitDepthShort, eImageBitDepthHalf, eImageBitDepthFloat };
    const int nDepths = sizeof(depths) / sizeof(depths[0]);
    int firstDepth = 0;
    while ((firstDepth < nDepths - 1) && (depths[firstDepth] != bitdepth)) {
        ++firstDepth;
    }
    if (bitdepth == eImageBitDepthNone) {
        firstDepth = 0;
    }

    // Only RGBA holds every channel of RGB and of Alpha. A 2-channel colour layout has no superset, and
    // the other colour pairs would have the conversion make up channels the entry doesn't have.
    const int nComps = layer.getNumComponents();
    const bool probeRGBA = layer.isColorLayer() && ((nComps == 1) || (nComps == 3));
    const ImageLayerDesc* layers[] = { &layer, probeRGBA ? &ImageLayerDesc::getRGBAComponents() : nullptr };

    for (unsigned int mipmapLevel = coarsestMipmapLevel;; --mipmapLevel) {
        for (const ImageLayerDesc* candidate : layers) {
            if (!candidate) {
                continue;
            }
            for (int d = firstDepth; d < nDepths; ++d) {
                if (visitor(forEntry(mipmapLevel, *candidate, depths[d]))) {
                    return true;
                }
            }
        }
        if (mipmapLevel <= finestMipmapLevel) {
            return false;
        }
    }
}

void
ImageKey::fillHash(Hash64* hash) const
{
    hash->append(_nodeHashKey);
    if (_frameVaryingOrAnimated) {
        hash->append(_time);
    }
    hash->append(_view);
    hash->append(_pixelAspect);
    hash->append(_draftMode);
    hash->append(_fullScaleWithDownscaleInputs);
    hash->append(_mipmapLevel);
    hash->append(_bitDepth);
    for (const char c : _layer) {
        hash->append<unsigned char>((unsigned char)c);
    }
}

bool
ImageKey::operator==(const ImageKey & other) const
{
    if (_frameVaryingOrAnimated && (_time != other._time)) {
        return false;
    }

    return _nodeHashKey == other._nodeHashKey && _view == other._view && _pixelAspect == other._pixelAspect && _draftMode == other._draftMode && _fullScaleWithDownscaleInputs == other._fullScaleWithDownscaleInputs && _mipmapLevel == other._mipmapLevel && _bitDepth == other._bitDepth && _layer == other._layer;
}

NATRON_NAMESPACE_EXIT
