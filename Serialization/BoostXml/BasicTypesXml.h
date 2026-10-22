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

#ifndef Serialization_BoostXml_BasicTypesXml_h
#define Serialization_BoostXml_BasicTypesXml_h

#include "Global/Macros.h"

#include "Serialization/BasicSerialization.h"

#if !defined(Q_MOC_RUN) && !defined(SBK_RUN)
GCC_DIAG_UNUSED_LOCAL_TYPEDEFS_OFF
// clang-format off
GCC_DIAG_OFF(unused-parameter)
#include <boost/archive/basic_archive.hpp>
#include <boost/archive/xml_oarchive.hpp>
#include <boost/serialization/base_object.hpp>
#include <boost/serialization/list.hpp>
#include <boost/serialization/nvp.hpp>
#include <boost/serialization/split_free.hpp>
#include <boost/serialization/version.hpp>
#include <boost/serialization/void_cast.hpp>
GCC_DIAG_UNUSED_LOCAL_TYPEDEFS_ON
GCC_DIAG_ON(unused-parameter)
// clang-format on
#endif

// Mirrors FORMAT_SERIALIZATION_CHANGES_TO_RECTD/RECTI in Engine/FormatSerialization.h.
#define NATRON_SERIALIZATION_FORMAT_CHANGES_TO_RECTD 2
#define NATRON_SERIALIZATION_FORMAT_CHANGES_TO_RECTI 3
#define NATRON_SERIALIZATION_FORMAT_VERSION NATRON_SERIALIZATION_FORMAT_CHANGES_TO_RECTI

namespace boost {
namespace serialization {

    template <class Archive>
    void
    serialize(Archive& ar,
              NATRON_NAMESPACE::Serialization::RectISerialization& rect,
              const unsigned int /*version*/)
    {
        ar& ::boost::serialization::make_nvp("Left", rect.x1);
        ar& ::boost::serialization::make_nvp("Bottom", rect.y1);
        ar& ::boost::serialization::make_nvp("Right", rect.x2);
        ar& ::boost::serialization::make_nvp("Top", rect.y2);
    }

    template <class Archive>
    void
    serialize(Archive& ar,
              NATRON_NAMESPACE::Serialization::RectDSerialization& rect,
              const unsigned int /*version*/)
    {
        ar& ::boost::serialization::make_nvp("Left", rect.x1);
        ar& ::boost::serialization::make_nvp("Bottom", rect.y1);
        ar& ::boost::serialization::make_nvp("Right", rect.x2);
        ar& ::boost::serialization::make_nvp("Top", rect.y2);
    }

    template <class Archive>
    void
    serialize(Archive& ar,
              NATRON_NAMESPACE::Serialization::FormatSerialization& format,
              const unsigned int /*version*/)
    {
        ::boost::serialization::void_cast_register<NATRON_NAMESPACE::Serialization::FormatSerialization, NATRON_NAMESPACE::Serialization::RectISerialization>(
            static_cast<NATRON_NAMESPACE::Serialization::FormatSerialization*>(nullptr),
            static_cast<NATRON_NAMESPACE::Serialization::RectISerialization*>(nullptr));
        ar& ::boost::serialization::make_nvp("RectI", ::boost::serialization::base_object<NATRON_NAMESPACE::Serialization::RectISerialization>(format));
        ar& ::boost::serialization::make_nvp("Pixel_aspect_ratio", format.pixelAspectRatio);
        ar& ::boost::serialization::make_nvp("Name", format.name);
    }

    template <class Archive>
    void
    save(Archive& ar,
         const NATRON_NAMESPACE::Serialization::VariantSerialization& variant,
         const unsigned int /*version*/)
    {
        std::string typeTag = variant.typeTag;

        ar& ::boost::serialization::make_nvp("Type", typeTag);
        if (typeTag == "bool") {
            bool v = variant.boolValue;
            ar& ::boost::serialization::make_nvp("Value", v);
        } else if ((typeTag == "int") || (typeTag == "uint")) {
            int v = variant.intValue;
            ar& ::boost::serialization::make_nvp("Value", v);
        } else if (typeTag == "double") {
            double v = variant.doubleValue;
            ar& ::boost::serialization::make_nvp("Value", v);
        } else if (typeTag == "string") {
            std::string v = variant.stringValue;
            ar& ::boost::serialization::make_nvp("Value", v);
        } else if (typeTag == "stringlist") {
            std::list<std::string> v = variant.stringListValue;
            ar& ::boost::serialization::make_nvp("Value", v);
        }
    }

    template <class Archive>
    void
    serialize(Archive& ar,
              NATRON_NAMESPACE::Serialization::VariantSerialization& variant,
              const unsigned int version)
    {
        ::boost::serialization::split_free(ar, variant, version);
    }

} // namespace serialization
} // namespace boost

BOOST_CLASS_VERSION(NATRON_NAMESPACE::Serialization::FormatSerialization, NATRON_SERIALIZATION_FORMAT_VERSION)

#endif // Serialization_BoostXml_BasicTypesXml_h
