# Crimson Desert (Steam, patch 2.04.00) executable recon for a dynamic-seasons ASI plugin

Static analysis only. Nothing was executed, nothing was written to the PC. Binary analysed: `bin64/CrimsonDesert.exe`, 333,779,352 bytes, PE32+ x64, link timestamp 2026-10-29 04:08:06 UTC, embedded build tag `BlackSpace_CD_Cert_ReleaseSteam_x64_cd_alpha_2026_10_29_03_38_25`, linker MSVC 14.44, ImageBase 0x140000000, EntryPoint RVA 0xA0BDF80. All addresses below are RVAs (add 0x140000000 for the default VA; at runtime use module base + RVA).

Working files: `/tmp/claude-0/-home-claude/2808c463-40d5-5257-b966-1524784d482d/scratchpad/recon_C/` (scripts `pe_overview.py`, `strings_dump.py`, `xrefs.py`, `dis.py`, `vtables.py`, `stubs.py`; outputs `overview.txt`, `str.ascii.txt`, `str.utf16.txt`, `str.rtti.txt`, `vtables.json`, `kw_weather.txt`, `appendix_vt.md`, `dis_install.txt`, `dis_crc.txt`).

---

## 1. PE overview and protection

### Sections

| name | RVA | VSize | RawSize | flags | entropy | real role (inferred) |
|---|---|---|---|---|---|---|
| `.text1` | 0x1000 | 0x5585000 (89 MB) | same | R-X | 6.59 | original game code (MSVC), 223k pdata functions |
| `.idata` | 0x5586000 | 0x1739000 (24 MB) | same | R-- | 5.64 | original `.rdata` (IAT, strings, RTTI COLs, vtables) |
| `.rsrc` | 0x6CBF000 | 0x878000 | 0x36CA00 | RW- | 6.32 | original `.data`/`.bss` (globals, singletons) |
| `.00cfg` | 0x7537000 | 0x2D9000 | same | R-- | 0.00 | 2.9 MB of a single repeated byte: placeholder filled at runtime |
| `.rodata` | 0x7810000 | 0x3000 | same | R-- | 5.67 | small (orig `.00cfg`/tls data) |
| `.xtext` | 0x7813000 | 0x30000 | same | R-- | 6.85 | real resource directory (DataDir[RESOURCE] points here) |
| `.rdata` | 0x7843000 | 0x1ED000 | same | R-- discardable | 5.45 | relocated misc data |
| **`.code`** | 0x7A30000 | 0xC48B9D0 (206 MB) | same | **RWX** | 6.67 | **Denuvo**: entry point, 12 of 14 TLS callbacks, VM and ~20-25k relocated game functions |
| `.ecode` | 0x13EBC000 | 0x1BAF94 | same | R-X | 2.33 | protector data/code (TLS data range starts here) |
| `.debug$P` | 0x14077000 | 0x238 | 0x400 | RWX | 2.33 | protector |
| `.reloc` | 0x14078000 | 0x8DDC | same | R-- | 5.91 | contains the export + import directories |
| `.edata` | 0x14081000 | 0x2D85BC | same | R-- | 7.29 | actually the exception directory (`.pdata`, 248,613 RUNTIME_FUNCTIONs) |

Section names are scrambled/renamed (real `.pdata` is named `.edata`, real `.rsrc` is `.xtext`, real `.data` is `.rsrc`). Checksum field set, Authenticode signature present (SECURITY dir 0x5198 bytes). CFG GuardFlags 0x100 with 0 CF functions (CFG effectively off).

### Imports, exports, TLS, debug

* Normal game imports: KERNEL32, USER32, d3d12/dxgi/d3d11, vulkan-1 (delay-load, 107 funcs), steam_api64 (9: `SteamAPI_RestartAppIfNecessary`, `SteamInternal_SteamAPI_Init`, ...), sentry (crash reporting), amd_ags, FidelityFX loader, XeSS/XeSS-FG/XeLL, Streamline `sl.interposer.dll`, libpng18, WinHTTP, bcrypt, HID/SetupAPI, MFPlat, gdiplus, MSVCP140/VCRUNTIME140.
* **Second, lowercase import block** (classic Denuvo signature): `kernel32.dll` (GetVolumeInformationW, GetComputerNameW, GetSystemDirectoryW, ...), `ntdll.dll` (NtQuerySystemInformation, NtCreateSection, NtMapViewOfSection, NtReadVirtualMemory, NtCreateThreadEx, NtSuspendThread, RtlAddVectoredExceptionHandler, ...), `kernelbase.dll!VirtualQuery`, **`tbs.dll!Tbsip_Submit_Command`** (TPM access, hardware fingerprint), `wininet.dll` (HttpOpenRequestA, InternetConnectA, ...), `advapi32` CryptAcquireContextA, `user32` dialog/menu APIs, `xinput1_4 #3`.
* Exports: 556, all Audiokinetic Wwise (`AK::SoundEngine::*`, `CAkEmitter`, ...). Nothing game-specific exported.
* TLS directory: 14 callbacks. 2 in `.text1` (0x4B996EC, 0x4B99C30, CRT) and 12 in `.code` (0xB0EFB98, 0xB7F2280, 0x12FEC490, 0x10BC6760, 0xD901E70, 0x8A0C4B0, 0x10F52B60, 0xB139C30, 0x125A4DD0, 0x11F45880, 0xD17F840, 0xC191910). Entry point (0xA0BDF80) is inside `.code`.
* Debug directory: CodeView RSDS, **PDB name `WindowsLauncher_shielding.pdb`** (age 4, GUID 7BC5AE5D-5DDF-4E1D-846E-90AAF46CAC5F), i.e. the PDB of the protection ("shielding") post-process, no game PDB path. Also POGO (`GCTL`) and ILTCG entries (original section map leaks there).
* No Steam CEG/stub `.bind` section, no VMProtect `.vmp0/.vmp1`, no Themida/Arxan markers found. No `.arch/.srdata/.xpdata` names either, but this is the newer Denuvo layout where names are scrambled.

