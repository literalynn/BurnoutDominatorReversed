# Development

`tools/install.py` chains the steps below. Running them by hand is useful when working on the recompiler, the runtime or the game integration. Requirements: Python 3.11+, CMake 3.21+, a C++20 compiler (MSVC x64 with the Windows SDK on Windows). The tools install no global configuration.

## Work directory

Commands run from the repository root. Generated and local data (`local/`, `generated/`, `build/`) live in the work directory: the repository itself by default, or the folder given by `BDR_WORK_DIR` or by `work.json` (written by `python tools/project.py workdir <folder>` and by `tools/install.py`). `build/…` paths below are relative to it.

## 1. Extract the disc

```powershell
python tools/project.py extract --iso "D:\path\to\Burnout Dominator (Europe) (En,Fr,De,Es,It).iso" --all --hash-iso
```

`local/disc/` receives the 570 files of the disc, including the executable and the 12 IRX modules. The original ISO is still needed to keep the real sector addresses: `run` reads it at every launch, from the location recorded in `local/paths.json`. The inventory, ELF segments, sections, strings and SHA256 hashes are written to `local/`. These files, saves and `generated/` are ignored by Git.

## 2. Build the tools

```powershell
python tools/project.py configure --generator "Visual Studio 18 2026" --arch x64
python tools/project.py build --jobs 8
```

On Linux/macOS, use `--generator Ninja` and no `--arch`; `configure` adds `-msse4.1` on x86-64. CMake downloads the dependencies at the versions pinned in the project files. This builds `ps2_recomp`, `ps2_analyzer`, the runtime and every test; no game file is needed.

## 3. Translate the game

```powershell
python tools/project.py generate --tool build/ps2xRecomp/Release/ps2_recomp.exe --function-map data/functions.ee.csv --augment
python tools/project.py audit
```

The game has no symbols: the function map comes from a Ghidra analysis and ships in [data/functions.ee.csv](../data/README.md) (no game bytes; provenance and how to redo it in [data/README.md](../data/README.md)). `--augment` adds the code reached only through pointers. `.data` is also flagged executable on this disc, so function boundaries matter: without them, data gets translated into bogus instructions.

Generation never forces a function to return success and configures no `skip` or arbitrary instruction patch. `local/translation_audit.json` holds the recompiler's real errors and static counts. `--regenerate` keeps the previous sources in `local/backups/`.

## 4. Build and run the game

```powershell
python tools/project.py configure --game --generator "Visual Studio 18 2026" --arch x64
python tools/project.py build --game --jobs 8
python tools/project.py run
```

`run` finds the program in the work directory's `build/`; `--exe` picks another one. Useful options:

| Option | Effect |
|---|---|
| `--headless --seconds N` (or `--smoke-seconds N`) | no window or audio, stop after N seconds |
| `--status-ms N` | print a scheduler/IOP status line every N ms |
| `--log NAME --tail N` | log file under `local/logs/`, lines printed at the end |
| `--dump-frames DIR [--dump-every-ms N]` | save the presented frame as PNG every N ms (default 1000) |
| `--trace-calls 0xADDR,…` | log calls and returns of these guest functions |
| `--trace-watch 0xADDR,…` | guest memory words printed with each trace line |
| `--no-iso` | serve sectors from a virtual image of the extracted files instead of the ISO (different sector numbers) |
| `--load-state [FILE]` | restore a save state before the game runs (default: the quick slot `local/states/quick.bdrstate`) |
| `--state FILE` | save state slot used by F5 (save) and F7 (load) in the window |
| `--save-state-at VSYNC` | save to the slot once this VSync is reached (for scripted or headless runs) |
| `--vu-capture` | record the VU0/VU1 microprograms that still run in the interpreter, for the next build |

Save states hold the whole machine (EE, VU, GS VRAM, IOP, SPU2 and the runtime's kernel), about 50 MB each. A save waits for a point where no callback or IOP call is in flight, usually a few milliseconds; the window prints `[state] saved …` once it is written. They belong to the build that wrote them: rebuild after a change to the runtime and older states may no longer load.

Exit codes: `124` = time limit reached (an observation, not a pass), `3` = missing guest function, `1` = failure. More diagnostics are driven by environment variables (`BDR_STATUS_DETAIL`, `BDR_TRACE_SIF`, `BDR_TRACE_IPU`, `BDR_PROFILE`…), listed in [BOOT.md §10](BOOT.md#10-observation-tools).

Editing `ps2xRuntime/include/ps2_runtime.h` (or another header included by the generated code) rebuilds the ~51,000 generated files; prefer changes in `.cpp` files or in `src/`.

## Compiled VU microprograms

The VU0 and VU1 microprograms the game uploads are game code too. A run with `--vu-capture` records the ones that have no compiled version into `local/vu-programs`; the next `configure --game` and `build --game` turn them into C++ (`tools/compile_vu_programs.py`). Without recordings everything runs in the VU interpreter, which is several times slower. The recordings come from the game and stay on your machine.

## Tests

```powershell
python -m unittest discover -s tests/python
```

Native test programs are built by `project.py build`: `ps2x_tests` (run it from the repository root), `ps2_iop_emulator_tests`, `ps2_iop_import_tests`, `ps2_iop_compatibility_tests`, `ps2_iop_import_version_tests`, `ps2_iso9660_tests`, `ps2_spu2_tests` and `burnout_overrides_tests`. CI ([`.github/workflows/build.yml`](../.github/workflows/build.yml)) runs them with GCC, Clang and MSVC.

## Working without the disc on the build machine

`python tools/cloud_bundle.py --iso <ISO>` (on the machine with the disc) writes `bdr-cloud.zip`: the ELF, SYSTEM.CNF, the IOP modules, the analysis reports, the function map, previous run logs and as many small disc files as fit in 30 MB. Unzip it into a folder `W` elsewhere and use `BDR_WORK_DIR=W` with the steps above. Disc reads beyond the bundled files fail, so this only helps debugging the boot up to the first missing file.

## Repository layout

| Path | Contents |
|---|---|
| `ps2xRecomp/`, `ps2xRuntime/`, `ps2xIOP/`, `ps2xAnalyzer/`, `ps2xTest/`, `ps2xStudio/`, `android/`, `vita/` | PS2Recomp snapshot (commit in [`UPSTREAM.json`](../UPSTREAM.json)), with the changes listed in [CHANGES.md](CHANGES.md) |
| `src/` | game integration: runner (`burnout_main.cpp`), SDK bindings by address and ROM0 profile (`burnout_overrides.cpp`), sampling profiler |
| `cmake/Burnout.cmake` | links the translated functions into the `burnout_dominator` executable |
| `tools/` | Python driver (`project.py`, `install.py`), ISO9660/ELF tools, function map augmentation, VU program compiler, cloud bundle, Ghidra scripts |
| `data/` | Ghidra function map of the game (names, addresses, sizes; no game bytes) |
| `tests/` | Python tests of the tools, native tests of the bindings |
| `docs/` | status, boot analysis, findings, identified SDK functions, upstream changes |
| `Installer.bat`, `Play.bat` | Windows one-click install and launch |
