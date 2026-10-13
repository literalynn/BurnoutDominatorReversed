# Sony SDK functions identified in SLES_546.27

The ELF has no symbols. The functions below were identified by static analysis (disassembly, Ghidra pseudocode, string references, RPC SIDs, callers). `src/burnout_overrides.cpp` binds them to the runtime's implementations; `tests/native/burnout_overrides_tests.cpp` checks every binding.

Rules applied:

1. Bind only an identity established by evidence: RPC SID and function number, referenced error strings or semaphore names, library version marker, arity compatible with the SDK API.
2. The runtime handler must have the same ABI: direction and order of the arguments, return value.
3. Bind together every entry point of a library that talks to the IOP: once some are replaced, the internal state of the guest library (RPC clients, semaphores) is no longer kept up to date.

Why bind: the runtime does not emulate the SIF hardware. A guest library that waits for an IOP RPC server loops forever if nobody serves it (runs 1 and 2 in [BOOT.md](BOOT.md)).

> **String addresses.** The address column of `local/analysis/SLES_546.27.strings.txt` is a file offset, not a VA. Segment 0: VA = offset − 0x1000 + 0x100000. Segment 1 (`.rodata`/`.data`): VA = offset − 0x306000 + 0x408080. Example: offset 0x32ADA0 "Libcdvd bind err %d CD_Init %d" = VA 0x42CE20.
>
> **Ghidra pseudocode.** For 0x378480, 0x378558, 0x3783C0 and 0x3781A0, Ghidra hides the return value (uncached `| 0x20000000` read of the reply buffer). Read the disassembly.

## Version markers

| VA | Marker |
|---|---|
| 0x3D4CC0 | `PsIIlibcdvd 3000` |
| 0x3D6878 | `PsIIlibdbc  3020` |
| 0x3E0A40 | `PsIIlibpad2 3020` |
| 0x3E0A70 | `PsIIlibscf  3000` |
| 0x3E36C0 | `PsIIlibmc   3020` |

## Functions bound to the runtime

