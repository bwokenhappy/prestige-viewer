#!/usr/bin/env python3
"""Image-level noise and detail comparison of bench still keys (BenchStillRaw .bin files, slbench.cpp writeStillRaw).

A still key's .bin holds the hold's MEAN image (display RGB, 0..1) and the per-pixel TEMPORAL VARIANCE of the
luminance over the hold (BenchFreezeStills: the world clock stands still, so the variance is rendering noise only).

  still_detail.py noise A.bin [B.bin ...]
      per file: temporal noise of the luminance in 0..255 units -- mean std, p50 / p95 / p99 of the per-pixel std,
      over all pixels and over the EDGE pixels (strong gradient in the reference-free mean: contacts, crevices,
      thin geometry, silhouettes), and the share of pixels whose std exceeds 2 / 4 / 8 units (visible flicker).

  still_detail.py compare REF.bin TEST.bin [--out DIR] [--tile 16]
      TEST's mean image against a REFERENCE mean (many more rays / passes, or a long accumulation):
        err      mean |L_test - L_ref| (0..255) and its p95 over 16 x 16 tiles -- the average error;
        ssim     luminance SSIM (8 x 8 Gaussian window), mean and the worst 1 % of tiles;
        detail   the small-scale structure: a Laplacian-of-Gaussian (sigma 1 px) band of both images. Per tile
                 LOST = energy of the reference's band missing from the test (|band_ref| - |band_test|, positive
                 part), ADDED = the test's band energy the reference does not have; both relative to the
                 reference's band energy over the image. Blur, aliasing loss and half-resolution softening show as
                 LOST; noise, fireflies, stair-steps as ADDED. Worst tiles listed with their pixel position;
        edge     the same errors over the reference's edge pixels only (top 10 % gradient magnitude).
      --out writes heat maps (lost / added / |err|) and the worst tiles' crops (ref | test, x4) as PNGs.

  still_detail.py aa A1.bin A2.bin   the A/A check: 'compare' of two runs of the same build and settings plus the
      difference of their noise figures -- the harness's own floor (both should be near 0).

Units: luminance = Rec. 709 weights of the display values x 255."""
import argparse, json, os, struct, sys
import numpy as np

W709 = np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)


def load(path):
    with open(path, 'rb') as f:
        magic = f.read(8)
        if magic != b'SLSTILL1':
            raise SystemExit('%s: not a still file' % path)
        w, h, frames = struct.unpack('<3I', f.read(12))
        mean = np.frombuffer(f.read(w * h * 12), dtype='<f4').reshape(h, w, 3)
        var = np.frombuffer(f.read(w * h * 4), dtype='<f4').reshape(h, w)
    # SLSTILL1 writes readCapture/LLImageRaw rows verbatim (bottom-up), despite
    # the writer's top-row comment. Normalize both arrays like PNG encoding so
    # heat maps, crops and pixel coordinates agree with the saved still image.
    mean = mean[::-1]
    var = var[::-1]
    return {'w': w, 'h': h, 'frames': frames, 'mean': mean, 'var': var, 'lum': (mean @ W709) * 255.0,
            'std': np.sqrt(np.maximum(var, 0.0)) * 255.0}


def conv1(a, k, axis):
    r = len(k) // 2
    pad = [(0, 0)] * a.ndim
    pad[axis] = (r, r)
    p = np.pad(a, pad, mode='edge')
    out = np.zeros_like(a)
    n = a.shape[axis]
    for i, kv in enumerate(k):
        sl = [slice(None)] * a.ndim
        sl[axis] = slice(i, i + n)
        out += kv * p[tuple(sl)]
    return out


def gauss(a, sigma):
    r = max(1, int(3 * sigma + 0.5))
    x = np.arange(-r, r + 1, dtype=np.float32)
    k = np.exp(-0.5 * (x / sigma) ** 2)
    k /= k.sum()
    return conv1(conv1(a, k, 0), k, 1)


def grad_mag(L):
    gx = np.zeros_like(L); gy = np.zeros_like(L)
    gx[:, 1:-1] = 0.5 * (L[:, 2:] - L[:, :-2])
    gy[1:-1, :] = 0.5 * (L[2:, :] - L[:-2, :])
    return np.sqrt(gx * gx + gy * gy)


def log_band(L):
    g = gauss(L, 1.0)
    lap = np.zeros_like(g)
    lap[1:-1, 1:-1] = g[1:-1, 2:] + g[1:-1, :-2] + g[2:, 1:-1] + g[:-2, 1:-1] - 4.0 * g[1:-1, 1:-1]
    return lap