### What Denuvo did to the code (measured)

* **All 566,634 vtable entries (42,474 unique virtual functions, 15,777 RTTI vtables) point into `.text1`.** Vtables were not moved.
* 4,142 of those unique virtual functions (about 10%) are 5-byte **`jmp rel32` stubs from `.text1` into `.code`**; the body was relocated (and often mutated) by Denuvo. A scan for 16-aligned `jmp` stubs into `.code` finds ~20,600 such relocated functions overall, consistent with 25,276 pdata entries inside `.code` (~10 MB of relocated functions; the rest of the 206 MB is VM/protector).
* Relocated functions range from lightly transformed (normal MSVC prologue, but constants hidden as `mov edx,[rip-x]; sub edx,0x489cffc8`) to heavily obfuscated (stack games, opaque arithmetic). They are the Denuvo "trigger" functions; they run the license/tamper checks inline.
* A random sample of 4,000 normal `.text1` functions disassembled with capstone had 29,575 direct call/jmp instructions and 0 of them target `.code`: unprotected game code is plain, unobfuscated MSVC output with intact pdata unwind info.

**Relevant protected (stubbed) functions for this project** (vtable slot -> stub -> body):
`ResourceLoader` slot1 0x157F910 -> 0xAE86700 (init) and slot2 0x157FDD0 -> 0xAE88C90 (add DLC); `ResourceLoadWorker_Package` slot1; `FileLoaderWindows` slot5; `PAClimateTexture` slot0 0x28C4C10 -> 0xE1B43D0; `TemperatureArea` slots 0..3; `AICondition_CheckCurrentGameTime` slot1 0x24B4D40 -> 0xD359530 (evaluate); `AICondition_CheckWeather` slot1 0x24BE4C0 -> 0xD44EEB0; `ConditionData_Weather` slot16; `ConditionData_IsCoveredBySnow` slot21; `Weather*SimulatorScriptComponent` slot0 (and 9/12); `TreeComponentServer` slots 0/85/90; `SequencerGamePlayEventSetTimeOfDay` slots 0/99/101. Several weather/climate functions also have a second copy inside `.code` (string xrefs from both `.text1` and `.code`, e.g. `_isManualWeather`, `_currentWeather`, `climate_texture_1.dds`, `_snowAmount`).

### Verdict: protection and impact on an ASI plugin

**Denuvo Anti-Tamper (current generation) on top of an otherwise plain MSVC x64 build with full RTTI.** No VMProtect/Themida/Steam stub.

* Loading an ASI via a `winmm.dll`/`dinput8`/`version.dll` proxy is not blocked (the folder already contains `winmm.ini` + `Seasons.asi`, a third-party seasons mod whose log reports `hooks: on` with MinHook, so in-process hooking demonstrably works on this build).
* **Hook strategy that works well:** resolve classes at runtime by RTTI name (scan `.idata` for the `.?AV...` TypeDescriptor, then COL, then vtable), and either swap the vtable slot pointer or MinHook the `.text1` address the slot points to. This is patch-robust because it needs no byte signature at all.
* For unprotected `.text1` functions, AOB signature scanning is reliable (no mutation; same compiler output across patches except for normal code drift).
* For the ~10% stubbed functions: hook the 5-byte `jmp` stub in `.text1` (MinHook relocates a `jmp rel32` fine) or the vtable slot, never patch bytes inside `.code`. Do not build AOB signatures from `.code` bodies: they are mutated per build and likely re-generated each patch. Calling into them is fine; they are just slower (Denuvo triggers).
* Avoid hardware breakpoints / debug-register tricks and avoid touching TLS/`.code`/`.ecode` pages; Denuvo queries NtQuerySystemInformation and has VEH. Keep the plugin a normal in-process hooker.
* Every game patch will re-shuffle `.code` and may change which functions are stubbed; use RTTI + vtable-slot resolution and keep AOBs as a fallback only.

---

## 2. Debug info and RTTI

* No game PDB path (only `WindowsLauncher_shielding.pdb`). Source paths leak through asserts/loggers: `d:\bs\cd_alpha\code\trunk\BlackSpace\Base\Base\PAPackFileResourceMap.cpp`, `...\PAGameVersionMap.cpp`, `...\GamePlay\GameClientModule\...`, `...\GameServerModule\...`, Havok `Havok_2024_2`.
* **RTTI is fully present**: 16,568 `.?A` type names, 15,777 Complete Object Locators, 15,308 classes in namespace `pa` (Pearl Abyss), plus `pa::scriptComponent`, `pa::gameClientScript`, `pa::uiCommonScript`, `pa::engineScript`, Havok (`hk*`), ICU 78, winrt.

### Classes of interest (with vtable RVAs)

Weather / climate / temperature / snow (`pa::` unless noted):

