## Pending

### 2026-10-10T15:36:00-04:00 — change-request
- refs: M91, M84, M85, M86, M87, M81
- Add a new milestone, M91 - NVIDIA GPU Validation (user-requested 2026-10-10). Every GPU milestone so far is verified only on lavapipe and RADV (RX 7900 XTX); the user wants proof that the Vulkan + Slang path also works on an NVIDIA host. It needs physical access to an NVIDIA machine, so it is a stub until one is available. Board row (append after M90; ID M91 checked free against origin/plan at bccb8f659 per the two-hosts-share-the-plan-branch decision): `| M91 | NVIDIA GPU Validation | todo | M84 | [M91-nvidia-gpu-validation.md](PLAN/MILESTONES/M91-nvidia-gpu-validation.md) |`. Not eligible for any lane on either current host. Full file content for `PLAN/MILESTONES/M91-nvidia-gpu-validation.md`:

  ```markdown
  # M91 - NVIDIA GPU Validation

  > Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

  Confirm the GPU work (Vulkan 1.3 compute, offline Slang→SPIR-V, host-import, the Vulkan→GL viewer hand-off) runs correctly on an NVIDIA GPU with the proprietary driver, not only on RADV and lavapipe. Run the existing GPU test and bench tooling on an NVIDIA host, fix any vendor-specific failures (or file them as follow-ups if large), and record the numbers next to the RX 7900 XTX ones. Scope is whatever GPU milestones (M84 onward, plus M81/M85–M87 if done by then) have landed when an NVIDIA host is available.

  Blocked on: access to an NVIDIA host (none is available yet — the user will say when), and M84 - GPU Placement And Residency being `done` so there is a promoted `Engine/Gpu/` and `GpuTests` binary to validate.

  Acceptance sketch:
  - `GpuTests` and a full engine ctest with `NATRON_GPU=force` are green on the NVIDIA device, including semaphore and GL interop tests.
  - GPU-vs-CPU kernels (Grade, FIR Blur, and any later native kernels) agree within the same tolerances as on RADV.
  - Auto mode selects the NVIDIA device; host-import (`VK_EXT_external_memory_host`) and the PBO/semaphore viewer path work under NVIDIA's GL, or fall back cleanly to hostsync/readback with the reason logged.
  - `GpuBench` / transfer-bench numbers recorded in the GPU design note alongside the RADV figures; any vendor-specific workaround recorded as a decision.
  ```
