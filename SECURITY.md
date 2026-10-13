# Security Review — swiss-gc

**Date:** 2026-10-13
**Scope:** This fork (`cube/swiss` GameCube application, `wii/booter`, `pc/*` host tools, `cube/packer`, `buildtools`), ~104k lines of C/C++ plus the vendored `libtinysmb` (libogc2) code path Swiss links against.
**Method:** Adversarial code review executed as 18 parallel, attack-surface-scoped audit passes (network servers/clients, file parsers, device handlers, firmware flashing, GUI, memory/string sweeps, integer/allocation sweeps, host tools). All Critical and most High findings below were **re-verified by direct source inspection after the sweep**; items marked *(reported)* are consistent multi-agent findings with quoted code but were not individually re-read during consolidation.

> **Platform context.** Swiss runs on the GameCube/Wii in bare-metal PowerPC supervisor mode: **no MMU protection, no ASLR, no stack canaries, no NX**. Any memory-corruption bug is realistically a full code-execution primitive, and corruption of device-write paths can permanently brick attached flash hardware (WKF, Qoob, KunaiGC, PicoLoader-style devices). Treat every finding here as more severe than it would be on a hosted OS. Most issues exist in upstream `emukidid/swiss-gc` too; per this fork's policy, **do not file them upstream as LLM-assisted reports** without a human rewrite/review.

---

## Threat model

Attacker-controlled inputs to Swiss:

| Vector | Examples |
|---|---|
| Crafted files on SD/IDE/USB/NAND-adjacent media | `.gcm/.iso/.tgc/.dol/.elf`, cheat `.txt` + `.chtsel`, `swiss.ini` (legacy), `.cli/.dcp`, `backdrop.tpl`, jewel-case `.png`, `.mp3`, `.gci/.gcs/.sav` saves |
| Malicious/MITM network **servers** | FTP, FSP (UDP), SMB (via libtinysmb), SSP |
| Malicious network **clients** | Anyone on LAN hitting `httpd` (port 80) or `wiiload` listener |
| Crafted discs / drives | Burned DVD-R (ISO9660 PVD, FST, multigame tables), NPDP/dev drives, spoofed ATA IDENTIFY, modified firmware (GCLoader/WKF strings) |
| Crafted memory cards | Corrupted directory/FAT, crafted `.gci` imports |
| Host side (inverted trust) | Compromised/buggy console attacking `pc/usbgecko` server & `pc/sspd`; LAN attackers vs `pc/sspd` (UDP, spoofable) |

---

## Executive summary

| Severity | Count (deduped) |
|---|---|
| Critical | 10 |
| High | 24 |
| Medium | 26 |
| Low / hardening | 21 |

The dominant, highest-confidence exploit chains:

1. **Browse-time RCE from a crafted disc image**: `strcpy` of an FST string-table entry into a 256-byte stack buffer (`gcm.c:91`), reachable by simply *listing* a folder containing the image (banner metadata thread). **(S-01)**
2. **Boot-time heap smash from a crafted disc image**: uncapped `filesToPatch[]` (512-entry fixed array) filled from disc-controlled FST entries (`gcm.c` `parse_gcm`/`parse_tgc`). **(S-02)**
3. **Boot-time arbitrary-address write**: `(topAddr - MaxFSTSize) & ~31` wraps to any u32 address, then the FST (attacker bytes) is DMA'd there (`swiss.c:1327`). **(S-03)**
4. **Remote RCE via FTP server**: 255-byte stack buffer in `ftp_execute()` sprintf'd with a `CWD` command containing a server-induced ~1 KB path (`ftp_devoptab.c:609`). **(S-05)**
5. **Boot-time stack smash via crafted `swiss.ini`**: 2048-entry (~700 KB) stack array with unbounded entry count (`config.c:407+`). **(S-04)**
6. **Dependency RCE**: linked `libtinysmb` parses NBT/SMB responses with unbounded length-driven `memcpy`/`strcpy` into 64 KB heap / stack buffers. **(S-10)**
7. **Host compromise from console**: `pc/usbgecko` server has multiple unbounded `sprintf`/`strcpy` of console-supplied paths into 1–4 KB heap/BSS buffers. **(S-09)**

---

# 1. Crafted game images & executables (`gcm.c`, `dvd.c`, `patcher.c`, `elf/sidestep`)

### S-01 — Critical · Stack smash in `get_fst_details()` via FST filename *(verified)*
`cube/swiss/source/gcm.c:76-101`

```c
u32 entries = *(unsigned int*)&FST[8];            // attacker-controlled
u32 string_table_offset = 12*entries;             // unbounded / wrappable
...
strcpy(filename /* char filename[256] on stack */,
       &FST[string_table_offset + filename_offset]);
```
FST contents, `entries`, `string_table_offset` and the 24-bit `filename_offset` are all bytes from the disc image; the source string has no guaranteed NUL and no bound check against the FST buffer. **Trigger: browsing a directory containing the image** (`populate_meta` → `get_gcm_banner` → `get_fst_details("opening.bnr")`, `filemeta.c:350`) — no user confirmation — plus `parse_gcm_add()` at boot. Overwrites saved LR with attacker bytes. **Fix:** use `strlcpy`, and validate `entries*12 <= fstSize`, `string_table_offset+filename_offset < fstSize` before any read.

### S-02 — Critical · `filesToPatch[]` (512 entries) overflowed by disc-controlled FST *(verified)*
`gcm.c:302-394` (`parse_gcm`), `gcm.c:416-528` (`parse_tgc`), `gcm.c:113-131` (`parse_gcm_add`); allocation at `swiss.c:2381` (`memalign(32, sizeof(ExecutableFile)*512)`).

