# Boot of SLES_546.27

Sequence established by static analysis (disassembly, Ghidra pseudocode, call graph), then checked against bounded runs of the recompiled binary. The SDK functions mentioned are documented in [SDK_FUNCTIONS.md](SDK_FUNCTIONS.md).

## 1. crt0 (`entry` 0x100008)

| Address | Action |
|---|---|
| 0x100008–0x100110 | Clears the GPRs/FPRs, `sync`. |
| 0x10011C–0x100194 | Clears the BSS 0x432B00–0x1F9BB00. |
| 0x1001C8 | `syscall 0x3C` SetupThread(gp = 0x43A4F0, stack 0x1FF0000, size 0x10000, args 0x433080, root 0x100220). |
| 0x1001E4 | `syscall 0x3D` SetupHeap(0x1F9BB00, −1). |
| 0x1001E8 | `jal 0x3B2BD0`: libkernel initialisation. |
| 0x1001F0 | FlushCache(0), then `ei`. |
| 0x100208 | `main(argc = *0x433080, argv = 0x433084)` = 0x21B3F8. |
| 0x100210 | `j 0x322630`: exit(main()). |

## 2. Initialisation (main → 0x207820)

`0x207820` initialises the global game state, then calls among others:

1. `0x2156B8`: IOP reboot (§3);
2. `0x215928`, `0x215918`, `0x1E7018`, `0x1E7050`, `0x2160C8(0x561E78)`, `0x214128`, `0x1E6BE0`;
3. `0x1E68A0(0x3C)` and `0x1E6668(0x41855555, …)` if the byte at 0x432E98 is 0, otherwise `0x1E68A0(0x32)` and `0x1E6668(0x41A00000, …)`: 60 or 50 frames per second, i.e. NTSC or PAL (deduced from the constants);
4. further on, `0x1B48F8` (at 0x207A44) → `0x1E6EB8` → `0x38B0F8` → `0x38B050` → `0x38AFE0`: reads `rom0:ROMVER` through fileio.

## 3. IOP reboot (0x2156B8)

```
sceSifInitRpc(0)                                   0x3B0020
sceCdInit(0)                                       0x377CA0
do r = sceSifRebootIop("cdrom0:\IOP\IOPRP300.IMG;1") while (r == 0)   0x3B2370
do r = sceSifSyncIop() while (r == 0)              0x3B2320
sceSifInitRpc(0)
0x3B1E70()   loadfile client reset (EE only)
0x3B1430()   fileio client reset (EE only)
sceCdInit(0); sceCdGetDiskType(); sceCdMmode(2 if DVD, 1 if CD); sceCdDiskReady(0)
```

The command actually sent is `rom0:UDNL cdrom0:\IOP\IOPRP300.IMG;1`. The runtime treats the reboot as a reset of the emulated IOP; the modules of IOPRP300.IMG (CDVDMAN, CDVDFSV, FILEIO, LOADFILE, MODLOAD, SIFCMD…) are not run. Their EE services are provided by the bindings in `src/burnout_overrides.cpp`.

## 4. Loading the IOP modules (0x215798)

`sceSifInitIopHeap()` (0x3B1B48), then 11 iterations over the pointer table 0x3C5268, each `0x215688(path, 0, 0)` → `sceSifLoadModule(path, 0, NULL)`. A negative return stops the loop.

| # | Module | Role (deduced) |
|---|---|---|
| 1 | `cdrom0:\IOP\SIO2MAN.IRX;1` (VA 0x419230) | SIO2 manager |
| 2 | SIO2D.IRX | SIO2 driver |
| 3 | DBCMAN.IRX | libdbc controller manager (SID 0x80001300) |
| 4, 5 | DS2O.IRX (the same entry 0x419288, loaded twice) | DualShock 2 controller |
| 6 | MCMAN.IRX | Memory card |
| 7 | MCSERV.IRX | Memory card RPC server (SID 0x80000400) |
| 8 | LIBSD.IRX | Sound (SPU2) |
| 9 | RWA.IRX | RenderWare Audio |
| 10 | B4ROUTE.IRX | Criterion audio routing |
| 11 | GTFSCDVD.IRX | Criterion GTFS file system |

Not loaded: PADMAN.IRX and MC2_S1.IRX (on the disc, no reference). MTAPMAN is not on the disc, but `0x395A30` looks up a "multitap_manager" module by name.

Then: `0x1E2278(0x561D18, 1, …, 0x419368, 3, 0x20)` initialises the GTFS client, which binds SID 0x475453 ("GTS") at 0x1E23C8. Then `0x2159F0` → `0x1E60E0` initialises the controllers.

In the runtime, a `cdrom0:` module is first loaded physically into the R3000 interpreter; the HLE service of the same name (DBCMAN, MCSERV, LIBSD) is only used if that load fails. The emulated IOP has no SIO2 device model: the physically loaded SIO2MAN, DBCMAN, DS2O and MCMAN will therefore probably see neither controller nor memory card (deduced, to check at the first run that reaches this step).

## 5. IRX import coverage by the virtual IOP kernel

Static classification of the import tables of the disc's IRX against the built-in libraries of `ps2xIOP/src/emulator`. Only the gaps are listed.

| Module | Missing import | Effect before the fix | State |
|---|---|---|---|
| MCMAN, MC2_S1 | cdvdman:24 sceCdReadClock | warning, v0 = 0 | **Implemented** (JST time in BCD) |
| RWA | thbase:43 GetSystemTimeLow | silent return, v0 unchanged | **Implemented** |
| SIO2D, MCMAN | secrman:6 SecrAuthCard | warning, v0 = 0 (authentication failure) | Left as is: without a SIO2 model, no card can answer |
| RWA | ioman:4–8 open/close/read/write/lseek | warning, v0 = 0 | Open. A call at 0x7F90 opens with mode 0x602 (write, create, truncate): probably debug output |
| RWA, GTFSCDVD | sifcmd:8, 10, 11 (command handler tables) | v0 = 0, no handler registered: EE commands never arrived | **Implemented** |
| GTFSCDVD, DBCMAN, PADMAN | sifman:32 sceSifSetDmaIntr | v0 = 0, neither transfer nor callback: the GTFS read thread sleeps forever | **Implemented** |

