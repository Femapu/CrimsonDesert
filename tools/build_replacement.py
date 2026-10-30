#!/usr/bin/env python3
"""Build the phase-2 test replacement for ui/texture/cd_loading_00.dds (QuattroStagioni).

Input : the ORIGINAL stored entry bytes (128-byte DDS header + LZ4 mip 0), 30119 bytes.
Output: test.bin (same size, same header, LZ4 block re-encoded to the exact original size),
        manifest.ini, preview PNGs, mip0_modified.raw.

The image change ("autumn" test): the loading-logo emblem (atlas rect 1,183,135,135) is recoloured
to an autumn-orange gradient by transforming the BC3 colour endpoints of its blocks (indices kept),
and a thin bright-orange ring is drawn around it. All other atlas blocks stay byte-identical.

Usage: python3 -I tools/build_replacement.py --orig <stored.orig> --lz4exact build/qs_lz4exact \
           --out dist/bin64/scripts/QuattroStagioni --preview build/preview
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np  # noqa: E402

import bc3  # noqa: E402
import cdformats as cf  # noqa: E402

# ---- target entry (CrimsonDesert 2.04.00, group 0012) ------------------------------------------
TARGET = dict(
    id='cd_loading_00',
    path='ui/texture/cd_loading_00.dds',
    group='0012',
    pamt_hash=0x96C76FDD,
    paz_index=3,
    paz_size=955464432,
    offset=24565408,
    stored_size=30119,
    size=262272,
    flags=0x01,
)
LOGO_RECT = (1, 183, 135, 135)          # from ui/xml/texture/cd_loading_00.xml ("cd_loading_logo")
RING_CENTER = (65.0, 251.0)             # measured from the emblem's alpha
RING_R_IN, RING_R_OUT = 61.5, 64.5
RING_RGB = (255, 128, 16)
# Luminance -> autumn gradient stops
GRAD = [(0, (36, 10, 0)), (110, (205, 78, 8)), (170, (255, 140, 26)), (255, (255, 222, 140))]


def autumn(rgb):
    """Map colours (..., 3) to the autumn gradient by luminance (piecewise linear)."""
    rgb = np.asarray(rgb, dtype=np.float64)
    lum = rgb[..., 0] * 0.299 + rgb[..., 1] * 0.587 + rgb[..., 2] * 0.114
    out = np.zeros(rgb.shape)
    xs = [g[0] for g in GRAD]
    for ch in range(3):
        out[..., ch] = np.interp(lum, xs, [g[1][ch] for g in GRAD])
    return np.clip(np.rint(out), 0, 255).astype(np.int32)


def modify(raw, w, h):
    data = bytearray(raw)
    blocks = bc3.block_view(data, w, h)
    x, y, rw, rh = LOGO_RECT
    bx0, by0, bx1, by1 = x // 4, y // 4, (x + rw - 1) // 4, (y + rh - 1) // 4
    region = blocks[by0:by1 + 1, bx0:bx1 + 1]
    e0, e1 = bc3.get_endpoints(region)
    n0, n1 = autumn(e0), autumn(e1)
    transparent = (region[..., 0] == 0) & (region[..., 1] == 0)  # alpha endpoints 0/0: block fully clear
    n0[transparent], n1[transparent] = e0[transparent], e1[transparent]
    bc3.set_endpoints(region, n0, n1)  # recolour visible blocks, indices untouched

    # Ring: re-encode only the blocks the annulus touches (supersampled coverage, alpha-over).
    img = bc3.decode_bc3(bytes(data), w, h).astype(np.float64)
    cx, cy = RING_CENTER
    changed = 0
    for by in range(by0, by1 + 1):
        for bx in range(bx0, bx1 + 1):
            ys, xs = np.mgrid[by * 4:by * 4 + 4, bx * 4:bx * 4 + 4]
            cov = np.zeros((4, 4))
            for sy in (0.125, 0.375, 0.625, 0.875):
                for sx in (0.125, 0.375, 0.625, 0.875):
                    r = np.hypot(xs + sx - cx, ys + sy - cy)
                    cov += ((r >= RING_R_IN) & (r <= RING_R_OUT)) / 16.0
            inside = (xs >= x) & (xs < x + rw) & (ys >= y) & (ys < y + rh)
            cov *= inside
            if not cov.any():
                continue
            px = img[by * 4:by * 4 + 4, bx * 4:bx * 4 + 4].copy()
            a_dst = px[..., 3] / 255.0
            a_out = cov + a_dst * (1 - cov)
            for ch in range(3):
                num = RING_RGB[ch] * cov + px[..., ch] * a_dst * (1 - cov)
                px[..., ch] = np.where(a_out > 0, num / np.maximum(a_out, 1e-9), 0)
            px[..., 3] = a_out * 255.0
            blocks[by, bx] = np.frombuffer(bc3.encode_block(np.clip(np.rint(px), 0, 255).astype(np.uint8)), np.uint8)
            changed += 1
    return bytes(data), (bx1 - bx0 + 1) * (by1 - by0 + 1), changed


def save_previews(orig_raw, new_raw, w, h, outdir):
    from PIL import Image
    os.makedirs(outdir, exist_ok=True)
    for name, raw in (('original', orig_raw), ('replacement', new_raw)):
        img = Image.fromarray(bc3.decode_bc3(raw, w, h), 'RGBA')
        bg = Image.new('RGBA', img.size, (24, 26, 40, 255))
        bg.alpha_composite(img)
        bg.convert('RGB').save(os.path.join(outdir, 'cd_loading_00_%s.png' % name))
        x, y, rw, rh = LOGO_RECT
        crop = bg.crop((x, y, x + rw, y + rh)).resize((84, 84), Image.LANCZOS)  # as shown in game
        crop.convert('RGB').save(os.path.join(outdir, 'logo84_%s.png' % name))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--orig', required=True, help='original stored entry bytes (30119 B)')
    ap.add_argument('--lz4exact', required=True, help='path of the qs_lz4exact tool')
    ap.add_argument('--out', required=True, help='output folder (test.bin + manifest.ini)')
    ap.add_argument('--preview', help='folder for PNG previews and mip0_modified.raw')
    args = ap.parse_args()

    orig = open(args.orig, 'rb').read()
    t = TARGET
    assert len(orig) == t['stored_size'], 'unexpected original size %d' % len(orig)
    oh, oh0 = cf.cd_hash(orig), cf.hashlittle(orig, 0)
    info, mips = cf.decode_per_mip(orig, t['size'])
    assert info['fourcc'] == b'DXT5' and info['mips'] == 1 and info['width'] == 512, info
    hl = info['header_size']
    target_block = info['stored'][0]
    assert hl + target_block == len(orig)
    print('original: %s %dx%d %s mips=%d, mip0 stored %d -> raw %d, hash %08X/%08X' % (
        t['path'], info['width'], info['height'], info['fourcc'].decode(), info['mips'], target_block,
        info['mip_sizes'][0], oh, oh0))

    new_raw, nblocks, ring_blocks = modify(mips[0], info['width'], info['height'])
    diff_blocks = sum(1 for i in range(0, len(new_raw), 16) if new_raw[i:i + 16] != mips[0][i:i + 16])
    print('modified: %d logo blocks recoloured, %d ring blocks re-encoded, %d of %d blocks differ' % (
        nblocks, ring_blocks, diff_blocks, len(new_raw) // 16))

    with tempfile.TemporaryDirectory() as td:
        rp, bp = os.path.join(td, 'mip0.raw'), os.path.join(td, 'mip0.lz4')
        open(rp, 'wb').write(new_raw)
        r = subprocess.run([args.lz4exact, rp, str(target_block), bp], capture_output=True, text=True)
        print('qs_lz4exact:', r.stdout.strip())
        if r.returncode != 0:
            sys.exit('exact-size encoding failed: ' + r.stderr)
        block = open(bp, 'rb').read()

    repl = orig[:hl] + block
    assert len(repl) == len(orig)
    # strict re-check with an independent decoder
    _, check = cf.decode_per_mip(repl, t['size'])
    assert check[0] == new_raw
    rh, rh0 = cf.cd_hash(repl), cf.hashlittle(repl, 0)

    os.makedirs(args.out, exist_ok=True)
    open(os.path.join(args.out, 'test.bin'), 'wb').write(repl)
    manifest = """; QuattroStagioni substitution manifest (phase 2 test). Generated by tools/build_replacement.py.
; Every value is verified against the installed game at startup; any difference disables the entry.
[game]
version=2.04.00

[entry.{id}]
enabled=1
description=loading logo recoloured autumn-orange with an orange ring (phase 2 test)
path={path}
group={group}
pamt_hash=0x{pamt_hash:08X}
paz_index={paz_index}
paz_size={paz_size}
offset={offset}
stored_size={stored_size}
size={size}
flags=0x{flags:02X}
orig_hash=0x{oh:08X}
orig_hash0=0x{oh0:08X}
data=test.bin
data_hash=0x{rh:08X}
data_hash0=0x{rh0:08X}
""".format(oh=oh, oh0=oh0, rh=rh, rh0=rh0, **t)
    open(os.path.join(args.out, 'manifest.ini'), 'w', newline='\r\n').write(manifest)
    print('wrote %s (%d bytes, hash %08X/%08X) and manifest.ini' % (os.path.join(args.out, 'test.bin'), len(repl), rh, rh0))

    if args.preview:
        save_previews(mips[0], new_raw, info['width'], info['height'], args.preview)
        open(os.path.join(args.preview, 'mip0_modified.raw'), 'wb').write(new_raw)
        print('previews in', args.preview)


if __name__ == '__main__':
    main()
