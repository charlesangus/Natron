# Finds OpenUSD and checks it is one Natron can load.
#
# USD is not built here: the dev image's base, aswf/ci-vfxall (pinned in tools/ci/local/Dockerfile),
# installs it under /usr/local -- shared libraries (libusd_*.so, not monolithic), Python bindings
# built against the image's one Python, the Alembic file-format plug-in (usdAbc), Storm and HgiGL.
# Its package config sits at the prefix root (/usr/local/pxrConfig.cmake) and exports
# un-namespaced targets (usd, sdf, tf, usdImagingGL, ...).
#
# Must be included after find_package(Python3): USD's bindings are only loadable by the Python they
# were built against, and Natron embeds that interpreter, so the two have to be the same minor
# version. pxrConfig.cmake itself only checks that when Python3 has not been found yet.

set(NATRON_USD_MIN_VERSION 2411)

# The image's USD was built by Conan, and its pxrConfig.cmake still points each dependency's
# <Pkg>_DIR at the Conan build tree, which the image does not keep. Point them at the image's own
# installs instead (pxrConfig.cmake only sets them when undefined), and stand in for OpenVDB, which
# the image installs without a package config.
find_path(NATRON_USD_PREFIX NAMES pxrConfig.cmake PATHS ${CMAKE_PREFIX_PATH} ${CMAKE_SYSTEM_PREFIX_PATH} NO_DEFAULT_PATH)
if(NOT NATRON_USD_PREFIX)
    message(FATAL_ERROR "OpenUSD not found: no pxrConfig.cmake under CMAKE_PREFIX_PATH or the system prefixes. The dev image (tools/ci/local/Dockerfile) provides it at /usr/local.")
endif()
foreach(_natron_usd_dep Imath MaterialX OpenColorIO OpenSubdiv)
    if(NOT DEFINED ${_natron_usd_dep}_DIR AND EXISTS "${NATRON_USD_PREFIX}/lib/cmake/${_natron_usd_dep}")
        set(${_natron_usd_dep}_DIR "${NATRON_USD_PREFIX}/lib/cmake/${_natron_usd_dep}")
    endif()
endforeach()
unset(_natron_usd_dep)
if(NOT DEFINED PTex_DIR AND EXISTS "${NATRON_USD_PREFIX}/lib/cmake/Ptex")
    set(PTex_DIR "${NATRON_USD_PREFIX}/lib/cmake/Ptex")
endif()
if(NOT DEFINED OpenVDB_DIR)
    set(OpenVDB_DIR "${CMAKE_CURRENT_LIST_DIR}/usd-image-deps")
endif()

# USD's garch links OpenGL::GL without finding OpenGL itself.
find_package(OpenGL REQUIRED)
find_package(pxr CONFIG REQUIRED PATHS "${NATRON_USD_PREFIX}" NO_DEFAULT_PATH)

# The same Conan build names MaterialX's targets materialx::<Lib>; MaterialX's own config, found
# above, names them <Lib>.
foreach(_natron_mtlx_lib MaterialXCore MaterialXFormat MaterialXGenShader MaterialXGenGlsl MaterialXRender)
    if(TARGET ${_natron_mtlx_lib} AND NOT TARGET materialx::${_natron_mtlx_lib})
        add_library(materialx::${_natron_mtlx_lib} ALIAS ${_natron_mtlx_lib})
    endif()
endforeach()
unset(_natron_mtlx_lib)

if(PXR_VERSION LESS NATRON_USD_MIN_VERSION)
    message(FATAL_ERROR "OpenUSD ${PXR_MAJOR_VERSION}.${PXR_MINOR_VERSION}.${PXR_PATCH_VERSION} found at ${pxr_DIR}; Natron needs ${NATRON_USD_MIN_VERSION} or newer (boost-free, oneTBB).")
endif()

# pxrConfig.cmake carries the Python USD was built against only as the version it asks
# find_dependency() for, and only when USD was built with Python support.
file(READ "${pxr_DIR}/pxrConfig.cmake" _natron_pxr_config)
string(REGEX MATCH "find_dependency\\(Python3 \"([0-9]+)\\.([0-9]+)\" EXACT" _natron_pxr_python_match "${_natron_pxr_config}")
unset(_natron_pxr_config)
if(NOT _natron_pxr_python_match)
    message(FATAL_ERROR "OpenUSD at ${pxr_DIR} was built without Python bindings; Natron needs them (import pxr from Natron's Python).")
endif()
set(NATRON_USD_PYTHON_VERSION "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}")
unset(_natron_pxr_python_match)
if(NOT NATRON_USD_PYTHON_VERSION VERSION_EQUAL "${Python3_VERSION_MAJOR}.${Python3_VERSION_MINOR}")
    message(FATAL_ERROR "OpenUSD at ${pxr_DIR} was built against Python ${NATRON_USD_PYTHON_VERSION}, but Natron uses Python ${Python3_VERSION} (${Python3_EXECUTABLE}). They must be the same version.")
endif()

# What Engine's scene code (Engine/Nodes/Scene/) links: authoring and composing layers, the stage,
# UsdGeom's stage metadata (up axis, meters per unit), cameras and xforms, UsdLux's lights,
# UsdShade's materials, UsdUtils's layer-stack flattening and asset-path rewriting for USDWrite,
# and USDRender's Storm render: UsdImagingGLEngine on HgiGL, its AOV render buffers, garch's GL
# loader for the framebuffer it renders under, the camera framing and conforming and the GL
# lights it hands the engine; and hdsi's implicit-surface scene index, which tessellates UsdGeom's
# Cube, Sphere and the like for a delegate such as Cycles that takes only meshes.
set(NATRON_USD_ENGINE_LIBRARIES usdImagingGL usdImaging hdx hdsi hd hgi hgiGL glf garch cameraUtil usdUtils usd sdf usdGeom usdLux usdShade sdr pcp ar kind plug vt gf tf work trace arch)

# What the viewer's scene view (Gui/SceneViewport) links: the same Storm render as USDRender.
set(NATRON_USD_GUI_LIBRARIES ${NATRON_USD_ENGINE_LIBRARIES})

foreach(_natron_usd_target ${NATRON_USD_GUI_LIBRARIES})
    if(NOT TARGET ${_natron_usd_target})
        message(FATAL_ERROR "OpenUSD at ${pxr_DIR} does not export the ${_natron_usd_target} library.")
    endif()
endforeach()
unset(_natron_usd_target)

message(STATUS "OpenUSD ${PXR_MAJOR_VERSION}.${PXR_MINOR_VERSION}.${PXR_PATCH_VERSION} (Python ${NATRON_USD_PYTHON_VERSION}) at ${pxr_DIR}")