`entries = *(u32*)&FST[8]` drives the walk; every `*.dol/.elf/switcher.prs/appl2*.img/exec*.img` name (and each nested `.tgc`) appends a ~300-byte `ExecutableFile` with disc-controlled `offset/size/name` — **no cap anywhere**. `switcher.prs`/`appl2.img`/`exec.img` entries don't even need valid payloads. ~600 fake entries ≈ 7 KB of FST. Runs at `check_game()`/boot, before any hash check. The unconditional `boot.bin` append (`gcm.c:389-394`, also `swiss.c:2414/2422` BS2 appends) overflows even at exactly 512. **Fix:** pass capacity in, stop appending at 512, error out; clamp `entries <= fstSize/12`.

### S-03 — Critical · FST staging address wraps → attacker bytes written at arbitrary MEM1/MEM2 address *(verified)*
`swiss.c:1293-1322` (TGC), `swiss.c:1326-1347` (GCM)

```c
u32 fstAddr = (topAddr - GCMDisk.MaxFSTSize) & ~31;   // u32 wrap → arbitrary u32
u32 fstSize = (fileToPatch->fstSize + 31) & ~31;      // uncapped, from image
devices[DEVICE_CUR]->readFile(file, (void*)fstAddr, fstSize);
...
memcpy((void*)bi2Addr /* fstAddr-0x2000 */, &GCMDisk+0x440, 0x2000);
```
`MaxFSTSize`/`FSTSize` (GCM) and `tgcHeader.fstLength` (TGC) are raw u32s from the image (`gcm.c:239-241, 292-293` take them at face value; only the disc magic is checked). `topAddr - X` wraps for `X > topAddr` and is steerable to any 32-aligned address; the device then writes up to ~4 GB of file content there, plus an 8 KB header copy at `fstAddr-0x2000`. Deterministic code execution before the game starts. **Fix:** reject `fstSize/MaxFSTSize` above a small sane ceiling, require `fstOffset+fstSize <= file size` (u64 math), clamp `fstAddr >= 0x80003100`.

### S-04 — Critical · Boot-time stack overflow in legacy config migration *(verified)*
`config/config.c:406-441, 677-679`

```c
ConfigEntry configEntries[2048];      // ~700 KB stack array
...
configEntriesCount++;                 // never bounded
strncpy(configEntries[configEntriesCount].game_name, value, 64);
```
A crafted legacy `swiss.ini` with >2048 `ID=` blocks smashes the LWP main-thread stack with attacker-shaped `strncpy` bytes, at every boot, before UI. **Fix:** `if (configEntriesCount >= 2047) break;`, fix the epilogue index math, or heap-grow the array.

### S-05 — Critical · Remote stack overflow in FTP command construction *(verified)*
`devices/ftp/ftp_devoptab.c:594,609` (`char buf[FTP_MAX_LINE /*255*/]; sprintf(buf, "%s\r\n", cmd);`) with commands built at `:1423` (`sprintf(buf1029, "CWD %s%s", env->share, path_abs)`), `:1722,1757` (`RETR/STOR`).

`path_abs` reaches ~1024 bytes via nested server-listed directory names (each ≤254 chars); merely *browsing* into a deep directory on a malicious/MITM FTP server pumps up to ~1031 bytes into a 255-byte stack buffer with attacker-chosen filename bytes → saved-LR overwrite → RCE. Companion `.bss` overflow: `strcpy(last_cmd /*[255]*/, cmd)` at `:925/:1113`. **Fix:** size command buffers to `FTP_MAXPATH+16`, use `snprintf` + length check / `-ENAMETOOLONG`; `strlcpy` for `last_cmd`.

### S-06 — Critical · Unbounded memcpy into the 64-byte SRAM shadow *(verified)*
`devices/system/deviceHandler-SYS.c:260-269` (`write_rom_sram`), dispatch `:451-461`.

Neither `file->offset` nor `length` is clamped to `rom_sizes[ROM_SRAM]` (64 B). The file-copy flow can land a file named `sram.bin` on `sys:/` (SYS `readDir` fails for dirs, so it is the auto-selected destination root for copies) → up-to-256 KB-per-chunk attacker data overwrites libogc state following the SRAM shadow. Same lack of clamp in `read_rom_sram` (read-side leak) and `read_rom_aram`/`write_rom_aram` DMA (`:338-354`, unbounded ARAM write). **Fix:** clamp `offset/length` against `rom_sizes[file->fileBase]` in both dispatchers; reject `fileBase >= NUM_ROMS`.

### S-07 — Critical · ISO9660 parser: disc-controlled name length memcpy + unbounded entry count *(verified)*
`devices/dvd/dvd.c:513-598` (`read_direntry`), heap table `DVDToc` = 2048 × `file_entry` (`dvd.h:15-27`, `name[128]`).

- `memcpy(name, direntry, nl)` with `nl` = disc byte 0–255 → up to 127-byte heap overwrite into `flags/sector/size` and the next entry.
- `files++` is never capped at `MAXIMUM_ENTRIES_PER_DIR` (`:595`, repeated `".."` records also increment) → arbitrary-length heap overwrite from directory-record bytes.
- `name[nl-2]` read/write for `nl < 2` (`:576`); `nrb == 0` loops forever (`read_directory`).

Trigger: opening a burned DVD-R in the DVD browser. **Fix:** clamp `nl` to `sizeof(name)-1`, reject `nl<1`/short records, cap `files`, bound `direntry` to the 2048-byte sector window.

### S-08 — Critical · `adjust_tgc_fst()` OOB writes driven by in-file entry count *(verified)*
`gcm.c:399-414` — `entries=*(u32*)&FST[8]`; loop writes `*(u32*)&FST[i*12+4] = ...` with no relation to the FST buffer's actual length (caller `parse_tgc` uses attacker `fstLength`); invoked both on the heap FST and later **in MEM1** at the staged `fstAddr` (`swiss.c:1303/1307`). Sequential, attacker-influenced 4-byte stores past the buffer. Also `parse_tgc` never NULL-checks `get_fst()` (`gcm.c:455-460`) → NULL deref; and `sprintf(filesToPatch[].name /*[256]*/, "%s/%s", tgcname, ...)` at `gcm.c:438,451` overflows `name` into the struct's `type/fstOffset/fstSize` for ≥243-char TGC names. **Fix:** NULL-check, clamp iterations to `fstLength/12`, `snprintf` the name, cap TGC name length earlier.