Unknown ordinals of the built-in libraries without fallback (thbase, thsemap, thevent, sifcmd, sifman, sysclib) used to return silently. They now produce a `[IOP] unhandled built-in import` warning, once per import.

## 6. Bounded runs

Command: `python tools/project.py run --headless --seconds 20 --status-ms 1000`. Code 124 = time limit reached, 3 = missing function.

| # | Bindings | Result | Cause |
|---|---|---|---|
| 1 | none | 124; PC stuck at 0x3B0178, RA 0x2156D4; IOP: 0 instructions | The guest sceSifInitRpc waits for the IOP's reply over the SIF hardware, which is not emulated. |
| 2 | libsif (6) | 124; PC stuck at 0x377E28, RA 0x377D74 | The guest sceCdInit retries binding SID 0x80000592: no CDVDFSV server. |
| 3 | + loadfile, iopheap, libcdvd (15) | 3; JALR from 0x1E3910 to 0x1E5020 | 0x1E5020 is only reached through a pointer (a0 = 0x535BC4, whose first word is 0x4027E0): missing from the Ghidra map, between FUN_001E4EE8 (end 0x1E501C) and FUN_001E5030. |
| — | fix | `tools/augment_function_map.py`: +2,191 entry points (1,684 in gaps, 507 inside functions), 10 rejected. Regeneration: 51,161 C++ files, 0 unhandled instructions, 4,420 warnings, 0 errors; `ptr_001e5020_0x1e5020.cpp` exists. Full rebuild succeeded (10 min 22 s). | |
| 4 | + fileio, internal libcdvd (21), PAL ROMVER | Linux (cloud, without ISO), 124; JALR to 0 at 0x32C054 | `RwEngineInit` (0x32CBF0) fails: `EndOfHeap` is 0x1F00000, below the heap base 0x1F9BB00. Every `sbrk` (0x3AD6B0) fails, RenderWare writes through a null pointer and its function table (0x1F578E4) stays empty. |
| 5 | + `SetupHeap(-1)` ending at the thread's stack; runtime memory below the image | 124; loop at 0x372BC8 | `RwEngineInit` succeeds and the 11 IRX load. The EE waits for reply 0x12 from RWA.IRX: EE SIF commands did not reach the IRX handlers. |
| 6 | + libsif commands (28), sifcmd:8–11 and sifman:32 on the IOP side | 124; boot loop 0x21B5B0 in state 4 | RWA answers, GTFS reads `Language/Fonts/dirtyEra.bin` (RPC 1, 3, 5, DMA then command 4). State 3 asks for `Data/GlobalE.txd` (808 KB), missing from the 30 MB cloud bundle: only a run with the ISO can go further. One GIF frame per loop. |
| 7 | + real ISO (Windows) | 124; state machine 0x207D08 in state 9 (0x29DA28 waits for the `sound_generic.awd` stream to open); the LOADING screen stops moving | RWA receives and answers SIF commands, but its engine is clocked by SPU2 IRQs, which the emulated IOP did not generate (§8). |
| 8 | + SPU2 (§8) | 124; EE looping at 0x3B1DA0 (caller 0x3B1D1C) | All of `sound_generic.awd` is transferred (commands 0x32/0x33). Then 0x396A78 calls 0x3B1EA8 (sceSifSearchModuleByName): 0x3B1CE0 retries forever binding the loadfile SID 0x80000006, which no server serves (loadfile is inside IOPRP300.IMG, not run). |
| 9 | + sceSifSearchModuleByName (29 bindings), IOP and timers advanced in batches | 124; main loop (0x208390, sub-state 7), boot state 0x1C, LOADING screen at about 93%; 25 to 50 frames per second | `0x207D08` has finished. The loading scene waits for `TRACKS/EATRAX1.RWS`: blocker of the time (§9). The reads of `FE/FEMAIN.BIN` and `TRACKS/US/S7_V1/STATIC.DAT` succeeded. |
| 10 | + CD reads past an allocated block (§9) | 124; loading finishes (sub-state 5) then the main thread sleeps at 0x3ACF08 (`SleepThread` called by 0x1E7328), no more frames | 0x1E7328 waits for an alarm (`SetAlarm(6, 0x1E7308, thread)`) that never fires: libkernel's kernel patch replaced the alarm system calls (§11). |
| 11 | + system calls replaced by unrecompiled code served by the runtime | 124; black screen, EE looping at 0x38A2C8 | Alarms work; the game starts the intro video and its MPEG-2 parser waits for data from the IPU (§12). |

Runs 4 to 6 were made on Linux from the `tools/cloud_bundle.py` bundle, without the ISO (`--disc` only, virtual image). The boot state machine is `0x207D08`, state at 0x51B920: 1 = IOP modules, 2 = font, 3 and 4 = `Data/Global%c.txd`, then the rest.

During runs 1 and 2, the VBlank counter advances by 60 per second: the game has not called SetGsCrt yet, which switches the runtime to 50 Hz in PAL.

## 7. Next blockers

1. Reading `rom0:ROMVER`: **solved** (fileio bindings, 16-byte ROM0 profile).
2. Physical loading of the 11 IRX: **solved** (imports of §5).
3. GTFS client (SID "GTS"): **solved** on the real ISO; the game reads the font, `FE/FEMAIN.BIN` and `TRACKS/US/S7_V1/STATIC.DAT` (run 9).
4. Reading `TRACKS/EATRAX0.RWS` / `EATRAX1.RWS`: **solved** (§9).
5. EE alarms replaced by libkernel's kernel patch: **solved** (§11).
6. Intro video (IPU): **solved** in runs 12 to 14 (§12–13).
7. Memory card: **solved** by the libmc binding (§14); the owner's run 20 reaches the 50 Hz / 60 Hz choice. Controllers: DualShock 2 on SIO2 port 1 (§14), to confirm on the owner's machine by pressing CONTINUE.

