## Pending

### 2026-10-25T12:00:00Z — info
- refs: Engine/Knob.cpp setExpressionInternal; found during M92
`setExpressionInternal` rethrows a caught `const std::exception&` with `throw e;`, slicing a `std::runtime_error` to a plain `std::exception` and dropping its type. Use `throw;`.

### 2026-10-25T06:30:00Z — info
- refs: M167
A Switch's per-file key/value metadata (custom header attributes) always comes from its input 0, not the input it routes to at that frame, so a Write over a keyed Switch writes the first source's custom attributes into every file. Found while writing M167 P5.T4 (NativeWriteMetadata EachFileOfASequenceTakesItsOwnFramesFormat checks only size and PAR). Fix belongs in Switch / NativeEffectBase::getOutputMetadata routing; outside M167's files.

### 2026-10-25T07:40 — info
- refs: none
- **Nightly on main has failed since `9ab236449`** (runs 40163566294, 40167954015 and 40170317744). The `release-build-and-test` job fails `GuiThreadGLBinding` (from M161 - Startup Compositor Crash) and `TextRenderer` (from M172 - GL Widget Context Loss) in the release build; PR CI's debug build passes them. Found while the gpu PM dispatched Nightly on M171's branch (run 40170808335, which shows the same two failures). Needs an owner, likely bosley-native-nodes, which shipped both. M171 doesn't touch these tests.
