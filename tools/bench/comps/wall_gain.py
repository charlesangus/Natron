#!/usr/bin/env python3
"""Re-expresses render-profile-report.py's net GPU gain in wall-clock terms.

The report compares summed task time with transfer time, but the CPU spreads tasks over many
threads while a transfer is one serial stream. Given a one-thread profile (every task's own cost)
and the measured wall time per frame of the same comp rendered on all cores, this scales each
region's task time by the CPU's effective parallelism before subtracting the transfer.

  wall_gain.py <serial.steady.jsonl> <wall-seconds-per-frame> [--heavy 20] [--speedup 10]
"""

import argparse
import json
import os
import subprocess

REPORT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "render-profile-report.py")
SCENARIOS = (("staging 13.3 GB/s", 13.3), ("DMA 26.6 GB/s", 26.6), ("unified", 0.0))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("profile")
    ap.add_argument("wall_s", type=float)
    ap.add_argument("--heavy", type=float, default=20.0)
    ap.add_argument("--speedup", type=float, default=10.0)
    args = ap.parse_args()

    res = json.loads(subprocess.check_output(
        ["python3", REPORT, args.profile, "--json", "--heavy-ms-per-mpx", str(args.heavy),
         "--gpu-speedup", str(args.speedup)]))[0]
    frames = res["frames"]
    wall_ms = args.wall_s * 1000.0
    parallelism = res["total_task_ms"] / frames / wall_ms
    print("serial %.0f ms/frame, wall %.0f ms/frame, parallelism %.1f, heavy share %.0f%%, %d region(s)/frame"
          % (res["total_task_ms"] / frames, wall_ms, parallelism, 100 * res["heavy_share"],
             res["region_instances"] / frames))
    for name, gbps in SCENARIOS:
        gain = 0.0
        for sig in res["region_signatures"]:
            cpu_wall = sig["cpu_ms"] / sig["frames"] / parallelism
            transfer = sig["boundary_bytes"] / sig["frames"] / (gbps * 1e9) * 1000.0 if gbps else 0.0
            net = cpu_wall * (1 - 1 / args.speedup) - transfer
            if net > 0:
                gain += net * sig["frames"] / frames
        print("  %-18s saves %.0f ms/frame (%.0f%% of wall)" % (name, gain, 100 * gain / wall_ms))


if __name__ == "__main__":
    main()