| Class | vtable | notes |
|---|---|---|
| `ServerWeatherManager` | 0x5EE7FE8 (1 slot: 0x2EB1680) | authoritative weather state (single-player game still runs client/server split); fn near 0x2EB1763 references `texture/climate_texture_1.dds` |
| `WeatherComponent` | (template binds only: `ReflectMetaObjectBind<WeatherComponent>`) | engine render-side component; strings "WeatherComponent", "Local Weather", "WeatherName", "ClimateTextureCPURead" |
| `PAClimateTexture` | 0x5E897C0 (slot0 stubbed) | climate map object |
| `scriptComponent::WeatherSimulatorScriptComponent` | 0x59D6A18 | Pascript-compiled weather logic (`WeatherSimulator.pacpp`) |
| `scriptComponent::weatherTimeSimulatorScriptComponent` | 0x59D6B88 | **weather time simulation (best calendar/time hook candidate)** |
| `scriptComponent::WeatherSurfaceSimulatorScriptComponent` | 0x59D6AE0 | wet/snow surface accumulation (`weathersurfacesimulator.pacpp`) |
| `scriptComponent::WeatherCloudSimulatorScriptComponent` | 0x59D64A0 | clouds |
| `scriptComponent::WeatherEffectScriptComponent` / `worldWeatherEffectScriptComponent` | 0x59D6970 / 0x59D6E68 | rain/snow particle effects |
| `TriggerEventHandler_GameWeather` | 0x59C4030 | trigger volumes forcing weather |
| `TriggerEventHandler_CloudVolumeOff`, `CloudDensityVolumeComponent` | 0x59C47C0 | |
| `ChangeWeatherBuffData`, `CommonChangeWeatherBuffProcessor`, `ClientChangeWeatherBuffProcessor` | 0x5CA1A30, 0x5939EE0, 0x5938490 | gameplay "change weather" buff: a ready-made code path that sets weather |
| `WeatherEffectBuffData`, `Common/ClientWeatherEffectBuffProcessor` | 0x5CA2660, 0x5939858, 0x5938ED8 | |
| `TrocTrSyncWeatherStateReq`, `TrocTrWetnessReq`, `TrocTrCreateProjectileByWeatherReq` | 0x5EDBD08, 0x5ED9448, 0x5EDA248 | internal client/server packets: weather state sync |
| `AICondition_CheckWeather`, `ConditionData_Weather` | 0x5C6B8D0, 0x5CEC788 | read current weather (good place to observe the WeatherState getter) |
| `ConditionData_IsCoveredBySnow`, `GimmickEventHandlerData_ClearCoveredSnow` | 0x5CE8AB0, 0x5D245B8 | snow cover queries |
| `TemperatureArea` (struct) | 0x5B57178 | temperature zones; all 4 main slots stubbed |
| `uiCommonScript::UIGamePlayControlCommon_TemperatureGauge/TemperatureItem` | 0x5A18370 / 0x5A173B0 | HUD temperature |
| `GimmickEventHandlerData_RequestEnvironmentReadBackData` | 0x5D6B300 | GPU weather readback (puddle/snow) |
| `FogSaveData`, `GetWorldFogDataFetchResultSet` | 0x5C49A78, 0x5EE8360 | map fog-of-war, not atmospheric |

Time:

| Class | vtable | notes |
|---|---|---|
| `AICondition_CheckCurrentGameTime` | 0x5C6C708 | evaluate (slot1) stubbed into `.code` 0xD359530; follow it at runtime to find the game-clock getter |
| `SequencerGamePlayEventSetTimeOfDay_ITimelineEventData` (+ enum `TimeOfDayEventType`) | 0x59AFC08 | cutscene event that sets time of day |
| `UITimeOfDayEvent` | 0x59C71E8 | UI event `UITimeOfDayEvent_UPDATE_TIME` |

Vegetation / terrain: `TreeComponent`, `TreeComponentServer` (0x5ED3818), `TreeMeshTransformSocketServer`, `ConditionData_IsInGrassField` (0x5CE4B90), `TerrainRegion*` (spawn/navi info only), `ClientFrameEventTreePulling`, `ClientFrameEventWindForce/WindVolume`. No foliage/vegetation *material* or season classes in RTTI.

Resources / packages / filesystem:

| Class | vtable | notes |
|---|---|---|
| `ResourceLoader` | 0x5AAA630 | main loader; slot1 = init (fails -> installation error), slot2 = add DLC; both stubbed |
| `BasicsResourceLoader`, `IResourceLoader` | 0x5A9ACF8, 0x5A9AD98 | |
| `PackFileResourceMap` | 0x5AA8BA8 | parses `meta/0.papgt` and each `NNNN/0.pamt` (code at 0x1550420.. 0x1555xxx) |
| `GameResourceMap`, `LocalDirectoryResourceMap`, `EditorLocalDirectoryResourceMap`, `BasePackageResourceMap` | 0x5AA1950, 0x5AAA428, 0x5AAA3C8, 0x5A9AA70 | a loose-file (local directory) resource map exists, used by editor/dev builds |
| `ResourceLoadWorker_Package` / `_ResourceMap` / `_LocalDirectory`, `BaseResourceLoadWorker` | 0x5AAA4B0, 0x5AAA540, 0x5AAA5B8, 0x5AAA350 | async read workers |
| `ResourceHandler_Paz`, struct `AsyncReadDesc_Paz` | 0x5AAA528 | `.paz` reads |
| `FileLoaderWindows`, `IFileLoader` | 0x5AA05C0, 0x5AA0588 | OS file access (Win32 + `CreateFile2`) |
| `PackFileDownloadManager` (+ `_PASync`, `_Temp`) | 0x5A992B8 | patch/download manager |
| `PatchListHash` (anon ns) | 0x56DA2E8 | |
| `ReflectObjectResourceManagerBase`, `ResourceReferencePath_*` | 0x5E91888 | |
| Havok: `hkxTextureFile`, `hkxTextureInplace`, `hkFileSystem` | | |

Full table with vfunc RVAs and stubbed slots: `recon_C/appendix_vt.md` (copied in Appendix A below).

---

## 3. Strings

Counts (non-`.code` sections): 321,117 ASCII, 2,558 UTF-16 (UTF-16 is almost only Win32/UI plumbing; the game uses UTF-8, with many Korean developer messages).

### Seasons

**No `season`, `autumn`, `winter`, `summer` string anywhere in the executable** (all `spring` hits are physics springs). The game has no built-in season concept or season enum.

