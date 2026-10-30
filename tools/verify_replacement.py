#!/usr/bin/env python3
"""Offline verification of a QuattroStagioni replacement against the original entry and the indexes.

  python3 -I tools/verify_replacement.py --manifest dist/bin64/scripts/QuattroStagioni/manifest.ini \
      --orig tests/data/cd_loading_00.stored.orig [--pamt <0012/0.pamt>] [--pathc <meta/0.pathc>]

Checks (all must pass):
  * original bytes hash to the manifest values; replacement (data=) hashes to its manifest values
  * same stored length; per-mip DDS: identical 128/148-byte header => identical template + mip sizes
  * both decode strictly (own pure-Python LZ4 decoder, input fully consumed, exact sizes) and, if the
    'lz4' module is installed, also with the reference C decoder (lz4.block.decompress)
  * the decoded replacement is a valid DDS of identical header/dimensions/format/mip count
  * optional: the pamt record equals the manifest; the pathc record matches the header mip sizes
"""
import argparse
import bisect
import configparser
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cdformats as cf  # noqa: E402

ok = True


def check(cond, msg):
    global ok
    print(('  OK   ' if cond else '  FAIL ') + msg)
    ok &= bool(cond)


def num(v):
    return int(v, 0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--manifest', required=True)
    ap.add_argument('--orig', required=True)
    ap.add_argument('--pamt')
    ap.add_argument('--pathc')
    a = ap.parse_args()

    cp = configparser.ConfigParser(inline_comment_prefixes=(';',))
    cp.read(a.manifest)
    for sec in [s for s in cp.sections() if s.startswith('entry.')]:
        m = cp[sec]
        print('[%s] %s' % (sec, m['path']))
        orig = open(a.orig, 'rb').read()
        repl = open(os.path.join(os.path.dirname(a.manifest), m['data']), 'rb').read()
        size, flags = num(m['size']), num(m['flags'])
        check(len(orig) == num(m['stored_size']), 'original length %d == stored_size' % len(orig))
        check(len(repl) == len(orig), 'replacement length %d == original length' % len(repl))
        check(cf.cd_hash(orig) == num(m['orig_hash']) and cf.hashlittle(orig, 0) == num(m['orig_hash0']),
              'original hashes %08X/%08X' % (cf.cd_hash(orig), cf.hashlittle(orig, 0)))
        check(cf.cd_hash(repl) == num(m['data_hash']) and cf.hashlittle(repl, 0) == num(m['data_hash0']),
              'replacement hashes %08X/%08X' % (cf.cd_hash(repl), cf.hashlittle(repl, 0)))
        if flags >> 4:
            print('  (encrypted entry: decode checks need the ChaCha20 layer, skipped)')
            continue
        if flags & 15 == 1:
            io_, mo = cf.decode_per_mip(orig, size)
            ir, mr = cf.decode_per_mip(repl, size)
            hl = io_['header_size']
            check(orig[:hl] == repl[:hl], 'DDS header identical (%d bytes): %dx%d %s mips=%d stored mip sizes %s' % (
                hl, ir['width'], ir['height'], ir['fourcc'].decode(), ir['mips'], ir['stored'][:ir['mips']]))
            check([len(x) for x in mr] == io_['mip_sizes'], 'strict pure-Python decode of every mip (sizes %s)' %
                  [len(x) for x in mr])
            changed = sum(1 for x, y in zip(mo, mr) if x != y)
            check(changed > 0, '%d mip(s) differ from the original' % changed)
            try:
                import lz4.block
                p = hl
                for i, raw in enumerate(ir['mip_sizes']):
                    st = ir['stored'][i] if i < 4 else raw
                    if st < raw:
                        d = lz4.block.decompress(repl[p:p + st], uncompressed_size=raw)
                        check(d == mr[i], 'reference C LZ4 (lz4.block) decodes mip %d to the same %d bytes' % (i, raw))
                    p += st
            except ImportError:
                print('  (python lz4 module not installed: reference C decoder check skipped)')
            if a.pathc:
                d = open(a.pathc, 'rb').read()
                _, _, tsize, tcount, n = struct.unpack_from('<5I', d, 0)
                keys_off = 0x1C + tsize * tcount
                keys = struct.unpack_from('<%dI' % n, d, keys_off)
                k = cf.cd_hash(('/' + m['path']).encode())
                i = bisect.bisect_left(keys, k)
                found = i < n and keys[i] == k
                check(found, 'pathc has key %08X' % k)
                if found:
                    rec = struct.unpack_from('<5I', d, keys_off + 4 * n + 20 * i)
                    tmpl = d[0x1C + tsize * (rec[0] & 0xFFFF):0x1C + tsize * (rec[0] & 0xFFFF) + tsize]
                    check(list(rec[1:]) == ir['stored'], 'pathc mip sizes %s == header %s' % (list(rec[1:]), ir['stored']))
                    same = tmpl[:0x20] == repl[:0x20] and tmpl[0x30:hl] == repl[0x30:hl]
                    check(same, 'pathc template %d matches the header (outside the mip-size fields)' % (rec[0] & 0xFFFF))
        elif flags & 15 == 2:
            check(len(cf.lz4_decode_strict(repl, size)) == size, 'strict LZ4 decode to %d bytes' % size)
        if a.pamt:
            pm = cf.Pamt(open(a.pamt, 'rb').read())
            e = pm.find(m['path'])
            check(pm.self_hash == pm.computed_hash == num(m['pamt_hash']), 'pamt self hash %08X' % pm.self_hash)
            check(e is not None and (e['paz'], e['offset'], e['stored'], e['size'], e['flags']) ==
                  (num(m['paz_index']), num(m['offset']), num(m['stored_size']), size, flags),
                  'pamt record matches manifest: %s' % (e,))
            sizes = {i: s for i, _h, s in pm.paz}
            check(sizes.get(num(m['paz_index'])) == num(m['paz_size']), 'paz size %s' % sizes.get(num(m['paz_index'])))
    print('RESULT:', 'ALL CHECKS PASSED' if ok else 'FAILED')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
