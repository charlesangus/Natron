# ***** BEGIN LICENSE BLOCK *****
# This file is part of Natron <https://natrongithub.github.io/>,
# (C) 2018-2023 The Natron developers
# (C) 2013-2018 INRIA and Alexandre Gauthier-Foichat
#
# Natron is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# Natron is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
# ***** END LICENSE BLOCK *****

file(GLOB NatronSerialization_HEADERS *.h)
file(GLOB NatronSerialization_SOURCES *.cpp)

# BoostXml/ is the one subdirectory allowed to include boost.serialization
# and boost.archive (enforced by tools/ci/check_serialization_layering.sh);
# everything else here is plain data, see Serialization/README.md.
file(GLOB_RECURSE NatronSerializationBoostXml_HEADERS BoostXml/*.h)
file(GLOB_RECURSE NatronSerializationBoostXml_SOURCES BoostXml/*.cpp)
list(APPEND NatronSerialization_HEADERS ${NatronSerializationBoostXml_HEADERS})
list(APPEND NatronSerialization_SOURCES ${NatronSerializationBoostXml_SOURCES})

file(GLOB_RECURSE NatronSerializationBundle_HEADERS Bundle/*.h)
file(GLOB_RECURSE NatronSerializationBundle_SOURCES Bundle/*.cpp)
file(GLOB_RECURSE NatronSerializationYaml_HEADERS Yaml/*.h)
file(GLOB_RECURSE NatronSerializationYaml_SOURCES Yaml/*.cpp)
list(APPEND NatronSerialization_HEADERS ${NatronSerializationBundle_HEADERS} ${NatronSerializationYaml_HEADERS})
list(APPEND NatronSerialization_SOURCES ${NatronSerializationBundle_SOURCES} ${NatronSerializationYaml_SOURCES})

add_library(NatronSerialization STATIC ${NatronSerialization_HEADERS} ${NatronSerialization_SOURCES})

target_include_directories(NatronSerialization
    PUBLIC
        ..
)

# Boost is only ever used by Serialization/BoostXml/ translation units, kept
# PRIVATE so NatronSerialization's own public interface -- what Engine links
# PUBLIC -- depends on the standard library alone.
target_link_libraries(NatronSerialization
    PRIVATE
        Boost::headers
        Boost::serialization
        yaml-cpp::yaml-cpp
)

if(ZLIB_FOUND)
    target_link_libraries(NatronSerialization PRIVATE ZLIB::ZLIB)
    target_compile_definitions(NatronSerialization PRIVATE NATRON_SERIALIZATION_HAVE_ZLIB)
endif()
