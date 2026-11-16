"""Compares the deep EXRs that run.sh produced and sizes a banded DeepFrame payload."""

import math
import os
import sys

import OpenImageIO as oiio

FLOAT = 4
DCX_EXTRA = 8 + 4


def read_deep(path):
    inp = oiio.ImageInput.open(path)
    if not inp:
        return None
    spec = inp.spec()
    dd = inp.read_native_deep_image()
    inp.close()
    if dd is None:
        return None
    names = [dd.channelname(c) for c in range(dd.channels)]
    return spec, dd, names


def read_flat(path):
    if not os.path.exists(path):
        return None
    buf = oiio.ImageBuf(path)
    pixels = buf.get_pixels(oiio.FLOAT)
    if pixels is None:
        return None
    return buf.spec(), pixels


def flatten(spec, dd, names):
    w, h = spec.width, spec.height
    idx = {n: names.index(n) for n in ("R", "G", "B", "A") if n in names}
    zc = names.index("Z") if "Z" in names else None
    rgba = [[[0.0] * 4 for _ in range(w)] for _ in range(h)]
    counts = [[0] * w for _ in range(h)]
    for p in range(w * h):
        n = dd.samples(p)
        y, x = divmod(p, w)
        counts[y][x] = n
        if n == 0:
            continue
        order = sorted(range(n), key=lambda s: dd.deep_value(p, zc, s)) if zc is not None else range(n)
        acc = [0.0, 0.0, 0.0, 0.0]
        for s in order:
            a = dd.deep_value(p, idx["A"], s)
            t = 1.0 - acc[3]
            for k, ch in enumerate(("R", "G", "B")):
                acc[k] += t * dd.deep_value(p, idx[ch], s)
            acc[3] += t * a
        rgba[y][x] = acc
    return rgba, counts


def rmse(a, b, w, h, channels, flip):
    s, n = 0.0, 0
    for y in range(h):
        by = h - 1 - y if flip else y
        for x in range(w):
            for c in range(channels):
                d = a[y][x][c] - float(b[by][x][c])
                s += d * d
                n += 1
    return math.sqrt(s / n) if n else float("nan")


def compare(label, rgba, flat, w, h):
    if flat is None:
        print(f"  {label}: reference missing")
        return None
    spec, pixels = flat
    if spec.width != w or spec.height != h or spec.nchannels < 4:
        print(f"  {label}: reference is {spec.width}x{spec.height}x{spec.nchannels}")
        return None
    up = rmse(rgba, pixels, w, h, 4, False)
    flip = rmse(rgba, pixels, w, h, 4, True)
    best = min(up, flip)
    print(f"  {label}: rgba rmse={best:.5f} ({'rows flipped' if flip < up else 'same row order'})")
    return best


def compare_complete(label, rgba, flat, w, h):
    """Restricts the comparison to pixels whose deep alpha matches the flat alpha, i.e. where no
    surface seen through a presence < 1 hit is missing from the deep data."""
    if flat is None:
        return
    pixels = flat[1]
    s, n, skipped = 0.0, 0, 0
    for y in range(h):
        for x in range(w):
            ref = pixels[y][x]
            if abs(rgba[y][x][3] - float(ref[3])) > 1e-3:
                skipped += 1
                continue
            for c in range(4):
                d = rgba[y][x][c] - float(ref[c])
                s += d * d
                n += 1
    r = math.sqrt(s / n) if n else float("nan")
    print(f"  {label}: rgba rmse={r:.5f} over {n // 4} px; {skipped} px have less deep alpha than flat alpha")


def sample_diff(a, b):
    worst, mismatched = 0.0, 0
    (_, da, na), (_, db, nb) = a, b
    if na != nb:
        return None
    zc = na.index("Z")
    for p in range(da.pixels):
        n = da.samples(p)
        if n != db.samples(p):
            mismatched += 1
            continue
        oa = sorted(range(n), key=lambda s: da.deep_value(p, zc, s))
        ob = sorted(range(n), key=lambda s: db.deep_value(p, zc, s))
        for sa, sb in zip(oa, ob):
            for c in range(da.channels):
                worst = max(worst, abs(da.deep_value(p, c, sa) - db.deep_value(p, c, sb)))
    return worst, mismatched


def histogram(counts):
    hist = {}
    for row in counts:
        for n in row:
            key = n if n < 4 else 4
            hist[key] = hist.get(key, 0) + 1
    return " ".join(f"{'4+' if k == 4 else k}:{hist[k]}" for k in sorted(hist))


