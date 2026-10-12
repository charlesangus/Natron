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

#ifndef Engine_DeviceImage_h
#define Engine_DeviceImage_h

#include "Global/Macros.h"

#include <cstddef>
#include <memory>

#include "Engine/EngineFwd.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief An image whose pixels live in device memory, so that it can be passed between device tasks without going
 * through RAM. Implementations hide the graphics API from the engine.
 **/
class DeviceImage {
public:
    virtual ~DeviceImage() { }

    /**
     * @brief The device memory the image occupies.
     **/
    virtual std::size_t bytes() const = 0;

    virtual RectI bounds() const = 0;

    virtual int nComps() const = 0;

    /**
     * @brief A new RAM copy of the pixels. The device pixels stay valid. Returns null on failure.
     **/
    virtual ImagePtr downloadToRam() = 0;

    /**
     * @brief Like downloadToRam(), then frees the device memory: bytes() is 0 afterwards and the image must not be
     * used for anything else. Returns null on failure, in which case the device pixels stay valid.
     **/
    virtual ImagePtr evictToRam() = 0;
};

typedef std::shared_ptr<DeviceImage> DeviceImagePtr;

NATRON_NAMESPACE_EXIT

#endif // Engine_DeviceImage_h
