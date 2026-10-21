# Run by ctest, through the include file natron_add_gtest_suites() generates: lists a gtest
# binary's suites and writes one add_test() per suite to CTEST_FILE.
#
# It also runs standalone with `cmake -P`, taking the same arguments as -D variables.

cmake_minimum_required(VERSION 3.16)

function(_natron_gtest_suites_quote out_var value)
    if(value STREQUAL "")
        set(${out_var} "\"\"" PARENT_SCOPE)
    elseif(value MATCHES "[^-./:a-zA-Z0-9_]")
        set(${out_var} "[==[${value}]==]" PARENT_SCOPE)
    else()
        set(${out_var} "${value}" PARENT_SCOPE)
    endif()
endfunction()

function(_natron_gtest_suites_is_disabled out_var name)
    # gtest's own test for a disabled suite or case, "DISABLED_*:*/DISABLED_*", which also
    # covers a parameterized instantiation such as Prefix/DISABLED_Suite.
    if(name MATCHES "^DISABLED_" OR name MATCHES "/DISABLED_")
        set(${out_var} TRUE PARENT_SCOPE)
    else()
        set(${out_var} FALSE PARENT_SCOPE)
    endif()
endfunction()

# Sets <prefix>_suites to the listed suites in order and, for each suite at index i, <prefix>_<i>
# to its enabled cases. A suite left with no enabled case is dropped.
function(_natron_gtest_suites_list prefix executable filter working_dir timeout)
    set(command "${executable}" --gtest_list_tests)
    if(NOT filter STREQUAL "")
        list(APPEND command "--gtest_filter=${filter}")
    endif()
    execute_process(
        COMMAND ${command}
        WORKING_DIRECTORY "${working_dir}"
        TIMEOUT ${timeout}
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error_output
        RESULT_VARIABLE result
    )
    if(NOT "${result}" STREQUAL "0")
        list(JOIN command " " command)
        message(FATAL_ERROR
            "Listing the tests of a gtest binary failed.\n"
            "  Command: ${command}\n"
            "  Working directory: ${working_dir}\n"
            "  Timeout: ${timeout}\n"
            "  Result: ${result}\n"
            "  Output:\n${output}\n"
            "  Error output:\n${error_output}\n"
        )
    endif()

    # gtest separates a suite or case name from its "# TypeParam = ..." or "# GetParam() = ..."
    # with two spaces. The parameter text is arbitrary and may hold list separators or brackets,
    # so it goes before the output is split into lines. Neither can be part of a test name, and
    # an unbalanced bracket left elsewhere would stop the list splitting on separators.
    string(REGEX REPLACE "  +#[^\n]*" "" output "${output}")
    string(REGEX REPLACE "[]\r;[]" "" output "${output}")
    string(REPLACE "\n" ";" lines "${output}")

    set(suites "")
    set(index -1)
    set(in_suite FALSE)
    foreach(line IN LISTS lines)
        string(REGEX REPLACE " +$" "" line "${line}")
        if(line STREQUAL "")
            continue()
        endif()
        if(line MATCHES "^  ([^ ]+)$")
            if(in_suite)
                set(case "${CMAKE_MATCH_1}")
                _natron_gtest_suites_is_disabled(case_disabled "${case}")
                if(NOT case_disabled)
                    list(APPEND cases_${index} "${case}")
                endif()
            endif()
        elseif(line MATCHES "^([^ ]+)\\.$")
            set(suite "${CMAKE_MATCH_1}")
            _natron_gtest_suites_is_disabled(in_suite_disabled "${suite}")
            if(in_suite_disabled)
                set(in_suite FALSE)
            else()
                math(EXPR index "${index} + 1")
                list(APPEND suites "${suite}")
                set(cases_${index} "")
                set(in_suite TRUE)
            endif()
        else()
            # A banner such as the "Running main() from ..." line some mains print first.
            set(in_suite FALSE)
        endif()
    endforeach()

    set(kept "")
    set(i 0)
    foreach(suite IN LISTS suites)
        if(NOT "${cases_${i}}" STREQUAL "")
            list(LENGTH kept kept_index)
            list(APPEND kept "${suite}")
            set(${prefix}_${kept_index} "${cases_${i}}" PARENT_SCOPE)
        endif()
        math(EXPR i "${i} + 1")
    endforeach()
    set(${prefix}_suites "${kept}" PARENT_SCOPE)
endfunction()

