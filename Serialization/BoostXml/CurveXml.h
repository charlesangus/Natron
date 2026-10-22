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

#ifndef Serialization_BoostXml_CurveXml_h
#define Serialization_BoostXml_CurveXml_h

#include "Global/Macros.h"

#include "Serialization/CurveSerialization.h"

#if !defined(Q_MOC_RUN) && !defined(SBK_RUN)
GCC_DIAG_UNUSED_LOCAL_TYPEDEFS_OFF
// clang-format off
GCC_DIAG_OFF(unused-parameter)
// /opt/local/include/boost/serialization/smart_cast.hpp:254:25: warning: unused parameter 'u' [-Wunused-parameter]
#include <boost/archive/xml_oarchive.hpp>
#include <boost/serialization/nvp.hpp>
#include <boost/serialization/vector.hpp>
GCC_DIAG_UNUSED_LOCAL_TYPEDEFS_ON
GCC_DIAG_ON(unused-parameter)
// clang-format on
#endif

// Non-intrusive adapters for CurveSerialization/KeyFrameSerialization: the
// same NVP names, nesting and (unset, so default 0) class versions that
// Curve::serialize/KeyFrame::serialize used to emit, so the .ntp bytes are
// unchanged even though these plain-data types carry no intrusive serialize.
namespace boost {
namespace serialization {

    template <class Archive>
    void
    serialize(Archive& ar,
              NATRON_NAMESPACE::Serialization::KeyFrameSerialization& keyframe,
              const unsigned int /*version*/)
    {
        ar& ::boost::serialization::make_nvp("Time", keyframe.time);
        ar& ::boost::serialization::make_nvp("Value", keyframe.value);
        ar& ::boost::serialization::make_nvp("InterpolationMethod", keyframe.interpolation);
        ar& ::boost::serialization::make_nvp("LeftDerivative", keyframe.leftDerivative);
        ar& ::boost::serialization::make_nvp("RightDerivative", keyframe.rightDerivative);
    }

    template <class Archive>
    void
    serialize(Archive& ar,
              NATRON_NAMESPACE::Serialization::CurveSerialization& curve,
              const unsigned int /*version*/)
    {
        ar& ::boost::serialization::make_nvp("KeyFrameSet", curve.keyframes);
    }

} // namespace serialization
} // namespace boost

#endif // Serialization_BoostXml_CurveXml_h
