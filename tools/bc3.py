"""Minimal BC3 (DXT5) block codec helpers used by the QuattroStagioni offline tools.

Only what phase 2 needs: decode a BC3 surface to RGBA8 (for previews and verification) and
re-colour BC3 blocks in place (endpoint transform, indices untouched) or write solid blocks.
"""
import numpy as np


def _565_to_rgb(c):
    r = (c >> 11) & 31
    g = (c >> 5) & 63
    b = c & 31
    return np.stack([(r * 255 + 15) // 31, (g * 255 + 31) // 63, (b * 255 + 15) // 31], -1).astype(np.int32)


def rgb_to_565(rgb):
    rgb = np.clip(np.asarray(rgb, dtype=np.int32), 0, 255)
    r = (rgb[..., 0] * 31 + 127) // 255
    g = (rgb[..., 1] * 63 + 127) // 255
    b = (rgb[..., 2] * 31 + 127) // 255
    return ((r << 11) | (g << 5) | b).astype(np.uint16)


def decode_bc3(data, w, h):
    """Return an (h, w, 4) uint8 RGBA image from raw BC3 blocks."""
    bw, bh = (w + 3) // 4, (h + 3) // 4
    blk = np.frombuffer(data, dtype=np.uint8, count=bw * bh * 16).reshape(bh, bw, 16)
    a0 = blk[..., 0].astype(np.int32)
    a1 = blk[..., 1].astype(np.int32)
    abits = np.zeros((bh, bw), dtype=np.uint64)
    for i in range(6):
        abits |= blk[..., 2 + i].astype(np.uint64) << np.uint64(8 * i)
    # alpha palette (8 entries)
    pal_a = np.zeros((bh, bw, 8), dtype=np.int32)
    pal_a[..., 0] = a0
    pal_a[..., 1] = a1
    big = a0 > a1
    for i in range(1, 7):
        pal_a[..., 1 + i] = np.where(big, ((7 - i) * a0 + i * a1) // 7, 0)
    for i in range(1, 5):
        pal_a[..., 1 + i] = np.where(big, pal_a[..., 1 + i], ((5 - i) * a0 + i * a1) // 5)
    pal_a[..., 6] = np.where(big, pal_a[..., 6], 0)
    pal_a[..., 7] = np.where(big, pal_a[..., 7], 255)
    c0 = blk[..., 8].astype(np.int32) | (blk[..., 9].astype(np.int32) << 8)
    c1 = blk[..., 10].astype(np.int32) | (blk[..., 11].astype(np.int32) << 8)
    cbits = (blk[..., 12].astype(np.uint32) | (blk[..., 13].astype(np.uint32) << 8) |
             (blk[..., 14].astype(np.uint32) << 16) | (blk[..., 15].astype(np.uint32) << 24))
    e0, e1 = _565_to_rgb(c0), _565_to_rgb(c1)
    # BC2/BC3 colour blocks are always 4-colour (no punch-through mode)
    pal_c = np.stack([e0, e1, (2 * e0 + e1) // 3, (e0 + 2 * e1) // 3], 2)  # (bh,bw,4,3)
    out = np.zeros((bh * 4, bw * 4, 4), dtype=np.uint8)
    for py in range(4):
        for px in range(4):
            k = py * 4 + px
            ci = ((cbits >> np.uint32(2 * k)) & np.uint32(3)).astype(np.int64)
            ai = ((abits >> np.uint64(3 * k)) & np.uint64(7)).astype(np.int64)
            rgb = np.take_along_axis(pal_c, ci[..., None, None].repeat(3, -1), 2)[..., 0, :]
            a = np.take_along_axis(pal_a, ai[..., None], 2)[..., 0]
            out[py::4, px::4, :3] = rgb
            out[py::4, px::4, 3] = a
    return out[:h, :w]


def block_view(data, w, h):
    """Writable (bh, bw, 16) uint8 view over a bytearray of BC3 blocks."""
    bw, bh = (w + 3) // 4, (h + 3) // 4
    return np.frombuffer(data, dtype=np.uint8, count=bw * bh * 16).reshape(bh, bw, 16)


def get_endpoints(blk):
    c0 = blk[..., 8].astype(np.int32) | (blk[..., 9].astype(np.int32) << 8)
    c1 = blk[..., 10].astype(np.int32) | (blk[..., 11].astype(np.int32) << 8)
    return _565_to_rgb(c0), _565_to_rgb(c1)


def set_endpoints(blk, rgb0, rgb1):
    c0 = rgb_to_565(rgb0)
    c1 = rgb_to_565(rgb1)
    blk[..., 8] = c0 & 0xFF
    blk[..., 9] = c0 >> 8
    blk[..., 10] = c1 & 0xFF
    blk[..., 11] = c1 >> 8


def solid_block(rgb, alpha):
    """16-byte BC3 block of a single colour and alpha."""
    c = int(rgb_to_565(np.array(rgb)))
    return bytes([alpha, alpha, 0, 0, 0, 0, 0, 0, c & 0xFF, c >> 8, c & 0xFF, c >> 8, 0, 0, 0, 0])


def _alpha_palette(a0, a1):
    if a0 > a1:
        return [a0, a1] + [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)]
    return [a0, a1] + [((5 - i) * a0 + i * a1) // 5 for i in range(1, 5)] + [0, 255]


def encode_block(rgba):
    """Simple BC3 encoder for one 4x4 RGBA block (uint8 array (4,4,4)) -> 16 bytes.
    Alpha: min/max endpoints, nearest of 8 levels. Colour: endpoints from the extremes along the
    principal axis of the (alpha-weighted) pixels, nearest of 4 palette entries."""
    px = rgba.reshape(16, 4).astype(np.int32)
    a = px[:, 3]
    a0, a1 = int(a.max()), int(a.min())
    if a0 == a1:
        abits = 0
        a_bytes = [a0, a1]
    else:
        pal = np.array(_alpha_palette(a0, a1))
        idx = np.abs(a[:, None] - pal[None, :]).argmin(1)
        abits = 0
        for i, v in enumerate(idx):
            abits |= int(v) << (3 * i)
        a_bytes = [a0, a1]
    rgb = px[:, :3].astype(np.float64)
    w = np.maximum(a, 1).astype(np.float64)
    mean = (rgb * w[:, None]).sum(0) / w.sum()
    d = rgb - mean
    cov = (d * w[:, None]).T @ d
    axis = np.linalg.eigh(cov)[1][:, -1] if cov.any() else np.array([1.0, 1.0, 1.0]) / np.sqrt(3)
    proj = d @ axis
    hi, lo = rgb[proj.argmax()], rgb[proj.argmin()]
    c0, c1 = int(rgb_to_565(hi)), int(rgb_to_565(lo))
    e0, e1 = _565_to_rgb(np.array(c0)), _565_to_rgb(np.array(c1))
    pal = np.stack([e0, e1, (2 * e0 + e1) // 3, (e0 + 2 * e1) // 3]).astype(np.int32)
    cidx = ((px[:, None, :3] - pal[None, :, :]) ** 2).sum(2).argmin(1)
    cbits = 0
    for i, v in enumerate(cidx):
        cbits |= int(v) << (2 * i)
    out = bytes(a_bytes) + abits.to_bytes(6, 'little') + bytes([c0 & 0xFF, c0 >> 8, c1 & 0xFF, c1 >> 8])
    return out + cbits.to_bytes(4, 'little')
