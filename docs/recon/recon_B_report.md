# Recon B: Crimson Desert 2.04.00 archive formats and the "Seasons" 1.0.2 breakage

Scope: read-only static analysis of files staged from the user's PC (nothing written back, game not launched).
Seasons.asi was analysed only for interoperability; none of its data is reproduced here beyond format facts.

Working directory: `/tmp/claude-0/-home-claude/2808c463-40d5-5257-b966-1524784d482d/scratchpad/recon_B/`
- `scripts/` parsers and probes (run with `python3 -I`)
- `out/` disassembly, strings, extracted samples, weather-related file list

## 0. TL;DR

| Topic | Finding | Confidence |
|---|---|---|
| Hash | One function for everything: Bob Jenkins lookup3 `hashlittle(data, initval=0xC5EDE)` | Verified on every staged file |
| PAMT self hash | `pamt[0:4] == hashlittle(pamt[12:], 0xC5EDE)` | Verified on 20 base + 9 DLC pamt |
| PAZ hash | `pamt.paz[i].hash == hashlittle(whole .paz file, 0xC5EDE)` | Verified (0013/0, 0016/0, 0001/3) |
| PAPGT | 2.04 layout: 4-byte header, no global hash, 64-byte trailer (signature-like) appended | Header/entries verified; trailer algorithm unknown |
| PAMT 2.04 change | Name tables are now a compact trie (LEB128 relative parent + u8 len), extensions moved to a separate table, extension index stored in the top byte of the old `flags` dword | Verified against Seasons' own old-format parser |
| Compression | low nibble of entry byte 18: 0 none, 1 per-mip DDS (raw header + LZ4 per mip), 2 LZ4 block | Verified by extraction |
| Encryption | high nibble 3 = ChaCha20, key/IV from `hashlittle(basename.lower(), 0xC5EDE)` (lazorr410 scheme). No ICE, names are plaintext | Verified (xml/.material/.txt/.paloc decrypt) |
| Why Seasons finds 0 paths | It parses PAMT name nodes with the pre-2.04 layout (u32 parent, 0xFFFFFFFF root, u8 len). All 95/2010/1/785 paths still exist in 2.04 under the same groups | Verified |
| Why the game refuses to start | Seasons rewrites PAPGT using the old 12-byte header: writes its "own hash" (`hashlittle(papgt[12:])` = 0x0C56CD5B, reproduced exactly) over bytes 4..8, which in 2.04 are entry 0000's flags; plus 4 group hashes change and the 64-byte trailer no longer matches | Reproduced bit-exact |
| Weather/clock/temperature | Hard-coded CrimsonDesert.exe RVAs with a short prologue-byte check, plus fixed global RVAs, plus a heap scan for a vtable pointer. No AOB signature scanning | Disassembled; all RVAs invalid in 2.04 exe layout |

## 1. File formats (patch 2.04.00)

All integers little-endian. `H(x)` = `hashlittle(x, 0xC5EDE)` (lookup3, returns primary `c`).
The asi itself contains the folded constant `lea ebx,[rdx-0x2145E233]` (= 0xDEADBEEF + 0xC5EDE) at asi RVA 0x205E, confirming the seed.

### 1.1 `NNNN/0.pamt` (index of a group)