### Weather / climate / snow (RVA, string)

* Enum `WeatherState` (used as `TypedFlag32<WeatherState>`, i.e. bit flags): `Cloudy` 0x5CB6670, `Sunny` 0x5CB6678, `Storm` 0x5CB6680, `Rainy` 0x5CB6688, `Snowy` 0x5CB6690, `Windy` 0x5CB6698 (immediately followed by `Thursday..Wednesday` day names). Debug message 0x5D0A628 "[ConditionData(%#)]: Weather(Rainy) ..." confirms the enum name form.
* Effects: `fx_weather_a__snow.system.effect` 0x59D6750, `__rain`, `__heavyrain`, `__heavysnow`, `__snowdust`, `__desertdust`, `__lightning`, `small_rain`; Wwise env names `ENV_WIND`, `ENV_RAIN` 0x59D68F0; audio RTPCs `Precipitation`, `IceRatio`, `SkyVisibility`, `TimeOfDay` 0x5E56160..0x5E56190.
* **Climate map**: `texture/climate_texture_1.dds` 0x5EE7FC0, `texture/climate_texture_2.dds` 0x60A11D8, `texture/climatesand_texture.dds` 0x60A1340; doc string 0x60A1210 "**R: Temperature, G: Humidity, B: WidSpeed.**"; shader bindings `g_climateTex1`, `g_climateTex2`, `g_climateSandTex`; properties `_climateTexture1/2`, `_climateSandTexture`, `_enableClimateTexture`, `_climateTemperature`, `_climateHumidity`, `_climateWind`, `_climateWindDegree`, `ClimateTextureCPURead`, `ClimateTextureGroup`, `1_Climate`. Temperature is therefore sampled from a world-space texture; offsetting it per season is a data-level change.
* Weather constants (reflection properties of `WeatherConstant`/`GameGlobalEffectInfo_Weather`): `_precipitation`, `_snowRate`, `_snowAmount`, `_snowPuddleRate`, `_snowRatio`, `_weatherWeight`, `_weatherData`, `_weatherStateFlag`, `_blockedWeatherStateFlag`, `_temperatureRange`, `_activeTemperatureMinMax`, `_fixWeather`, `_isManualWeather` 0x610E428, `_currentWeather` 0x60BA8B0, `_currentTemperature` 0x610DF68, `_resultWeatherRef`, `_weatherConstantBuffer`, `WeatherConstantGroup`, `WeatherConstant_NoSave`, `StageInfo_Weather` (`_weatherInfo`, `_weatherStartBlendTime`, `_weatherEndBlendTime`, `_weatherIngTime`), `PAWeatherManager` 0x610E440 (reflection type registered in fn 0x371A60).
* Setter example: fn 0x40A88E0 = property setter for `_isManualWeather` (byte at object+0x31, fires reflection change notification through vtable +0x218/+0x248). Manual-weather mode exists in the engine.
* Snow rendering: `renderSnow`, `_renderSnow`, `DeformSnow`, `deformedsnow`, `g_snowPack(UAV)`, `SnowMaterialWrite`, `GlobalSnowDecalShading`, `Texture/SnowMask.dds`, `Texture/SnowNormal.dds`, `Texture/snow_white_noise.dds`, `_maskSnowDeformableSurface`, **`renderGlobalAfterSSDMFoliagesSnow` / `GlobalAfterSSDMFoliagesSnow` 0x60D6DE8** (a dedicated snow-on-foliage pass), `readbackPuddleSnow`, `NearPuddleSnowReadBack`, `WeatherReadBackData`.
* Temperature gameplay: `Temperature`, `ClimateTemperature` 0x5B52D20, `_temperatureWarning`, `cd_icon_temperature_*`, `cpp-temperature-hot/cold`, UI events `ChangeWeatherState` 0x592D560 and `ChangeTemperatureValue` 0x592D578 (registered in the UI event table fn 0x8A4230).
* Weather post-process presets: `/object/preset/weatherpostprocess/preset_*.prefab`, `/object/prefab/preset/housingweather.prefab`.

### Time / calendar

* `InGameTime(%s)` 0x594BFE0 in a debug overlay (fn ~0xBDDB43).
* `_bankRefreshGameTimeDay`, `_hour`, `_minute`, `_resetHour`, `_hourAliasName`, `_minuteAliasName`, `eErrNoInvalidGameTime`, `AICondition_CheckCurrentGameTime`.
* `UI_Common_Month_1..12` 0x5A22738.. (localisation keys; xref 0x1048A4C; likely date formatting for saves/news, not a game calendar; unverified), `UI_Modal_DayCountText`, `UI_TradeAgreement_DayCount`, day-of-week names next to the weather enum.
* Time of day debug: console commands `PearlAbyssEngine.Debug.CycleTimeOfDayPrev/Next`, `TimeOfDayForward/Backward(x2)` 0x607ADE8..0x607AF40, registered at 0x397F304.. with handlers 0x3982EE0 (-0.2), 0x3982F10 (+0.2), 0x3982F40 (-2.0), 0x3982F70 (x2). Each handler does `obj = ctx->[+0x30]->[+0x60]; obj->vtbl[+0x108](delta * dt)`: **slot 33 (+0x108) of that object is an "advance time of day" virtual**, the cleanest observed entry into the time-of-day system. Also slash commands `/settimeofdayupperlimit`, `/settimeofdaylowerlimit` 0x60BBBE8.
* `TimeScale`, `_globalTimeScale`, `PearlAbyssEngine.Debug.FreezeTimeScale`.

### Vegetation

