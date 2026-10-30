"""Crimson Desert 2.04 archive formats, read-only reference implementation for the QuattroStagioni tools.

Contents: lookup3 hashlittle, .pamt parser (2.04 trie layout), strict LZ4 block decoder,
ChaCha20 key/IV derivation (for encrypted entries), per-mip DDS decoder.
Run Python with -I when reading game data.
"""
import struct

M32 = 0xFFFFFFFF
CD_SEED = 0xC5EDE


# ---------------------------------------------------------------------------- lookup3
def _rot(x, k):
    return ((x << k) | (x >> (32 - k))) & M32


def hashlittle(data, initval=0):
    """Bob Jenkins lookup3 hashlittle(), primary 32-bit result."""
    n = len(data)
    a = b = c = (0xDEADBEEF + n + initval) & M32
    p = 0
    while n > 12:
        x, y, z = struct.unpack_from('<III', data, p)
        a = (a + x) & M32; b = (b + y) & M32; c = (c + z) & M32
        a = (a - c) & M32; a ^= _rot(c, 4);  c = (c + b) & M32
        b = (b - a) & M32; b ^= _rot(a, 6);  a = (a + c) & M32
        c = (c - b) & M32; c ^= _rot(b, 8);  b = (b + a) & M32
        a = (a - c) & M32; a ^= _rot(c, 16); c = (c + b) & M32
        b = (b - a) & M32; b ^= _rot(a, 19); a = (a + c) & M32
        c = (c - b) & M32; c ^= _rot(b, 4);  b = (b + a) & M32
        n -= 12; p += 12
    if n == 0:
        return c
    x, y, z = struct.unpack('<III', bytes(data[p:]) + b'\0' * (12 - n))
    a = (a + x) & M32; b = (b + y) & M32; c = (c + z) & M32
    c ^= b; c = (c - _rot(b, 14)) & M32
    a ^= c; a = (a - _rot(c, 11)) & M32
    b ^= a; b = (b - _rot(a, 25)) & M32
    c ^= b; c = (c - _rot(b, 16)) & M32
    a ^= c; a = (a - _rot(c, 4)) & M32
    b ^= a; b = (b - _rot(a, 14)) & M32
    c ^= b; c = (c - _rot(b, 24)) & M32
    return c


def cd_hash(data):
    return hashlittle(data, CD_SEED)


# ---------------------------------------------------------------------------- pamt
def _varint(b, p):
    v = s = 0
    while True:
        c = b[p]; p += 1
        v |= (c & 0x7F) << s; s += 7
        if not c & 0x80:
            return v, p


def _trie(tab):
    names, p = {}, 0
    while p < len(tab):
        start = p
        back, p = _varint(tab, p)
        ln = tab[p]; p += 1
        names[start] = (names[start - back] if back else '') + tab[p:p + ln].decode('latin1')
        p += ln
    return names


class Pamt:
    """Read-only 2.04 .pamt parser. entries() yields dicts with path, paz, offset, stored, size, flags."""

    def __init__(self, data):
        u32 = lambda o: struct.unpack_from('<I', data, o)[0]
        self.raw = data
        self.self_hash, npaz, self.tag = u32(0), u32(4), u32(8)
        self.computed_hash = cd_hash(data[12:])
        p = 12
        self.paz = [struct.unpack_from('<III', data, p + 12 * i) for i in range(npaz)]
        p += 12 * npaz
        n = u32(p); dirtab = data[p + 4:p + 4 + n]; p += 4 + n
        n = u32(p); filetab = data[p + 4:p + 4 + n]; p += 4 + n
        n = u32(p); self.folders = [struct.unpack_from('<IIII', data, p + 4 + 16 * i) for i in range(n)]; p += 4 + 16 * n
        n = u32(p); self.files = [struct.unpack_from('<IIIIHBB', data, p + 4 + 20 * i) for i in range(n)]; p += 4 + 20 * n
        n = u32(p); exttab = data[p + 4:p + 4 + n]
        self.exts = [e.decode('latin1') for e in exttab.split(b'\0')[:-1]] or ['']
        self.dirnames, self.filenames = _trie(dirtab), _trie(filetab)

    def entries(self):
        for _fh, dno, first, cnt in self.folders:
            d = self.dirnames.get(dno, '') if dno != 0xFFFFFFFF else ''
            for i in range(first, first + cnt):
                no, off, stored, size, pazi, flags, ext = self.files[i]
                name = self.filenames[no] + (self.exts[ext] if ext < len(self.exts) else '')
                path = (d + '/' + name if d else name).lower().lstrip('/')
                yield dict(path=path, paz=pazi, offset=off, stored=stored, size=size, flags=flags, record=i)

    def find(self, path):
        path = path.replace('\\', '/').lower().lstrip('/')
        for e in self.entries():
            if e['path'] == path:
                return e
        return None