```
0x00 u32 self_hash        = H(pamt[12:])
0x04 u32 paz_count
0x08 u32 0x610E0232       (constant in all base and DLC pamt; format/version tag, NOT hashed)
0x0C paz table, paz_count x { u32 index; u32 paz_hash = H(entire N.paz); u32 paz_size }
     u32 dir_table_len   ; dir_table   (trie, see below)
     u32 file_table_len  ; file_table  (trie)
     u32 folder_count    ; folder_count x 16 B { u32 folder_hash = H(dirpath); u32 dir_node_off (0xFFFFFFFF = root);
                                                 u32 first_file; u32 file_count }
     u32 file_count      ; file_count x 20 B  { u32 name_node_off; u32 paz_offset; u32 stored_size; u32 size;
                                                 u16 paz_index; u8 flags; u8 ext_index }
     u32 ext_table_len   ; ext_table = "\0.dds\0.xml\0..." (index 0 = no extension)
```
- Trie node (2.04): `varint back` (LEB128; 0 = root, else distance in bytes back to the parent node start within the same table) + `u8 len` + `len` chars. Full name = parent full name + chars. Directory chunks carry their own "/" (e.g. "gamedata", "/stringtable"). File name = trie name + ext_table[ext_index].
- `flags`: low nibble = compression (0 none, 1 per-mip DDS, 2 LZ4 block; lazorr410 also lists 3 custom and 4 zlib, not seen in staged data), high nibble = crypto (3 = ChaCha20). Observed values 0x00, 0x01, 0x02, 0x30, 0x32.
- Pre-2.04 (from lazorr410 `paz_parse.py` and Seasons' parser at asi RVA 0x22C0): node = `u32 parent_offset (0xFFFFFFFF root) + u8 len + chars`, names included extensions, and the record's last dword was `flags` with paz index in the low bits and compression in bits 16..19. Byte 19 (now ext_index) is the new field.
- Empty group pamt (33 B, e.g. dlc_01/0001): hash 0x99B5A2F3, paz_count 0, all tables empty. This value appears in the DLC papgt for every empty group.
- Offsets are u32: data must sit below 4 GB in a paz (Seasons limits itself to 2 GB).

### 1.2 `N.paz`
Plain concatenation of entry blobs, 16-byte aligned, no header. Integrity value lives in the pamt paz table.

### 1.3 Entry payloads
- Type 2: raw LZ4 block (no frame), `lz4.block.decompress(blob, uncompressed_size=size)`.
- Type 1 (DDS only): the 128-byte (or 148-byte DX10) DDS header is stored raw; `dwReserved1[0..3]` (offset 0x20) hold the stored sizes of mips 0..3; a mip whose stored size is below its raw size is an LZ4 block, otherwise raw; mips 4+ are raw. Verified on all 16 DDS of `0001/3.paz` (DXT1/DXT5/BC5, 64x512 to 2048x2048), output starts with `DDS ` and sizes match exactly.
- Crypto 3: ChaCha20 (OpenSSL 16-byte IV form). `seed = H(basename.lower())` where basename includes extension; `iv = pack('<I',seed)*4`; `key = concat(pack('<I', (seed ^ 0x60616263) ^ d) for d in [0,0x0A0A0A0A,0x0C0C0C0C,0x06060606,0x0E0E0E0E,0x0A0A0A0A,0x06060606,0x02020202])`. Decrypt first, then decompress. Verified: `material/dist/*.material` start with BOM + `<Technique`, `paconfig.txt` (crypt+LZ4), `*.paloc` start with `paloc`.
- Names in pamt are NOT encrypted (no BDO-style ICE).

### 1.4 `meta/0.papgt` (group list)
2.04 layout (735 B, base and DLC):
```
0x00 u8 ?, u8 ?, u8 entry_count (0x27 = 39), u8 0x02 (base: 81 16 27 02, DLC: 8B 16 27 02)
0x04 entry_count x 12 B { u32 flags; u32 name_off; u32 pamt_hash }
       flags: byte0 = optional (0/1), bytes1..2 = language mask (0x7FFF = all, 0x0001..0x4000 = one language)
     u32 strtab_len ; strtab = "0000\0" "0001\0" ...
     64 B trailer (high entropy)
```
- All `pamt_hash` values equal `H(pamt[12:])` of the matching group (20/20 base, 9/9 DLC staged).
- The base papgt also lists 0036..0040 (optional=1, single-language), which exist only under dlc/dlc_01.
- Pre-2.04 layout (CDUMM wiki): 12-byte header with `[4:8] = H(papgt[12:])`, entries from 0x0C, no trailer.
- Trailer: not SHA-512/SHA3-512/BLAKE2b/SHA-256/384 of any prefix, not HMAC with papk/paver as key. Most likely an asymmetric signature (64 B fits Ed25519 or ECDSA r||s). Unknown.

### 1.5 Other meta files
- `meta/0.paver` (28 B): `u16 major=2, u16 minor=4, u16 patch=0, u32 count=1, { u32 len=6, "dlc_01", u32 0 }, u32 H(paver[0:24])`. DLC paver is 0.0.0.
- `meta/0.papk` (32 B): 28 opaque bytes + `u32 H(papk[0:28])`. Purpose unknown (key material? obfuscated).
- `dlc/dlc_01/meta/0.paeid` (75 B): `u32 7, "5001840"` (looks like a Steam DLC app id) + 64-byte opaque block (signature-like, same size as the papgt trailer).
- `meta/0.pathc` ("size table" in Seasons' log), 7.5 MB:
  ```
  0x00 u32 0, u32 0, u32 tmpl_size=148, u32 tmpl_count=664, u32 n=311639, u32 0x10, u32 0x3D1
  0x1C tmpl_count x 148-byte DDS header templates
       n x u32 key = H("/" + path.lower())   (sorted ascending, binary search)
       n x 20 B { u32 0xFFFF0000|tmpl_index ; u32 stored size of mips 0..3 (same as the dds dwReserved1 values) }
       1361-byte tail (unknown)
  ```
  Verified for `/object/texture/cd_bg_ground_grass_01.dds`, `/tree/texture/leaf_weed_grass_06a_color.dds`, `/texture/climate_texture_1.dds`. A replacement texture must keep the template (format, dims, mip count) and mip sizes or pathc must be patched too.

### 1.6 Working code
- `scripts/hashes.py`: lookup3 `hashlittle/hashlittle2` plus CRC32/CRC32C/xxHash candidates
- `scripts/pamt.py`: 2.04 pamt parser (`python3 -I pamt.py <0.pamt> [n]`)
- `scripts/papgt.py`: papgt parser (detects 4-byte vs 12-byte header)
- `scripts/pathc.py`: pathc parser with path lookup
- `scripts/cdcrypto.py`: ChaCha20 key/IV derivation
- `scripts/dds_type1.py`: per-mip DDS decoder
- `scripts/extract.py <group_dir> <substring> <outdir> [max]`: full pipeline (decrypt, LZ4, DDS type 1)
- Samples in `out/extract/` and `out/extract2/`

## 2. Why Seasons reports "0 of N season paths found"

`bin64/Seasons/data/seasons.idx` is text, one line per file copy, matching the asi format string
`%15[^|]|%15[^|]|%511[^|]|%llu|%u|%u|%u|%u|%u|%u|%u`:
`season | group | plaintext path | offset in NNNN.dat | stored size | size | flags | mip0..3 stored sizes`.
8678 lines: spring 2892, summer 2893, autumn 2892, winter 1; groups 0000 (95 paths), 0001 (2012), 0002 (1, `texture/climate_texture_1.dds`), 0015 (785). It stores paths, not hashes. The `.dat` files are raw stored blobs (type-1 DDS, `DDS ` magic).

Checked against the 2.04 pamt (`scripts/probe5.py`):
- 0000: 95/95 present, summer copy identical stored size/size/flags for 95/95.
- 0001: 2010/2012 present (missing: `tree/texture/fruit_cattail_01_color.dds`, `..._subsurface.dds`); 1162 of 2010 have a different stored size (textures re-encoded in 2.04, e.g. 239762 vs 239748).
- 0002: present, but the climate map changed format: pre-2.04 512x512 RGBA8 with 10 mips (1398228 B), 2.04 512x512 RGBA8 with 1 mip (1048704 B).
- 0015: 785/785 present and identical.
- No season path is overridden by dlc_01; folder assignment unchanged.

So the paths exist. The failure is in parsing: Seasons' path builder (asi RVA 0x22C0) walks parents with `cmp r9d,-1` / `lea ecx,[r9+5]` / `mov r9d,[table+off]`, i.e. the pre-2.04 node format (u32 parent, u8 len at +4, chars at +5) and full names with extensions. On the 2.04 trie this produces garbage names, so no lookup matches. Hashing did not change (still H with 0xC5EDE) and the paz table/header layout did not change.

## 3. Why the game shows "problem with the game installation"

Seasons still patches the index files in memory even with 0 matches:
1. PAMT: it enlarges the paz-table size of the archive it serves data from. Reproduced exactly for 0002: paz0 size 179105312 changed to 252379136 = region (alignUp(end + 64 MiB, 16 MiB) = 251658240, matching the log's "region at") + alignUp(709326, 64 KiB). The new H(pamt[12:]) is exactly the logged 0x011025C7. Same mechanism gives 414A8CB6 -> 5B57ABAF, etc.
2. PAPGT: it writes the 4 new group hashes, then computes "own hash" with the old layout: `H(papgt[12:])` over the modified file = 0x0C56CD5B (reproduced bit-exact) and writes it to bytes 4..8. In 2.04 those bytes are entry 0000's flags (0x007FFF00, the language mask), which is what the log prints as "own hash 007FFF00". So the 0000 entry is corrupted, and the 64-byte trailer no longer matches the content.
Either change alone is enough for the game's integrity check to fail. Setting `LoadPlugins=0` (current state) avoids it.

## 4. Seasons.asi static analysis

PE x64 DLL, built 2026-10 (pdb `C:\Users\emrec\Desktop\Seasons\asi\bin\Seasons.pdb`), MinHook + HDE64 (per THIRD_PARTY.txt). Imports include d3d11 (`D3D11On12CreateDevice`), d2d1, DWrite, dxgi (`CreateDXGIFactory1`), user32 raw input, Toolhelp thread APIs (MinHook).

File-layer hooks (names passed to MinHook at asi 0x7812): `CreateFileW, CreateFileA, CreateFile2, ReadFile, GetOverlappedResult, GetOverlappedResultEx, CloseHandle, CreateFileMappingW, GetFileSizeEx, GetFileSize, GetFileInformationByHandle, GetFileInformationByHandleEx`. No Nt* hooks. It tracks handles to `\0.pamt`, `\meta\0.papgt`, `\meta\0.pathc` and `N.paz`, patches index bytes as they are read ("read %zu bytes at %llu of %S: %d entries set to %s"), and serves `.dat` bytes for reads beyond the real paz end ("archive %S opened: season data served past %llu"). Files mapped instead of read are reported ("mapping %S - not changed"). It also scans memory for copies of the indexes the game already loaded ("the game's copy of %S found at %p").

Season switching at runtime is GPU-side: it hooks the D3D12 copy commands on the game's queue ("watching the game's texture uploads"), recognises textures by mip-content signatures ("%d %s files known by their mips (%zu signatures)") and overwrites GPU textures with the season copy ("gpu: %s (%s, mip %u) copied into %p"). Overlay (Control Menu) hooks the swap chain's Present1/SetColorSpace1 and draws via D3D11On12 + Direct2D, with an HDR path.

Game-state access: no AOB scanning. It uses fixed RVAs relative to `GetModuleHandleW(NULL)`, waiting up to 60 x 2 s for the code to match:
| CrimsonDesert.exe RVA (pre-2.04 build) | Use | Guard |
|---|---|---|
| 0x17FA700 | HUD weather query, hooked; returns flags Rainy=1, Snowy=2, Windy=4, Fog=8, Cloudy=0x10, Sunny=0x20 | 20 prologue bytes `48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 60` |
| 0x3DC4730 | Weather update, hooked; detour writes floats in `this->[0x60]->[0x18]` at +0x12C/+0x130/+0x134/+0x168/+0x16C (rain/snow/cloud strengths) and `this->[0x60]->[0x20]+0xA8`; reads game hour via `this->[0xE8]` vtable slot +0x100 | 18 bytes `48 89 5C 24 10 55 56 57 48 83 EC 50 C5 F8 29 74 24 40` |
| 0x6A6D218 | u32 game day counter (accepted if < 100000) | none |
| 0x6D69198 | pointer chain `[+0x40] -> [+0xF68] -> u32 @+0x4001C8, +0x4001D0` (climate map parameters) | null checks only |
| 0x6D63848/898/8E8/938 | four floats: snow temperatures and deep-snow thresholds, read and written | none |
| 0x5AD1A10 | vtable/class pointer; VirtualQuery scan of private RW heap pages for objects referencing it (climate maps, e.g. `/texture/climate_texture_1.dds`) | none |

Calendar is computed by the mod itself from the game day/hour ("by the day, 30 days a season"); snow depth history is kept in `snow_depth.txt`; temperatures are applied by editing climate-map texels ("spring -6, ... winter -24 degrees against the game's").

2.04 exe check (staged CrimsonDesert.exe, 333 MB, PE timestamp 1793246886, sections `.text1/.idata/.rsrc/.00cfg/.rodata/.xtext/.rdata/.code(196 MB)/.ecode/.edata`, i.e. a protector layout): at 0x17FA700 the bytes are mid-function (`48 89 74 24 60 33 F6 ...`), at 0x3DC4730 also mid-function; the global RVAs now fall inside `.idata`/`.rsrc`. All hard-coded addresses are invalid for 2.04. The 18-byte weather-update prologue occurs exactly once in `.text1` at RVA 0x967B30 (weak candidate only, unverified); the HUD prologue occurs 43 times (useless as a signature). Dereferencing the old global RVAs could even crash.

## 5. Reusable design for our own seasons mod

1. Do not edit or re-hash PAPGT/PAMT on 2.04. The papgt trailer looks like a signature we cannot regenerate. Any approach that changes index bytes (as Seasons and CDUMM do) is fragile from 2.04 on.
2. Preferred texture path: serve replacement bytes for existing entries without changing the index. In the ReadFile/GetOverlappedResult hook, when a read covers `[paz_offset, paz_offset+stored_size)` of a known entry in `N.paz`, substitute our blob. Constraint: same stored_size (pad LZ4 or store raw if it fits; type-1 DDS lets each mip be raw or LZ4, which makes size matching easy) and identical DDS template/mip sizes so `0.pathc` stays valid. Build the entry map with `pamt.py` logic at startup (hash the path with H, match folder hash and trie name).
3. Runtime season switching: copy Seasons' GPU idea (hook `ID3D12GraphicsCommandList::CopyTextureRegion`/`CopyBufferRegion` on the upload path, identify textures by a hash of mip data) so seasons change without reloading. Optionally rely only on the file hook and accept that a change applies on next stream-in.
4. Game state: use real AOB signatures (32+ bytes, wildcards for rel32/displacements) resolved after the protector has unpacked `.text1`, with a version table keyed by exe PE timestamp/size. Derive globals from RIP-relative instructions found by signature instead of fixed RVAs. Fail closed: if any signature misses, disable that feature and never touch files.
5. Calendar: keep our own day counter driven by the game's clock (hook or sampled), persist in our own file.
6. Climate/temperature: the climate maps (`texture/climate_texture*.dds`, `climatesand_texture.dds`, `snowmask.dds` in group 0002) are now single-mip 512x512 RGBA8; texel edits must target the 2.04 format. Weather data candidates for later study: `object/bin__/prefab/preset/weather*.prefab` (0000), `sequencer/baseseq/contents/weather/*.paseq` (0014), `material/dist/postprocessweatherblend.material` (0003), effects `fx_weather_a__*.pae` (0007). Full list: `out/weather_related_files.txt`.
7. Safety: read-only on disk, all patching in memory, a kill switch, and a log similar to Seasons'.

## 6. Unknown / not verified
- Algorithm and key of the 64-byte papgt trailer and paeid block; meaning of papk bytes 0..27 and papgt header bytes 0..1, 3.
- Whether the game checks paz hashes (full-file lookup3 over ~900 MB) or only sizes at startup.
- Whether 0x967B30 is really the 2.04 weather-update function; the other 2.04 addresses were not located (needs runtime dump after unpacking or a deeper static pass on `.text1`).
- pathc 1361-byte tail and fields 0x10/0x3D1; pathc record field 2 for 1-mip textures (climate shows 1048576).
- Compression types 3/4 were not observed in staged groups.
- The exact pre-2.04 papgt header bytes (taken from CDUMM documentation; we have no pre-2.04 file).

Sources used for cross-checking: lazorr410/crimson-desert-unpacker (`PAZ_DECRYPTION.md`, `python/paz_parse.py`), CDUMM PAPGT page on deepwiki, thegameswiki Crimson Desert modding page.
