# Included by ctest from the generated test file, after every per-suite test has been added.
#
# A name that matches no registered test would make set_tests_properties() skip it silently, so
# each one is checked against the names the registrations recorded in NATRON_GTEST_SUITE_TESTS.

# Each suite below asserts wall-clock bounds of a few seconds or less. Under `ctest -j` the other
# suites' render threads compete for the same cores and can push them past it, so they run alone.
set(_natron_serial_suites
    RenderSchedulerAbortTiming
    GraphScalingTiming
    StagedImageEffectTiming
)

# Suites whose cases add up to more than the default 300 s, at three times the slower of the
# legacy and task-graph scheduler runs.
set(_natron_long_suite_timeouts
    BaseTest 540
    FrameGraphBuild 900
)

set(_natron_long_suites "")
set(_natron_long_timeouts "")
foreach(_item IN LISTS _natron_long_suite_timeouts)
    if(_item MATCHES "^[0-9]+$")
        list(APPEND _natron_long_timeouts "${_item}")
    else()
        list(APPEND _natron_long_suites "${_item}")
    endif()
endforeach()

foreach(_suite IN LISTS _natron_serial_suites _natron_long_suites)
    if(NOT _suite IN_LIST NATRON_GTEST_SUITE_TESTS)
        message(FATAL_ERROR "SuiteProperties.cmake: '${_suite}' is not a registered test; "
                            "update the suite name or it would silently lose its properties.")
    endif()
endforeach()

set_tests_properties(${_natron_serial_suites} PROPERTIES RUN_SERIAL TRUE)

set(_natron_i 0)
foreach(_suite IN LISTS _natron_long_suites)
    list(GET _natron_long_timeouts ${_natron_i} _timeout)
    set_tests_properties(${_suite} PROPERTIES TIMEOUT ${_timeout})
    math(EXPR _natron_i "${_natron_i} + 1")
endforeach()
