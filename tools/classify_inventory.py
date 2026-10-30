#!/usr/bin/env python3
"""Inventory v1: seasonal profiles for every texture of the v0 inventory (paths and numbers only).

  python3 -I tools/classify_inventory.py [data/partial/vegetation_inventory.json] [data/vegetation_classes_v1.json]

Input: the v0 inventory (heuristic classes from path names). Output: the season targets only, each with
  profile  seasonal behaviour (deciduous, conifer, grass, flower, crop_cereal, ...; see docs/dev/CONTRACTS.md)
  looks    which looks change it ("SLAW" = all non summer looks; summer is always vanilla)
  tier     0 global maps, 1 distant/large area, 2 near vegetation, 3 secondary surfaces
  asset    key shared by the colour/subsurface/impostor textures of one asset (same seed, same timing)
plus a summary (counts and bytes per profile/tier) and the reasons entries were excluded.
No game data is read or written: only the v0 json.
"""
import collections
import json
import re
import sys

SRC = sys.argv[1] if len(sys.argv) > 1 else 'data/partial/vegetation_inventory.json'
DST = sys.argv[2] if len(sys.argv) > 2 else 'data/vegetation_classes_v1.json'

COLOUR_ROLES = {'albedo', 'subsurface'}
EXCLUDED_REGIONS = {'desert_dry', 'abyss'}

EVERGREEN_BROAD = ('palm', 'coconut', 'banana', 'banyan', 'mangrove', 'olive', 'camellia', 'buxus', 'bamboo',
                   'rubber', 'cacao', 'ensete', 'sasa', 'jungle', 'laurel', 'holly', 'magnolia', 'eucalyptus',
                   'taro', 'elephant', 'chaya', 'bonsai')
CONIFER = ('pine', 'spruce', 'fir_', 'cypress', 'juniper', 'redcedar', 'redwood', 'cedar', 'yew', 'aleppo',
           'conifer', 'needle', 'larch', 'hemlock', 'heterophylla', 'sequoia')
CEREAL = ('barley', 'wheat', 'corn', 'rapeseed', 'sugarcane', 'rice', 'oat', 'rye', 'millet', 'straw',
          'amaranth', 'cotton')
FLOWER = ('flower', 'daisy', 'marigold', 'amaryllis', 'orchid', 'azalea', 'hibiscus', 'lavender', 'rose',
          'borage', 'tulip', 'poppy', 'lily', 'iris', 'peony', 'sunflower', 'camomile', 'chamomile', 'lupin',
          'bluebell', 'violet', 'scorzonera', 'alchemilla', 'wildflower', 'annuals', 'conospermum', 'salvia')
GRASS = ('grass', 'fescue', 'cogon', 'clover', 'reed', 'cattail', 'weed', 'sedge', 'rush', 'fern', 'herb',
         'sorrel', 'pennyroyal', 'samphire', 'lespedeza', 'artemisia', 'fennel')
SHRUB = ('bush', 'shrub', 'berr', 'rubus', 'thorn', 'azalea', 'buxus', 'rosemary', 'hedge', 'sapling')
VINE = ('ivy', 'hedera', 'vine', 'moss', 'creeper')
UNDERWATER = ('seaweed', 'seacoral', 'seakelp', 'seamustard', 'algae', 'starfish', 'shell', 'coral', 'kelp')
INDOOR = ('flowerpot', '_pot_', 'pot_', 'basket', 'cd_in_', '_in_flower', 'bouquet', 'vase', 'wreath', 'dpf_')
DEAD = ('burnt', 'deadleaves', 'dead_', '_dead', 'rotten', 'dry_', 'withered', 'stump', 'bare')

NON_COLOUR_NAME = re.compile(r'_(normal|n|sp|mask|height|depth|dmap|ao)(_[a-z]{1,2})?\.dds$')
ROLE_SUFFIX = re.compile(r'(_impostor)?_(color|subsurface|normal|depth|specular|height|dmap|mask|sp|n|tr|c|d|mix|ao)'
                         r'(_[a-z]{1,2})?$')


def has(path, words):
    return any(w in path for w in words)


def asset_key(path):
    name = path.rsplit('/', 1)[-1].rsplit('.', 1)[0]
    prev = None
    while prev != name:
        prev, name = name, ROLE_SUFFIX.sub('', name)
    return name