def tiles(a, t):
    h, w = a.shape
    h2, w2 = h - h % t, w - w % t
    return a[:h2, :w2].reshape(h2 // t, t, w2 // t, t).mean(axis=(1, 3))


def ssim_map(x, y):
    C1, C2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
    mx, my = gauss(x, 1.5), gauss(y, 1.5)
    sxx = gauss(x * x, 1.5) - mx * mx
    syy = gauss(y * y, 1.5) - my * my
    sxy = gauss(x * y, 1.5) - mx * my
    return ((2 * mx * my + C1) * (2 * sxy + C2)) / ((mx * mx + my * my + C1) * (sxx + syy + C2))


def edge_mask(L, q=0.90):
    g = grad_mag(gauss(L, 0.7))
    return g >= np.quantile(g, q)


def noise_figs(s, edge=None):
    std = s['std']
    e = edge if edge is not None else edge_mask(s['lum'])
    out = {'frames': s['frames']}
    for name, m in (('all', np.ones_like(std, dtype=bool)), ('edge', e)):
        v = std[m]
        out[name] = {'mean_std': float(v.mean()), 'p50': float(np.quantile(v, 0.5)), 'p95': float(np.quantile(v, 0.95)),
                     'p99': float(np.quantile(v, 0.99)), 'gt2': float((v > 2).mean()), 'gt4': float((v > 4).mean()),
                     'gt8': float((v > 8).mean())}
    return out


def compare(ref, test, tile=16, out_dir=None, tag='cmp'):
    if (ref['w'], ref['h']) != (test['w'], test['h']):
        raise SystemExit('size mismatch')
    Lr, Lt = ref['lum'], test['lum']
    err = np.abs(Lt - Lr)
    terr = tiles(err, tile)
    ss = ssim_map(Lt, Lr)
    tss = tiles(ss, tile)
    br, bt = log_band(Lr), log_band(Lt)
    ar, at = np.abs(br), np.abs(bt)
    eref = float(ar.mean()) + 1e-6
    lost = np.maximum(ar - at, 0.0)
    added = np.maximum(at - ar, 0.0)
    tl, ta = tiles(lost, tile) / eref, tiles(added, tile) / eref
    em = edge_mask(Lr)
    res = {
        'err_mean': float(err.mean()), 'err_tile_p95': float(np.quantile(terr, 0.95)), 'err_tile_max': float(terr.max()),
        'ssim_mean': float(ss.mean()), 'ssim_tile_p01': float(np.quantile(tss, 0.01)), 'ssim_tile_min': float(tss.min()),
        'detail_lost': float(lost.mean() / eref), 'detail_added': float(added.mean() / eref),
        'detail_lost_tile_p99': float(np.quantile(tl, 0.99)), 'detail_lost_tile_max': float(tl.max()),
        'detail_added_tile_p99': float(np.quantile(ta, 0.99)), 'detail_added_tile_max': float(ta.max()),
        'edge_err_mean': float(err[em].mean()), 'edge_detail_lost': float(lost[em].mean() / eref),
        'edge_detail_added': float(added[em].mean() / eref),
        'band_energy_ratio': float(at.mean() / eref),
    }
    worst = []
    for name, tm in (('lost', tl), ('added', ta), ('err', terr)):
        idx = np.argsort(tm.ravel())[::-1][:6]
        for i in idx:
            ty, tx = divmod(int(i), tm.shape[1])
            worst.append({'kind': name, 'x': tx * tile, 'y': ty * tile, 'value': float(tm[ty, tx])})
    res['worst_tiles'] = worst
    if out_dir:
        from PIL import Image
        os.makedirs(out_dir, exist_ok=True)
        def heat(a, name, scale):
            v = np.clip(a / scale, 0, 1)
            img = np.stack([v, np.sqrt(v) * 0.6, 1 - v], -1) * 255
            Image.fromarray(img.astype(np.uint8)).resize((a.shape[1] * tile // 2, a.shape[0] * tile // 2), Image.NEAREST).save(
                os.path.join(out_dir, '%s_%s.png' % (tag, name)))
        heat(tl, 'lost', max(1e-3, float(np.quantile(tl, 0.999))))
        heat(ta, 'added', max(1e-3, float(np.quantile(ta, 0.999))))
        heat(terr, 'err', max(1e-3, float(np.quantile(terr, 0.999))))
        def crop(s, x, y, r=48):
            m = np.clip(s['mean'] * 255, 0, 255).astype(np.uint8)
            x0, y0 = max(0, x + tile // 2 - r), max(0, y + tile // 2 - r)
            return m[y0:y0 + 2 * r, x0:x0 + 2 * r]
        for k, wt in enumerate(worst):
            a, b = crop(ref, wt['x'], wt['y']), crop(test, wt['x'], wt['y'])
            if a.shape != b.shape or a.size == 0:
                continue
            pair = np.concatenate([a, np.full((a.shape[0], 4, 3), 255, np.uint8), b], 1)
            Image.fromarray(pair).resize((pair.shape[1] * 4, pair.shape[0] * 4), Image.NEAREST).save(
                os.path.join(out_dir, '%s_worst%02d_%s_%d_%d.png' % (tag, k, wt['kind'], wt['x'], wt['y'])))
    return res


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('mode', choices=['noise', 'compare', 'aa'])
    ap.add_argument('files', nargs='+')
    ap.add_argument('--out')
    ap.add_argument('--tile', type=int, default=16)
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args()
    if a.mode == 'noise':
        res = {f: noise_figs(load(f)) for f in a.files}
    elif a.mode == 'compare':
        ref = load(a.files[0])
        res = {}
        em = edge_mask(ref['lum'])
        for f in a.files[1:]:
            t = load(f)
            r = compare(ref, t, a.tile, a.out, os.path.basename(f).replace('.bin', ''))
            r['noise'] = noise_figs(t, em)
            res[f] = r
        res['reference_noise'] = noise_figs(ref, em)
    else:
        s1, s2 = load(a.files[0]), load(a.files[1])
        em = edge_mask(s1['lum'])
        r = compare(s1, s2, a.tile, a.out, 'aa')
        n1, n2 = noise_figs(s1, em), noise_figs(s2, em)
        r['noise_1'], r['noise_2'] = n1, n2
        res = r
    if a.json:
        print(json.dumps(res, indent=1))
    else:
        print(json.dumps(res, indent=1)[:20000])


if __name__ == '__main__':
    main()
