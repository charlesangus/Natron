## Pending

### 2026-10-20T07:10:00Z — info
- refs: none
- Flaky test: `BaseTest.DataKindErrorClearsWhenTheOffendingInputIsRemoved` (`Tests/DataKindProjectLoad_Test.cpp:227`) failed in PR #69's debug legacy leg because the project save couldn't open its temp file (`QIODevice::read (QFile, "/tmp/1792477735903"): device not open`, then "Save: Failed to save"). The temp name looks like a millisecond timestamp under `/tmp`, so parallel ctest processes saving in the same millisecond can collide. Unrelated to M78's diff; it passed in the other legs and on re-run. Worth a small fix (unique temp names, e.g. QTemporaryFile) in whichever milestone owns project save.