SpeedTree is used (`SpeedTreeRaw`, `GrassBranch`, `GrassFrond`, `GrassLeaf`, `WeedLeaf`, `TreeLeaf`, `MATERIAL_ID_TREE_LEAF`, `MATERIAL_ID_GRASS`), SSDM far grass (`UseSSDMFarGrass`, `g_ssdmFarGrassData`), `RenderLargeScaleGrassInstances`, `foliageDensityLevel`, `_windLeaf2Flag`, terrain texture example `object/texture/pa_terrain_grass_0086.dds`. Only 117 `.dds` literals in the exe: foliage textures are referenced from data (pamt/prefabs), so a foliage swap must happen at the resource layer (file read or GPU upload), not via exe strings.

### Packages and the installation error

Strings: `%s/%s/%d.paz` 0x5AA87F0, `0.papgt` 0x5AA88E8, `0.pamt` 0x5AA8A4C, `%s/meta/%s`, `0.paver` 0x5A97C80 (`%#/meta/%#`, read by PAGameVersionMap), `*.admeta`, `buildconfig/gameversionmap.xml`, `%#/BuildConfig/gameversion.xml`, `ResourceCRC`, `InstallationKey`. Korean messages (translated):

* 0x5AA89A0 "PAPGT file load failed!"
* 0x5AA89C0 "packMeta file load failed. Check the path [%s]"
* **0x5AA8A10 "packMeta Crc mismatch. file(%u) papgt(%u) pamt(%u)"**
* 0x5AA8A70 / 0x5AA8AD0 "packGroupTreeMeta file load failed / is corrupted. %# (0)"
* 0x5AA9C70 / 0x5AA9D10 "A problem occurred while computing the papgt hash. %# (2)/(1)"
* 0x5AA9CC0 "In this environment the package path cannot be changed arbitrarily."
* 0x5AA9DE8.. "Failed to load DLC (0/1/2)"
* 0x590A9A0 "Failed to initialise the resource loader. The resource map or resource package path may be the problem."
* 0x590AC80 "Failed while adding DLC to the resource loader. The resource map or resource package path may be the problem."
* **0x590AA30 "There may be a problem with the game installation. Please reinstall."**

Not found: `papk`, `pathc`, `paeid` (the `meta/0.papk`, `meta/0.pathc`, `dlc/*/meta/0.paeid` files are not named in exe strings; their names are probably built or live in `.code`).

### How the startup integrity check works

1. Startup fn 0x6D2CB0 (huge app init: SteamAPI_RestartAppIfNecessary/Init, CPU check, platform services) creates the `ResourceLoader` (vtable 0x5AAA630, stored at app+0x38) and calls **`ResourceLoader::init` (vtable slot1, Denuvo stub 0x157F910 -> 0xAE86700)**. On failure (0x6D35BE) it logs 0x590A9A0; later it calls **slot2 (add DLC, stub 0x157FDD0 -> 0xAE88C90)** for DLC packages (0x6D38F5), failure logs 0x590AC80. In both cases, if global flag byte 0x6E7D538 is set and 0x70C8DFF != 1, it shows `MessageBox("There may be a problem with the game installation. Please reinstall.", "Error", 0x52000)` via wrapper 0x15F5100 at **0x6D3962..0x6D3976**, then exits init.
2. The loader reads `meta/0.papgt` (pack group tree meta: list of groups `0000..00NN` + DLC groups, each with a 32-bit CRC) and computes the papgt's own hash (the "papgt hash" error path lives in the Denuvo-relocated init, no plain-code xref).
3. For each group, `PackFileResourceMap` fn **0x1552400** opens `NNNN/0.pamt` (pack meta = file index), reads the first 8 bytes, then reads bytes `[12 .. size)` and hashes them with fn **0x1540810**. It compares the result (0x155258C) with the CRC stored for that group in papgt (`entries[idx].crc` at +8 in 12-byte records at `this+0x40`) **and** with the first dword of the pamt header (0x1552594). Any mismatch logs 0x5AA8A10 and fails the load, which bubbles up to the installation-error dialog.
4. **Hash = Bob Jenkins lookup3 `hashlittle()` with initval 0x000C5EDE** (fn 0x1540810 seeds `a=b=c = len + 0xDEBA1DCD` = `0xDEADBEEF + len + 0xC5EDE`; rotates 4/6/8/16/19/4 as `ror 28/26/24/16/13/28`). It is a checksum, not a signature: no RSA/ECDSA over the package meta was found (the many `*_verify` strings are OpenSSL, used for HTTPS).
   **Verified empirically** on the staged read-only copy of `0002/0.pamt` (7,530 bytes): header dword0 = 0x08312440 = `hashlittle(pamt[12:], 0xC5EDE)`, and the same value is stored as the group CRC in `meta/0.papgt` at offset 36. Likewise 0000 (0x414A8CB6 at papgt+12), 0001 (0x7D67BFA8 at +24), 0015 (0x56439057 at +192). Script: `recon_C/lookup3.py`.
   `meta/0.papgt` layout (735 bytes): u32 header value (0x02271681 in this build), then 12-byte records `{u32 flags, u32 nameOffset, u32 pamtCrc}` (flags seen 0x007FFF00, 0x00010000, 0x00020000, ...; nameOffset steps of 5 = "NNNN\0" in a trailing string table). The header value is **not** `hashlittle(0xC5EDE)` (nor seed 0, nor CRC32) of any contiguous byte range of the file (brute-forced); its algorithm lives in the Denuvo-relocated init ("papgt hash" error path) and was not determined statically.
5. Per-file `.paz` contents are located via the pamt index (`%s/%s/%d.paz`); the CRC chain is papgt -> pamt only. Editing a `.paz` or appending data therefore requires rewriting the pamt entry, recomputing the pamt header hash, updating the group CRC in `0.papgt`, and the papgt's own hash. The installed third-party Seasons mod does exactly that (log: `index ...\0001\0.pamt ... hash 7D67BFA8 -> 2F0400F4`, `group list: 4 of 4 index hashes changed (own hash 007FFF00 -> 0C56CD5B)`) and appends its season textures past the end of the existing archives.

