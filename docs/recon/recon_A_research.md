# Recon A: Crimson Desert "dynamic seasons" mod landscape (as of 2026-10-30)

Scope: web and source research for writing our own seasons mod for Crimson Desert (Steam app 3321460), targeting patch 2.04.00 (released 2026-10-29 with the "Charting the Unknown" DLC). Nothing here involves redistributing or patching Khione95's work.

Legend: **[C]** = confirmed from a primary source (code, official page, API data). **[R]** = reported by a secondary source (news site, AI-assisted wiki, forum). **[S]** = my speculation / inference, not verified.

Local shallow clones made during this research (read-only reference, not redistributed):
- `/home/claude/lukefz/pycrimson`
- `/home/claude/ext/potter420_crimson-rs`
- `/home/claude/ext/exodiaprivate-eng_dmm-parser`
- `/home/claude/ext/hzeemr_crimsonforge`
- `/home/claude/ext/nattkh` (NattKh save editor + game mods)
- `/home/claude/ext/lazorr410_crimson-desert-unpacker`
- `/home/claude/ext/cdumm` (CDUMM mod manager)
- `/home/claude/ext/tkhquang_cdt` (CrimsonDesertTools ASI mods)
- `/home/claude/ext/Khione95_Seasons-of-Pywel`
- `/home/claude/ext/nostyx_cw` (Crimson Weather)

---

## 1. Seasons of Pywel (Nexus mod 3630)

### Access method
- Direct WebFetch/curl of https://www.nexusmods.com/crimsondesert/mods/3630?tab=files and the description tab: **blocked** (Cloudflare "Just a moment..." 403).
- Data below came from the public Nexus GraphQL API (`https://api.nexusmods.com/v2/graphql`, queries `legacyModsByDomain` and `modFiles`, game id 8969) **[C]**, plus the author's GitHub repo **[C]**.

### Version and dates [C]
| File id | Version | Category | Upload (UTC) |
|---|---|---|---|
| 16677 | 1.0 | OLD_VERSION | 2026-10-01 20:29 |
| 16680 | 1.0.1 | OLD_VERSION | 2026-10-01 21:43 |
| 16705 | 1.0.2 | OLD_VERSION | 2026-10-02 16:13 |
| 16881 | 1.0.3 | OLD_VERSION | 2026-10-05 21:13 |
| 17303 | **1.0.4** | MAIN | **2026-10-17 12:03** |

- Note: the user referred to v1.0.2; the current release is **1.0.4** (mod `updatedAt` 2026-10-17T12:03:50Z). Size ~1.24 GB (1,238,282 KB). 221 endorsements, 7,841 downloads at query time. Category: Gameplay.
- GitHub repo last commit: 2026-10-17 "1.0.4: less rain again; the desert keeps the game's own temperatures and weather". https://github.com/Khione95/Seasons-of-Pywel
- **No 2.04-compatible update exists or is announced** as of 2026-10-30 (no file newer than 2026-10-17; no mention of 2.04 or the DLC in the description). **[C]**
- Its own description states: "After a game update, parts the mod cannot find are turned off (see the log) until the mod is updated." **[C]** So "broke with 2.04" likely means partial self-disable rather than a crash **[S]**.
- Crimson Weather 0.8.2 (mod 632) added "a minimal compatibility patch for Seasons of Pywel" **[C]** (Nexus description via GraphQL).

