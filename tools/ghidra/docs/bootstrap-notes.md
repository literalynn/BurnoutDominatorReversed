# EE bootstrap seen in the pseudocode

The PAL entry is `0x00100008`, in `local/decompiled/functions/00100008_entry.pseudocode.c`. The decompilation keeps inferred types such as `undefined4` and automatic names. Values attached to different parts of the zero register are artefacts of the decompiler's representation; check the R5900 instructions before changing a memory operation.

The pseudocode shows this sequence:

1. `SYNC(0x10)` and zeroing of `0x00432B00` to `0x01F9BB00`, with aligned 16-byte writes then edge handling.
2. System calls `0x3C` and `0x3D`. PS2Recomp's current dispatcher maps these ids to `SetupThread` and `SetupHeap`.
3. Call to `0x003B2BD0`, which calls nine initialisation subroutines. Their full meaning is still to be identified.
4. `FlushCache(0)` and `EI()`.
5. Call to `0x0021B3F8` with the global data at `0x00433080` and `0x00433084`.
6. Call to `0x00322630` after it returns; this routine calls `0x00321E18`, `0x003A2A50`, then a thunk at `0x003B2DE0`.

The code at `0x0021B3F8` notably prepares a path string, uses `GetThreadId`, then calls `ChangeThreadPriority(..., 10)`. It repeats a sequence until `FUN_00207D08(0x00491540)` returns non-zero, then waits for the global byte `DAT_0051B910` to become non-zero. These addresses are observation points to tell initialisation progress from a wait. They do not justify forcing these results or replacing the loops with fake returns.

Useful files:

- `00100008_entry.pseudocode.c`
- `003B2BD0_FUN_003b2bd0.pseudocode.c`
- `0021B3F8_FUN_0021b3f8.pseudocode.c`
- `00322630_FUN_00322630.pseudocode.c`

The exact body ranges and export attempts are in `local/decompiled/manifest.json`. No Ghidra project name and no byte of the original ELF was changed by this export.