## 8. SPU2 and RenderWare audio (RWA)

**SPU2 model** (`ps2xIOP/src/emulator/core/iop_spu2.cpp`, 16-bit accesses from `iop_memory.cpp`).

- Registers at 0x1F900000: 0x10-byte voices (VOLL, VOLR, PITCH, ADSR1, ADSR2, ENVX, VOLX), voice addresses SSA/LSAX/NAX, core registers at +0x400 per core (PMON, NON, VMIXL/R, MMIX, ATTR, IRQA, KON, KOFF, TSA, DATA, ADMAS, ENDX, STATX), master volumes at 0x760 + 0x28 per core, SPDIF_IRQINFO at 0x7C2.
- ATTR: 0x8000 enables the core, 0x40 the IRQs, bits 5:4 select the transfer (1 PIO write, 2 DMA write, 3 DMA read). STATX is 0 when idle, 0x400 during a transfer and 0x80 once finished as long as the transfer mode stays active. libsd (run physically) waits for `(STATX & 0x7FF) == 0`: a STATX forced to 0x80 when idle blocked `sceSdInit`.
- SPU DMA: channel 4 (core 0, CHCR 0x1F8010C8, IRQ 0x24) and channel 8 (core 1, CHCR 0x1F801508, IRQ 0x28); CHCR 0x01000201 = start RAM → SPU, BCR = (blocks << 16) | 16 words. An address IRQ (IRQA reached by a voice or a transfer) raises IOP IRQ 9 and SPDIF_IRQINFO.
- Clock: at every sample (48 kHz), an enabled core writes the mixer output buffers to SPU2 RAM (voice 1, voice 3, left and right mix, 0x200 halfwords each; for core 0, halfwords 0x400 to 0x5FF receive voice 1) and tests IRQA on both cores. RWA places IRQA in one of these buffers (halfwords 0x400 then 0x500) to receive an IRQ every 256 samples: that is its engine's clock, without which opening a stream never finishes.
- Limits: no reverb, noise, pitch modulation, ADMA or volume sweeps; linear interpolation; no output to the host's sound card.

**RWA protocol** (SIF commands, see also SDK_FUNCTIONS.md).

- EE → IOP: command 0, type in word 4 of the packet (0x49, 1, 2, 0x11, 0x13, 0x15, 0x4E, 0x30, 0x32, 5, 0x14 at boot). IOP → EE: command 1, with its own reply type (observed: 1 → 9, 2 → 0xA, 0x11 → 0x1A with IOP address 0x12C030, 0x13 → 0x1B with 0xC25B0 and 0x100000, 0x15 → 0x1C with 0x5A850, 0x30 → 0x31, 0x32 → 0x33, 0x4E → 0x4F, 5 → 0xD, 0x14 → 0x14).
- 0x32: stream slice request; the 0x70-byte descriptor goes to IOP 0x12C030. Reply 0x33 carries the completion function 0x3721C0, which clears the "pending" flag (0x10) of the descriptor and calls the stream slot's callback. 0x372110 waits for that bit.
- EE: command 1 handler at 0x3719D8 (8-entry queue at 0x1F64740, `iSignalSema` of the semaphore stored at 0x432D78), thread 2 (entry 0x362F50). IOP: command 0 handler at RWA+0x3D70 (8-entry queue and SignalSema), thread T11 (RWA+0x4044, 0x38B0), SPU transfers through 0x4568 then 0xA44C (list at 0xC300 + 8 × index), channel pumps T12 and T13, ISR threads of priority 9.
- Steady state: every frame, command 0x14 (0x680 bytes to IOP 0x5A850, with an increasing counter) and reply 0x14.

## 9. Refused CD read (run 9, solved in run 10)

The 0x207D08 state machine has finished (state 0x1C at 0x51B920) and `main` (0x21B3F8) loops on 0x208390, whose sub-state at 0x51B950 stays at 7: as long as 0x208E50 returns nothing, the loading scene (object 0x4C1B38, state 8 at +0x1C; sub-object 0x1CEDB00, state 7) waits for the file request `tracks/eatrax1.rws` (object 0x1CF55C0, handle state 2 at +0x24C).

Observed:

