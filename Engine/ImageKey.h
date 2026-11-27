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

#ifndef IMAGEKEY_H
#define IMAGEKEY_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <list>
#include <string>

#include "Global/Enums.h"

#include "Engine/KeyHelper.h"
#include "Engine/ViewIdx.h"
#include "Engine/EngineFwd.h"

NATRON_NAMESPACE_ENTER

class ImageKey
    :  public KeyHelper<U64>
{
public:

    U64 _nodeHashKey;
    double _time;
    double _pixelAspect;
    int /*ViewIdx*/ _view; // store it locally as an int for easier serialization
    bool _draftMode;
    bool _frameVaryingOrAnimated;

    //When true that means the image has been computed based on inputs using a mipmaplevel != 0
    //hence it is probably not very high quality, even though the mipmap level is 0
    bool _fullScaleWithDownscaleInputs;

    unsigned int _mipmapLevel;
    std::string _layer;
    int /*ImageBitDepthEnum*/ _bitDepth; // store it locally as an int for easier serialization

    ImageKey();

    ImageKey(const CacheEntryHolder* holder,
             U64 nodeHashKey,
             bool frameVaryingOrAnimated,
             double time,
             ViewIdx view,
             double pixelAspect,
             bool draftMode,
             bool fullScaleWithDownscaleInputs);

    /**
     * @brief This key for the one entry of the given level, layer and depth. The constructor leaves
     * those three blank; an Image completes its key from its own params, so a key with them blank
     * addresses nothing in the cache.
     **/
    ImageKey forEntry(unsigned int mipmapLevel, const ImageLayerDesc& layer, ImageBitDepthEnum bitdepth) const;

    /**
     * @brief This key with draft mode off. A full-quality entry answers a draft request as well as a draft one would,
     * so a draft lookup tries it first.
     **/
    ImageKey nonDraft() const;

    /**
     * @brief The layer a request is cached under: a colour request shares the node's first colour
     * output whichever colour layout it asked for, any other layer is itself. Null if the node has
     * no colour output for a colour request.
     **/
    static const ImageLayerDesc* cachedLayerForRequest(const ImageLayerDesc& requested, const std::list<ImageLayerDesc>& outputComponents);

    /**
     * @brief Calls visitor with each key whose entry can answer a request, best first, until it
     * returns true, and returns whether it did. Levels run from coarsestMipmapLevel down to
     * finestMipmapLevel; within a level, layer before the RGBA that holds its channels, and within
     * a layer, bitdepth before deeper ones.
     *
     * A miss costs one lookup per combination: at most 2 layers and 4 depths per level.
     **/
    bool forEachServingKey(unsigned int coarsestMipmapLevel,
                           unsigned int finestMipmapLevel,
                           const ImageLayerDesc& layer,
                           ImageBitDepthEnum bitdepth,
                           const std::function<bool(const ImageKey&)>& visitor) const;

    void fillHash(Hash64* hash) const;

    U64 getTreeVersion() const
    {
        return _nodeHashKey;
    }

    bool operator==(const ImageKey & other) const;

    double getTime() const
    {
        return _time;
    }

    ViewIdx getView() const
    {
        return ViewIdx(_view);
    }

    template<class Archive>
    void serialize(Archive & ar, const unsigned int version);
};

NATRON_NAMESPACE_EXIT

#endif // IMAGEKEY_H