def classify(x):
    """Returns (profile, looks, tier) or (None, reason)."""
    p, cls, role, grp, region = x['path'], x['cls'], x['role'], x['grp'], x['region_hint']

    # global maps: the biggest lever, handled by dedicated recipes
    if cls == 'global_colormap':
        return 'global_colormap', 'SLAW', 0
    if cls == 'climate_map':
        if 'climate_texture_1' in p:
            return 'climate_temperature', 'SLAW', 0
        return None, 'climate map not used (2/sand)'
    if cls in ('snow_ice',):
        return None, 'snow textures (phase B, snow system)'
    if grp == 'terrain_tile':
        if cls == 'terrain_color_tile':
            return 'terrain_color', 'SLAW', 1
        return None, 'terrain data tile (height/normal/region/mask)'
    if grp == 'terrain_global':
        return None, 'terrain region map (data)'

    if region in EXCLUDED_REGIONS:
        return None, 'excluded region ' + region
    if role not in COLOUR_ROLES:
        return None, 'non colour role ' + role
    if NON_COLOUR_NAME.search(p):  # B2: role guessed wrong, the file name says normal/mask/...
        return None, 'non colour map by name'
    if cls in ('prop_nonveg',) or has(p, INDOOR):
        return None, 'indoor or prop'
    if has(p, UNDERWATER) or (cls in ('aquatic', 'impostor_aquatic') and 'water' not in p and 'lily' not in p
                              and 'reed' not in p and 'cattail' not in p):
        return None, 'underwater/aquatic'
    if cls in ('fruit', 'tree_other'):
        return None, 'fruit/other (kept vanilla)'
    if has(p, DEAD):
        return None, 'already dead/burnt'
    if 'cactus' in p or 'desert' in p or 'badland' in p:
        return None, 'arid plant'

    impostor = grp == 'impostor'
    tier = 1 if impostor else 2
    base = cls.replace('impostor_', '')

    if base in ('bark', 'bark_conifer', 'bark_moss'):
        return 'bark', 'W', 3
    if base == 'terrain_ground_grass':
        return 'ground_grass', 'SLAW', 2
    if base == 'terrain_ground_soil':
        return 'ground_soil', 'LW', 3
    if base == 'leaf_litter':
        return 'leaf_litter', 'SLAW', 3
    if base == 'mossy_surface':
        return 'moss_surface', 'LW', 3
    if base == 'ground_decal_veg':
        return 'ground_decal', 'SLAW', 3
    if base == 'object_foliage':
        if has(p, FLOWER):
            return 'flower', 'SLAW', 2
        if has(p, ('crop',)):
            return ('crop_cereal' if has(p, CEREAL) else 'crop_veg'), 'SLAW', 2
        if has(p, VINE):
            return 'vine', 'SLAW', 2
        return 'deciduous', 'SLAW', 2

    if region == 'swamp_jungle' or base == 'evergreen_broadleaf' or has(p, EVERGREEN_BROAD):
        return 'evergreen', 'LW', tier
    if base in ('conifer',) or has(p, CONIFER):
        return 'conifer', 'W', tier
    if base == 'aquatic':
        return 'wetland', 'LAW', tier
    if base == 'crop':
        return ('crop_cereal' if has(p, CEREAL) else 'crop_veg'), 'SLAW', tier
    if base == 'flower' or has(p, FLOWER):
        return 'flower', 'SLAW', tier
    if base in ('grass', 'stem_plant') or has(p, GRASS):
        return 'grass', 'SLAW', tier
    if base == 'bush' or has(p, SHRUB):
        return 'shrub', 'SLAW', tier
    if base == 'vine_moss' or has(p, VINE):
        return 'vine', 'SLAW', tier
    if base in ('broadleaf',):
        return 'deciduous', 'SLAW', tier
    return None, 'unclassified ' + cls


def main():
    src = json.load(open(SRC))
    rows, excluded = [], collections.Counter()
    issues = collections.Counter()
    for x in src['entries']:
        r = classify(x)
        if r[0] is None:
            excluded[r[1]] += 1
            continue
        profile, looks, tier = r
        fmt = x['fmt'] if isinstance(x['fmt'], str) and x['fmt'].isprintable() else None
        if fmt is None:
            issues['unknown format'] += 1
        region = x['region_hint']
        if profile in ('terrain_color', 'global_colormap', 'climate_temperature'):
            region = 'by_tile' if profile == 'terrain_color' else 'world'
        rows.append(dict(path=x['path'], group=x['group'], paz=x['paz'], offset=x['offset'], stored=x['stored'],
                         size=x['size'], fmt=fmt, w=x['w'], h=x['h'], mips=x['mips'],
                         mip_stored=x.get('mip_stored', []), role=x['role'], cls=x['cls'], profile=profile,
                         looks=looks, tier=tier, region=region, asset=asset_key(x['path'])))

    rows.sort(key=lambda r: (r['tier'], r['profile'], r['path']))
    by_profile = collections.defaultdict(lambda: [0, 0, 0])
    by_tier = collections.defaultdict(lambda: [0, 0, 0])
    for r in rows:
        for d, k in ((by_profile, r['profile']), (by_tier, r['tier'])):
            d[k][0] += 1
            d[k][1] += r['size']
            d[k][2] += r['stored']
    total = sum(r['size'] for r in rows)
    summary = dict(
        targets=len(rows), assets=len({r['asset'] for r in rows}),
        bytes_per_look_set=total, mb_per_look_set=round(total / 2**20, 1),
        dlc_targets=sum(r['group'].startswith('dlc') for r in rows),
        by_profile={k: dict(count=v[0], mb=round(v[1] / 2**20, 1)) for k, v in sorted(by_profile.items())},
        by_tier={str(k): dict(count=v[0], mb=round(v[1] / 2**20, 1)) for k, v in sorted(by_tier.items())},
        by_format=dict(collections.Counter(r['fmt'] or 'unknown' for r in rows).most_common()),
        excluded=dict(excluded.most_common()), issues=dict(issues))
    out = dict(
        version=1, game=src.get('game'), source='data/partial/vegetation_inventory.json (v0, not yet verified)',
        note='Paths/offsets/sizes only, no game data. Looks: S spring, L late summer, A autumn, W winter '
             '(summer = vanilla). Verify on the PC with tools/verify_inventory.py.',
        summary=summary, entries=rows)
    with open(DST, 'w') as f:
        json.dump(out, f, indent=0, separators=(',', ':'))
    print(json.dumps(summary, indent=1))


if __name__ == '__main__':
    main()
