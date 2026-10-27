# Spike: Storm (HgiGL) in a QOpenGLWidget docked beside an immediate-mode compat widget,
# on one global GL 4.5 compatibility default format. Throwaway; not part of the Natron build.
cmake_minimum_required(VERSION 3.24)
project(StormSpike CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_AUTOMOC ON)
set(NATRON_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../.." CACHE PATH "Natron checkout holding cmake/NatronUsd.cmake")
find_package(Python3 COMPONENTS Interpreter Development REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGL OpenGLWidgets)
include(${NATRON_SOURCE_DIR}/cmake/NatronUsd.cmake)
add_executable(storm-spike main.cpp)
# Qt's emit macro collides with oneTBB's emit() members, pulled in by USD's headers.
target_compile_definitions(storm-spike PRIVATE QT_NO_KEYWORDS)
target_link_libraries(storm-spike PRIVATE Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets usdImagingGL usdGeom usd sdf gf tf OpenGL::GL)