| Address | Function | Handler | Evidence |
|---|---|---|---|
| 0x3AF8D8 | sceSifInitCmd | `sceSifInitCmd` | One-time initialisation guard 0x3E1F88; initialises the SIF command data (0x1F6F150…). Called first by 0x3B0020. |
| 0x3B0020 | sceSifInitRpc | `sceSifInitRpc` | Guard 0x3E1F8C; RPC data at 0x1F70B80; registers SIF command handlers 0x80000008/9/A/C; tests `sceSifGetReg(0x80000002)`. Run 1 loops at 0x3B0178. |
| 0x3B0668 | sceSifBindRpc | `sceSifBindRpc` | References "SceSifrpcBind" (VA 0x431CE8) at 0x3B06D4. |
| 0x3B0848 | sceSifCallRpc | `sceSifCallRpc` (system call list) | References "SceSifrpcCall" (VA 0x431CF8) at 0x3B0990. |
| 0x3B2320 | sceSifSyncIop | `sceSifSyncIop` | Called in a loop after the IOP reboot in 0x2156B8. |
| 0x3B2370 | sceSifRebootIop | `sceSifRebootIop` | Prefix "rom0:UDNL" (VA 0x431FD0) then calls sceSifResetIop 0x3B21C8. Argument: `cdrom0:\IOP\IOPRP300.IMG;1` (VA 0x419348). |
| 0x3AFBA8 | sceSifAddCmdHandler | `sceSifAddCmdHandler` | `(cid, handler, data)`: a negative cid indexes the system table (+0xC of the 0x1F6F158 data), otherwise the user table (+0x14); 12-byte entry `{handler, data, gp}`. Callers: RWA 0x371900 (cid 1), GTFS 0x1E2390/0x1E23A4 (cid 4 and 5), 0x203228 (cid 6), libcdvd 0x377A88, fileio 0x3B11E8/0x3B1200, guest sceSifInitRpc. |
| 0x3AFC20 | sceSifRemoveCmdHandler | `sceSifRemoveCmdHandler` | Same indexing, clears `handler`. Callers: RWA 0x371950, libcdvd 0x3779D0. |
| 0x3AFB90 | sceSifSetCmdBuffer | `sceSifSetCmdBuffer` | `(table, count)`: writes the user table (+0x14) and its count (+0x18), returns the previous one. Only caller: RWA 0x3718EC. |
| 0x3AFDA8 | sceSifSendCmd | `sceSifSendCmd` (system call list, which delegates to the `ps2_stubs` version) | `(cid, packet, psize, src, dest, size)`, arguments 5 and 6 in `$t0`/`$t1`; calls the core 0x3AFC70 with mode 0. RWA sends command 0 at 0x371548, 0x371600, 0x371810; GTFS at 0x1E2DB4. |
| 0x3AFDE8 | isceSifSendCmd | `sceSifSendCmd` | Same wrapper with mode 1 (interrupt context). The runtime finishes its SIF transfers within the call: the interrupt variant is the same call. Only callers: the EE-side RPC handlers (0x3B03F0–0x3B0604), installed by the guest sceSifInitRpc. |
| 0x3AFB58 | sceSifExitCmd | `sceSifExitCmd` | `DisableDmac(5)`, `RemoveDmacHandler(5, …)`, resets the 0x3E1F88 guard. Only caller 0x3B01C8, reached by the guest sceSifRebootIop. |
| 0x3AF8B0 | sceSifGetSreg | `sceSifGetSreg` | Reads word `index` of the 0x1F6F300 table. Only caller: the guest sceSifInitRpc (0x3B0178). |
| 0x3B2180 | sceSifLoadModule | `sceSifLoadModule` (system call list) | Wrapper of `_SifLoadModule(path, argc, argv, &res, 0)`; loadfile client, SID 0x80000006; data 0x1F725C0–0x1F727E8. |
| 0x3B1EA8 | sceSifSearchModuleByName | `sceSifSearchModuleByName` | loadfile client (SID 0x80000006), RPC 9: sends `{name}` (copied over 0xFC bytes, last byte forced to 0), reply `{module id}`. It first goes through 0x3B1CE0 (binds the SID then calls initialisation RPC 0xFF) and 0x3B1DE0 (checks the returned version). Only caller: 0x396A78, which formats the message "Error: IOP module missing: %s. You must load this module before initilizing the memcard library." (VA 0x42FDD8) when the result is negative. The runtime returns the id of the loaded IOP module whose internal name (IRX IOPMOD header: `mcman`, `mcserv`, `sio2man`…) is identical, otherwise −1. Without this binding, 0x3B1CE0 retried forever to bind SID 0x80000006, which the emulated IOP does not serve: loadfile is part of IOPRP300.IMG, which it does not run (run 8 in [BOOT.md](BOOT.md)). |
| 0x3B4E28 | sceMcInit (libmc 3020) | `sceMcInit` | Creates the two libmc semaphores (ids at 0x3E36D4, 0x3E36D8), `sceMcSync(0, 0, 0)` (0x3B5700), `sceSifInitRpc(0)`, binds MCSERV's SID 0x80000400 on client 0x1F76100, then RPC 0xFE. Only caller: 0x396720, after 0x396A78 checked that `sio2man`, `mcman` and `mcserv` are loaded. |
| 0x3B5050 | sceMcEnd | `sceMcEnd` | `sceMcSync`, then `DeleteSema` of both libmc semaphores. |
| 0x3B5700 | sceMcSync | `sceMcSync` | `(mode, *cmd, *result)`: PollSema/WaitSema on the semaphore at 0x3E36D4 depending on `mode`, current command at 0x3E36D0. Returns 1 when a command finished, 0 while one runs (mode 1), −1 when none was started; polled every frame by 0x396B20. |
| 0x3B5820 | sceMcGetInfo | `sceMcGetInfo` | RPC 1, NOWAIT, `(port, slot, *type, *free, *format)`: port and slot at +4/+8 of the packet 0x1F76180, the three pointers saved at 0x1F76128/2C/30 for the end function 0x3B57C8, which copies them from the reply (uncached) and `iSignalSema`s. Run 18: the RPC never completed. |
| 0x3B50C0 | sceMcOpen | `sceMcOpen` | RPC 2 `(port, slot, name, mode)`, name copied (0x3FF bytes) to the packet at +0x14. |
| 0x3B51E8 | sceMcMkdir | `sceMcMkdir` | `sceMcOpen(port, slot, name, 0x40)`, then current command 0xB. |
| 0x3B5220 | sceMcClose | `sceMcClose` | RPC 3 `(fd)`. |
| 0x3B52E0 | sceMcSeek | `sceMcSeek` | RPC 4 `(fd, offset, origin)`. |
| 0x3B5470 | sceMcRead | `sceMcRead` | RPC 5 `(fd, buffer, size)`, end function 0x3B53C0 copies the unaligned edges. |
| 0x3B5588 | sceMcWrite | `sceMcWrite` | RPC 6 `(fd, buffer, size)`. |
| 0x3B5EB0 | sceMcFlush | `sceMcFlush` | RPC 0xA `(fd)`. |
| 0x3B5B80 | sceMcChdir | `sceMcChdir` | RPC 0xC `(port, slot, path, current dir)`. |
| 0x3B59A0 | sceMcGetDir | `sceMcGetDir` | RPC 0xD `(port, slot, name, mode, max entries, table)`, arguments 5 and 6 in `$t0`/`$t1`; end function 0x3B5AF0. |
| 0x3B5F70 | sceMcSetFileInfo | `sceMcSetFileInfo` | RPC 0xE `(port, slot, name, info, flags)`. |
| 0x3B5D90 | sceMcDelete | `sceMcDelete` | RPC 0xF `(port, slot, name)`. |
| 0x3B5CC0 | sceMcFormat | `sceMcFormat` | RPC 0x10 `(port, slot)`. |
| 0x3B1B48 | sceSifInitIopHeap | `sceSifInitIopHeap` | iopheap client, SID 0x80000003. Called by 0x215798 before loading the modules. |
| 0x3B1BD0 | sceSifAllocSysMemory | `sceSifAllocSysMemory` | iopheap client, RPC 4, sends `{size, mode, addr}` built from `(mode, size, addr)`: typical call `(0, 0x40700, 0)`. The handler reads the size from a1. Callers: 0x1FB828, 0x29D608, 0x3629A8, 0x3692C0, 0x372160. |
| 0x3B1C50 | sceSifFreeIopHeap | `sceSifFreeIopHeap` | RPC 2, sends `{addr}`. Callers: 0x29DDC8, 0x369110. |
| 0x3B1468 | sceOpen | `sceOpen` | fileio client, SID 0x80000001; references "SceStdioOpenSema" at 0x3B15B8. Called with `(path, 1)`. The three fileio functions have only two callers, 0x379908 and 0x38AFE0. |
| 0x3B16F8 | sceClose | `sceClose` | References "SceStdioCloseSema" at 0x3B1778. |
| 0x3B1878 | sceRead | `sceRead` | References "SceStdioReadSema" at 0x3B1910. Called with `(fd, buf, n)`. |
| 0x377CA0 | sceCdInit | `sceCdInit` | Binds SID 0x80000592 in 0x1F65BE8 until `server != 0`; strings "Libcdvd bind err %d CD_Init %d" and "Libcdvd Exit" (mode 5). Called twice with 0 by 0x2156B8, return ignored. Run 2 loops at 0x377E28. |
| 0x3781A0 | sceCdDiskReady | `sceCdDiskReady` | "NEW DiskReady Call"; SID 0x8000059C; after 17 failures, falls back to 0x377F88. |
| 0x377F88 | sceCdDiskReady (old protocol, internal) | `sceCdDiskReady` | "OLD DiskReady Call"; SID 0x8000059A; only caller 0x378360. |
| 0x378458 | sceCdGetDiskType | `sceCdGetDiskType` | Wrapper of 0x3783C0 that turns −1 into 0. The caller 0x215730 compares with 0x12 (PS2 CD) and 0x14 (PS2 DVD). The runtime returns 0x14: the ISO (4,660,166,656 bytes) is a single-layer DVD. |
| 0x3783C0 | GetDiskType, internal core | `sceCdGetDiskType` | S-command 3 on client 0x3D6840; only caller 0x378460. |
| 0x378480 | sceCdMmode | `sceCdMmode` | S-command 0x22 (initial table `{0x22, 4, 4}` at 0x3D6868, never written); called with 2 (DVD) or 1 (CD). |
| 0x378558 | sceCdReadClock | `sceCdReadClock` | S-command 1, 16-byte reply, copies 8 bytes from rbuf+4. Callers 0x1E6F18 and 0x1E6F98: BCD decoding after libscf conversion (0x38B568 = JST → GMT, −540 min). The runtime therefore returns UTC+9 time, like a console clock. |
| 0x377AD8 | sceCdSyncS | `sceCdSyncS` | "S cmd wait"; polls sceSifCheckStatRpc (0x3B0A48) on the S-command client. |

