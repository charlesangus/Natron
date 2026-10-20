## Pending


### 2026-10-20T10:30:00Z — change-request
- refs: none
- Proposed follow-up milestone (E5 - GPU Compute or E6 - Render Performance), from M109 - GPU Region Engine Hardening's gate decision: cut the first-render page-zeroing cost of GPU region download destinations. A fresh 4 GiB destination (Erode 40 at 16k) waits ~100–256 ms while the kernel zeroes new pages at ~16–24 GB/s, about a third of region wall in a fresh process. Candidates: transparent or explicit huge pages for pooled `HostImageMemory` buffers, a pre-faulted and pinned destination pool reused across renders and frames, or prefaulting on an idle thread at startup. The CPU path pays the same zeroing, so judge it on fresh-process single-frame wall, Auto against Off and CPU-only against main.
