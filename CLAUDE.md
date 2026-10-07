# Burnout Dominator Reversed

Static recompilation of Burnout Dominator PS2 (PAL, SLES_546.27, SHA256 locked in `project.json`) to native code with PS2Recomp. Windows first, Linux/macOS next. The owner writes in French: answer in French; user-facing docs (`README.md`, `docs/*.md`) are French; code, comments and commit messages are English.

## Layout

- Upstream PS2Recomp snapshot at the root (`ps2xRecomp`, `ps2xRuntime`, `ps2xIOP`, `ps2xTest`, ...), commit in `UPSTREAM.json`. Change it minimally and list every change in `docs/CHANGES.md`.
- Project code: `src/burnout_main.cpp` (runner), `src/burnout_overrides.cpp` (SDK bindings by address + ROM0 profile), `cmake/Burnout.cmake`, `tools/` (Python driver and analysis), `tests/python`, `tests/native`.
- State and evidence: `docs/STATUS.md`, `docs/BOOT.md` (boot sequence, IOP modules, run history), `docs/SDK_FUNCTIONS.md` (identified SDK functions), `docs/FINDINGS.md`.
- Game files, Ghidra output, generated C++ (~51k files) and builds are never committed. One exception: `data/functions.ee.csv`, the Ghidra function map (generated names, addresses and sizes, no game bytes; provenance and SHA256 in `data/README.md`), shipped so that `tools/install.py` works without Ghidra. They live in a work directory chosen by `BDR_WORK_DIR` or the ignored `work.json` (on the owner's PC: `C:\Users\lynnb\bdr-work`, outside the iCloud-synced checkout). Cloud sessions have no game files.

## Commands

```
python tools/install.py --iso <ISO>   # one-command install (Installer.bat on Windows): extract, build tools, generate, build the game
python tools/project.py configure [--game] [--generator Ninja|"Visual Studio 18 2026" --arch x64] [--lto]
python tools/project.py build [--game] --jobs N
python tools/project.py generate --tool <ps2_recomp> --function-map <functions.ee.csv> --augment --regenerate
python tools/project.py run --headless --seconds 30 --status-ms 1000 --log runN.log --tail 120 [--no-iso] [--trace-calls 0xA,0xB] [--trace-watch 0xC]
python -m unittest discover -s tests/python
```

Without `--game`, `build` compiles the tools, the runtime and every test (no game files needed). Run `ps2x_tests` from the repository root (one test reads `ps2xRecomp` headers by relative path). Other test programs: `ps2_iop_{emulator,import,compatibility,import_version}_tests`, `ps2_iso9660_tests`, `burnout_overrides_tests`. CI (`.github/workflows/build.yml`) runs them on GCC, Clang and MSVC.

`run` exit codes: 124 = time limit reached (an observation, not a pass), 3 = missing guest function, 1 = failure.

## Cloud sessions

The owner can upload at most 30 MB, never the ISO. On the PC with the disc, `python tools/cloud_bundle.py --iso <ISO>` writes `bdr-cloud.zip` (Desktop): ELF, SYSTEM.CNF, all `IOP/` files, ELF/IRX reports, the Ghidra map, previous run logs, the disc files read during the LOADING screen (`BOOT_FILES`) and as many small disc files as fit, with `bundle.json` as manifest. In the cloud: unzip it into a scratch directory `W`, then with `BDR_WORK_DIR=W`: `project.py configure --generator Ninja`, `build`, `generate --tool W/build/ps2xRecomp/ps2_recomp --function-map W/local/analysis/ghidra/functions.ee.csv --augment`, `configure --game`, `build --game`. Without the ISO, disc reads beyond the bundled files fail: debug the boot up to the first missing file, and say so.

## Rules

- Never make guest code return a fabricated success, skip a function or stub game code to get further. Bind an SDK function to a runtime handler only when its identity is established (RPC SID and function number, referenced strings or semaphore names, library version marker, matching arity) and the handler's ABI matches. Bind every IOP-facing entry point of a library together. Record the evidence in `docs/SDK_FUNCTIONS.md` and the expected handler in `tests/native/burnout_overrides_tests.cpp`.
- Upstream files are CRLF and `.gitattributes` does not normalize them: keep CRLF when editing them (read bytes, edit, write back with `\r\n`). Project files are LF.
- Editing `ps2xRuntime/include/ps2_runtime.h` forces a full rebuild of the generated code (about 10 minutes with 15 cores); prefer changes in `.cpp` files or in `src/`.
- Addresses in `local/analysis/SLES_546.27.strings.txt` are file offsets: VA = offset - 0x1000 + 0x100000 (segment 0), offset - 0x306000 + 0x408080 (segment 1).
- Ghidra pseudocode drops uncached reads (`| 0x20000000`) in libcdvd; check return values in the disassembly.
