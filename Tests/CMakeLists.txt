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

set(Tests_HEADERS BaseTest.h CacheMemoryPressureGuard.h DataKindTestEffect.h DeepRenderTestEffect.h FlatExrReader.h)
set(Tests_SOURCES
    google-test/src/gtest-all.cc
    google-mock/src/gmock-all.cc
    BaseTest.cpp
    ChannelSetRender_Test.cpp
    CLArgs_Test.cpp
    Curve_Test.cpp
    DataKind_Test.cpp
    DataKindProjectLoad_Test.cpp
    DeepExpressionEvaluator_Test.cpp
    DeepFlatten_Test.cpp
    DeepImage_Test.cpp
    DeepImageCache_Test.cpp
    DeepNodes_Test.cpp
    DeepPipeline_Test.cpp
    DeepPixelOps_Test.cpp
    DeepReadWrite_Test.cpp
    DeepRenderPipeline_Test.cpp
    FileSystemModel_Test.cpp
    GeneratorLayer_Test.cpp
    Hash64_Test.cpp
    Image_Test.cpp
    KnobChannelSet_Test.cpp
    KnobFile_Test.cpp
    KnobLayerSelect_Test.cpp
    Knob_Test.cpp
    LayerKnobsRender_Test.cpp
    LayerKnobs_Test.cpp
    LayerRegistry_Test.cpp
    Lut_Test.cpp
    Metadata_Test.cpp
    OSGLContext_Test.cpp
    ProjectOCIO_Test.cpp
    ProjectSerialization_Test.cpp
    PyPlugExport_Test.cpp
    ReadFormat_Test.cpp
    RenderRange_Test.cpp
    RotoLayer_Test.cpp
    TrackerLayer_Test.cpp
    Tracker_Test.cpp
    TypedPassthrough_Test.cpp
    WriteAllLayers_Test.cpp
    wmain.cpp
)
add_executable(Tests ${Tests_HEADERS} ${Tests_SOURCES})
target_link_libraries(Tests
    PRIVATE
        NatronEngine
        Qt::Core
        Python3::Python
        openMVG
)
target_compile_definitions(Tests
    PRIVATE
        NATRON_TESTS_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
target_include_directories(Tests
    PRIVATE
        ..
        ../libs/SequenceParsing
        google-test/include
        google-test
        google-mock/include
        google-mock
)
# Widget tests live in their own binary: they need a QApplication, and the Tests
# binary's AppManager already owns a QCoreApplication that a QApplication cannot
# be created next to. guimain.cpp builds the QApplication on the offscreen
# platform, so the suite runs without a display.
set(GuiTests_SOURCES
    google-test/src/gtest-all.cc
    google-mock/src/gmock-all.cc
    LayerChannelRow_Test.cpp
    guimain.cpp
)
add_executable(GuiTests ${GuiTests_SOURCES})
target_link_libraries(GuiTests
    PRIVATE
        NatronGui
        NatronEngine
        Qt::Core
        Qt::Widgets
        Python3::Python
)
target_include_directories(GuiTests
    PRIVATE
        ..
        google-test/include
        google-test
        google-mock/include
        google-mock
)

include(GoogleTest)
# PRE_TEST, not the default POST_BUILD: discovery runs the binary, which
# constructs an AppManager and calls load() before RUN_ALL_TESTS() (even
# under --gtest_list_tests), so it needs libpython3.13, OFX_PLUGIN_PATH and
# the built OFX bundles -- all guaranteed at ctest time, not at build time.
# TIMEOUT 300: the whole suite runs in under a second locally and the
# slowest single case is well under a second too, so 300s per case is pure
# headroom -- its only job is to turn a scheduler wait with no predicate,
# no loop and no timeout (OutputSchedulerThread.cpp's bufEmptyCondition
# wait) into a failed test instead of a process that never exits.
# DISCOVERY_TIMEOUT 120, not the 5s default: PRE_TEST discovery runs the binary,
# and wmain.cpp passes --clear-openfx-cache, so every discovery pays a cold rescan
# of all four OFX bundles plus Python and OCIO init before gtest prints the list.
# Blowing the 5s budget is a FATAL_ERROR that fails ctest before a single test runs.
gtest_discover_tests(Tests DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 120 PROPERTIES TIMEOUT 300)
gtest_discover_tests(GuiTests DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 120 PROPERTIES TIMEOUT 300)
