## Pending

### 2026-10-26T23:00:00Z — change-request
- refs: M101
- From gpu (user decision, 2026-10-26): cost seeds are void (`PLAN/DECISIONS/2026-10-26-weight-based-gpu-placement.md`). Strike any unfinished M101 - Clean-Sheet Merge Node task or clause that measures or writes a `*.costseed.inc`, and don't add the README cost-seed checklist step. M239 - Weight-Based GPU Placement deletes the seed machinery; if M101 lands first, its already-committed seed file is removed in M239's rebase.

### 2026-10-27T12:00:00Z — change-request
- refs: M123, M185
- From cat-plan, re-filed from the 2026-10-27T05:00:00Z entry (M122 - Native Colour Utilities and M129 - Clean-Sheet Noise Node are done, so only these two remain). Cost seeds are abolished (`PLAN/DECISIONS/2026-10-26-weight-based-gpu-placement.md`). M239 - Weight-Based GPU Placement deletes `*.costseed.inc`, `Engine/GpuCostSeeds/`, `render-profile-report.py --emit-cost-table` and the measured cost model, and nodes declare `gpuWeight()` instead (Engine/GpuWeight.h). Override it only when cost grows with a knob (`gpuSizeScaledWeight`) or the work always wants the GPU (`GpuWeight::heavy()`). In M123 - Native Sharpen And Glow and M185 - Variable Warps, strike any unfinished task or clause that measures or writes cost seeds, and don't add new `*.costseed.inc` files. Seed files already on a branch are deleted when that branch rebases after M239 merges, or by M239's rebase if your milestone merges first.

### 2026-10-27T18:00:00Z — change-request
- refs: M203.P1.T2, M203.P1.T3, M203.P2.T1
- From cat-plan (user, 2026-10-27). M246 - Vulkan Inference Provider will later register onnx-vulkan-rs, an ONNX Runtime plugin execution provider, through `RegisterExecutionProviderLibrary`. Keep M203 - ONNX Inference Backend open to that without building it: (1) M203.P1.T2: the loader keeps a data-driven provider table (name, how it is detected, how it is appended to session options) rather than hard-coding the Auto order and probe list, so a plugin EP library registered after `OrtEnv` creation can add an entry. (2) M203.P1.T3: `aiDevice`'s choices come from that table, not a fixed enum. Persist the provider name, so an entry added later doesn't shift saved values. (3) M203.P2.T1: the `SessionCache` key has room for an EP-specific part; M246 needs the input shape there, because the Vulkan plugin needs concrete input dimensions. No plugin loading, no Vulkan entry and no new UI in M203.

### 2026-10-27T07:30:00Z — info
- refs: none
- From gpu: main at 622c03219, after M178 - Render Context Core merged, fails two tests 5 times out of 5 in a local debug build with ctest on RADV (`NATRON_DEV_GPU=1`): `PersistentMessageTest.RenderFailingAfterItsChannelCheckPassedKeepsTheError` (Tests/PersistentMessage_Test.cpp:437) and `ShuffleRenderTest.ExplicitRowVariesPerFrameOnShuffleDisableChain` (Tests/ShuffleRender_Test.cpp:361). Both fail on `hasPersistentMessage()` being unexpectedly true. They passed on a branch based just before M178. Unconfirmed suspect: M178's new `contextCountWarningMessage` source in Node.cpp. Neither test uses a node the gpu PM's branches touch. It hasn't been checked whether PR CI on lavapipe also fails them. Needs an owner: a fix milestone or the M178 author.
