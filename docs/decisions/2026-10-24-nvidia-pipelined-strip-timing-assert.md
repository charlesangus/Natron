# Pipelined-strip timing test asserts nesting, not 5% equality

In pipelined-strips mode, `GpuResidencyTest.RegionMembersAreTimedOnTheDevice` asserts that member timestamps nest within the strip kernel time rather than equal it within 5%. The cause is the compute→transfer queue-ownership release barriers, which are recorded inside the timed strip interval after the last member's timestamp. NVIDIA charges real time for them, while lavapipe and RADV make them nearly free. That time is correct device time that no member owns. Whole and serial strips keep the 5% check. No engine change.
