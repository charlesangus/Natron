# Included by ctest after gtest_discover_tests() has added the discovered cases, so these names
# exist by the time it runs; a name that no longer exists is ignored rather than an error.
#
# Each case below asserts a wall-clock bound of a few seconds or less. Under `ctest -j` the other
# cases' render threads compete for the same cores and can push them past it, so they run alone.
set_tests_properties(
    RenderSchedulerAbort.AbortDropsQueuedTasksAndWaitsForRunningOnes
    RenderSchedulerAbort.AbortUnderAOneOutputBudgetReleasesEverything
    RenderSchedulerAbort.AbortOfAnUnknownRenderDoesNothing
    RenderSchedulerAbort.WriterRangeStopsSoonAfterEngineAbort
    GraphScalingTest.DiamondLadderConnectIsFast
    GraphScalingTest.FrameVaryingFallbackIsLinearOnDiamondLadder
    StagedImageEffectTest.AnAbortReachesABandRunningOnAPoolThread
    PROPERTIES RUN_SERIAL TRUE
)