### S-09 — Critical · Host-PC compromise from the console in `swissserver` (usbgecko) *(verified)*
`pc/usbgecko/main.c`

- `setCurPath`/`cache_path`: console sends exactly 4096 bytes with no NUL guarantee; `sprintf(path=malloc(4096), "%s/%s", curPath, ent->d_name)` (heap overflow) then `sprintf(cached_files[i].name /*[1024]*/, "%s", path)` (BSS overflow) — `main.c:104-112,138-148,212`.
- `cached_files[2048]` unbounded count (`main.c:76,147`) — >2048 host dir entries = megabyte-scale BSS smash.
- `ASK_SERVEFILE`: `strcpy(served_file /*[1024]*/, filename)` from a possibly unterminated 1024-byte read; console can also name **any absolute host path** (`fopen` + stream back) — arbitrary host file disclosure by design (`main.c:235-246`).
- `send_file_data`: unchecked `malloc(req->size)`, `fread` result ignored, then `gecko_write(buffer, req->size)` — leaks PC heap memory to the console up to 4 GB/request (`main.c:158-168`).
- `pc/usbgecko/gecko.c:150-169`: `read()` (ssize_t) stored in `size_t` → error branch is dead, `left`/`buf` drift → infinite loop / wild writes on unplug.

**Fix:** force NUL after reads, `snprintf` everywhere, cap entries, jail served paths under a configured root (reject `..`/absolute escapes/symlinks), transmit only bytes actually read, fix `gecko_read` signed handling and propagate errors.

### S-10 — Critical (dependency) · `libtinysmb` (libogc2) parses server bytes into fixed buffers
*Third-party library linked via `-ltinysmb`; audited against libogc2 sources; reaches the wire through `devices/smb/deviceHandler-SMB.c`.*

- `SMBCheck()` computes NBT `readlen` (24-bit, server-controlled up to 16 MB) and `smb_recv`s it into the ~65 KB `message` buffer — massive heap overflow on a single reply to the negotiate (`libtinysmb/smb.c:581-599`).
- FIND_FIRST2/NEXT2 filename `strcpy`/`utf16_to_utf8` into a 766-byte stack `SMBDIRENTRY` with no destination bound (`smb.c:2160-2165, 2251-2256`) — stack smash on first directory listing.
- READ_ANDX: `DataLength/DataOffset` used unchecked in `memcpy` of file data (`smb.c:1865-1876`) — heap overflow on any file read.
- Negotiate primary-domain copied byte-by-byte into `p_domain[64]` until NUL (`smb.c:939-947`).
- `SMB_WriteFile`'s server-reported count underflows the write cache (`smb_devoptab.c:152-167`) → gigabyte-scale heap **exfiltration** to the rogue server.

**Fix:** patch/upgrade libtinysmb (clamp `readlen <= sizeof(message)`, bound all name/data copies to destination size, validate all offsets/counts against received length), and consider removing SMB support until then. Note also NTLMv1-only auth and **plaintext SMB password stored in `swiss.ini`** (`config.c:190-193`) — a rogue server harvests crackable challenge-response; document and warn.

---

# 2. Network-facing code

### S-11 — High · `wiiload` accepts incomplete/never-written decompression output, then boots it *(verified)*
`wiiload.c:110-130` — only `ret < 0` fails; `Z_NEED_DICT`/trailing-garbage/premature-EOF and `deflate_size==0` all return a heap buffer that may be partially or wholly **uninitialized/stale heap**, which is then `memcmp`'d, parsed as DOL/ELF, copied to ARAM and jumped to (`wiiload.c:171-186`). No upper bound on `inflate_size`/`deflate_size`. **Fix:** require terminal `ret == Z_STREAM_END` and `total_out == outsize`, reject zero/max sizes, fail on `Z_NEED_DICT`.

### S-12 — High · `wiiload` has no receive timeout while the UI thread is suspended *(verified)*
`wiiload.c:86-89, 168, 212-218` — after a valid 12-byte HAXX header, the main thread is suspended and `net_read` blocks forever; any LAN host can permanently freeze the console with ~12 bytes. Companion defects *(verified)*: `read() < size` signed/unsigned comparison treats errors as success (`:144/:158`, stale-stack header/args reuse); `args == NULL` then `strncasecmp(args, ...)` → NULL deref (`:177`); `strncasecmp` with attacker `args_size` prefix-matches "SDLOADER.BIN" routing logic (`:177`); `int pos` wraps in `pos*100/insize` and `pos<insize` (`:104,126`). **Fix:** socket recv timeouts, don't suspend the UI for network I/O, explicit `ret != (int)size` checks, NUL-safe dispatch, `size_t` accounting.

### S-13 — High · FSP directory parser: OOB reads → 4 GB wild `memchr` scan *(reported, consistent)*
`devices/fsp/fsplib.c:640-670` — remaining-record test is measured against the protocol *block*, not the heap buffer; a short final block makes `data[dirpos+8]` OOB and `datasize - dirpos` wrap → `memchr(ptr,0,~4 GB)` and 255-byte `strncpy` of OOB heap into the listing. Also `fsp_pkt_read` accepts `len > recv_len - FSP_HSIZE` making `xlen` underflow (`:201-209`), feeding OOB reads in `fsp_opendir`/`fsp_getpro`/`fsp_fread`; `fsp_stat` parses uninitialized stack when the server sends a header-only reply (`:1194-1203`); unbounded directory accumulation + unchecked `reallocarray` (`deviceHandler-FSP.c:60-75`) → remote OOM crash. **Fix:** validate `dirpos+9 <= datasize`, fix the `fsp_pkt_read` length check, require minimum reply lengths, cap total dir size, check allocations.

