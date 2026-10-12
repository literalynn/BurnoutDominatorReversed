# Project status

The full reverse-engineering goal is not reached yet. This repository holds the reproducible work, the recompilation pipeline and the observations verified on the original disc. It does not claim a playable port or recovered original source code.

## Observed identity

- ISO: 4,660,166,656 bytes, SHA256 `dc4fc4e9700d6a5ae26531618d880541b1ff2d2deaf191f1359b98d3f63f96a5`.
- BOOT2: `SLES_546.27`, 3,472,132 bytes, SHA256 `fe8b4b28e165620a35e55bcf6fb452de16cf7982ea6584ab28dc5f6b4b187301`.
- ELF32 little endian, MIPS R5900, entry point `0x00100008`, 2 PT_LOAD segments, 79 sections, no symbol table.
- `.text`: address `0x00100000`, 2,878,248 bytes. `.data` is also marked executable; that does not prove its words are instructions.
- 64 DVP/VU overlays with their overlay tables; 12 IOP IRX modules.
- Disc inventory: 570 files, 50 directories. Every file is extracted locally; sector reads must use the original ISO.

Kept evidence: `local/disc_inventory.json`, `local/analysis/`, `local/analysis_validation.json`.

## Checks done so far

- 20 Python tests of the ISO9660/ELF32 parsers pass: bounds, endianness, symbols, paths, partial and full extraction.
- SHA256 of the 13 extracted binaries and of SYSTEM.CNF verified.
- PS2Recomp and PS2Analyzer tools built with MSVC x64, upstream commit pinned in `UPSTREAM.json`.
- Read-only [REA](https://github.com/morluto/rea) diagnosis: no engine configured; its documented Windows scope does not cover PS2 ELF/R5900.
- First heuristic attempt: 8,514 functions generated, 0 functions forced to a stub, 0 functions skipped. The report shows **5,216 unhandled instructions** and 2,086 warnings. The first and last errors are in `.data`. The recompiler's exit code 0 therefore does not guarantee a correct translation.
- Ghidra 12.1.4 analysis with EmotionEngine Reloaded 2.1.38: 8,860 functions identified in `.text`, plus 40,109 executable labels; the 65 records outside `.text` are kept separately.
- Refined translation: **0 unhandled instructions, 0 errors**, 4,158 warnings about indirect branches; 48,961 translated bodies and 8 automatic SIF adapters from the engine, 0 skipped functions. The 8 adapters are listed in `generated/ps2_recompiled_stubs.h`; they are not invented success returns.
- 437/437 upstream engine tests and 45/45 GS cache CTest tests pass on Windows. These tests cover the engine; they do not validate Burnout's races.
- Disc access fixed: EE/IOP file searches now return the original LBAs. Five IOP test programs pass, including seven ISO test groups; offsets above 4 GiB verified. On the real disc: SYSTEM.CNF at sector 2,265,203 and SLES_546.27 at sector 2,263,507.
- SPU2 (`ps2_spu2_tests`): ADPCM decoding, registers, PIO and DMA transfers, voice IRQ timing, transfer IRQ, mixer clock, loop end, audio output: pass.
- After run 11 (Windows, MSVC): `ps2x_tests` 443/443, the five ps2xIOP suites, `ps2_spu2_tests` and `burnout_overrides_tests` (29 bindings) pass.
- After run 18 (Windows, MSVC): `ps2x_tests` 452/452 and all IOP, SPU2, ISO9660 and Burnout binding suites pass; the RPC test really waits on DelayThread and an alarm, then repeats 128 calls without running out of stacks. The 43 Python tests pass. The IPU and 64-bit branch commits also passed the Windows/GCC/Clang CI.
- Linux (Ubuntu 24.04, GCC 13, Ninja): `ps2_recomp`, `ps2_analyzer`, the runtime and every test build; 438/438 engine tests, the 5 ps2xIOP suites and `burnout_overrides_tests` pass. The game also builds and starts on Linux (cloud session, `-O1`, from the `tools/cloud_bundle.py` bundle, without the ISO).

## Game boot

Details and evidence: [BOOT.md](BOOT.md) and [SDK_FUNCTIONS.md](SDK_FUNCTIONS.md).

- First Windows executable: 134 MB, built without LTO in 9 min 20 s.
- Bounded runs 1 to 3: the game goes through crt0, `main`, SIF and libcdvd initialisation, then the third run stops on an indirect call to 0x1E5020, code missing from the Ghidra map.
- `tools/augment_function_map.py` adds 2,191 entry points reached through pointers; regeneration (51,161 files, 0 errors) and the full rebuild succeeded.
- Since then: 29 SDK functions bound to the runtime (libsif with SIF commands, loadfile, iopheap, fileio, libcdvd), a 16-byte European ROMVER, `sceCdReadClock` and `GetSystemTimeLow` on the IOP side, a warning for every unknown built-in IOP import.
- Runs 4 to 6 (Linux, without the ISO):
  - `RwEngineInit` succeeds once the end of the heap is fixed;
  - the runtime's own memory moves below the game image;
  - SIF commands between the EE and the IRX modules (RWA audio, GTFS file system) and `sceSifSetDmaIntr` work;
  - boot reads the font `Language/Fonts/dirtyEra.bin`, then waits for `Data/GlobalE.txd`, missing from the cloud bundle;
  - the game sends one GIF frame per wait loop.
- Runs 7 to 9 (Windows, with the ISO):
  - a real SPU2 on the IOP side gives RWA the clock it expects: all of `sound_generic.awd` is then transferred;
  - `sceSifSearchModuleByName` (0x3B1EA8) is bound: the game checks that IOP modules (`sio2man`, `mcman`, `mcserv`, `multitap_manager`) are present before initialising the memory card library;
  - the boot state machine finishes (state 0x1C), the game enters its main loop and draws the LOADING screen (orange bar);
  - the IOP and the EE timers advance in batches: from 3.6 to 25–50 frames per second.
- Runs 10 and 11:
  - CD reads that overflow an allocated IOP block are done like on the console: the LOADING screen finishes;
  - system calls replaced by libkernel's kernel patch (alarms, event flags), whose code copied into kernel memory cannot run, are served by the runtime: `SetAlarm` wakes the main thread again.

Runs 12 to 18: the IPU model delivers the MPEG data and the signed branches of the generated code now test the full 64-bit register. Regeneration (51,161 files, no unhandled instruction, no error) and the Windows rebuild succeeded. The game gets past the former video blocker and now shows **PROFILE**. SIO2 PIO transport for disconnected ports, RPC execution in scheduled IOP threads and IOP alarms are implemented and tested. The memory card check is still pending: next step is the MCSERV GetInfo reply and the EE libmc callback. The menu and the races are not reached yet. Details: [BOOT.md](BOOT.md) §13–14.

To resume on the machine that has the ISO (the observation variables are described in BOOT.md §10):

```powershell
python tools/project.py run --headless --seconds 60 --status-ms 2000 --log run12.log --tail 120
```

## Requests for once the game is playable

These goals assume the game reaches the races; they come after boot. Leads already established:

- **File mods.** The game reads its data through GTFSCDVD.IRX, i.e. by sectors (`sceCdRead` in the emulated IOP), not by EE file names. A `mods/<path on the disc>` overlay therefore has to work at sector level: an LBA → file table taken from the ISO9660 directory, direct replacement when the file is not larger than the original, a rebuilt virtual image otherwise (`iop_cdvd.cpp` already builds a virtual ISO). To check: if GTFS reads the ISO9660 directories itself, new sizes and LBAs reach it without further changes.
- **Code mods.** Any guest function can be replaced by a native one (the mechanism of `src/burnout_overrides.cpp`); an API that loads native libraries from `mods/` follows directly.
- **Frame rate.** At boot, `0x207820` sets the frame rate and the simulation step from the byte at 0x432E98: `0x1E68A0(60)` and `0x1E6668(16.667 ms)`, or `0x1E68A0(50)` and `0x1E6668(20 ms)`. The physics assume this fixed step: running the loop faster would break it. 60 Hz is therefore available natively; above that (up to 480 Hz), the simulation must stay at 60 Hz and rendering (objects, camera) must be interpolated between steps, which requires identifying the game loop and the RenderWare scene.
- **2K resolution.** The runtime renders the GS on the CPU only (`gs_cpu_backend`), at PS2 resolution. A higher internal resolution needs GPU GS rendering behind the existing `GSRasterBackend` interface: runtime work, independent of the game.
- **Controllers.** The runtime reads a PC controller (Xbox, DualSense…) but only for `libpad`. Burnout uses `libdbc`/`libpad2` with DBCMAN.IRX and DS2O.IRX, which talk to the SIO2 port: the emulated IOP only has a SIO2 transport for disconnected ports and the simplified DBCMAN service reads no input. It needs either an emulated SIO2 fed by the PC controller, or a high-level DBCMAN/libpad2; a priority as soon as the game shows something.
- **Loading times.** Disc reads (simulated CDVD latency: 128 IOP cycles) and memory card (host files) are already nearly instant. What remains are waits coded in the game (minimum loading screen duration, save delays), to find once those screens are reached.

## Work in progress and remaining criteria

The Ghidra/R5900 analysis refines function boundaries. Logs and the translation audit must be checked before any claim of full coverage. Building the runtime and running the exact binary are separate steps.

Still to establish: indirect code and executed data, VU and IOP overlays, real boot, menu, races, physics, collisions, input, graphics, DMA/VIF/GIF synchronisation, sound, videos and saves. Observed behaviour must be compared with PS2/PCSX2 execution. Linux and macOS must be built and tested on their own systems.

The upstream runtime is still experimental: partial hardware support, limited VU/GS performance and incomplete audio. A successful Windows build only establishes a native executable made of the translated functions.
