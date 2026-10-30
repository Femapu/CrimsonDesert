#!/usr/bin/env python3
"""Browse Crimson Desert 2.04 archive indexes (read-only).

  python3 -I tools/index_browser.py <game_or_staged_dir> search <regex> [--group 0012] [--limit 50]
  python3 -I tools/index_browser.py <game_or_staged_dir> info <path> [--group 0012]
  python3 -I tools/index_browser.py <game_or_staged_dir> extract <path> <out_file> [--group 0012]
  python3 -I tools/index_browser.py <game_or_staged_dir> groups

<game_or_staged_dir> holds NNNN/0.pamt (and dlc/dlc_01/NNNN/0.pamt). 'extract' needs the .paz too;
it decrypts (ChaCha20) and decompresses (LZ4 / per-mip DDS) and writes the plain file.
"""
import argparse
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cdformats as cf  # noqa: E402

COMP = {0: 'none', 1: 'dds-per-mip', 2: 'lz4'}
CRYPT = {0: 'plain', 3: 'chacha20'}


def pamts(root, group=None):
    pats = [os.path.join(root, '[0-9][0-9][0-9][0-9]', '0.pamt'), os.path.join(root, 'dlc', '*', '[0-9]*', '0.pamt')]
    for pat in pats:
        for p in sorted(glob.glob(pat)):
            g = os.path.relpath(os.path.dirname(p), root).replace('\\', '/')
            if group and not g.endswith(group):
                continue
            yield g, p


def describe(g, e, pm):
    sizes = {i: s for i, _h, s in pm.paz}
    return '%-18s paz %-2d @%-10d stored %-9d size %-9d flags %02X (%s, %s) paz_size %s  %s' % (
        g, e['paz'], e['offset'], e['stored'], e['size'], e['flags'], COMP.get(e['flags'] & 15, '?'),
        CRYPT.get(e['flags'] >> 4, '?'), sizes.get(e['paz'], '?'), e['path'])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('root')
    ap.add_argument('cmd', choices=['search', 'info', 'extract', 'groups'])
    ap.add_argument('args', nargs='*')
    ap.add_argument('--group')
    ap.add_argument('--limit', type=int, default=100)
    a = ap.parse_args()

    if a.cmd == 'groups':
        for g, p in pamts(a.root, a.group):
            pm = cf.Pamt(open(p, 'rb').read())
            print('%-18s files %-7d hash %08X (%s) paz %s' % (g, len(pm.files), pm.self_hash,
                  'ok' if pm.self_hash == pm.computed_hash else 'BAD', [(i, s) for i, _h, s in pm.paz][:8]))
        return
    if not a.args:
        sys.exit('missing argument')
    shown = 0
    for g, p in pamts(a.root, a.group):
        pm = cf.Pamt(open(p, 'rb').read())
        if a.cmd == 'search':
            rx = re.compile(a.args[0], re.I)
            for e in pm.entries():
                if rx.search(e['path']):
                    print(describe(g, e, pm))
                    shown += 1
                    if shown >= a.limit:
                        return
        else:
            e = pm.find(a.args[0])
            if not e:
                continue
            print(describe(g, e, pm))
            print('  pamt hash %08X, record %d, pathc key %08X' % (pm.self_hash, e['record'],
                  cf.cd_hash(('/' + e['path']).encode())))
            if a.cmd == 'extract':
                paz = os.path.join(os.path.dirname(p), '%d.paz' % e['paz'])
                with open(paz, 'rb') as f:
                    f.seek(e['offset'])
                    blob = f.read(e['stored'])
                if e['flags'] >> 4 == 3:
                    blob = cf.chacha_xor(blob, e['path'].split('/')[-1])
                comp = e['flags'] & 15
                if comp == 2:
                    blob = cf.lz4_decode_strict(blob, e['size'])
                elif comp == 1:
                    info, mips = cf.decode_per_mip(blob, e['size'])
                    blob = bytes(blob[:info['header_size']]) + b''.join(mips)
                open(a.args[1], 'wb').write(blob)
                print('  extracted %d bytes to %s' % (len(blob), a.args[1]))
            shown += 1
    if not shown:
        print('no match')


if __name__ == '__main__':
    main()