- `BDR_TRACE_CDVD=1`: about 560 `sceCdRead` per second, always `lsn=2007487` (start of `TRACKS/EATRAX1.RWS`), 4 sectors, to IOP 0x12C210 (RWA's buffer).
- `BDR_IOP_PROFILE=1`: nearly 100% of IOP instructions are in GTFSCDVD.IRX, 0x1110–0x1150, and the cdvdman stubs 0x2168 (`sceCdRead`) and 0x2180/0x2188: this is the inner loop of 0x10CC, `do { sceCdDiskReady(1); …; sceCdRead(…) } while (result == 0)`. `sceCdRead` therefore returns 0 every time and the loop leaves no time to the other IOP threads, so RWA request 0x32 never gets its 0x33 reply.

Cause, verified by logging the refusal: the game reads 4 sectors (0x2000 bytes) at the start of a 0x1840-byte block allocated at 0x12C210; the last 0x7C0 bytes land in free memory (first free byte 0x12DA50). `readSectors` (`iop_cdvd.cpp`, upstream) refused any read whose destination was not entirely allocated or written (`IopMemory::ownsRamRange`) and returned 0, which GTFSCDVD retried forever. The console's CD DMA writes whole sectors wherever it is told: `readSectors` now only checks the IOP RAM bounds and reports the overflow (at most 4 times). The same read targets `EATRAX0.RWS` (LSN 1911758) or `EATRAX1.RWS` depending on the randomly chosen track.

## 10. Observation tools

Environment variables, to set before `python tools/project.py run …` (all off by default):

| Variable | Effect |
|---|---|
| `BDR_STATUS_DETAIL=1` | on every `[status]` line: EE threads (entry, pc, ra, priority, wait), semaphores, flags, IOP kernel threads and objects, log of its operations |
| `BDR_STATUS_WATCH=0xA,0xB+N,@0xC+OFF` | guest memory words added to the status: one word, N words, or the word at OFF from the pointer stored at 0xC |
| `BDR_TRACE_SIF=1` | SIF commands both ways (first 1,200, 80 for GTFS commands 4 and 5) |
| `BDR_TRACE_CDVD=1` | every `sceCdRead` (LBA, sectors, destination); the ISO's ISO9660 directory gives back the file |
| `BDR_IOP_PROFILE=1` | where the IOP spends its instructions (16-byte slices, module + offset) |
| `BDR_TRACE_IOPCALLS`, `BDR_TRACE_IOPHW`, `PS2X_IOP_TRACE_SPU`, `BDR_TRACE_GS` | calls of selected IOP functions, IOP hardware writes, SPU2, GS |
| `BDR_TRACE_KERNEL=1` | `SetSyscall` (number, handler, recompiled code or not), `SetAlarm`, `CancelAlarm` and alarm expiry |
| `BDR_TRACE_IPU=1` | IPU commands and DMA starts (§13) |
| `BDR_EE_CYCLE_SCALE=P` | charges P% of the EE cycles reported by the generated code (default 100). The recompiler charges 32 cycles per backward branch and 8 per call, whatever the work done; this knob tests whether that count slows the game |
| `BDR_GS_THREADS=N` | GS rasterization threads (default: half the cores, at most 8); `1` draws immediately on the game thread, for comparisons |
| `BDR_TRACE_SIO2=1` | SIO2 commands (port, bytes sent and received); identical consecutive pad polls are skipped (§14) |
| `BDR_PAD_SCRIPT=cross@1300-1310,start@2000-2004` | holds controller buttons on port 1 between two VSync ticks (headless runs) |
| `BDR_DETERMINISTIC=1` | VBlank, alarms and timers on EE cycles only: identical runs, idle time skipped, speed unrelated to real time |
| `BDR_WATCH_WORD=0xADDR[:0xVALUE]` | after every dispatched guest function, reports a change of the word at ADDR (or its first change to VALUE) with the last 32 dispatched functions |
| `BDR_DISPATCH_HISTORY=1` | keeps the last 256 scheduler dispatches (thread, pc, sp, ra) and prints them when the scheduler reaches a pc without generated code |
| `BDR_TRACE_GS_VSYNC=N` | every primitive drawn during VSync tick N: primitive type, frame, TEX0 (base, width, format, size, CLUT base, format, mode, offset), FBMSK, ZBUF, TEST, ALPHA, CLAMP, TEX1, XYOFFSET, first two vertices (position, UV, ST) and first vertex colour |
| `BDR_DUMP_GS_DIR=dir` | with `BDR_TRACE_GS_VSYNC=N`: writes each render target of VSync N to `dir` as a PPM (`target_NNN_fbpP_psmM.ppm`) when the game moves on to another target, to follow multi-pass effects |
| `BDR_PROFILE_DELAY_S=N` | with `BDR_PROFILE=1`: starts sampling after N seconds, to profile one phase (e.g. the race) |
| `BDR_PROFILE_DUMP=file` | with `BDR_PROFILE=1`: writes every sampled address with its count, module, symbol and offset, to map the samples onto a compiler listing (`/FAcs`): the PDB has no inlined source lines for the compiled VU1 programs |
| `PS2X_GS_BACKEND=cpu` | keeps the CPU rasterizer in a build made with paraLLEl-GS (the default when Vulkan works) |
| `BDR_PGS_RECORD=file` | paraLLEl-GS: records every backend input (initial VRAM, GIF packets with their path, registers, presentations) |
| `BDR_PGS_REPLAY=file` | in `ps2x_tests`: replays a recording into the CPU rasterizer and paraLLEl-GS and lists, at each presentation, the 8 KiB pages that differ; `BDR_PGS_REPLAY_FIRST=N` stops at the first record leaving more than N differing bytes (with `BDR_PGS_REPLAY_DUMPFB=fbp,fbw,psm`, both buffers go to `replay_divergence.ppm`); `BDR_PGS_REPLAY_DUMP=dir` writes the displayed buffer of both sides for the first 16 presentations |
| `BDR_PGS_VRAM_PPM=dir` | paraLLEl-GS: at each presentation, the DISPFB1 buffer as read back from its VRAM |
| `BDR_PGS_TRACE=1` | paraLLEl-GS: prints the display registers at each presentation and the transfer registers of each local-to-host readback |
| `BDR_PGS_RECORD_FROM=N`, `BDR_PGS_RECORD_VSYNCS=M` | with `BDR_PGS_RECORD`: records only VSync ticks N to N+M, starting with the VRAM of tick N |
| `BDR_TRACE_DMA_CHAIN=N` | prints the tags (address, id, QWC, ADDR) of up to N DMA chains whose data exceeds 512 KiB |
| `BDR_TRACE_DMA_KICKS=first,count` | every GIF, VIF0 and VIF1 DMA start of VSync ticks first..first+count, with the bytes it queued, what each channel still has queued and the PATH3 mask |
| `BDR_VU_REPLAY=stem` (ps2x_tests) | replays a `PS2_VU_CAPTURE_DIR` capture (`stem.code/.data/.state`) on the VU1 interpreter step by step; `BDR_VU_REPLAY_PCS=pc,...` prints `BDR_VU_REPLAY_VF=n,...` registers, some VI registers, P, Q and the cycle count whenever execution reaches those addresses; `BDR_VU_REPLAY_OUT=file` saves VU data memory at the end |
| `BDR_TRACE_VIF1=first,count` | every VIF1 command of VSync ticks `first` to `first+count`, with TOPS/ITOPS/BASE/OFST/DBF/CYCLE/MODE |
| `BDR_WATCH_DUMP=file` | with `BDR_WATCH_WORD`: EE RAM at the first report (reports also list the GPRs) |
| `BDR_RDRAM_DUMP=vsync,file` | EE RAM (32 MiB) at that VSync tick |
| `BDR_TRACE_VIF1_DATA=1` | with `BDR_TRACE_VIF1`: the first qword of each V4-32 UNPACK of 1 to 5 qwords (microcode parameters); `=2` the first qword of every UNPACK, `=3` also up to 8 more qwords of V4-32 UNPACKs |
| `BDR_VU_BADKICK_DIR=dir` | VU1 data, code and registers and EE RAM at the first XGKICK whose first GIF tag has only NLOOP/EOP and a non-zero REGS word, or whose PACKED data runs past VU1 qword 1006; VU1 data and registers at the last kick before it |
| `BDR_PROFILE_STACKS=N` | with `BDR_PROFILE_SECONDS`: prints N call stacks of the profiled thread, 200 ms apart |
| `BDR_PROFILE_SECONDS=N` | with `BDR_PROFILE=1`: after N seconds of sampling, prints the report and the sampled thread's call stack, even if that thread never returns (hangs) |
| `BDR_GS_STATS=1` | once a second: primitives queued to the GS raster workers, barriers inserted because a texture reads pending writes, serial operations, worker launches, drains and the time the game thread waited for them |
| `BDR_PROFILE_THREAD=name` | with `BDR_PROFILE=1`: samples that thread instead of `GameThread`, e.g. `GSRaster0` for a GS raster worker |
| `BDR_STOP_AT_VSYNC=from,to` | stops the run at VSync `to` and prints the wall time and the CPU time of `GameThread`, `GsWorker` and `GsSubmit` since VSync `from`, with the VU1 pairs run compiled and interpreted. With `BDR_DETERMINISTIC=1` a replay does the same work every time, so the CPU times compare builds (Windows only for the CPU times) |
| `BDR_PROFILE=1` | sampler of the guest thread (EE and IOP run on it; GS rasterization too with `BDR_GS_THREADS=1`); prints the most frequent functions at the end of the run. Windows only; needs the PDB, produced by `/DEBUG` (`cmake/Burnout.cmake`) |

With these tools, the profile showed the game ran at 7% of real time because the IOP and the timers were advanced every 8 EE cycles; they now are in batches of 2048 cycles (CHANGES.md).

## 11. libkernel's kernel patch (run 10)

At boot, libkernel installs two patches of the EE kernel, as on the console (trace `BDR_TRACE_KERNEL=1`):

- 0x3B2AC0 then 0x3B2E78 (alarms): `SetSyscall(0x83, 0x3B2A68)` (address lookup) and `SetSyscall(0x5A, …)` (copy), copy of code into kernel memory, `SetSyscall(0x5B, 0x80076000)`, then for 0xFC, 0xFE, 0xFD, 0xFF, 0x12C and 0x8: `SetSyscall(n, syscall 0x5B(n))`.
- 0x3B24F0 (event flags): `SetSyscall(0x5A, 0x3B2498)`, copy of 0x330 bytes from 0x3E2038 to 0x80075000, `SetSyscall(0x5B, 0x80075000)`, `SetSyscall(0x54, 0x3B2840)`, then 0x55 to 0x59.

The code copied into kernel memory is not recompiled: the runtime cannot run it. Before, a system call whose replaced handler was not recompiled code returned −1; `syscall 0x5B` therefore returned −1, and `SetAlarm`, `iSetAlarm`, `ReleaseAlarm`, `iReleaseAlarm` as well as event flags 0x55–0x59 all failed. `sleep(6)` (0x1E7328: `SetAlarm(6, 0x1E7308, thread)` then `SleepThread`) never woke up. Such a call is now served by the runtime's implementation (`dispatchSyscallOverride`, `Syscalls/System.cpp`). `SetSyscall` still writes the entry into the kernel table visible to the game, which the patch also uses to write kernel words through signed indexes. Test: "a syscall override that is not recompiled code falls back to the builtin" (`ps2x_tests`).

## 12. Intro video (run 11)

After loading, the screen turns black and the main thread spins in 0x38A2C8: the MPEG-2 stream parser of the game's video library (start codes 0x1B3 sequence, 0x1B8 GOP, 0x100 picture, 0x1B7 end) looks for a start code (`0x388030(…, 0x18)` until 1, then `0x3880C0(…, 8)`), reading bits from the IPU decoder. The DMA and GIF counters stop moving. The runtime had no IPU emulation: its registers were only stored, and DMA channels 3 and 4 were not processed.

## 13. IPU and signed branches (runs 12 and 13)

An IPU model is now wired to the registers and FIFOs of `PS2Memory`, and to DMA channels 3/4. The tests check among others resuming a command after more data arrives, BDEC to RAW16, CSC conversion and source chains. BDEC waits for the output DMA, then detects the next start code after the stuffing bytes. Intra samples are saturated to 0–255; inter blocks keep their signed residuals. IDEC is still not implemented. No guest video code is replaced or skipped.

Run 12 (`run12-codex-ipu.log`, 60 s, code 124): the loading screen finishes, then the screen stays black. The PC is now in `0x387DC8` (CMD wait), notably `0x387EF4`, and the game keeps restarting the input DMA. At the end: 739,491 DMA starts and 426 GIF packets, no missing function.

Run 13 (`run13-codex-ipu-trace.log`, 24 s, `BDR_TRACE_IPU=1`): the stream does arrive. Three 8-bit FDECs give `0x0001B328`, `0x01B32801`, then `0xB32801E0`. On the last one, BUSY is cleared (`CTRL=0x00800008`) and the FIFO holds 8 qwords, with input QWC `0xFF7`. Yet the game keeps waiting: the `BGEZL` at `0x387E08` and `0x387EF8` are translated with `GPR_S32`. That tests bit 31 of the data, instead of bit 63, which holds BUSY in the CMD register read by `LD`.

The recompiler (`control_flow_emitter.cpp`) now uses `GPR_S64` for BLEZ/BGTZ/BLTZ/BGEZ and all their likely/link variants. The twelve variants are covered by a test; the 452 runtime/recompiler tests pass with MSVC. The 64-bit register comparison is also what [PCSX2's R5900 interpreter](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/Interpreter.cpp) does. The C++ files must be regenerated and the game rebuilt to apply this fix.

`BDR_TRACE_IPU=1` traces the first 160 commands and DMA starts, then one in 10,000: command, DATA, CTRL, BP, TOP and DMA registers. Off by default, the trace stays in the local logs ignored by Git.

## 14. PROFILE screen and memory card (runs 14 to 18)

After regenerating and rebuilding with the 64-bit fix, run 14 (`run14-codex-branch64.log`, 60 s, code 124) gets past the former video blocker. The trace shows more than 440,000 IPU commands, BDEC included; the game shows the **PROFILE** screen, with the memory card check message. Captures are in `local/diagnostics/run14-codex-branch64/`. This does not validate the full fidelity of videos and sound.

Run 15 (`run15-codex-profile-card.log`, 24 s): the EE waits in `sceSifCallRpc` (0x3B0848, caller 0x3B594C). The SIO2MAN thread waits for flag 0x2000, set by its IRQ 17 handler at SIO2MAN+0x584. No SIO2 emulation produced this interrupt. MCMAN makes transfers through SIO2MAN, with delays and semaphores.

A SIO2 PIO transport for disconnected ports was added: `0xFF` bytes, RECV1 reporting no device, IRQ 17 on CTRL START and INTR acknowledgement. The reference behaviour is documented by [PCSX2's Sio2](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/SIO/Sio2.cpp) and [its port states](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/SIO/SioTypes.h). Run 16 (`run16-codex-sio2.log`, 25 s): SIO2MAN does receive the interrupt and goes back to waiting for a command; the check is still stuck. Memory cards and controllers are not reported as present.

The physical RPC function ran through `callFunction` outside any IOP thread (T0). Its waits therefore did not suspend the server. It now runs in a temporary thread, with a reusable stack, while the scheduler advances the other threads and devices. Run 17 (`run17-codex-rpc-thread.log`): this fix shows that MCMAN waits for its 100 µs alarm before resuming WaitSema. `thbase:35` SetAlarm returned without registering a callback; the RPC hits its limit and produces an explicit error.

IOP alarms 35–38 are now handled by the emulator's import dispatcher (identities and ABI in [PS2SDK's thbase.h](https://github.com/ps2dev/ps2sdk/blob/master/iop/system/threadman/include/thbase.h)). A synthetic module checks DelayThread then SetAlarm/SleepThread/iWakeupThread and 128 successive RPC calls, with a real resume in the same thread and stack reuse.

Run 18 (`run18-codex-iop-alarm.log`, 35 s, code 124): no runtime error and no missing function; the PROFILE screen stays on the memory card check. SIO2MAN's command thread goes back to waiting on 0x4155, the RPC servers get through their flag waits, and the EE stays in 0x3B0848 during the libmc callback 0x3B57C8. Still to check: MCSERV's GetInfo reply (SID 0x80000400, function 1), the data transferred to the EE and the end of the libmc callback, before blaming this wait on the missing device alone.

Targeted resume:

```powershell
$env:BDR_STATUS_DETAIL = '1'
$env:BDR_STATUS_WATCH = '0x1F776C0+4,0x1F76600+8,0x3E36D0+3,0x1F76128+3'
python tools/project.py run --headless --seconds 35 --status-ms 5000 --log run19-libmc.log --tail 120
```

Run 19 (owner, `run19-sectors.log`, 40 s, `BDR_TRACE_CDVD=1`): same wait as run 18. Its 549 disc reads (6,994 sectors) were packed with `cloud_bundle.py --sectors-log` and replayed in the cloud on a sparse image (`tools/sparse_iso.py`). The replay diverges at read 420: the music position comes from a Mersenne Twister with a fixed seed (0x1105, seeded at 0x1321E0) whose number of draws depends on timing, so another song position (unrecorded, read as zeros) is chosen.

All 16 libmc 3020 entry points are now bound to the runtime's memory card (SDK_FUNCTIONS.md), stored in host files under `--save`. Cloud replay with these bindings: `sceMcGetInfo` (port 0: PS2 card, 8,192 free clusters, formatted), `sceMcChdir("..")`, `sceMcGetDir("*")` complete; the PROFILE screen ("Checking memory card (PS2) in MEMORY CARD slot 1") is followed by the next front-end screen. What follows reads sectors that were not recorded (the `TVID/ABG.M2V` background video), so it is not evidence; it has to be confirmed on the machine with the ISO.

Run 20 (owner, `run20-sectors.log`, 40 s, code 124, commit 64e1009): 45/45 bindings, `sceMcChdir`/`sceMcGetDir` complete, no runtime error; the screen shows the 50 Hz / 60 Hz choice with CONTINUE, which needs a controller. The screenshot shows rendering defects on that screen (yellow horizontal bands, white discs), still to analyse.

DualShock 2 on SIO2 (`ps2xIOP/src/emulator/core/iop_sio2_pad.cpp`): pad packets (first byte 0x01) on port 1 (SIO2 port 0) are answered by a DualShock 2 model following the controller protocol (commands 0x40–0x4F; reference: [PCSX2's PadDualshock2](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/SIO/Pad/PadDualshock2.cpp)); RECV1 reports port 1 present and port 2 empty. The input comes from the runtime's pad backend (first gamepad, otherwise the keyboard) through `IopHost::readControllerPad`. Cloud replay of run 20 with `BDR_TRACE_SIO2=1`: DS2O polls in digital mode, enters configuration (0x43), reads 0x45/0x46/0x47/0x4C, sets analog and locks it (0x44 01 03), maps the motors (0x4D 00 01), asks for every pressure byte (0x4F FF FF 03), probes 0x40, leaves configuration, then polls 21 bytes per frame in mode 0x79. SIO2MAN sends these packets by PIO; DMA is unused here. The replay then stops on the LOADING screen with the RWA lag already seen in run 19 (unrecorded music sectors), so the 50/60 Hz screen and CONTINUE must be checked on the owner's machine. `BDR_PAD_SCRIPT="cross@1300-1310"` holds a button between two VSync ticks for headless runs.

## 15. Menus, a crash in the front-end scripts, speed (runs 25 to 38)

Run 26 (Windows, 120 s, `BDR_PAD_SCRIPT` pressing Cross every 250 VSyncs, frames every 4 s): the game goes through the 50 Hz / 60 Hz screen, the EA and Criterion logos, the title screen and PROFILE. Rendering defects are visible: the BURNOUT DOMINATOR logo misses a vertical slice, textures in random colours sit behind the title text, white discs and yellow bands on the 50 Hz screen, thin vertical seams.

Runs 25 and 27 stop around VSync 600–660 (50 Hz screen) with a jump to 0xB415CB, run 26 at VSync ~2600 on PROFILE with a jump to 0 from 0x11F51C; runs 28–30 with the same settings do not. In runs 25 and 27 every register is identical: the front-end script interpreter (opcode loop 0x10694C, opcode function 0x10E118, which saves `ra` at `0(sp)`) returns to 0xB415CB, a script byte pointer. That value legitimately lives at 0x1FFF760, the instruction pointer variable of a caller's frame passed down by address (trace of 0x10E118 with `--trace-watch 0x1FFF760`); the crash means a new frame was placed on top of that live variable, i.e. the stack pointer had moved up. 0x10E118 had been resumed by the scheduler at 0x10E170 after its call to 0x1022D0 (a virtual call) left the thread. Not caused by the GS threads (same crash with `BDR_GS_THREADS=1`) nor by the scratchpad stack swap (0x1277D8 / 0x127858 save `sp` in 0x432DC0, which never changes in these runs). `BDR_DETERMINISTIC=1` runs at cycle scales 60–150 never reproduce it, while six parallel headless runs (more host load) crash in one to three cases. `BDR_DISPATCH_HISTORY=1` then shows every crash the same way: the timer 1 interrupt (handler 0x3A0EA8, empty callback table) is taken while the main thread is in the mutual recursion 0x336458 → 0x335D88 → 0x336458 → 0x335D88 (GIF packet building, scratchpad flushes), and the thread resumes at 0x336488, the return address of the *outer* call to 0x335D88, with `sp` still in the inner frames. Cause: `dispatchGuestBranch` took the unwinding call for a finished one because `ctx->pc` equalled the callee's entry (CHANGES.md, checkpoint unwinding). **Fixed**: 0 crashes in 12 runs. Neither instant memory card completion nor interrupts delivered between `di` and `ei` were the cause (tested); the scheduler now masks the latter anyway.

Speed (headless, intro video): 25 VSync/s with the game thread waiting 42% of the time for GS drains (TEXFLUSH, uploads, CLUT loads, FINISH). With the queued serial operations (CHANGES.md): 31 VSync/s with 8 workers, the same with 12 or 16; the game thread itself now needs about 26 ms per VSync (20 ms for full PAL speed): EE dispatch and scheduler overhead, IPU decoding and colour conversion, C++ exceptions used for thread switches. In deterministic mode with `BDR_EE_CYCLE_SCALE=125` the boot stops at the RWA wait (0x372BC8).

Menus and saves (runs 48 to 53, `BDR_DETERMINISTIC=1` with `BDR_PAD_SCRIPT`, which repeats exactly): Cross every 120 VSyncs from 1100 to 3200 reaches the PROFILE menu (Load / Create Profile) at VSync ~3325; Down at 3400 and Cross at 3520 choose Create Profile, then Cross every 300 VSyncs answers Autosave (Yes), accepts the default name BURNOUT and selects the first profile slot. The save then failed ("Save failed. An error occurred while saving game data"): the game opened `view.ico`, issued `sceMcWrite` and `sceMcFlush` back to back and never got the write's byte count (CHANGES.md, memory card saves). Fixed: the three save files are written and "Save Successful" appears at VSync ~4800, followed by the EA TRAX banner and the menu background videos. A card left half-written by an earlier failed save makes the next attempt fail in another way (an `Open` of `BESLES-54627-BD` while the current directory is `/` returns -5), to be looked at.

World Tour (run 55, same script, then Cross every 400 VSyncs from 7800 and every 500 from 13500): the menu video plays until VSync ~12600 (Start and Cross do not skip it), then WORLD TOUR / SERIES SELECT (Classic Series), EVENT SELECT, CAR SELECT with the 3D car (VSync ~14100), LOADING ("Burnout Chaining is back!") and the event briefing BURNOUT CHALLENGE / CLASSIC SERIES with CONTINUE (VSync ~15000). The 3D scenes are badly corrupted and the speed drops to 4 VSync/s as soon as they are drawn, which looks like a freeze. `BDR_TRACE_GS_VSYNC=N` prints every primitive drawn during VSync N (texture format, CLUT, frame) to analyse these screens.

Profile of that phase (run 56, `BDR_PROFILE=1`): the VU1 interpreter takes about 45% of the game thread (pipeline commit, operand normalisation, pair scheduling, exact FMAC rounding); Burnout draws all its 3D through VU1 microprograms. Skipping the per-cycle pipeline scan until the next result is due gave no measurable gain (still 4 VSync/s) and was dropped: the cost is in executing the microcode itself, which calls for a VU1 recompiler rather than tuning the interpreter. In the car select frame (VSync 14140), textured primitives use PSMT4 (most), PSMT8, PSMT8H and CT32 with 32-bit CSM1 palettes near the top of VRAM; the corrupted menu images are paletted textures, cause not identified yet.

VU1 engine from the Dragon Quest VIII fork (CHANGES.md, run 58, same script): the interpreter alone runs the first 3D scenes at 8–9 VSync/s instead of 4, reaching VSync 16,263 instead of 15,509 in the same 560 s; no crash. `run --vu-capture` recorded 114 VU1 program entries for compilation.

Race rendering (runs 59–71, same script, race from VSync ~15,900). With the upstream GS fixes (CHANGES.md, GS rasterizer fixes) the vertical blue and white stripes are gone and the race start shows a coherent but washed-out, blurred scene. `BDR_DUMP_GS_DIR` (run 61) shows the 3D scene itself is drawn correctly in the colour buffer at page 80 (road, sky, power lines); the blur comes from the post-processing chain. That chain copies the scene in 64-pixel strips into 128-pixel buffers at pages 240 and 241, which alias the main Z buffer (ZBP 0xF0); downsamples them with bilinear sprites into pages 272 and 400; and adds the result back to page 80 (ALPHA `Cd + Cs`). The strip pass is set up as a subtractive threshold, `(Cs - Cd) * 255 / 128`, but PABE is on and the source alpha is 0 (MODULATE with vertex alpha 0), so the GS skips blending and the pass is a plain copy: all 32,768 compared pixels match the scene. Without a reference capture it is not settled whether this, and so the brightness of the result, matches the console.

Speed in the race: 1 to 1.5 VSync/s. The game thread waits for the GS 82–86% of the time (`wait[gs=]`); a raster worker (`BDR_PROFILE_THREAD=GSRaster0`) is busy 91% of the time, mostly sampling textures (bilinear with 32-bit CLUTs: four texel reads, CLUT lookups and float interpolation per pixel). The texture page cache copied 8 KiB on every page change; keeping 64 copies (CHANGES.md, texture page cache) removed that cost, but the speed did not change. Neither did 14 workers instead of 8. `BDR_GS_STATS=1` explains why: the game submits about 380,000 primitives per second, roughly 250,000 per VSync, with no read barriers or serial operations, and every worker walks every primitive. A CPU rasterizer of this design cannot reach full speed at that load. Upstream pull request ran-j/PS2Recomp#285 (work in progress) adds a GS backend on paraLLEl-GS (Vulkan compute, LGPLv3); that is the likely route to a playable race.

Race on paraLLEl-GS (runs 84–98). The first race runs at about 16 VSync/s and the picture stays still: the clock shows 132.40 and the frames only differ by interlacing. A recording of four VSync ticks (`BDR_PGS_RECORD_FROM=17000`) holds nothing but one 646 KB PATH2 packet sent 30 times: a HUD gauge (bottom left) drawn 80 times per packet, from 80 DMA REF tags to distinct buffers holding identical vertices. No scene geometry is sent, so the 3D image on screen is left over from earlier frames, and the game thread spends its time waiting for the GPU on these flushes. Every run since run 59 also logged `[VU1 reserved lower] pc=0x18e8 instruction=0xfffffff8` from the race loading on: not an instruction but the XGKICK code for a GIF tag with FLG 3, which stopped VU1 at each such kick (fixed, CHANGES.md). With that fix the error is gone and the car select shows more of its scene, but its textures stay noisy, as with the CPU rasterizer, and the race still does not advance. Neither the DMA scratchpad bit nor the GIF/VIF order changed that. Next leads: texture data reaching the GS (uploads and CLUTs of the car select), and what the race loop waits for.

Race start, GIF desynchronisation (runs 99–104). From VSync 15,894 the GS receives local-to-host readbacks with impossible registers (BITBLTBUF 0x3b0e6f503cf942cc, TRXREG asking for 15 to 32 MB): vertex data read as A+D register writes. A recording of VSync 15,885–15,897 run through a GIF parser (`BDR_PGS_RECORD`, per-path tag state) finds the first such write in a PATH1 packet (VU1 XGKICK). The two kicks before it carry a sound tag (`lo` 0x302e400000008050, `hi` 0x412: NLOOP 80, EOP, PRE, PACKED, STQ/RGBAQ/XYZF2, 3,856 bytes); the bad one has the same NLOOP and EOP (`lo` 0x8050) but zero PRE/PRIM/NREG and float data in REGS, so it is read as 80 loops of 16 registers (20,496 bytes) over a normal 3-qword vertex stream. The microcode wrote the vertices and NLOOP but the tag template part (PRIM, NREG, REGS) is not in that output buffer: the template normally comes from the header VIF1 unpacks into VU memory. VIF1's TOP/TOPS/DBF handling for MSCAL, MSCNT, OFFSET and BASE and the TOPS-relative UNPACK address match PCSX2; next step: compare VU1 data memory at a good and at this bad kick (`PS2_VU_CAPTURE_DIR`) to see which header write is missing. On the GS side these packets make paraLLEl-GS and the CPU rasterizer draw garbage and wait for impossible readbacks, which matches the noisy textures and the race that does not advance.

Why the template is missing (run 106, `BDR_VU_BADKICK_DIR`, which dumps VU1 data, code and registers at the first bad kick). The microprogram keeps two output buffers in VI13/VI14, set by its init code to 0x220 and 0x306; init also loads the tag template into VF1 from the constant at qword 1006 (`LQ.yzw vf1, 1006(vi0)`) and stores it into both buffers (`SQ.yzw vf1, 0(vi13)` / `0(vi14)`). Each batch then writes only NLOOP|EOP (`ISW.x`), kicks the buffer and swaps them. At the bad kick, qwords 1000–1014 of VU1 memory hold vertices instead of constants: a batch of 80 vertices (1 + 240 qwords) written at 0x306 ends at qword 1015 and overwrites the template at 1006, so the next init stores vertices as the template. The vertex loop reads its input from both ends (`LQI` up, `LQD` down) until the pointers cross and never checks the output size, so the batch size comes from what VIF1 put in the input buffer (TOP, ITOP and the batch header). The console must therefore send batches of at most 77 vertices here: our VIF1 input to this program is larger than the console's. Next: check VIF1 UNPACK (modes, masks, USN, ROW/COL, CYCLE) and ITOP against PCSX2 on this batch.

Remaining limits: SIO2 DMA and dmacman, memory card on SIO2 (bound at libmc level instead), vibration output, menu and races. No game result is forced.
