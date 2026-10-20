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

set(Tests_HEADERS BaseTest.h CacheMemoryPressureGuard.h CountingTestEffect.h DataKindTestEffect.h DeepRenderTestEffect.h FlatExrReader.h InputChangedFetchTestEffect.h MultiplanarTestEffect.h NativeMetadataTestEffect.h NativeParity.h Native/NativeGolden.h Native/NativeGpuParity.h PerProcessUserDirs.h RenderBothWays.h)
set(Tests_SOURCES
    google-test/src/gtest-all.cc
    google-mock/src/gmock-all.cc
    AddLayers_Test.cpp
    AddLayersRender_Test.cpp
    BaseTest.cpp
    ChannelSetRender_Test.cpp
    CLArgs_Test.cpp
    ColorViews_Test.cpp
    ColorViewsRender_Test.cpp
    ColorWheelMath_Test.cpp
    ContentHasher_Test.cpp
    Curve_Test.cpp
    DataKind_Test.cpp
    DataKindProjectLoad_Test.cpp
    DeepChannelNodes_Test.cpp
    DeepExpressionEvaluator_Test.cpp
    DeepFlatten_Test.cpp
    DeepImage_Test.cpp
    DeepImageCache_Test.cpp
    DeepLayers_Test.cpp
    DeepNodes_Test.cpp
    DeepPipeline_Test.cpp
    DeepPixelOps_Test.cpp
    DeepReadWrite_Test.cpp
    DeepRenderPipeline_Test.cpp
    DefaultChannelSet_Test.cpp
    FileSystemModel_Test.cpp
    FrameGraphBuild_Test.cpp
    FrameRenderContext_Test.cpp
    FrameStoreHandoff_Test.cpp
    GeneratorLayer_Test.cpp
    GLScheduler_Test.cpp
    GpuContext_Test.cpp
    GpuPlacement_Test.cpp
    GpuResidency_Test.cpp
    GraphScaling_Test.cpp
    GraphScalingExpressionDeps_Test.cpp
    GraphScalingNaming_Test.cpp
    GraphScalingTLS_Test.cpp
    Hash64_Test.cpp
    HashInvariants_Test.cpp
    HashPluginSweep_Test.cpp
    Image_Test.cpp
    KnobChannelSet_Test.cpp
    KnobFile_Test.cpp
    KnobLayerSelect_Test.cpp
    KnobShuffleMap_Test.cpp
    KnobContentHash_Test.cpp
    LazyHash_Test.cpp
    NodeContentHash_Test.cpp
    FileStamp_Test.cpp
    RotoContentHash_Test.cpp
    Knob_Test.cpp
    LayerKnobsRender_Test.cpp
    LayerKnobs_Test.cpp
    LayerRegistry_Test.cpp
    Lut_Test.cpp
    MemoryInfo_Test.cpp
    Metadata_Test.cpp
    NativeParity.cpp
    Native/NativeGolden.cpp
    Native/NativeGpuParity.cpp
    NodeCategory_Test.cpp
    TabMenuSettings_Test.cpp
    NodeColorSampler_Test.cpp
    OSGLContext_Test.cpp
    PersistentMessage_Test.cpp
    PluginIdentity_Test.cpp
    ImagePasteBands_Test.cpp
    PoolParallelFor_Test.cpp
    PoolSharing_Test.cpp
    ProjectColorManagement_Test.cpp
    ViewerDisplayTransform_Test.cpp
    ViewerScheduler_Test.cpp
    ProjectOCIODefaults_Test.cpp
    ProjectOCIOPlugins_Test.cpp
    ProjectOCIO_Test.cpp
    ProjectSerialization_Test.cpp
    PyPlugExport_Test.cpp
    PyPlugInstantiate_Test.cpp
    ReadFormat_Test.cpp
    RemoveLayers_Test.cpp
    RemoveLayersRender_Test.cpp
    RenderBothWays.cpp
    RenderBothWaysSelf_Test.cpp
    RenderProfile_Test.cpp
    RenderRange_Test.cpp
    RenderScheduler_Test.cpp
    RenderSchedulerAbort_Test.cpp
    RenderSchedulerMode_Test.cpp
    RotoFeatherDrag_Test.cpp
    RotoLayer_Test.cpp
    SchedulerEquivalence_Test.cpp
    SchedulerWriters_Test.cpp
    Shuffle_Test.cpp
    ShufflePluginList_Test.cpp
    ShuffleRender_Test.cpp
    TimeVaryingLayers_Test.cpp
    TrackerLayer_Test.cpp
    Tracker_Test.cpp
    TypedPassthrough_Test.cpp
    WriteAllLayers_Test.cpp
    ImageConvert_Test.cpp
    ImageMetadata_Test.cpp
    NativeMetadata_Test.cpp
    NativeMetadataPolicy_Test.cpp
    OfxMetadataBridge_Test.cpp
    wmain.cpp
)
# Per-node native tests are picked up without editing this list; CONFIGURE_DEPENDS re-globs on
# every build, so a newly added file is compiled without re-running cmake by hand.
file(GLOB Tests_NATIVE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/Native/*_Test.cpp)
list(APPEND Tests_SOURCES ${Tests_NATIVE_SOURCES})
# Same CONFIGURE_DEPENDS auto-pickup for the serialization-boundary tests.
file(GLOB Tests_SERIALIZATION_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/Serialization/*_Test.cpp)
list(APPEND Tests_SOURCES ${Tests_SERIALIZATION_SOURCES})
add_executable(Tests ${Tests_HEADERS} ${Tests_SOURCES})
target_link_libraries(Tests
    PRIVATE
        NatronEngine
        Qt::Core
        Qt::Concurrent
        Python3::Python
        openMVG
)
if(NATRON_GPU)
    include(${CMAKE_SOURCE_DIR}/cmake/FetchSlang.cmake)
    include(${CMAKE_SOURCE_DIR}/cmake/SlangKernels.cmake)
    # StagedImageEffectGpu_Test registers this at run time as its test node's GPU kernel. GpuTests
    # already owns a target named pixelwrap_scale.
    slang_add_kernel(staged_test_pixelwrap_scale ${CMAKE_SOURCE_DIR}/Engine/Gpu/Kernels/pixelwrap.slang ENTRY pixelwrap_scale)
    target_link_libraries(Tests PRIVATE staged_test_pixelwrap_scale)
endif()
# Compiler, build type and the flags that set optimisation and FMA contraction, so fast
# (Release at -O1), release (RelWithDebInfo) and debug builds stamp distinct goldens.
string(TOUPPER "${CMAKE_BUILD_TYPE}" NATRON_GOLDEN_BUILD_TYPE_UPPER)
string(STRIP "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${NATRON_GOLDEN_BUILD_TYPE_UPPER}}" NATRON_GOLDEN_FLAGS)
string(REGEX REPLACE "[^A-Za-z0-9.+=-]+" "_" NATRON_GOLDEN_FLAGS "${NATRON_GOLDEN_FLAGS}")
set(NATRON_GOLDEN_CONFIG "${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}-${CMAKE_BUILD_TYPE}-${NATRON_GOLDEN_FLAGS}")
target_compile_definitions(Tests
    PRIVATE
        NATRON_GOLDEN_CONFIG="${NATRON_GOLDEN_CONFIG}"
        NATRON_TESTS_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
        NATRON_TESTS_PYPLUGS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../Gui/Resources/PyPlugs"
        NATRON_TESTS_CIMG_HEADER="${CMAKE_CURRENT_SOURCE_DIR}/../build/assets/CImg/CImg.h"
)
target_include_directories(Tests
    PRIVATE
        .
        ..
        ../libs/SequenceParsing
        ../libs/xxhash
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
    ColorAdjustPanel_Test.cpp
    ColorKnobUndoMerge_Test.cpp
    ColorPickerLabel_Test.cpp
    ColorTrackballGlobalMode_Test.cpp
    ColorTrackballHud_Test.cpp
    ColorTrackballTarget_Test.cpp
    ColorWheelWidget_Test.cpp
    CurveTangentJoin_Test.cpp
    EdgeNearbyGrab_Test.cpp
    KnobGuiString_Test.cpp
    KnobUndoHash_Test.cpp
    LayerChannelRow_Test.cpp
    NodeGraphConnect_Test.cpp
    NodeGraphPickUp_Test.cpp
    NodeGraphSplice_Test.cpp
    NodeGuiUserColorSerialization_Test.cpp
    ProjectGuiXmlEquivalence_Test.cpp
    RenderDialogRange_Test.cpp
    ScaleSliderQWidget_Test.cpp
    ShuffleMatrix_Test.cpp
    TabMenuBridge_Test.cpp
    ThemedIcon_Test.cpp
    ViewerDataSerialization_Test.cpp
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
target_compile_definitions(GuiTests
    PRIVATE
        NATRON_TESTS_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
        NATRON_TESTS_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/output"
)
target_include_directories(GuiTests
    PRIVATE
        ..
        ../libs/SequenceParsing
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
gtest_discover_tests(Tests DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 120
    PROPERTIES TIMEOUT 300)
gtest_discover_tests(GuiTests DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 120 PROPERTIES TIMEOUT 300)
# PRE_TEST discovery only learns the case names at ctest time, so per-case properties are applied
# from an include that ctest runs after the discovery includes registered above.
set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/SerialTests.cmake)

if(NATRON_GPU)
    add_subdirectory(Gpu)
endif()

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME RenderProfileReportSelfTest
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/bench/render-profile-report.py --self-test)
    add_test(NAME TabTabTabPython
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Tests/python/run_tabtabtab_tests.py)
endif()