### Requirements / install [C]
- ASI plugin `Seasons.asi` (x64) + data folder `bin64\Seasons\` (season textures in `bin64\Seasons\data`).
- Manual: **Ultimate ASI Loader as `winmm.dll` in `bin64`**. Or via DMM (Definitive Mod Manager) ASI tab, which "comes with its own ASI loader".
- Config: `bin64\Seasons.ini` (hotkey, `[Testing] ControlMenu`), `bin64\Seasons\Seasons.ini` (settings), `bin64\Seasons\snow_depth.txt` (per-save snow depth keyed by in-game day*24+hour), `bin64\Seasons\Seasons.log`.
- Compatible with ReShade / RenoDX; works with other ASI plugins.

### Technique as described by the author (README + research notes) [C]
Source: https://github.com/Khione95/Seasons-of-Pywel (README.md, docs/research-notes.md, asi/src/*.cpp). Built with MinHook, VS 2026 toolset v145.
- **archive.cpp**: hooks kernelbase file APIs (`CreateFileW/A`, `CreateFile2`, `ReadFile`, `GetOverlappedResult(Ex)`, `CloseHandle`, `CreateFileMappingW`, `GetFileSize(Ex)`, `GetFileInformationByHandle(Ex)`). Patches the game's `<group>/0.pamt` (file index), `meta/0.pathc` (texture header/size table) and `meta/0.papgt` (group list) **as they are read**, and again in memory on a season change. "The season data sits in regions past the end of the game's archives" (i.e. virtual extension of `.paz` files; offsets in the patched PAMT point past EOF and reads are served from `bin64\Seasons\data`). "No game files changed on disk."
- **gpu.cpp**: identifies long-lived textures (far trees/impostors, far terrain, climate texture) by the first 64 bytes of each mip in D3D12 `CopyTextureRegion` uploads; on a season change re-uploads the season's mips through the game's queue; tracks texture release via a private-data object.
- **climate.cpp**: writes season values into the game's **CPU climate maps** (class `PAClimateTexture`, 512x512 u32 texels, 80 m per texel) live. Two copies: weather manager +0x48 and climate manager +0xD8. Sampler is bilinear. Channel semantics from notes: B = day's top temperature `clamp((2B/255-1)*50, -50, 50)` (B 25 = -40 C, B 190 = +24.5 C); G = humidity / day-night range (night = top - ((1-G/255)*18+7)); R = wind (R*0.25 clamped 0..60). Deep-snow threshold globals (cvars) e.g. SNOW_START -5, SNOW_FULL -20, DEEP_SNOW -20, DEEP_SNOW_FULL -30.
- **weather.cpp**: hooks the climate manager update after it composes weather layers ("WeatherCompose") and writes rain/snow/cloud/humidity into the composed `WeatherConstant` (`+0x12C` precipitation, `+0x130` cloudiness, `+0x134` humidity, `+0x168` snow, `+0x16C` rain). Hooks the HUD weather-flag query (bit 0 rainy, 1 snowy, 2 windy, 3 fog, 4 cloudy, 5 sunny). Reads the game clock and HUD day counter. Author credits "the hook list of the Crimson Weather mod" for finding WeatherCompose.
- **menu.cpp / overlay.cpp**: F5 Control Menu drawn with Direct2D over the D3D12 swap chain, HDR- and ReShade-aware.
- Address resolution: **hard-coded RVAs** for the 2.03.x build (e.g. `WEATHER_QUERY = 0x17FA700`, `WEATHER_UPDATE = 0x3DC4730`, `CLIMATE_VTABLE = 0x5AD1A10`, `CLIMATE_ROOT = 0x6D69198`, cvar globals `0x6D63848...`) guarded by expected function-prologue byte checks; archive patching uses file-content signatures. **[C]** This design explains why a new EXE (2.04) disables features until the author updates RVAs **[S]**.
- Season textures are built offline from the user's own game copy with Python tools (`tools/autumn_tint.py`, `autumn_plants.py`, `autumn_tiles.py`, `autumn_ground.py`, `climate_dds.py`, packed with `build_season_data.py` / `lz4pad.py`). Reads archives through "Crimson Browser Sharp's archive index".

Key research findings in `docs/research-notes.md` useful for our own design [C, author's empirical notes]:
- Winter visuals: `texture/climate_texture_1.dds` (group 0002, BGRA8) set to the "snow mountain" pixel (B25,G127,R63,A255) everywhere except a hand-traced desert mask produced snow on ground/objects + frost. Must be shipped **with a full mip chain** (DMM re-encodes single-mip textures and garbles values).
- `climate_texture_1` is read only at game start; the game builds the deep-snow layer from it. Live changes require writing the CPU climate map and re-uploading the GPU texture.
- `texture/climate_texture_2.dds` is an R10G10B10A2 **height map** of the playable square (x 64..268, y 178..383 of 512).
- Autumn/spring: vegetation colour follows `global_colormap` (DXT5 2048) and `global_extra_region_tintcolormap` (DXT1 2048); far look comes from 785 terrain colour tiles (`terrain/color/terrain_x_y_color_c.dds`, DXT1 512), 1,586 tree impostor colour textures (`tree/impostor`, DXT1 1024), ~837 tree colour atlases, and 95 ground grass textures. Total ~1.2 GB per non-summer season set.
- No regional weather zones in `.palevel/.prefab` (scan of 71,312 files); regions come from climate textures only. Weather objects live in `rootlevel.palevel` (GlobalWeather) and `object/bin__/prefab/preset/weather*.prefab`.
- Natural weather sequences: `sequencer/baseseq/contents/weather/cd_seq_contents_weather_{sunny,cloudy,rainy,wind,fog,...}`. No snow sequence.
- In-game date: HUD shows e.g. "Day 76 Sat 12:06 PM"; HUD day global at RVA 6A6D218 (2.03.x).

### Permissions / licence [C]
- **The Seasons-of-Pywel GitHub repo has no LICENSE file** (only `release/THIRD_PARTY.txt` for MinHook). Under default copyright this means all rights reserved: readable for learning, but code should not be copied into our mod **[S, legal default]**. The Nexus permission block itself could not be read (API description field does not include it; page blocked).
- Credits listed: Lazorr (encryption/unpacker), MrIkso (patching guide on ReSHAX), IzeDev, cracker, SoraSkySun, AerowynX, MinHook.

### Coverage
- DSOGaming, 2026-10-03: https://www.dsogaming.com/mods/crimson-desert-mod-adds-a-full-four-season-system/ **[R]**
- Gaming ProMax (technique summary incl. "patches the file index, texture size tables, and group lists as they're read"): https://gamingpromax.com/crimson-desert-seasons-of-pywel-mod-charting-unknown-dlc/ **[R]**
- iXBT, 2026-10-05: https://ixbt.games/en/news/2026/10/05/fanat-crimson-desert-predstavil-sistemu-sezonov-vremia-goda-budet-meniatsia-v-processe-proxozdeniia.html **[R]**
- No Reddit thread about 2.04 breakage was found by search.

---

## 2. Archive formats (.paz / .pamt / .papgt / .pathc / .paver / .paeid)

### Primary open-source references
| Project | Licence | Last commit | What it covers |
|---|---|---|---|
| pycrimson (LukeFZ) https://github.com/LukeFZ/pycrimson | MIT | 2026-03-24 | PAPGT, PAMT, PATHC readers, ChaCha20 key derivation with group key material, partial-DDS LZ4 reconstruction, reflection serializer, saves |
| crimson-rs (potter420 / "potter4208467" on Discord) https://github.com/potter420/crimson-rs | MIT | 2026-04-25 | Rust+Python: PAZ/PAMT/PAPGT read/write, `pack_mod()` overlay builder; `docs/archive-format.md` |
| dmm-parser (exodiaprivate-eng / RicePaddySoftware) https://github.com/exodiaprivate-eng/dmm-parser | **CDMTL v1.0 (restrictive, anti-redistribution)** | 2026-10-13 | Fork of crimson-rs; 122+ pabgb tables, DDS classify/validate, per-patch change log in `src/testenv.rs` |
| Crimson Desert Unpacker (lazorr410) https://github.com/lazorr410/crimson-desert-unpacker (Nexus mod 62) | MIT | 2026-03-23 | C++/C#/Python unpack + early in-place repack; `PAZ_DECRYPTION.md` |
| CDUMM (faisalkindi) https://github.com/faisalkindi/CrimsonDesert-UltimateModsManager (Nexus mod 207) | MIT | 2026-10-28 (v3.17.13) | Full overlay builder incl. **PATHC writer** (`src/cdumm/archive/pathc_handler.py`), texture overlay (`engine/texture_mod_handler.py`), bundles `asi_loader/winmm.dll` |
| CrimsonForge (hzeemr) https://github.com/hzeemr/crimsonforge | MIT | 2026-05-12 | Full decrypt/decompress/repack + PAMT/PAPGT checksum chain; reads `0.paver`; DDS decoder |
| NattKh CrimsonGameMods https://github.com/NattKh/CRIMSON-DESERT-SAVE-EDITOR-AND-GAME-MODS | MPL-2.0 | 2026-09-06 | Overlay-group rules (`CrimsonGameMods/MODDING_GUIDE.md`) |

### Layout [C, from pycrimson code + crimson-rs docs]
```
<game>/meta/0.papgt        group list (master registry)
<game>/meta/0.pathc        texture header collection (DDS headers + per-texture mip sizes)
<game>/meta/0.paver        game version
<game>/NNNN/0.pamt         per-group index (vanilla groups 0000-0035 per NattKh guide, Apr 2026)
<game>/NNNN/N.paz          headerless concatenated entry data
<game>/bin64/CrimsonDesert.exe
```

**Checksum / hash** [C]: Bob Jenkins lookup3 `hashlittle` with `initval = 0xC5EDE` (equivalently `a=b=c = len + 0xDEBA1DCD`, since 0xDEADBEEF + 0xC5EDE = 0xDEBA1DCD, verified). Used for: PAPGT and PAMT header checksum over post-header bytes, PAZ chunk checksums, PAMT directory-name hashes, PATHC keys (`hashlittle("/<path>")`), and ChaCha20 per-file nonce seed.

**PAPGT** [C]: header 12 B `{u32 unknown0, u32 checksum, u8 entry_count, u8 unk, u16 unk}`; entries 12 B `{u8 is_optional, u16 language (bitmask, 0x3FFF = all 14 languages), u8 zero, u32 group_name_offset, u32 pack_meta_checksum (= the group's 0.pamt header checksum)}`; then `i32 length` + C-string group-name buffer. Note `entry_count` is a u8.

**PAMT** [C]: header `{u32 checksum, u16 paz_count, u16 unk(0), u8 unk, u8[3] encrypt_info}`; then `paz_count x {u32 id, u32 checksum, u32 size}`; `u32 len` + directory-name trie buffer; `u32 len` + file-name trie buffer (trie node = `{i32 parent_offset (-1 root), u8 len, bytes}`, full name = walk parents); `u32 count` x directory `{u32 name_hash, i32 name_offset, u32 first_file, u32 file_count}`; `u32 count` x file `{u32 name_offset, u32 paz_offset, u32 stored_size, u32 size, u16 paz_index, u8 flags, u8 unk}`. Flags: low nibble = compression (0 none, 1 "partial" (partially compressed DDS), 2 LZ4, 3 zlib, 4 QuickLZ per pycrimson); high nibble = crypto (0 none, 1 ICE, 2 AES, 3 ChaCha20 per pycrimson enum).
- Discrepancy: lazorr's docs list 3 = "custom engine compression", 4 = zlib; NattKh's GUI labels {1: LZ4, 2: Zstd}. pycrimson's runtime only ever asserts LZ4 for compressed and ChaCha20 for encrypted entries. Treat values other than 0/1/2 (compression) and 0/3 (crypto) as unverified.

**Encryption** [C]: entries flagged ChaCha20 (mainly XML/config). pycrimson derivation: `seed = hashlittle(basename)`; nonce = seed (LE u32) x4; key = fixed 32-byte base `0E0F0C0D...0C0D0E0F` XOR nonce bytes XOR (encrypt_info[0]^[1]^[2]) from the PAMT header. lazorr's simpler formula (`seed ^ 0x60616263` with deltas) equals pycrimson's when the group uses default key material; LukeFZ notes the older one "is hardcoded to only work with packs that use the default encryption key material".
- **Black Desert comparison**: CD's PAMT enum still contains ICE and AES slots, but in practice CD uses **ChaCha20 + LZ4**, not BDO's ICE scheme **[C for CD behaviour per code; BDO claim not independently verified here]**.
- **zstd**: no open-source CD tool found decompresses PAZ entries with zstd; saves use ChaCha20 + LZ4 + HMAC-SHA256 (pycrimson, dmm-parser). What `libzstd.dll` is used for is unknown **[S]**.

**Partial DDS** [C]: textures with flag "partial" store the DDS with the first up-to-4 mips individually LZ4-compressed; per-mip stored sizes live in DDS header `reserved1` (offset +0x20) and/or in the PATHC record. Seasons notes: "the game needs [the +0x20 stored-size fields] (= mip sizes when plain)". DDS header byte 124 (`dwReserved2`) holds a 4-byte engine format id ("last4"), e.g. 0x1580 UI, 0x1280 character diffuse, 0x0480 normal (dmm-parser TEXTURE_MOD_AUTHORING.md).

**PATHC** [C]: header `{u64 reserved, u32 header_size (0x80 or 0x94), u32 header_count, u32 entry_count, u32 collision_count, u32 filenames_len}`; `header_count` DDS headers; `entry_count` sorted u32 path hashes; `entry_count x {u16 header_index, u8 coll_start, u8 coll_end, 16 B compressed_block_infos (4 mip sizes)}`; collision entries `{u32 filename_offset, u16 header_index, u16 unk, 16 B}`; filename buffer.
- Conflicting claims: dmm-parser `docs/BINARY_FORMATS.md` says PATHC is a runtime cache "mostly safe to ignore"; but CDUMM's texture handler and CrimsonForge's repack warning say **new DDS paths must be registered in PATHC** or they won't load (existing-path replacements are fine). Seasons also patches PATHC. Weight of evidence: PATHC registration is required for new or resized textures **[C/R]**.

**PAVER** [C]: starts with 3 x u16 LE major.minor.patch (CrimsonForge `ui/tab_about.py`); dmm-parser records full 10-byte values, e.g. 2.03.02 = `0200 0300 0200 e3cc19cb`. The trailing 4 bytes are unexplained **[S: likely a checksum]**.

**PAEID / dlc/dlc_01**: **no public documentation or tool support found** for `dlc/dlc_01/meta/0.paeid` or the DLC directory layout. CDUMM, CrimsonForge, crimson-rs, pycrimson and dmm-parser contain no reference to `paeid` or `dlc_01`. This needs first-hand reverse engineering on 2.04 files **[C absence of evidence]**.

### How mods add groups (overlay approach) [C]
- Create a new numeric group folder (`0036`+), with its own `0.paz` + `0.pamt`, and add an entry to `meta/0.papgt` with the correct `pack_meta_checksum`; recompute the PAPGT header checksum.
- Group names **must be numeric**; non-numeric groups register but silently don't load (NattKh `MODDING_GUIDE.md`, confirmed 2026-04-21).
- Priority: crimson-rs docs say the game scans PAPGT entries front-to-back, "first match wins", so mod managers insert their entry at the front. NattKh's guide says ordering is "not fully mapped" and to merge rather than rely on order. Conflicting; verify on 2.04 **[R]**.
- Early method (lazorr, March 2026) patched PAZ in place with same-size blobs and preserved `.paz` NTFS CreationTime because the game validated it; superseded by overlays **[R]**.
- Repacking caveats from The Games Wiki (AI-assisted, low confidence): https://thegameswiki.com/crimson-desert/wiki/modding **[R]**

### Format changes across patches [C, dmm-parser `src/testenv.rs`]
- 2.00.00 (2026-08-25, "Enhanced" update): data-table layout changes.
- **2.01.00 (2026-09-03): game-data tables moved and renamed**: `gamedata/binary__/client/bin/<t>.pabgb|.pabgh` became `gamedata/binarystaticinfo__/bin/<t>.staticinfobody|.staticinfoheader`. Older guides (NattKh, April) that require the `binary__/client/bin` path are outdated.
- 2.03.00 (2026-09-17, Steam build 25381195): 126 of 268 tables changed, 57 with layout drift.
- 2.03.01 (09-21) and 2.03.02 (09-23): content-only.
- 2.04.00 (2026-10-29): **no community format analysis published yet** (dmm-parser last commit 2026-10-13; CDUMM last release 2026-10-28, the day before). The official patch notes contain no modding or file-format statement.
- Also from dmm-parser: EXE size ~355-368 MB and changes every major patch (so every EXE-bound RVA moves).

---

## 3. Other ASI / DLL mods with source; exposure of calendar/weather/time

| Mod | Source / licence | Relevance |
|---|---|---|
| **Crimson Weather** (Nostyx, Nexus 632, v0.8.2, updated 2026-10-03 for 2.03.02) | https://github.com/Nostyxx/CrimsonWeather (no LICENSE file found) | ReShade `.addon64` (not ASI; "Ultimate ASI Loader is not required"). MinHook. **AOB-signature based** hooks named `WeatherTick`, `WeatherCompose`, `GetRainIntensity`, `GetSnowIntensity`, `GetDustIntensity`, `ProcessWindState`, `MinimapGameTimeUpdate`, `GameTimeGetter`, `GameTimeSetter`, plus time lower/upper clamp stores (`game/aob_scan.cpp`, `game/time_control.cpp`, `game/weather_hooks.cpp`). Controls real in-game time, day/night time scale, weather, clouds, fog, atmosphere, moon/Milky Way textures, per-region presets (Hernand, Demeniss, Delesyia, Pailune, Crimson Desert, Abyss). Best public reference for time + weather hooks. |
| **CrimsonDesertTools** (tkhquang): Gear Hider (Nexus 554, v0.9.1, 2.03.00), LiveTransmog | https://github.com/tkhquang/CrimsonDesertTools (MIT), framework https://github.com/tkhquang/DetourModKit | C++ ASI, AOB anchor registry with "self-heal" after patches; `docs/research/` RE knowledge base (verified on 2.03.00, FileVersion 1.0.0.2944, image base 0x140000000). Good model for update-resilient signature scanning. No weather/time code. |
| **Time and weather control** (DesertLink, Nexus 3557; merged into All-in-One menu, Nexus 3619 -> 3638) | No source link found | Set clock, skip hours, freeze clock, weather presets incl. snow. Closed source as far as found. |
| **Rest Anywhere** (Fyreon87, Nexus 3904, updated 2026-10-29 22:59) | No source link | ASI clock GUI that skips 1-72 hours; updated within hours of 2.04 release (suggests time-skip function was re-found on 2.04) **[S]**. |
| **Dynamic Body Weather** (neilliao0807, Nexus 3659) | No source link | ASI + ReShade; reads local snowfall/temperature; credits Seasons of Pywel and CrimsonWeather. |
| **RenoDX** (Nexus 7, v2.03.02-dlss5, 2026-09-29) | (RenoDX project, not inspected) | ReShade addon rendering overhaul; Seasons and Crimson Weather both interoperate with it. |
| Character Creator (Khione95, Nexus 837, v9.1.16, 2026-10-25) | Not checked | Same author; ASI. |

- No general-purpose **script extender** for Crimson Desert was found. DetourModKit (tkhquang) is the closest reusable framework **[C absence]**.
- Game-data tables with weather/climate fields (dmm-parser `docs/449_TABLE_CATALOG.md`) [C]: `GameGlobalEffectInfo` (`_weatherData`), `GameGlobalEffectInfo_Weather` (46 fields incl. `_precipitation`, `_cloudiness`, `_humidity`, `_snowAmount`, `_snowRate`, `_iceRatio`, `_enableClimateTexture`), `StageInfo` (`_weatherInfo`, blend times), `AutoSpawnTargetData` (`_weatherStateFlag`, `_temperatureRange`), `CharacterInfo` (`_weatherWeight`), `ElementalMaterialInfo` (`_useTemperatureTransferMargin`), `GimmickInfo` (`_snowRatio`). Engine reflection classes: `WeatherComponent`, `WeatherConstant`, `WeatherConstantGroup`, `AtmosphereConstant`, `SeaConstant` (`docs/ENGINE_INTERNALS.md`). A `WeatherSimulator.pacpp` source name is referenced (`docs/MOD_AUTHOR_GUIDE.md`).
- In-game calendar [R]: HUD shows day number, weekday and time (e.g. "Day 30 Wed 5:51 AM"); ~12 game minutes per real minute per The Games Wiki (AI-assisted): https://thegameswiki.com/crimson-desert/wiki/weather-system . Seasons' author measured "one game hour is about 2.5 real minutes" (inconsistent with the wiki; the mod author's measurement is the firsthand one) **[C vs R]**.

---

## 4. Executable protection

- **Denuvo Anti-tamper is still listed on the Steam store** as of 2026-10-30: Steam appdetails `drm_notice` = "Denuvo Anti-tamper / 5 different PC within a day machine activation limit". **[C]** (https://store.steampowered.com/api/appdetails?appids=3321460)
- Added about a week before the 2026-03-19 launch: https://www.pcgamesn.com/crimson-desert/denuvo-on-steam **[R]**
- Cracked by "voices38" on the 1.14.00 build (August 2026); **not** removed officially: https://notebookcheck.net/Crimson-Desert-Denuvo-DRM-removed-as-the-latest-AAA-game-to-become-piracy-victim.1359710.0.html **[R]**
- **No anti-cheat** is listed on the store; the game is single-player. **[C]**
- **Denuvo vs ASI loading**: proxy-DLL ASI loading **does work** in practice: Seasons, Gear Hider, Rest Anywhere, Dynamic Body Weather etc. all ship as ASI via Ultimate ASI Loader (`winmm.dll` recommended; `version.dll` / `dinput8.dll` alternatives per Gear Hider page https://www.nexusmods.com/crimsondesert/mods/554); CDUMM bundles `asi_loader/winmm.dll`; DMM ships its own loader. **[C]**
  - Contradiction: CrimsonForge's `tools/metabin_re/README.md` (April 2026) states Denuvo "blocks the usual DLL-proxy-loading trick (xinput1_4.dll, winmm, version, winhttp proxies all failed)". This is contradicted by the many working ASI mods; likely specific to that author's setup or an early build **[S]**.
  - Practical Denuvo effects documented by CrimsonForge: code pages are decrypted on demand, so debuggers must attach after the main menu (ScyllaHide "Denuvo x64 profile"); every patch moves all addresses. **[R]**
  - OptiScaler conflicts: OptiScaler is often installed as `version.dll`/`winmm.dll`; Nexus 634 and the Gear Hider page document workarounds. **[C]**
- **Patch 2.04 breaking ASI mods in general**: no news article or forum thread found yet (patch is ~1 day old). Pattern from changelogs: every major patch has required updates (Crimson Weather updated for 1.08 through 2.03.02; Gear Hider for 1.06 through 2.03.00). Any mod using hard RVAs (like Seasons) will need updates; AOB-based mods may survive **[C pattern, S for 2.04]**.

Official 2.04.00 patch notes (Steam, 2026-10-29): https://steamstore-a.akamaihd.net/news/externalpost/steam_community_announcements/1847920302236522 . Relevant items: new ocean/islands/underwater areas, ships, housing; "Save files created while the DLC is installed cannot be loaded ... in a game version that does not include the DLC"; upscaling and frame generation selectable separately; increased max stack sizes. Nothing on weather, seasons, modding or file formats. **[C]**

---

## 5. Textures

- **Container**: standard Microsoft DDS (magic `DDS `), both legacy FourCC (DXT1/DXT5) and DX10 headers (148-byte header) **[C]**. Engine quirks: (a) "partial" storage with first mips LZ4-compressed and per-mip sizes in header `reserved1` (+0x20) and PATHC; (b) engine format id ("last4") at byte 124 (`dwReserved2`).
- **Formats seen** [C]: BC1/DXT1 (terrain colour tiles, tree impostors, ground textures), BC3/DXT5 (`global_colormap`), BC4/BC5 (masks, normals), BC6H (HDR), BC7 (UI/character), R10G10B10A2_UNORM (`climate_texture_2`, item icons), R8/R16/R32 float/uint (region maps, masks), BGRA8 (`texture/climate_texture_1.dds`). Sources: CrimsonForge `version.py` changelog and `core/dds_reader.py`, dmm-parser `docs/TEXTURE_MOD_AUTHORING.md`, Seasons research notes.
- **Guidance** [R, dmm-parser doc]: colour textures BC7 sRGB (or BC1), normals BC5 linear, masks BC4/BC5, always full mips.
- **Tools**: NVIDIA Texture Tools Exporter / `nvtt_export` (https://developer.nvidia.com/texture-tools-exporter) recommended by dmm-parser docs; pycrimson `_dds` (DirectXTex-derived surface-size maths) and CrimsonForge's numpy DDS decoder for previewing; CDUMM's `pathc_handler.py` for PATHC registration; Seasons edited DXT1/DXT5 block endpoints directly in numpy (`tools/dxt1_channel.py`, `tools/autumn_tint.py`) to recolour without full re-encode. Mod managers (DMM) may re-encode single-mip textures, which corrupted data textures in Seasons' tests.

---

## Key implications for our own mod [S]
1. The proven architecture is: (a) ASI that serves alternative texture bytes by hooking file I/O and patching PAMT/PATHC/PAPGT in memory, plus D3D12 re-upload for resident textures; (b) live writes to the CPU climate map; (c) post-WeatherCompose hook for precipitation; (d) HUD day counter for the calendar. A simpler alternative for visuals is a numbered overlay group (0036+) built with MIT tools (crimson-rs/CDUMM), but that is static (one season per mount) and cannot switch live.
2. Use AOB signatures (Crimson Weather / DetourModKit style), not fixed RVAs, to survive patches.
3. Before coding: dump 2.04 `meta/0.papgt`, `meta/0.pathc`, `meta/0.paver`, `dlc/dlc_01/**` and diff against 2.03.02 to learn the DLC layout and whether `0.paeid` is a new registry that our archive hook must also handle.
4. Avoid copying code from Seasons-of-Pywel and CrimsonWeather (no licence) and dmm-parser (CDMTL); MIT/MPL projects (pycrimson, crimson-rs, CDUMM, CrimsonForge, lazorr unpacker, tkhquang, NattKh) are reusable with attribution.