libcdvd is only partly linked into the executable: 13 functions between 0x377870 and 0x378614. The strings of sceCdSearchFile, sceCdRead, the N-commands, the callback thread and sceCdSt* exist in `.rodata`, but no code references them and no bind exists for SIDs 0x80000595 and 0x80000597. The game reads the disc through Criterion's GTFSCDVD.IRX module, not through the EE libcdvd.

The internal variables of libcdvd (0x3D4CD0–0x3D4D20, 0x3D5EC0, 0x3D6300, 0x3D6740, 0x3D6840–0x3D6880, 0x1F659C0–0x1F659E0, 0x1F65BE0–0x1F65C60) are read by no code outside the library. A scan of every `jal`/`j` and of the data words finds only 8 external calls, all to the 5 public entry points.

## Identified functions, not bound

| Address | Identity | Status |
|---|---|---|
| 0x3B1E70 | loadfile client reset | EE only, no RPC. Called by 0x2156B8. |
| 0x3B1430 | fileio client reset | EE only, no RPC. Called by 0x2156B8. |
| 0x3AFC70 | sceSifSendCmd core (`mode` in a1) | Only callers: 0x3AFDA8 and 0x3AFDE8, both bound. It reads the IOP buffer address set by the guest sceSifInitCmd, which no longer runs. |
| 0x3AFE28, 0x3AF880, 0x3AF8A0 | DMAC 5 interrupt handler and libsif's "set sreg" / "change saddr" system handlers | Installed only by the guest sceSifInitCmd (bound): unreachable. |
| 0x3B0A48 | sceSifCheckStatRpc | Checked in the disassembly. |
| 0x3AFF70 | sceSifWriteBackDCache | Deduced. |
| 0x3B2960 / 0x3B29B0 | DI / EI | Deduced. |
| 0x3AF780 | scePrintf | Deduced. |
| 0x3ADDF0 | DelayThread | Deduced. |
| 0x377870, 0x377958, 0x377A50, 0x3779F8, 0x377B48 | libcdvd semaphores, exit, power-off handler and S-command lock | Unreachable once the public entry points are bound. |
| 0x38AFE0 | Reads 14 bytes of `rom0:ROMVER` into 0x3E0A88 (libscf) | Goes through fileio, now bound. |
| 0x379908 | Reads `rom0:ROMVER` up to the NUL; date of the 9 preceding bytes compared with 20010608 | Requires a 16-byte ROMVER ending in `\n\0`: ROM0 profile in `src/burnout_overrides.cpp`. |
| 0x396A78 | IOP module check by name (called by 0x395A30 for sio2man, mcman, mcserv, multitap_manager) | Calls 0x3B1EA8 (bound); if the result is negative, formats an error message into a 512-byte local buffer, with no other visible effect in the pseudocode. MTAPMAN is not on the disc, so a negative result for multitap_manager is expected. Not bound. |