### S-14 — High · httpd: infinite `while(1);` on accept error; no socket timeouts *(verified)*
`httpd.c:79-82` — any transient `net_accept` failure nails the console in a spin loop at normal thread priority (permanent HTTP death + CPU starvation until power cycle). `:86` — single-threaded handling with no `SO_RCVTIMEO`: one idle connection = permanent slowloris. `:99-103` — unauthenticated clients force full DVD re-init + UI draws from a 16 KB-stack network thread (thread-unsafe `DrawPublish`, mechanical thrash). `:105-112` — `memalign` unchecked and `DVD_Read` return ignored → on read error the previous sector's bytes (or first-iteration uninitialized heap) are streamed out (heap disclosure into the dump); also `httpd_in_use` flag set late/cleared early → race with device switching (`swiss.c:2897`). **Fix:** replace `while(1)` with backoff+continue, set recv/send timeouts, never draw on the httpd thread, check `DVD_Read` and `memalign`, fix flag ordering/atomicity.

### S-15 — High · `backdrop.tpl` parsed at boot by libogc2's unchecked TPL parser *(reported; dependency)*
`gui/FrameBufferMagic.c:3189-3260` (`DrawLoadBackdrop`, called from `main.c:314`) feeds a user-placeable file to `TPL_OpenTPLFromHandle`; libogc2's TPL parser mallocs `ntextures*sizeof(...)` (wrappable u32) then `fread`s attacker length into it, and honors per-descriptor file offsets — crafted `/swiss/backdrop.tpl` = code execution every boot. **Fix:** parse TPL with a checked in-repo loader, cap texture count/dimensions, verify offsets against file size.

---

# 3. Cheat, config & text parsers

### S-16 — High · Cheat install copies enabled codes into the 0x2000-byte WiiRD region with no size cap *(verified)*
`cheats/cheats.c:182-201` — `kenobi_install_engine()` zeroes the region then copies every enabled code pair; the only capacity check lives in the interactive UI (`FrameBufferMagic.c:2612`). `loadCheatsSelection()` (`cheats.c:338-343`) sets `enabled` from raw `.chtsel` bytes — and its "integrity" XXH64 is computed over the *attacker's own* `.txt`, so a cheat pack with matching `.chtsel` (or `AutoCheats=Yes`) streams attacker u32 pairs past `0x81800000` at boot. **Fix:** enforce `getEnabledCheatsSize()+12 <= kenobi_get_maxsize()` inside `kenobi_install_engine()` (and in the selection loader); reject `.chtsel` bytes not in {0,1}.

### S-17 — High · `calloc(1, file->size + 1)` wraps on 0xFFFFFFFF file sizes *(reported, consistent)*
`cheats.c:285`, `config.c:108`, `swiss.c:1629, 1655` — all four file-content loads. A crafted FAT entry or malicious network server advertising `size == 0xFFFFFFFF` wraps the allocation to 0 bytes and `readFile` then pours device data into the undersized block. **Fix:** reject sizes `>= 0xFFFFFFFF`, cap these parser inputs (few MB), check allocation results.

### S-18 — Medium · `.dcp` parameter parser NULL derefs & uninitialized heap slots *(reported, consistent)*
`config/dolparameters.c:96-113` — `Values=` before any `Name=` dereferences NULL `curParam`; malformed `{…}` tuples leave `reallocarray`'d `ParameterValue` slots uninitialized, later used as strings/pointers by `DrawArgsSelector` (`FrameBufferMagic.c:2453-2454` — also `% num_values` division-by-zero) and `populateArgz` (`dolparameters.c:135-143`). Crafted `<name>.dcp` next to a DOL crashes/boots-with-garbage on selection. **Fix:** guard `curParam`, zero new slots, count only successfully parsed values, treat `num_values==0` as inactive in the UI.

### S-19 — Low · Config/cheats robustness tail *(reported)*
`cheats.c:109` NULL cheat name deref on code-first files (UI crash); `config.c:435-441` `strncpy` without forced termination for name/comment/status + `print_debug("%s", game_id)` (`:1337`) reading past the 4-byte `game_id`; `config.c:1072` + `bba.c:33` unclamped `BBANetmask` (≥32 → UB shift, /32 networking). Fixes: force terminators, clamp netmask 0–32.

---

# 4. Devices, media & firmware

### S-20 — High · ATA IDENTIFY trim loop walks backwards through .bss with a u32 index *(verified)*
`devices/fat/ata.c:228 (`u32 i`), 301-316` — `while((serial[i]==' ' || !serial[i]) && i >= 0)` is always true for u32; an all-space attacker IDENTIFY serial/model (rogue adapter/drive firmware) walks `i` to 0xFFFFFFFF writing 0x00 backwards below `ataDriveInfo` (`model[48]`/`serial[24]`, `ata.h:86-96`) through neighboring globals. **Fix:** signed index, test bounds before access, start at `len-1`.

### S-21 — High · `DISC_INTERFACE` initializers misaligned with libogc2 struct *(verified against `/opt/devkitpro/libogc2/include/ogc/disc_io.h`)*
`ata.c:699-740` — 12 values into a 10-field struct: `__ataa_eraseSectors` lands in `clearStatus`, `__ataa_flush` in `shutdown`, `&__ataa_shutdown` + `~0` become a garbage 64-bit `numberOfSectors`, `bytesPerSector = 1`. Depending on the linked libdvm, this either kills ATA/IDE-EXI devices outright or presents fabricated geometry to FatFs (wrong-sector writes, cache mis-sizing). **Fix:** designated initializers + the two extra scalars removed; populate `numberOfSectors` from `ataDriveInfo.sizeInSectors` after identify. Also: `(lbaMid << 16)` int overflow in capacity math (`ata.c:289`), unbounded BSY/DRQ busy-waits (`ata.c:411,445,454,474,508,516,530`), `ataUnlock` password `memcpy` with `strlen` into 32-byte field (`ata.c:382`, currently dead code).

