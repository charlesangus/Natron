# natron_add_gtest_suites(<target> [TEST_PREFIX <prefix>] [TEST_FILTER <gtest filter>]
#                         [WORKING_DIRECTORY <dir>] [DISCOVERY_TIMEOUT <seconds>]
#                         [PROPERTIES <name> <value>...])
#
# Registers one ctest entry per gtest suite of <target>, named <prefix><suite>, rather than one per
# case as gtest_discover_tests() does: each entry is a process launch, and every launch of the test
# binaries pays the whole application start-up. Like gtest_discover_tests(... DISCOVERY_MODE
# PRE_TEST), the suites are listed at ctest time, and only again once the binary has changed.

set(NATRON_GTEST_SUITES_ADD_TESTS_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/NatronGtestSuitesAddTests.cmake")

function(natron_add_gtest_suites target)
    cmake_parse_arguments(PARSE_ARGV 1 arg
        ""
        "TEST_PREFIX;TEST_FILTER;WORKING_DIRECTORY;DISCOVERY_TIMEOUT"
        "PROPERTIES"
    )
    if(arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "natron_add_gtest_suites: unknown arguments: ${arg_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT arg_WORKING_DIRECTORY)
        set(arg_WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    endif()
    if(NOT arg_DISCOVERY_TIMEOUT)
        set(arg_DISCOVERY_TIMEOUT 60)
    endif()

    # A target may be registered several times, with different prefixes or filters; each
    # registration needs its own cached listing.
    get_property(counter TARGET ${target} PROPERTY NATRON_GTEST_SUITES_COUNTER)
    if(NOT counter)
        set(counter 0)
    endif()
    math(EXPR counter "${counter} + 1")
    set_property(TARGET ${target} PROPERTY NATRON_GTEST_SUITES_COUNTER ${counter})

    set(file_base "${CMAKE_CURRENT_BINARY_DIR}/${target}[${counter}]_suites")
    set(include_file "${file_base}_include.cmake")
    get_property(multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(multi_config)
        set(tests_file "${file_base}_tests-$<CONFIG>.cmake")
    else()
        set(tests_file "${file_base}_tests.cmake")
    endif()

    # The listing is cached against this include file too, so a change of prefix, filter or
    # properties at configure time re-lists even when the binary has not been rebuilt.
    string(CONCAT content
        "if(EXISTS \"$<TARGET_FILE:${target}>\")\n"
        "  include(\"${NATRON_GTEST_SUITES_ADD_TESTS_SCRIPT}\")\n"
        "  natron_gtest_suites_add_tests(\n"
        "    TEST_EXECUTABLE [==[$<TARGET_FILE:${target}>]==]\n"
        "    TEST_WORKING_DIR [==[${arg_WORKING_DIRECTORY}]==]\n"
        "    TEST_PREFIX [==[${arg_TEST_PREFIX}]==]\n"
        "    TEST_FILTER [==[${arg_TEST_FILTER}]==]\n"
        "    TEST_PROPERTIES [==[${arg_PROPERTIES}]==]\n"
        "    TEST_DISCOVERY_TIMEOUT [==[${arg_DISCOVERY_TIMEOUT}]==]\n"
        "    CTEST_FILE [==[${tests_file}]==]\n"
        "    CACHE_DEPENDS \"\${CMAKE_CURRENT_LIST_FILE};${NATRON_GTEST_SUITES_ADD_TESTS_SCRIPT}\"\n"
        "  )\n"
        "  include(\"${tests_file}\")\n"
        "else()\n"
        "  add_test(${target}_NOT_BUILT ${target}_NOT_BUILT)\n"
        "endif()\n"
    )

    if(multi_config)
        file(GENERATE OUTPUT "${file_base}_include-$<CONFIG>.cmake" CONTENT "${content}")
        file(WRITE "${include_file}"
            "include(\"${file_base}_include-\${CTEST_CONFIGURATION_TYPE}.cmake\")\n")
    else()
        file(GENERATE OUTPUT "${include_file}" CONTENT "${content}")
    endif()

    set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${include_file}")
endfunction()