# ---------------------------------------------------------------------------- LZ4
def lz4_decode_strict(src, out_size):
    """Pure-Python LZ4 block decoder with LZ4_decompress_safe semantics: the whole input must be
    consumed, the output must be exactly out_size bytes, offsets must be valid. Raises ValueError."""
    out = bytearray()
    p, n = 0, len(src)
    while True:
        if p >= n:
            raise ValueError('truncated block')
        token = src[p]; p += 1
        lit = token >> 4
        if lit == 15:
            while True:
                if p >= n:
                    raise ValueError('truncated literal length')
                c = src[p]; p += 1; lit += c
                if c != 255:
                    break
        if p + lit > n:
            raise ValueError('literals beyond input')
        out += src[p:p + lit]; p += lit
        if p == n:
            break  # last sequence
        if p + 2 > n:
            raise ValueError('truncated offset')
        off = src[p] | (src[p + 1] << 8); p += 2
        ml = token & 15
        if ml == 15:
            while True:
                if p >= n:
                    raise ValueError('truncated match length')
                c = src[p]; p += 1; ml += c
                if c != 255:
                    break
        ml += 4
        if off == 0 or off > len(out):
            raise ValueError('bad match offset %d at output %d' % (off, len(out)))
        if len(out) + ml > out_size:
            raise ValueError('output overflow')
        start = len(out) - off
        if off >= ml:
            out += out[start:start + ml]
        else:
            for i in range(ml):
                out.append(out[start + i])
    if len(out) != out_size:
        raise ValueError('decoded %d bytes, expected %d' % (len(out), out_size))
    return bytes(out)


# ---------------------------------------------------------------------------- ChaCha20 (crypto nibble 3)
_DELTAS = [0, 0x0A0A0A0A, 0x0C0C0C0C, 0x06060606, 0x0E0E0E0E, 0x0A0A0A0A, 0x06060606, 0x02020202]


def chacha_key_iv(basename):
    """Key/IV for entries flagged ChaCha20 with the default key material (lazorr410 scheme, MIT)."""
    seed = cd_hash(basename.lower().encode('utf-8'))
    iv = struct.pack('<I', seed) * 4
    kb = seed ^ 0x60616263
    return b''.join(struct.pack('<I', kb ^ d) for d in _DELTAS), iv


def chacha_xor(data, basename):
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms
    key, iv = chacha_key_iv(basename)
    return Cipher(algorithms.ChaCha20(key, iv), mode=None).encryptor().update(data)


# ---------------------------------------------------------------------------- DDS (compression 1)
_BPB = {b'DXT1': 8, b'ATI1': 8, b'BC4U': 8, b'BC4S': 8, b'DXT3': 16, b'DXT5': 16, b'ATI2': 16, b'BC5U': 16, b'BC5S': 16}
_DXGI_BPB = {70: 8, 71: 8, 72: 8, 79: 8, 80: 8, 81: 8, 73: 16, 74: 16, 75: 16, 76: 16, 77: 16, 78: 16,
             82: 16, 83: 16, 84: 16, 94: 16, 95: 16, 96: 16, 97: 16, 98: 16, 99: 16}


def dds_info(hdr):
    if hdr[:4] != b'DDS ':
        raise ValueError('not DDS')
    h, w = struct.unpack_from('<II', hdr, 12)
    mips = max(1, struct.unpack_from('<I', hdr, 28)[0])
    fourcc = bytes(hdr[84:88])
    hl, bpb, bpp = 128, None, None
    if struct.unpack_from('<I', hdr, 80)[0] & 4:
        if fourcc == b'DX10':
            hl = 148
            bpb = _DXGI_BPB.get(struct.unpack_from('<I', hdr, 128)[0])
        else:
            bpb = _BPB.get(fourcc)
    if bpb is None:
        bpp = struct.unpack_from('<I', hdr, 88)[0]
    sizes = []
    for i in range(mips):
        mw, mh = max(1, w >> i), max(1, h >> i)
        sizes.append(((mw + 3) // 4) * ((mh + 3) // 4) * bpb if bpb else mw * mh * bpp // 8)
    stored = list(struct.unpack_from('<4I', hdr, 0x20))
    return dict(width=w, height=h, mips=mips, fourcc=fourcc, header_size=hl, mip_sizes=sizes, stored=stored)


def decode_per_mip(blob, size):
    """Decodes a compression-1 stored DDS strictly; returns (info, [raw mip bytes...])."""
    info = dds_info(blob)
    p, mips = info['header_size'], []
    for i, raw in enumerate(info['mip_sizes']):
        st = info['stored'][i] if i < 4 else raw
        chunk = blob[p:p + st]
        if len(chunk) != st:
            raise ValueError('mip %d truncated' % i)
        mips.append(lz4_decode_strict(chunk, raw) if st < raw else bytes(chunk))
        if st > raw:
            raise ValueError('mip %d stored > raw' % i)
        p += st
    if p != len(blob):
        raise ValueError('layout uses %d of %d bytes' % (p, len(blob)))
    if info['header_size'] + sum(info['mip_sizes']) != size:
        raise ValueError('decoded size mismatch')
    return info, mips