### S-22 — High · Firmware flashing can brick hardware
*Converging findings — the flash paths assume trusted files and perfectly-behaving chips:*
- `swiss.c:2548-2574` (WKF `.fzn`): `memalign(32,0x1D0000)` unchecked and **`readFile` return value ignored** → short/error read leaves heap garbage which is then erased+programmed over all 464 WKF menu pages → unrecoverable brick (`wkf.c:161` writes unconditionally; no SPI ID check on this path, no image magic check).
- KunaiGC `deviceHandler-KunaiGC.c:164-169`: JEDEC capacity byte trusted → `block_count` underflow on small/clone chips → littlefs erases/writes past real capacity, aliasing into the recovery region.
- Qoob `deviceHandler-Qoob.c:158,165,300-326,387-390`: on-flash `entryHeader.size` trusted → delete erases beyond the entry (BIOS region) and size-0 entries hang the directory scan; 0xF4-char names printed with unbounded `%s`.
- All flash busy-waits lack timeouts (`wkf.c:84-100`, `deviceHandler-Qoob.c:211,275`, `kunaigc/spiflash.c:20`, `gcloader.c:59`): a faulted erase wedges the console mid-flash with "Do NOT Power Off" on screen.

**Fix:** check every alloc + read, verify device IDs in the write path, validate image magics, whitelist JEDEC IDs/capacity ranges, bound all busy-waits with datasheet timeouts, and abort cleanly on timeout.

### S-23 — High · ELF/DOL loaders trust header offsets unchecked *(reported, consistent — `aram/sidestep.c`, `elf.c`, `gcm.c`)*
`ELFtoARAM` (`sidestep.c:452-482`) uses attacker `e_phoff/e_phnum/p_offset/p_filesz` with no comparison against the actual buffer size (`valid_elf_image` checks only magic/type, `elf.c:30-58`; `boot_dol`, `wiiload`, and PATCH_ELF FST entries all flow here); `p_paddr + p_filesz` wrap defeats the ARAM size gate; `entrypoint[1]/[9]` are OOB derefs. Same missing phdr bounds in `patcher.c:18101-18108, 17767-17775, 17857-17865` (writes `p_paddr = p_vaddr` per phdr) and `gcm.c:187-198` (`calc_elf_segments_size`, unchecked `calloc` at `gcm.c:187`). `valid_dol_file`/apploader sizing uses wrap-prone u32 `offset+size` checks (`gcm.c:253, 436`; `nkit.c:2986-2989`). **Fix:** pass the buffer length in; require `e_phoff + e_phnum*32 <= size`, per-segment `p_offset + p_filesz <= size` (u64 math), sane `e_phnum` cap; check every alloc.

### S-24 — High · Patcher: game-specific patches act on attacker-sized buffers with fixed offsets *(reported)*
`patcher.c:13525-13886` — `Patch_GameSpecificFile()` largely ignores `length`: a 32-byte FST file named e.g. `claire.rel` (gameID-spoofed image) receives writes at `data + 0x190294` etc.; the GSX "bi2.bin" block does `memset(data + args, 0, length - args)` where a crafted `args > length` wraps to a multi-GB memset. `Patch_CheatsHook()` (`:17911-17974`) scans for `blr` unbounded past the buffer and writes the hook at the found offset; `Calc_Address` windows come from attacker section headers. GLRD64 second-DOL merge (`:18023-18092`) recomputes `length` from merged attacker offsets, then re-patches past the realloc. `installPatch2` (`:125-143`) decrements `top_addr` without a reservation check → blobs into `VAR_AREA`/NULL. Several scan loops read up to ~88 B past `length` (`:1798-1817` and copies). **Fix:** bound every scan by `length`, validate every fixed offset `< length`, clamp the `args` memset, keep installed-patch accounting inside the reserved arena.

### S-25 — High · Memory-card & SYS handler faults *(reported, consistent — `devices/memcard/deviceHandler-CARD.c`)*
- `.gci` import opens the card file by the **header-internal** attacker name (`:351-374`): if a same-named entry already exists (a real save — names/IDs are public knowledge), it is silently overwritten without any prompt, while the UI's overwrite check tests the outer `.gci` filename which never exists on-card.
- `CARD_SetStatus` writes attacker `icon_addr/comment_addr/banner_fmt/icon_speed` verbatim into the on-card directory (`:416-424`), persisting poisoned metadata consumed by the IPL/other tools — even after a partially failed write.
- `readFile` for Swiss-tagged files advances `offset` by 8192 but leaves `length` unchanged, then inflates the return value (`:261-264, 317-320`) → callers get "success" with an uninitialized tail; `DCFlushRange` uses the post-loop pointer (flushes the wrong region).
- Error paths leak `CARD` handles + sector buffers (`:253-256, 307-310`); `writeFile`'s `memalign` unchecked (`:383-384`); free-space accounting in `int` (`:120-144, 483`).

**Fix:** prompt against the internal entry name (or refuse overwrite), sanitize status fields, single-exit cleanup, check allocations, u64 accounting.

### S-26 — Medium · Multigame-disc table: size underflow → 4 GB fake entries *(reported)*
`deviceHandler-DVD.c:291-297` — GCOS/Cobra tables (fully disc-controlled): a zero/unsorted slot after a valid one yields `size = 0 - fileBase` (u32 wrap) attached to a bootable `.gcm` entry; downstream allocs/DMA sized accordingly. **Fix:** require strictly monotonic, in-range offsets; skip invalid slots.