Unrelated checks near the same code: "Check valid launch Failed/successed" 0x590AA80 (a `SafeLaunchDummyTest` / launcher command-line check at 0x6D2AE0), "Unsupported CPU detected" 0x590AAC0 (xref 0x423AE5E), Steam-required message 0x5B66D70. There is no evidence of the game hashing the exe or loaded DLLs itself; exe integrity is Denuvo's job.

---

## 4. Engine, scripting, config

* **Engine: Pearl Abyss BlackSpace Engine** (`BlackSpace`, `blackspace_release`, `BlackSpace Debug Console Helper.`, `----PABlackSpaceBoundary`, `d:\bs\cd_alpha\code\trunk\BlackSpace\...`). Middleware: Havok 2024.2 (physics, `hknp*`), Wwise (exported), SpeedTree, ICU 78, HarfBuzz, OpenSSL, Sentry, AMD FSR/FidelityFX, Intel XeSS/XeLL, NVIDIA Streamline, D3D12 primary + Vulkan delay-loaded.
* Architecture: client/server split even in single player (`Server*`/`Client*`/`Common*` processors, `TrocTr*Req/Ack` internal messages, `ServerWeatherManager`). Weather authority is server-side; the client receives `TrocTrSyncWeatherStateReq`.
* **Scripting: "Pascript"**, a proprietary language (`*.pas`) compiled ahead of time to C++ (`.pacpp`/`.pacpph`, `PASCRIPT_COMPILER_V2`, `PascriptCompile.%s.autogen.%#.bat`, `objectList.pacpph`). Script components (`pa::scriptComponent::*`, `pascriptComponent`) are native code inside the exe with Lua-style binding names (`open_/close_<component>`, `<component>_newComponent`, `_getComponentMember(Function)`). There is no Lua, Python, AngelScript or Squirrel runtime and no loose script files to edit: weather/time logic must be changed by hooking native code or by changing data.
* Data-driven gameplay: reflection system (`ReflectMetaObjectBind`, `SimpleReflectPropertyBind`, property names like `_snowRate`), StaticInfo tables (`gamedata/binarymisc__/`, `GameData/StaticInfo/AutoGenInfo__/%#_misc.%#.xml`), AI charts (`AICondition_*`), condition data (`ConditionData_*`).
* Config: `user_engine_option_save.xml` (+ `_temp`), `buildconfig/gameversionmap.xml`, `%#/BuildConfig/gameversion.xml`, `%#/buildconfig/%#/__resourcemap.xml`, `CDConfigCheckRemovingDevice.xml`. Save path seen in `persNeedsData.txt`: `%LOCALAPPDATA%\Pearl Abyss\CD\save\<steamid>`. No `.ini`.
* Built-in enums relevant to the project: `WeatherState` flags (Cloudy, Sunny, Storm, Rainy, Snowy, Windy), `TimeOfDayEventType`. **No season enum, no season data, no calendar beyond hour/minute/day counters.**
* Live-ops/online: ProductManager API `https://productmanager.pearlapis.com/api/v1/Product/GetProductConfigByBuildKey`, crossSave; irrelevant for offline modding but means the client talks to the network at startup.

---

## 5. Feasibility for a hooking-based dynamic seasons plugin

| Component | Approach | Difficulty |
|---|---|---|
| ASI loading | winmm/version proxy loader (already present) | trivial |
| Find classes | runtime RTTI scan (`.?AV...@pa@@` -> COL -> vtable) | easy, patch-robust |
| Read game day/time | observe `AICondition_CheckCurrentGameTime` evaluate (stubbed, follow at runtime), the TimeOfDay object reached via `ctx+0x30 -> +0x60` (vtable slot +0x108 advances time), or count day wraps of time-of-day; fallback: own calendar persisted next to the save (the existing mod does "30 days a season" by its own count) | medium |
| Force/bias weather (snow in winter, no snow in summer) | hook/set `_isManualWeather` + weather state (setter at 0x40A88E0 pattern), or reuse `ChangeWeatherBuffData` / `TriggerEventHandler_GameWeather` paths, or bias `WeatherState` selection in `ServerWeatherManager` | medium; server/client split needs care |
| Temperature per season | modify climate texture R channel (data swap of `climate_texture_1.dds`) or hook `TemperatureArea` (stubbed) / `ClimateTemperature` | data route easy, code route medium |
| Snow cover | engine already has snow amount/rate, deformable snow and foliage-snow passes driven by weather constants (`_snowAmount`, `_snowRate`) | medium |
| Foliage texture swap | no exe-level hook: intercept file reads (hook `FileLoaderWindows`/`ResourceHandler_Paz` or `ReadFile`/`CreateFile2`) and serve alternative `.dds`, or swap at GPU upload (D3D12 copy hooks, as the existing mod does) | hard (asset pipeline, streaming, cache invalidation on season change) |
| Integrity check | only relevant if the plugin modifies archives; recompute lookup3 (initval 0xC5EDE) for pamt and papgt, or avoid touching archives by serving data from memory | known, solvable |
| Denuvo | hook `.text1` stubs/vtable slots only, no `.code` patches, no AOBs from `.code` | manageable |

