

### 2026-10-24T01:22:11+00:00 — change-request
- refs: M119
- Also for the trackball/global mode control: 
  
  - global mode should make the cursor a crosshair (pitch some designs)
  - the hud can follow the cursor, offset below and to the right
  - would like to try out some various other UI designs for global mode, like the trackball appearing in C mode when you click, and similar images for the other modes that give feedback on what you're doing
  - could also try making the cursor change to that UI (trackball etc) 
  
  Pitch some designs.

### 2026-10-24T11:00:00Z — info
- refs: none
- Pre-existing race on main, found while diagnosing a `NativeWriteReadBackTest` flake in M138 - GUI Papercuts (reproduced about 58 failures in 480 suite runs with 8 parallel copies on 4 cores). `BlockingBackgroundRender::blockingRender` (`Engine/BlockingBackgroundRender.cpp:53-72`) returns when `notifyRenderFinished()` fires inside the scheduler thread's `threadLoopOnce` (`Engine/OutputSchedulerThread.cpp:2823`, `Engine/OutputEffectInstance.cpp:356`), but the thread only marks itself idle after that returns (`Engine/GenericSchedulerThread.cpp:523-526`). In the gap, `RenderEngine::isDoingSequentialRender()` is still true, so `NativeWrite::readBackActive()` (`Engine/Nodes/IO/NativeWrite.cpp:1396`) reports false and `EffectInstance.cpp:521` aborts renders on the Write. M138 worked around it in the test (wait for `isDoingSequentialRender()` to go false). An engine fix — `blockingRender` also waiting for the scheduler to go idle — needs care: if `notifyRenderFinished` starts the next queued sequence, the thread stays active and the wait could hang. Other callers that inspect a Write right after `startWritersRendering` may hit the same race.