### S-27 — Medium · Removable-media & driver tail *(reported)*
- FAT: remount frees the `FATFS` while cached `FFFIL*` handles stay open → UAF on next I/O (`deviceHandler-FAT.c:172-195, 348-359`); `readDir`/`diskio` unchecked allocs (`deviceHandler-FAT.c:105-148`, `diskio.c:63-71`); `renameFile` updates the handle name on `FR_NO_FILE` (`:414-419`); u64→u32 size/offset truncation (`:157,166,193,220`, also SMB/FTP/SSP stat-open paths — `readDir` checks `UINT32_MAX` but `statFile`/`readFile` don't); >4 GiB exFAT sizes.
- USB Gecko (console side): `open_dir` transmits 4096 bytes from a 1024-byte name buffer → 3 KB memory disclosure to the PC (`usbgecko.c:120-132`); `usbgecko_read_file` reports success regardless of short reads (feeds S-22).
- WODE: infinite wait on dead device (`deviceHandler-WODE.c:66-68`); empty error branches return stale `dvdbuffer` data (`wode.c:99-128, 315-317`). FlippyDrive: UB `1LL << (handle-1)` if device reports handle 0 (`flippy.c:134,543,569`).
- Info page: `sprintf(topStr[256], ...)` with device-supplied GCLoader version / WKF serial strings (`gui/info.c:87,103`) — rogue firmware overflow of a global when opening Info.
- `rt4k_deinit` calls `fclose(NULL)` on the init-failure path (`rt4k.c:133`).

---

# 5. Core file-management & UI

### S-28 — High · File-copy loop writes through unchecked `memalign` result *(verified)*
`swiss.c:2021-2023` — when a memory card/Qoob is involved, `chunkSize = curFile.size - curFile.offset` (full file; a network server can advertise ~4 GB); `readBuffer = memalign(...)` unchecked, then `readFile(&curFile, readBuffer, ...)` → device DMA into NULL / tiny region. Same unchecked-alloc pattern at `swiss.c:2148` (verify, 512 KB) and the `.fzn` path. **Fix:** check the allocation, cap copy chunking at 256 KB universally.

### S-29 — Medium · Copy/delete path & recursion robustness *(reported)*
`util.c:348-368` `deleteFileOrDir` recurses to media depth (~500 levels possible under PATHNAME_MAX) with each level holding its parent's full listing — stack exhaustion mid-delete. `swiss.c:1873-1876` (+`files.c:266-269`): `concat_path`/`strlcat` truncation ignored → truncated destination path collides with an unrelated existing file; existence check and write then hit the wrong file. `swiss.c:1128-1138`: `select_dest_dir` modulo/deref on `num_files == 0` after a mid-dialog refresh failure. `swiss.c:1950-1964`: unguarded call through `statFile == NULL` (FlippyDrive Flash rename flow). Several unchecked `calloc` results on attacker-adjacent paths (`swiss.c:1597,1623,1649,2303`). **Fixes:** iterative delete with depth cap, check concat returns, handle empty refreshes, guard optional device callbacks, check allocs.

### S-30 — High · Banner metadata: undersized-read OOB + unterminated strings into global `txtbuffer` *(verified + reported)*
`devices/filemeta.c:190-216` — only `bannerOffset + bannerSize > f->size` (wrapping u32 add) and `bannerSize != 0` are checked; a 32-byte crafted "BNR1" then suffers `memcpy(f->meta->banner, banner->pixelData, 6144)` + six `BNRDesc` reads out of the tiny heap allocation (`:204-215`) → heap disclosure rendered as textures/text. iconv output is not NUL-terminated and `fixBannerDesc` (`:125-145`) doesn't guarantee it either; `displayName` may point at an unterminated field consumed by `sprintf(txtbuffer /*2048 global*/, "%s", ...)` (`swiss.c:2709, 2739, 2749`) and `strdup` (`FrameBufferMagic.c:1568`) — a long surviving run overflows a `.bss` global. **Fix:** require full BNR sizes before any copy, u64 range math, force terminators in `fixBannerDesc`, precision-limit all `%s` consumers.

### S-31 — Medium · MP3 player offset math → OOB write / negative index *(verified)*
`mp3.c:96` — rewind computes `if (file->offset - 0x10000 > 0) offset -= 0x10000;` on a u32 → underflow wraps, bypasses the guard, and the progress renderer `txtbuffer[(int)(offset/size*30)] = '*'` (`mp3.c:31-32`, `txtbuffer` is 2048 B in .bss) writes at a wild index on the next frame — user-triggerable in the first second of any track. `mp3.c:144-146` — "Previous" on track 0 walks `allFiles[-1]` (heap-metadata pointer chase). `mp3.c:85` forward-skip uses wrap-prone `offset + 0x8000 < size`. Crafted zero-size `.mp3` makes `offset/size` NaN → `txtbuffer[INT_MIN]`. **Fix:** clamp the index to 0..29, refuse `size==0`, subtract-with-min for rewind, clamp PREV.

### S-32 — Low · Media/robustness tail *(reported)*
`pngdec.c:50-71`: duplicate IHDR chunks each leak up to ~4 MiB (`raw` overwritten, never freed) — crafted jewel-case art exhausts the 24 MB heap. Remaining pngdec/capture paths verified bounded. `jewelcase.c:1436`: `JewelCase_Enter` stores an unclamped `selected` (all current callers clamp first — fragile invariant). httpd send/RST handling for well-behaved clients (`httpd.c:90-125`).

---

# 6. Host-side PC tools (`pc/`)

### S-33 — High · `pc/sspd` remote memory-exhaustion DoS *(reported)*
`pc/sspd/src/fsp.rs:266-280` — FSP `CC_UP_LOAD` grows a per-source `Vec` to packet-chosen `pos+len`; one 12-byte UDP datagram with `pos=0xFFFFFFFF` forces a ~4 GiB zeroed allocation; the map is keyed by (spoofable) `SocketAddr` with no count/TTL cap. `server.rs:26,89` + `ssp.rs:143-157`: unbounded `streams` map (~124 KiB per READ, independent pacing). **Fix:** cap upload size, map size, add TTLs; pace globally.

### S-34 — Medium · `pc/sspd` root-jail escape via symlinks *(reported)*
`pc/sspd/src/fs.rs:20-30` — `resolve()` filters `..` but never canonicalizes; all file ops follow symlinks. A pre-existing out-pointing symlink inside the served root (common when serving real directories) lets remote clients read/write/delete/rename outside it. **Fix:** canonicalize and verify containment, or reject paths traversing symlink components.

### S-35 — Low · Misc host-tool robustness *(reported)*
`buildtools/dol2gci.cpp:186-237`: `size_t`→`int` truncation + signed overflow in GCI size math, unchecked `malloc` (needs ~2 GiB crafted DOL into the dev tool). `pc/jewelcase-preview/host_main.c:69-108`: several argv-driven stack overflows/NULL derefs (own-CLI only). `wii/booter/main.c`: ARGV write offsets come from the about-to-be-executed DOL (no privilege boundary crossed — OK by design).

---

# 7. Lower-severity findings (table)

| ID | Severity | Location | Issue |
|---|---|---|---|
| L-01 | Medium | ftp_devoptab.c:339-353 | PASV 227 parser `strchr()+1` before NULL check (latent — active mode hardcoded at :1126 with `if(true)`, `ftpUsePasv` ignored); also active-mode data socket accepts any peer on a sequential port |
| L-02 | Medium | ftp_devoptab.c:315-326, 1731-1770 | `SocketRecv` treats `ret==0` as success → infinite zero-byte recv loop; unbounded `goto read_retry`; `ftp_get_response` unlimited multi-line reply loop — server-side permanent hangs |
| L-03 | Low | ftp_devoptab.c:684-685 | one-byte stack underflow on truncated LIST line (`d[strlen(d)-2]`) |
| L-04 | Low | ftp_devoptab.c:2149 | uninitialized `file->envIndex` in `__ftp_open` (latent) |
| L-05 | Low | ftp_devoptab.c:1248-1256, 2489+ | dangling dir-list pointers after failed refresh (latent UAF/double-free) |
| L-06 | Low | deviceHandler-FTP.c:158-171, deviceHandler-FAT.c:157, deviceHandler-SMB.c:149 | u64→u32 size/offset truncation vs server/exFAT-claimed sizes (browse path checks, stat/open don't) |
| L-07 | Medium | deviceHandler-FTP.c:113-116, deviceHandler-FSP.c:60-75, deviceHandler-FAT.c:105-148 | server/sd-controlled listing growth with unchecked `reallocarray` → NULL deref crash (OOM) |
| L-08 | Low | fsplib.c:349-354, 594-602 | FSP names not truncated at `\n` → protocol field injection; `sin_port` byte order bugs (functional) |
| L-09 | Low | fsplib.c:1124 | `fsp_install` 2-byte stack overwrite when `timestamp!=0` (no in-tree caller) |
| L-10 | Medium | config.c:190-193, smb stack | NTLMv1-only, no signing; SMB password written in plaintext to swiss.ini |
| L-11 | Low | smb (libtinysmb) | DiskInformation parsed without validation (bad metrics); UTF converters negative-length reuse (`smb.c:226-303, 2011-2013`) |
| L-12 | Medium | swiss.c:1873-1876, files.c:266-269 | silent destination-path truncation → wrong-file overwrite (see S-29) |
| L-13 | Low | filelock.c:68-77 | unbalanced `unlockFile` wraps `lockCount` to 0xFFFFFFFF → self-wedge; no owner check |
| L-14 | Low | deviceHandler-CARD.c:120-144, 483 | free-space accounting in `int`; card size shift UB — wrong free-space info |
| L-15 | Low | wiiload.c:165, 236; wiiload.c:154-191 | `setenv` from worker threads; TCP/USB handler not mutually serialized → GUI/suspend races |
| L-16 | Low | httpd.c:90-125 | unchecked header sends + immediate close → RST truncation for well-behaved clients |
| L-17 | Low | gcm.c:141-142, 421-424, 432 | unchecked header `readFile` results → decisions on uninitialized stack structs |
| L-18 | Low | dvd.c:401-409 | non-terminated NPDP drive ID printed with `%s` (heap bytes in debug log) |
| L-19 | Low | util.c stripInvalidChars note | output ≤ 2× input — safe today because callers bound input < PATHNAME_MAX; document the invariant |
| L-20 | Low | gcm.c / calc sizes | `FSTOffset+FSTSize <= 0x8000` and apploader-size u32 wrap arithmetic weakens validity checks (nkit.c:2986-2989) |
| L-21 | Info | settings/config tables | enum↔string table bounds verified consistent; `DrawGetTextEntry` bounded; GUI `Draw*Label` strings are data, not printf formats (no format-string bugs found) |

---

# 8. Areas audited and found sound (non-exhaustive)

- `httpd` request parsing is primitive but bounded: 1024-byte recv into a 1026 buffer, literal `strncmp` route matching — no path traversal/URL-decode surface exists.
- `concat_path`/`concatf_path` (`files.c:202-260`) are `strlcpy`-correct and PATHNAME_MAX-bounded at every audited call site.
- `boot_dol` `.cli` handling, autoboot trailer parsing, update_recent, GUI text entry, renderer index math, comparator/sort/scan flattening, game-id handling — verified bounded.
- `cube/packer` xz-decompress stub (runs on-console at boot): bounds verified; embedded payload is build-time, boot image is CRC-protected.
- pngdec core decode loop (dims ≤1024, exact-size inflate, bounded palette/filter math), capture/JPEG/AVI encoders (clamped bit writer, `REC_MAX_BYTES` guards), uisound, scene3d (no external input), IPLFontWrite (NUL-driven, bounded carets).
- EXI glue (`exi.c`), drive-version sniffing, `dvd_error_str`, static patch tables' consistency (`make_pattern`/`compare_pattern` lengths), `readFileBlockAligned`, FatFs volume/index plumbing, ffsystem lock glue, rt4k socket printf usage.
- `pc/sspd` FSP/SSP parsers themselves are bounds-checked (Rust); issues there are resource-exhaustion/jail-escape only.
- `wii/booter`, `buildtools` Python utilities — no meaningful attacker boundary (documented in findings where applicable).

---

# 9. Recommendations (priority-ordered)

**Immediate (block next release):**
1. Fix the six browse/boot-time parsers: S-01 (strlcpy + FST bounds), S-02 (cap `filesToPatch`), S-03 (validate `MaxFSTSize`/`FSTSize`/`fstLength`), S-04 (bound legacy config entries), S-07 (ISO9660 `nl`/`files` caps), S-08 (TGC FST clamp + NULL check + `snprintf`).
2. Fix the network RCE: S-05 (FTP command buffer), and either patch libtinysmb or disable SMB (S-10); add recv timeouts to wiiload/httpd (S-12/S-14).
3. Fix host-side S-09 (usbgecko server) — any user running it is exposed.

**Short term:**
4. S-06 (SYS write clamps), S-11, S-16 (cheat cap), S-17 (calloc wrap), S-20/S-21 (ATA), S-22 (flash safeguards), S-23 (ELF bounds), S-24 (patcher length discipline), S-25 (CARD), S-28 (copy alloc checks), S-30 (banner), S-31 (MP3).
5. Add allocation-check + error-propagation pass: `grep -n "memalign\|malloc\|calloc\|reallocarray"` — dozens of call sites lack NULL checks on attacker-influenced sizes.
6. Add a `checked_add/checked_mul` discipline for every `offset+size` read from a file/header (u64 math), and reject >4 GiB sizes/offsets in `statFile`/`open` paths of all network devices exactly as `readDir` already does.

**Hardening / process:**
- Build with `-fstack-protector-strong` if devkitPPC permits (cheap even without TLS entropy), `-Wformat=2 -Werror=format-security`, and enable `-Wconversion`/`sign-compare` at least on `gcm.c`, `dvd.c`, `ftp_devoptab.c`, `fsplib.c`, `deviceHandler-*.c`.
- Fuzz harnesses on the PC for `get_fst_details`/FST parsing, ISO9660 `read_direntry`, cheat/config/`.dcp` parsers, `pngdec`, TPL (once an in-repo loader exists).
- Treating `libtinysmb` and libogc2 TPL as fork-vendored dependencies going forward would let bounds fixes land locally.
- Document that wiiload/httpd/SMB expose the console on the LAN and that SMB credentials are stored/transmitted weakly.

*Verification statement:* every Critical finding (S-01…S-10) and most Highs were re-confirmed by direct reads of the quoted lines during consolidation; remaining items are included with code citations from the audit passes. As with any audit, absence of a finding is not proof of absence — the DVDs/ISO9660, FTP LIST, FST and TGC parsers in particular warrant fuzzing after fixes.

---

# Remediation

All in-scope findings were fixed as individual commits on top of the audit tree (one commit per finding ID or tight cluster, verified by `docker/build.sh dev` builds):

| Finding | Fix commit |
|---|---|
| S-01 FST strcpy stack smash | `657ce648` |
| S-02 + S-08 filesToPatch/TGC bounds | `738e6473` |
| S-03 FST staging address validation | `6d693575` |
| S-04 legacy config stack overflow | `fd0d4385` |
| S-05 FTP command buffers | `ed6f480d` |
| S-06 SYS SRAM/ARAM clamps | `1d740326` |
| S-07 ISO9660 parser | `6e6b73c5` |
| S-09 swissserver host overflows | `fcaef45a` |
| S-10 **libtinysmb (dependency)** | not fixable in-repo — the SMB wire parser lives in libogc2; requires upstream libtinysmb bound-checking pass. Consider warning users not to expose SMB shares on untrusted networks. |
| S-11/S-12 wiiload | `8ee13392` |
| S-13 FSP client | `872e5b61` |
| S-14 httpd | `511d3d4a` |
| S-15 TPL backdrop | `af1bdc47` (in-repo pre-validator) |
| S-16 cheat engine cap | `2e760b0a` |
| S-17 size+1 allocations | `435f4dc4` |
| S-18 .dcp parser | `01f9f8c9` |
| S-19 config hygiene | `d956cb1d` |
| S-20 ATA IDENTIFY/timeouts | `e5356d06` |
| S-21 ATA DISC_INTERFACE init | no repo change — the code matches libogc2 `master` (12-field struct); the fault only surfaces when mis-building against the older packaged libogc2 (r2476). Build with `docker/build.sh` / current libogc2. |
| S-22 flash safety (WKF/Qoob/KunaiGC/GCLoader + .fzn read) | `77b234da` |
| S-23 ELF/DOL loaders | `0019a673` |
| S-24 patcher | `8d46a289` |
| S-25 memory-card import | `a1a3a276` |
| S-26 multigame table | `7d2fe655` |
| S-27 WODE/flippy/usbgecko/rt4k/info | `430c2f71` (`b99431` corrected msg) |
| S-28 copy/verify allocs | `45d2db9b` |
| S-29 copy/delete/dest-dir robustness | `45d2db9b` |
| S-30 banner metadata | `ee8e631e` (validation) + `45d2db9b` (consumers) |
| S-31 MP3 player | `397e271a` |
| S-32 pngdec/jewelcase | `af1bdc47` |
| S-33 sspd resource exhaustion | `9fa6861a` |
| S-34 sspd symlink jail | `9fa6861a` |
| S-35 dol2gci/jewelcase-preview | `c32d96d9` |
| L-01…L-09 FTP robustness | `ed6f480d` + `04963b29` |
| L-13 filelock, L-14 card accounting | `430c2f71`, `a1a3a276` |
| L-17/L-20 gcm read-checks/u64 math | `03f68950` + `03c36cc7` |
| L-18 NPDP print | `7d2fe655` |
| plus SMB glue size/name guards | `aee39e9a` |
| plus FAT/diskio guards | `d533d01a` |
| plus ELF machine check + DOL extension guard | `5294759a` |

Not fixed (deliberate): SMB NTLMv1/plaintext credential storage (documented; inherent to the protocol), wiiload's TCP/USB handler mutual exclusion (needs LAN attacker + simultaneous USB host;Low), httpd short-send/RST niceties (protocol robustness, not memory safety).
