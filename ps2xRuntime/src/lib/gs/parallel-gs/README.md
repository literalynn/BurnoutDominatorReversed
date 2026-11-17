# paraLLEl-GS `gs/` sources

The `gs/` directory of [paraLLEl-GS](https://github.com/Arntzen-Software/parallel-gs) (Arntzen Software, LGPL-3.0-or-later, see `COPYING.LGPLv3` in that repository) at commit `cc6184af7e0c03da603045ca371ffa5dae9b0655`, the commit `ps2xRuntime/cmake/ParallelGS.cmake` fetches. That file still fetches the repository for Granite and the shaders, then builds the `parallel-gs` library from the files here.

Burnout Dominator Reversed changes these files. Each change starts with a comment marked `[Burnout]` and is listed in `docs/CHANGES.md`. Until November 2026 the changes were string replacements applied at configure time (`ps2xRuntime/cmake/ParallelGSPatches.cmake`); the files here start from that result.