**Overall rating: feasible, medium-high effort (about 6.5/10 feasibility).** Weather/temperature biasing by hooks is realistic (RTTI-rich, unobfuscated `.text1`, reflection property names, clear weather enum, climate texture with documented channels). The hard part is visual seasonality of foliage: the engine has no seasons, so autumn/winter looks must come from replacement textures delivered through the resource layer or the GPU, with live swapping on day change. A key constraint is that several of the most relevant entry points (ResourceLoader init/DLC, climate texture, temperature areas, game-time and weather conditions) are Denuvo-relocated: they are hookable at their `.text1` stubs/vtable slots but cannot be signature-scanned or patched internally. An existing third-party mod ("Seasons of Pywel" 1.0.2, MinHook + D3D12 texture-upload hooks + archive append with hash fix-ups) is installed in the same folder and demonstrates the whole pipeline works on this build; it must be removed/disabled before testing our own plugin to avoid conflicts (both would hook the same functions and rewrite the same pamt/papgt).

---

## Appendix A: key vtables (RVA), vfunc[0..3], Denuvo-stubbed slot indices

| Class (RTTI) | vtable RVA | #vfuncs | vfunc[0..3] RVAs | Denuvo-stubbed slots |
|---|---|---|---|---|
| `.?AUTemperatureArea@pa@@` | 0x5B57178 | 6 | 18AC910 18AC980 18ADF30 18ABE30 | 0,1,2,3 |
| `.?AVAICondition_CheckCurrentGameTime@pa@@` | 0x5C6C708 | 6 | 202E580 24B4D40 24B4930 24B4B50 | 1 |
| `.?AVAICondition_CheckWeather@pa@@` | 0x5C6B8D0 | 6 | 1C292D0 24BE4C0 24BE4B0 24ADA20 | 1 |
| `.?AVAICondition_CheckWetnessReason@pa@@` | 0x5C68740 | 6 | 1C292D0 24AF730 24AF720 24AF0A0 | - |
| `.?AVBasePackageResourceMap@pa@@` | 0x5A9AA70 | 11 | 14A8B10 4B9D17E 4B9D17E 4B9D17E | - |
| `.?AVBaseResourceLoadWorker@pa@@` | 0x5AAA350 | 14 | 15818E0 4B9D17E 1582140 1582430 | - |
| `.?AVBasicsResourceLoader@pa@@` | 0x5A9ACF8 | 19 | 14A9310 4B9D17E 4B9D17E 14A93F0 | - |
| `.?AVChangeWeatherBuffData@pa@@` | 0x5CA1A30 | 12 | 2109690 3A9030 3A9030 3A9030 | - |
| `.?AVClientChangeWeatherBuffProcessor@pa@@` | 0x5938490 | 7 | A18150 225D540 A1DAC0 3A2890 | - |
| `.?AVClientWeatherEffectBuffProcessor@pa@@` | 0x5938ED8 | 7 | A17CF0 3A9030 A1D130 3A2890 | - |
| `.?AVCommonChangeWeatherBuffProcessor@pa@@` | 0x5939EE0 | 7 | A15970 225D540 225D560 3A2890 | - |
| `.?AVCommonWeatherEffectBuffProcessor@pa@@` | 0x5939858 | 7 | A14E10 3A9030 3A79F0 3A2890 | 2 |
| `.?AVConditionData_CheckEnterStageFog@pa@@` | 0x5CFB7C8 | 22 | 23FFFD0 3E9310 240D080 23FFC00 | 11,14,17 |
| `.?AVConditionData_IsCoveredBySnow@pa@@` | 0x5CE8AB0 | 22 | 23FFFD0 3E9310 240D080 23FFC00 | 11,14,17,21 |
| `.?AVConditionData_IsInGrassField@pa@@` | 0x5CE4B90 | 22 | 23FFFD0 3E9310 240D080 23FFC00 | 11,14,17 |
| `.?AVConditionData_Weather@pa@@` | 0x5CEC788 | 22 | 23FFFD0 3E9310 240D080 23FFC00 | 11,16,17 |
| `.?AVEditorLocalDirectoryResourceMap@pa@@` | 0x5AAA3C8 | 11 | 1581840 1581120 1580F60 3A2890 | - |
| `.?AVEnvironmentRecognitionFunctions@gameClientScript@pa@@` | 0x5A935A0 | 24 | 13FBBA0 13D4550 13D5820 13D51B0 | - |
| `.?AVFileLoaderWindows@pa@@` | 0x5AA05C0 | 6 | 1500040 1501690 1501BC0 1501A00 | 5 |
| `.?AVFogSaveData@pa@@` | 0x5C49A78 | 83 | 1A0FD40 1AA8F90 1556AF0 1556AF0 | 0,48 |
| `.?AVGameResourceMap@pa@@` | 0x5AA1950 | 12 | 1521B50 1551120 1551020 3A2890 | - |
| `.?AVGetWorldFogDataFetchResultSet@pa@@` | 0x5EE8360 | 7 | 2EB3040 2EB30A0 3A79F0 130AFE0 | 2,6 |
| `.?AVGimmickEventHandlerData_ClearCoveredSnow@pa@@` | 0x5D245B8 | 10 | 24EBA00 24F5910 24F5B20 24F5D30 | - |
| `.?AVGimmickEventHandlerData_RequestEnvironmentReadBackData@pa@@` | 0x5D6B300 | 12 | 25EB980 25EBB50 25EBC00 24F5D30 | 4 |
| `.?AVIEnvironmentRecognitionFunctions@pa@@` | 0x5A93668 | 24 | 13D2900 4B9D17E 4B9D17E 4B9D17E | - |
| `.?AVIFileLoader@pa@@` | 0x5AA0588 | 6 | 1501630 4B9D17E 3A2930 4B9D17E | - |
| `.?AVIPackageResourceMap@pa@@` | 0x5A9AB08 | 11 | 14A8A50 4B9D17E 4B9D17E 4B9D17E | - |
| `.?AVIResourceLoadWorker@pa@@` | 0x5AAA6D0 | 14 | 157EEA0 4B9D17E 4B9D17E 4B9D17E | - |
| `.?AVIResourceLoader@pa@@` | 0x5A9AD98 | 19 | 14A9260 4B9D17E 4B9D17E 4B9D17E | - |
| `.?AVLocalDirectoryResourceMap@pa@@` | 0x5AAA428 | 11 | 1580ED0 1581120 1580F60 3A2890 | - |
| `.?AVPAClimateTexture@pa@@` | 0x5E897C0 | 1 | 28C4C10 | 0 |
| `.?AVPackFileDownloadManager@pa@@` | 0x5A992B8 | 10 | 148C770 148C960 148CA60 148CC00 | - |
| `.?AVPackFileDownloadManager_PASync@pa@@` | 0x5A99310 | 10 | 148E000 148E0A0 148E1D0 148E6C0 | - |
| `.?AVPackFileDownloadManager_Temp@pa@@` | 0x5A99260 | 10 | 148C2F0 3A9030 148CA60 3A2890 | - |
| `.?AVPackFileResourceMap@pa@@` | 0x5AA8BA8 | 12 | 1550420 1551120 1551020 1551220 | - |
| `.?AVPatchListHash@?A0x2e94c82e@@` | 0x56DA2E8 | 2 | 47D17A0 47CFA40 | - |
| `.?AVResourceHandler_Paz@pa@@` | 0x5AAA528 | 2 | 1582A40 3A3420 | - |
| `.?AVResourceLoadWorker_LocalDirectory@pa@@` | 0x5AAA5B8 | 14 | 15848A0 1584940 1582140 1582430 | - |
| `.?AVResourceLoadWorker_Package@pa@@` | 0x5AAA4B0 | 14 | 1583070 1583110 1583460 1583800 | 1 |
| `.?AVResourceLoadWorker_ResourceMap@pa@@` | 0x5AAA540 | 14 | 1584660 1584700 1582140 1582430 | - |
| `.?AVResourceLoader@pa@@` | 0x5AAA630 | 19 | 157F7D0 157F910 157FDD0 14A93F0 | 1,2 |
| `.?AVSequencerGamePlayEventSetTimeOfDay_ITimelineEventData@pa@@` | 0x59AFC08 | 109 | 6CBC70 D61AD0 1556AF0 1556AF0 | 0,48,99,101 |
| `.?AVServerWeatherManager@pa@@` | 0x5EE7FE8 | 1 | 2EB1680 | - |
| `.?AVTreeComponentServer@pa@@` | 0x5ED3818 | 146 | 2AFCB90 2AFB440 1556AF0 1556AF0 | 0,48,85,90 |
| `.?AVTriggerEventHandler_CloudVolumeOff@pa@@` | 0x59C47C0 | 92 | 7C92B0 DC3890 1556AF0 1556AF0 | 48 |
| `.?AVTriggerEventHandler_GameWeather@pa@@` | 0x59C4030 | 92 | 7C92B0 DC39F0 1556AF0 1556AF0 | 48 |
| `.?AVTrocTrCreateProjectileByWeatherReq@pa@@` | 0x5EDA248 | 3 | C5E1F0 C5E180 2CBBE60 | - |
| `.?AVTrocTrSyncWeatherStateReq@pa@@` | 0x5EDBD08 | 3 | C5E1F0 C5E180 2C3ADF0 | - |
| `.?AVTrocTrWetnessReq@pa@@` | 0x5ED9448 | 3 | C5E1F0 C5E180 2CE70B0 | - |
| `.?AVUIGamePlayControlCommon_TemperatureGauge@uiCommonScript@pa@@` | 0x5A18370 | 167 | 100BF80 100C270 DEA740 6FE6A0 | 3,113 |
| `.?AVUIGamePlayControlCommon_TemperatureItem@uiCommonScript@pa@@` | 0x5A173B0 | 167 | 100AD80 100ADF0 DEA740 6FE6A0 | 3,113 |
| `.?AVUITimeOfDayEvent@pa@@` | 0x59C71E8 | 1 | DE2D00 | - |
| `.?AVWeatherCloudSimulatorScriptComponent@scriptComponent@pa@@` | 0x59D64A0 | 20 | E5DE80 E5DD10 3A9030 3A2890 | 0,9,12 |
| `.?AVWeatherEffectBuffData@pa@@` | 0x5CA2660 | 12 | 2109690 3A9030 3A9030 3A9030 | - |
| `.?AVWeatherEffectScriptComponent@scriptComponent@pa@@` | 0x59D6970 | 20 | E5F190 E5EC70 3A9030 3A2890 | 0,9 |
| `.?AVWeatherSimulatorScriptComponent@scriptComponent@pa@@` | 0x59D6A18 | 20 | E5DE80 E61950 3A9030 3A2890 | 0,9 |
| `.?AVWeatherSurfaceSimulatorScriptComponent@scriptComponent@pa@@` | 0x59D6AE0 | 20 | E65A90 E65930 3A9030 3A2890 | 0,12 |
| `.?AVworldWeatherEffectScriptComponent@scriptComponent@pa@@` | 0x59D6E68 | 20 | E67A40 E678B0 3A9030 3A2890 | - |

## Appendix B: caveats

* Static analysis only; class roles are inferred from names, strings and short disassembly, not verified at runtime.
* `.code` was not analysed beyond statistics (Denuvo VM). Anything only reachable there (papgt self-hash routine, game-clock getter body) needs runtime tracing.
* `UI_Common_Month_*` and the weekday names may belong to real-world date formatting; whether the game tracks an in-game day-of-year was not established. Treat "no in-game calendar" as likely, not proven.
* RVAs are for this exact build (link time 2026-10-29 04:08:06 UTC, size 333,779,352). They will move with the next patch; resolve via RTTI at runtime.
