# The dev image installs OpenVDB without a CMake package config, but its USD's pxrConfig.cmake asks
# for one (USD's hioOpenVDB links OpenVDB::openvdb). This stands in for it: cmake/NatronUsd.cmake
# points OpenVDB_DIR here.
if(NOT TARGET OpenVDB::openvdb)
    find_library(NATRON_OPENVDB_LIBRARY NAMES openvdb REQUIRED)
    find_path(NATRON_OPENVDB_INCLUDE_DIR NAMES openvdb/openvdb.h REQUIRED)
    add_library(OpenVDB::openvdb SHARED IMPORTED)
    set_target_properties(OpenVDB::openvdb PROPERTIES
        IMPORTED_LOCATION "${NATRON_OPENVDB_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${NATRON_OPENVDB_INCLUDE_DIR}"
    )
endif()
set(OpenVDB_FOUND TRUE)
