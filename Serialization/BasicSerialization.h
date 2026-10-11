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

#ifndef Serialization_BasicSerialization_h
#define Serialization_BasicSerialization_h

#include "Global/Macros.h"

#include <list>
#include <string>

#include "Serialization/SerializationFwd.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

/**
 * @brief Plain-data equivalent of RectI. Engine's RectI/RectD/Format keep their
 * own intrusive serialize (see RectI.h, RectD.h, Format.h) because the disk-cache
 * TOC (FrameKey.h, ImageKey.h, ImageParams.h) archives them directly; this type
 * is the plain-data shape other, non-cache call sites can use instead.
 **/
struct RectISerialization
{
    int x1, y1, x2, y2;

    RectISerialization()
        : x1(0)
        , y1(0)
        , x2(0)
        , y2(0)
    {
    }
};

/**
 * @brief Plain-data equivalent of RectD, see RectISerialization.
 **/
struct RectDSerialization
{
    double x1, y1, x2, y2;

    RectDSerialization()
        : x1(0.)
        , y1(0.)
        , x2(0.)
        , y2(0.)
    {
    }
};

/**
 * @brief Plain-data equivalent of Format: a RectI plus a pixel aspect ratio and
 * an optional display name, see RectISerialization.
 **/
struct FormatSerialization
    : public RectISerialization
{
    double pixelAspectRatio;
    std::string name;

    FormatSerialization()
        : RectISerialization()
        , pixelAspectRatio(1.)
        , name()
    {
    }
};

/**
 * @brief Plain-data equivalent of Variant: the handful of value types a Variant
 * may hold on disk, tagged the same way Variant::save/load tags them ("bool",
 * "int", "uint", "double", "string", "stringlist" or "null"). Variant is not
 * currently archived by any call site (see Engine/VariantSerialization.h), so
 * this type has no consumer yet either; it exists so a future one does not have
 * to depend on Engine/Variant.h, which pulls in QVariant.
 **/
struct VariantSerialization
{
    std::string typeTag;
    bool boolValue;
    int intValue;
    double doubleValue;
    std::string stringValue;
    std::list<std::string> stringListValue;

    VariantSerialization()
        : typeTag("null")
        , boolValue(false)
        , intValue(0)
        , doubleValue(0.)
        , stringValue()
        , stringListValue()
    {
    }
};

} // namespace Serialization
NATRON_NAMESPACE_EXIT

#endif // Serialization_BasicSerialization_h