def band_sizes(counts, w, per_sample, rows_list):
    h = len(counts)
    out = []
    for rows in rows_list:
        sizes = []
        for y0 in range(0, h, rows):
            samples = sum(sum(counts[y]) for y in range(y0, min(h, y0 + rows)))
            sizes.append(rows * w * 4 + samples * per_sample)
        out.append((rows, len(sizes), max(sizes), sum(sizes)))
    return out


def describe(label, path):
    got = read_deep(path)
    if got is None:
        print(f"{label}: no deep image at {path}")
        return None
    spec, dd, names = got
    w, h = spec.width, spec.height
    rgba, counts = flatten(spec, dd, names)
    total = sum(sum(r) for r in counts)
    print(f"{label}: {w}x{h} channels={names} samples total={total} mean/px={total / (w * h):.3f} "
          f"max/px={max(max(r) for r in counts)}")
    print(f"  per-pixel sample histogram {histogram(counts)}")
    alphas = [dd.deep_value(p, names.index("A"), s) for p in range(w * h) for s in range(dd.samples(p))]
    if alphas:
        partial = sum(1 for a in alphas if a < 0.999)
        print(f"  sample alpha min={min(alphas):.4f} partial(<1)={partial}/{len(alphas)}")
    return spec, rgba, counts, total, names


def main(out):
    j = lambda name: os.path.join(out, name)
    status = 0
    prog = describe("progressive deep (mcrt frame-complete dump)", j("progressive_beauty.exr"))
    batch = describe("batch deep", j("batch_deep.exr"))
    if prog is None:
        status = 1
    else:
        spec, rgba, counts, total, names = prog
        w, h = spec.width, spec.height
        compare("flatten(progressive deep) vs Hydra color over Arras", rgba, read_flat(j("progressive_hydra_color.exr")), w, h)
        compare("flatten(progressive deep) vs batch beauty", rgba, read_flat(j("batch_main.exr")), w, h)
        per_sample = len(names) * FLOAT
        print(f"DeepFrame sizing at {w}x{h} ({len(names)} float32 channels = {per_sample} B/sample, "
              f"+{DCX_EXTRA} B/sample with a DCX mask and flags, +4 B/pixel count):")
        for rows, n, biggest, whole in band_sizes(counts, w, per_sample, (16, 64, h)):
            print(f"  {rows:4d}-row bands: {n} messages, largest {biggest / 1024:.1f} KiB, frame {whole / 1024:.1f} KiB")
        mean = total / (w * h)
        for W, H in ((1920, 1080),):
            frame = W * H * 4 + mean * W * H * per_sample
            dcx = frame + mean * W * H * DCX_EXTRA
            print(f"  extrapolated {W}x{H} at {mean:.3f} samples/px: {mean * W * H / 1e6:.2f} M samples, "
                  f"{frame / 2**20:.1f} MiB ({dcx / 2**20:.1f} MiB with DCX); "
                  f"64-row band ~{frame * 64 / H / 2**20:.2f} MiB")
        compare_complete("flatten(progressive deep) vs batch beauty, complete pixels", rgba,
                         read_flat(j("batch_main.exr")), w, h)
    if batch is not None:
        spec, rgba, counts, total, names = batch
        compare("flatten(batch deep) vs batch beauty", rgba, read_flat(j("batch_main.exr")), spec.width, spec.height)
        compare_complete("flatten(batch deep) vs batch beauty, complete pixels", rgba,
                         read_flat(j("batch_main.exr")), spec.width, spec.height)
        if prog is not None:
            d = sample_diff(read_deep(j("progressive_beauty.exr")), read_deep(j("batch_deep.exr")))
            if d is not None:
                print(f"  progressive vs batch deep: max |sample value diff|={d[0]:.5f}, "
                      f"pixels with different sample counts={d[1]}")
        if prog is not None:
            diff = sum(abs(a - b) for ra, rb in zip(prog[2], counts) for a, b in zip(ra, rb))
            print(f"  per-pixel sample-count |progressive - batch| summed={diff} "
                  f"(totals {prog[3]} vs {total})")
    for tag in ("progressive", "flat"):
        p = read_flat(j(f"{tag}_hydra_color.exr"))
        if p is None:
            print(f"{tag}: no Hydra color")
    a, b = read_flat(j("progressive_hydra_color.exr")), read_flat(j("flat_hydra_color.exr"))
    if a and b and a[0].width == b[0].width:
        s = sum((float(x) - float(y)) ** 2 for ra, rb in zip(a[1], b[1]) for pa, pb in zip(ra, rb)
                for x, y in zip(pa, pb))
        n = a[0].width * a[0].height * a[0].nchannels
        print(f"Hydra color with deep RO vs without: rmse={math.sqrt(s / n):.6f}")
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