## To identify

- libdbc/libpad2 (markers above): client of the DBCMAN service (SID 0x80001300), physically loaded by the IOP.
- libmtap: second client at 0x1F77700 (0x3B6178–0x3B6430, RPC 1, 2, 3, 5, 6); the multitap manager is not loaded.
- Criterion's GTFS client: `FUN_001E2278(0x561D18, 1, …, 0x419368, 3, 0x20)` binds SID 0x475453 ("GTS") at 0x1E23C8; RPC 1 (init), 3 (open), 5 (read in blocks of 0xA800 bytes). GTFSCDVD.IRX copies the sectors to the EE through `sceSifSetDmaIntr` (sifman:32), then signals completion with SIF command 4, whose EE handler 0x1E2DF0 does `iSignalSema`. Served by the IRX in the emulated IOP: do not bind it.
- RenderWare audio client (RWA.IRX / B4ROUTE.IRX), 0x371630 (string "EE  RwaRPCTransfer : ERROR! DMA command queue FULL!" at VA 0x42C9B0): commands sent to the IOP through `sceSifSendCmd(0, …)` to the RWA+0x3D70 handler, registered by `sceSifAddCmdHandler(0, …)` in an 8-entry table (`sceSifSetCmdBuffer`). Replies through command 1 to 0x3719D8; the EE waits for reply 0x12 (flag gp−0x7A9C, loop 0x372BC8). Served by the IRX: do not bind it.
