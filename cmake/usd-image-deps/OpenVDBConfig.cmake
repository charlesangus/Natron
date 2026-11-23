# The dev image installs OpenVDB without a CMake package config, but its USD's pxrConfig.cmake asks
# for one (USD's hioOpenVDB links OpenVDB::openvdb). This stands in for it: cmake/NatronUsd.cmake
# points OpenVDB_DIR here.
if(NOT TARGET OpenVDB::openvdb)
    find_library(NATRON_OPENVDB_LIBRARY NAMES openvdb REQUIRED)
    find_path(NATRON_OPENVDB_INCLUDE_DIR NAMES openvdb/openvdb.h REQUIRED)
    # openvdb's half-float use references Imath's symbols; a real OpenVDBConfig carries this
    # dependency, so the stand-in must too or every consumer's link misses the DSO.
    find_package(Imath CONFIG QUIET)
    if(TARGET Imath::Imath)
        set(NATRON_OPENVDB_IMATH Imath::Imath)
    else()
        find_library(NATRON_IMATH_LIBRARY NAMES Imath Imath-3_2 Imath-3_1 REQUIRED)
        set(NATRON_OPENVDB_IMATH "${NATRON_IMATH_LIBRARY}")
    endif()
    add_library(OpenVDB::openvdb SHARED IMPORTED)
    set_target_properties(OpenVDB::openvdb PROPERTIES
        IMPORTED_LOCATION "${NATRON_OPENVDB_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${NATRON_OPENVDB_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES "${NATRON_OPENVDB_IMATH}"
    )
endif()
set(OpenVDB_FOUND TRUE)