function(natron_gtest_suites_add_tests)
    cmake_parse_arguments(PARSE_ARGV 0 arg
        ""
        "TEST_EXECUTABLE;TEST_WORKING_DIR;TEST_PREFIX;TEST_FILTER;TEST_PROPERTIES;TEST_DISCOVERY_TIMEOUT;CTEST_FILE;CACHE_DEPENDS"
        ""
    )
    if(NOT EXISTS "${arg_TEST_EXECUTABLE}")
        message(FATAL_ERROR "Test executable does not exist: ${arg_TEST_EXECUTABLE}")
    endif()
    if("${arg_TEST_WORKING_DIR}" STREQUAL "")
        get_filename_component(arg_TEST_WORKING_DIR "${arg_TEST_EXECUTABLE}" DIRECTORY)
    endif()
    if("${arg_TEST_DISCOVERY_TIMEOUT}" STREQUAL "")
        set(arg_TEST_DISCOVERY_TIMEOUT 60)
    endif()

    set(stale FALSE)
    if(NOT EXISTS "${arg_CTEST_FILE}" OR NOT "${arg_CTEST_FILE}" IS_NEWER_THAN "${arg_TEST_EXECUTABLE}")
        set(stale TRUE)
    endif()
    foreach(dependency IN LISTS arg_CACHE_DEPENDS)
        if(NOT "${arg_CTEST_FILE}" IS_NEWER_THAN "${dependency}")
            set(stale TRUE)
        endif()
    endforeach()
    if(NOT stale)
        return()
    endif()

    # gtest splits a filter at its first '-' into the patterns to run and the patterns to skip.
    set(positive "${arg_TEST_FILTER}")
    set(negative "")
    string(FIND "${arg_TEST_FILTER}" "-" dash)
    if(dash GREATER -1)
        string(SUBSTRING "${arg_TEST_FILTER}" 0 ${dash} positive)
        math(EXPR after "${dash} + 1")
        string(SUBSTRING "${arg_TEST_FILTER}" ${after} -1 negative)
    endif()

    _natron_gtest_suites_list(listed "${arg_TEST_EXECUTABLE}" "${arg_TEST_FILTER}"
        "${arg_TEST_WORKING_DIR}" "${arg_TEST_DISCOVERY_TIMEOUT}")
    if("${listed_suites}" STREQUAL "")
        message(FATAL_ERROR
            "${arg_TEST_EXECUTABLE} listed no enabled test"
            " for the filter '${arg_TEST_FILTER}'.")
    endif()

    # "<suite>.*" would run every case of the suite, so where the patterns to run keep only some
    # of them, the kept cases are named one by one instead.
    set(partial_check FALSE)
    if(NOT positive STREQUAL "" AND NOT positive STREQUAL "*")
        set(partial_check TRUE)
        _natron_gtest_suites_list(all "${arg_TEST_EXECUTABLE}" ""
            "${arg_TEST_WORKING_DIR}" "${arg_TEST_DISCOVERY_TIMEOUT}")
    endif()

    set(script "")
    set(names "")
    set(i 0)
    foreach(suite IN LISTS listed_suites)
        set(cases "${listed_${i}}")
        set(whole_suite TRUE)
        if(partial_check)
            list(FIND all_suites "${suite}" all_index)
            list(LENGTH cases case_count)
            list(LENGTH all_${all_index} all_count)
            if(NOT case_count EQUAL all_count)
                set(whole_suite FALSE)
            endif()
        endif()
        if(whole_suite)
            set(filter "${suite}.*")
        else()
            set(filter "")
            foreach(case IN LISTS cases)
                if(NOT filter STREQUAL "")
                    string(APPEND filter ":")
                endif()
                string(APPEND filter "${suite}.${case}")
            endforeach()
        endif()
        if(NOT negative STREQUAL "")
            string(APPEND filter "-${negative}")
        endif()

        _natron_gtest_suites_quote(name "${arg_TEST_PREFIX}${suite}")
        _natron_gtest_suites_quote(executable "${arg_TEST_EXECUTABLE}")
        _natron_gtest_suites_quote(filter_arg "--gtest_filter=${filter}")
        string(APPEND script "add_test(${name} ${executable} ${filter_arg})\n")

        _natron_gtest_suites_quote(working_dir "${arg_TEST_WORKING_DIR}")
        set(properties "")
        foreach(property IN LISTS arg_TEST_PROPERTIES)
            # A list-valued property, such as several ENVIRONMENT entries, arrives with its
            # separators escaped; one bracket argument holding the plain list keeps it one value.
            string(REPLACE "\\;" ";" property "${property}")
            _natron_gtest_suites_quote(property "${property}")
            string(APPEND properties " ${property}")
        endforeach()
        string(APPEND script
            "set_tests_properties(${name} PROPERTIES WORKING_DIRECTORY ${working_dir}${properties})\n")
        list(APPEND names "${name}")
        math(EXPR i "${i} + 1")
    endforeach()
    list(JOIN names " " names)
    string(PREPEND script "natron_gtest_suites_claim(${names})\n")

    # Written aside and renamed, so a listing that dies half way never leaves a cache that looks
    # current.
    file(WRITE "${arg_CTEST_FILE}.tmp" "${script}")
    file(RENAME "${arg_CTEST_FILE}.tmp" "${arg_CTEST_FILE}")
endfunction()

# ctest registers a second add_test() of an existing name as a second test, without a word, so
# two registrations of one binary sharing a prefix would run a suite twice.
function(natron_gtest_suites_claim)
    get_property(claimed GLOBAL PROPERTY NATRON_GTEST_SUITES_CLAIMED)
    foreach(name IN LISTS ARGN)
        list(FIND claimed "${name}" found)
        if(found GREATER -1)
            message(FATAL_ERROR "Two gtest suite registrations both add the ctest test '${name}'.")
        endif()
        list(APPEND claimed "${name}")
    endforeach()
    set_property(GLOBAL PROPERTY NATRON_GTEST_SUITES_CLAIMED "${claimed}")
endfunction()

if(CMAKE_SCRIPT_MODE_FILE)
    natron_gtest_suites_add_tests(
        TEST_EXECUTABLE "${TEST_EXECUTABLE}"
        TEST_WORKING_DIR "${TEST_WORKING_DIR}"
        TEST_PREFIX "${TEST_PREFIX}"
        TEST_FILTER "${TEST_FILTER}"
        TEST_PROPERTIES "${TEST_PROPERTIES}"
        TEST_DISCOVERY_TIMEOUT "${TEST_DISCOVERY_TIMEOUT}"
        CTEST_FILE "${CTEST_FILE}"
        CACHE_DEPENDS "${CACHE_DEPENDS}"
    )
endif()
