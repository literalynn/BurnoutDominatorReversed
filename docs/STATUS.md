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
- 502/502 engine tests and 54/54 GS cache CTest tests pass on Windows. These tests cover the engine; they do not validate Burnout's races.
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

Runs 12 to 18: the IPU model delivers the MPEG data and the signed branches of the generated code now test the full 64-bit register. Regeneration (51,161 files, no unhandled instruction, no error) and the Windows rebuild succeeded. The game gets past the former video blocker and now shows **PROFILE**. SIO2 PIO transport for disconnected ports, RPC execution in scheduled IOP threads and IOP alarms are implemented and tested. Run 18 stayed on the memory card check (MCSERV's GetInfo RPC never completed). libmc is now bound to the runtime's memory card (45 bindings, saves in host files); run 20 on the owner's machine gets past the check up to the 50 Hz / 60 Hz choice. A DualShock 2 is emulated on SIO2 port 1 and DS2O drives it in pressure mode (cloud trace); pressing CONTINUE is still to be checked with the ISO. The menu and the races are not reached yet. Details: [BOOT.md](BOOT.md) §13–14.

Runs 25 to 55 (October 14): a crash of the front-end scripts (a runtime unwinding bug) and the failed profile save (libmc command ordering) are fixed. With `BDR_DETERMINISTIC=1` and a scripted controller, the game goes through the title screen, creates and saves a profile, plays the menu video, enters World Tour (series, event and car select) and loads the first event up to its briefing screen. Remaining: heavily corrupted textures in the menus and in 3D, and speed: about 30–45 VSync/s in the menus, 4 VSync/s once 3D is drawn. Details: [BOOT.md](BOOT.md) §15.

Runs 56 to 71 (October 14): the VU1 engine of the Dragon Quest VIII fork, with compiled VU1 programs, doubles the speed of 3D scenes, and three upstream GS fixes remove the striped rendering. The game reaches the first race: the 3D scene renders correctly, the post-processing leaves it washed out and blurred, and the race runs at 1 to 1.5 VSync/s because the game sends about 250,000 GS primitives per VSync to a CPU rasterizer. A GPU GS backend (upstream paraLLEl-GS work) is the likely next step. Details: [BOOT.md](BOOT.md) §15.

Runs 72 to 84 (October 14): the GS now runs on paraLLEl-GS (Vulkan compute, CHANGES.md). Two bugs had to be fixed on the way: an endless loop in paraLLEl-GS's page tracking (patched at configure time) and VIF1 DIRECT image data re-sent under a synthetic GIF tag, which a GS that keeps the PATH2 tag state reads as pixels. The intro runs at 50–54 VSync/s, menus at 35–52, and the first race at about 16 VSync/s instead of 1–1.5, the game thread no longer waiting for the GS. The race shows road, scenery, traffic and a readable HUD, with strongly wrong texture colours; the menus had corrupted textures with the CPU rasterizer as well, so the texture data reaching the GS is the next suspect.

October 15: the race and the menus render correctly (CHANGES.md). The causes were in the recompiler and the runtime, not in the GS: `PABSW` read the wrong register (the track streamer loaded wrong sections), `CVT.W.S` rounded instead of truncating, host floats produced Inf/NaN that the EE never does, `SQRT.S` read fs instead of ft (every square root in the game was wrong: car bodies squashed flat, bumper-height chase camera), PATH3 texture uploads were released at the wrong time around MSKPATH3 windows (wrong textures in the race, the title logo and the 50/60 Hz screen) and DMA chains were cut at 4096 tags (wrong textures every other frame at high speed). The game now goes from boot through the menus, World Tour, car select and a full race with the chase camera and the car visible, at about 20 to 30 VSync per second (40 to 60% of full speed); in console time the game draws one frame per VSync (50 per second), as on a PAL console. Save states (F5/F7, `--load-state`) restore the whole machine exactly. After a race the game saves the profile to the memory card and goes on to the next event (three `sceMcGetDir` fixes). The car paint and its reflections are right since MR32 no longer stalls on lanes it does not read (CHANGES.md).

October 16-17: speed and the player side. Compiled VU0 microprograms (+49% in a race) and the declared frame-end idle loop (the EE made 3 million polling passes per second) bring a race to 42-51 VSync per second of 50 on a Ryzen/RTX 5090 PC; the medal and car-select screens, which render a 3D scene behind the menu, run at 35 and 47. VIF1, VU1 and the GS submission run on a second thread (`gs_thread`), which leaves the game thread 60% idle on this PC; the frame rate is now bound by the VU1 microprograms alone, compiled and cycle-accurate at about 10 ns per instruction pair, 1.5 million pairs per race frame. The next step for speed is a faster VU1 code generator (VF registers kept in XMM registers across a block, stall checks resolved at compile time where the entry state allows), not more threads. `settings.ini` next to `Play.bat` holds the window, vsync, frame-rate cap, aspect, SSAA and dead-zone settings; gamepads go through SDL2 (Xbox, PlayStation, Switch; keyboard always active). Resolution: 4x SSAA renders at an internal 2x, but paraLLEl-GS's high-resolution scan-out comes out duplicated side by side with Burnout's post-processing blits (its promoted back-buffer does not follow the frame through the blit), so the output stays 640x512 scaled to the window; a true higher resolution needs work in paraLLEl-GS's scan-out, not in the game. Frame rate: the game steps its simulation once per VSync (50 or 60 Hz); it reads the COP0 Count register for a frame-step governor (0x1E6728: 1 to 4 VSyncs per frame from elapsed cycles at 294.912 MHz) which the runtime leaves at 0, so the step is always 1 (slow motion rather than frame skipping when the host is slow). Higher frame rates would mean changing the game's fixed step, not the runtime.

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
- **Controllers.** Burnout uses `libdbc`/`libpad2` with DBCMAN.IRX and DS2O.IRX, which talk to the SIO2 port. The emulated IOP now answers the DualShock 2 protocol on port 1 (`iop_sio2_pad.cpp`), fed by the first PC gamepad or the keyboard: DS2O enters configuration, switches the pad to analog with pressure (mode 0x79) and polls it every frame (cloud trace, `BDR_TRACE_SIO2=1`). Vibration values are received but not yet sent to the PC gamepad; port 2 and the multitap stay empty.
- **Loading times.** Disc reads (simulated CDVD latency: 128 IOP cycles) and memory card (host files) are already nearly instant. What remains are waits coded in the game (minimum loading screen duration, save delays), to find once those screens are reached.

## Work in progress and remaining criteria

The Ghidra/R5900 analysis refines function boundaries. Logs and the translation audit must be checked before any claim of full coverage. Building the runtime and running the exact binary are separate steps.

Still to establish: indirect code and executed data, VU and IOP overlays, real boot, menu, races, physics, collisions, input, graphics, DMA/VIF/GIF synchronisation, sound, videos and saves. Observed behaviour must be compared with PS2/PCSX2 execution. Linux and macOS must be built and tested on their own systems.

The upstream runtime is still experimental: partial hardware support, limited VU/GS performance and incomplete audio. A successful Windows build only establishes a native executable made of the translated functions.
