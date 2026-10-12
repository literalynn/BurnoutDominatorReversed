# Burnout Dominator Reversed

[![build](https://github.com/literalynn/BurnoutDominatorReversed/actions/workflows/build.yml/badge.svg)](https://github.com/literalynn/BurnoutDominatorReversed/actions/workflows/build.yml)

A static recompilation of **Burnout Dominator** for PlayStation 2 (European release, `SLES_546.27`) into a native PC program. The game's MIPS R5900 code is translated to C++ with [PS2Recomp](https://github.com/ran-j/PS2Recomp) and runs on top of its runtime, which emulates the rest of the console (IOP, SPU2, GS, IPU…). Windows comes first; the C++20/CMake code base also builds on Linux, and macOS is planned.

**No game data is included.** You need your own copy of the disc: the installer checks that your ISO is exactly the expected European image (size and SHA256 pinned in [`project.json`](project.json)).

## Status

**Work in progress — not playable yet.** The game builds and boots: it goes through the LOADING screen and the intro video, then stops at the memory card check on the PROFILE screen. Menus, races, controllers and saves do not work yet.

A successful build is not proof of a faithful port. Verified results, run history and current blockers are in [docs/STATUS.md](docs/STATUS.md) and [docs/BOOT.md](docs/BOOT.md).

## Install and run

### Windows

You need:

- your own ISO of the European Burnout Dominator disc (SLES-54627). **Leave it where it is afterwards**: the game reads it at every launch. If you move it, drop it onto `Installer.bat` again;
- about 20 GB free on `C:` (the extracted disc, the translated code and the build go to `C:\bdr-work`);
- four programs, installed once. In the Terminal app:
  1. **Python**: `winget install Python.Python.3.12` (or <https://www.python.org/downloads/>, ticking **"Add python.exe to PATH"**);
  2. **Git**: `winget install Git.Git`;
  3. **PowerShell 7**: `winget install Microsoft.PowerShell`;
  4. **Visual Studio 2026 Community** (2022 also works): <https://visualstudio.microsoft.com/downloads/>, with the **"Desktop development with C++"** workload.

  Then close and reopen the Terminal so it finds them.

Get the project:

```powershell
cd $HOME
git clone https://github.com/literalynn/BurnoutDominatorReversed.git
explorer BurnoutDominatorReversed
```

In the folder that opens, **drop your ISO onto `Installer.bat`**. The first installation takes 30 to 90 minutes. If something is missing, the message says what to install: install it and run `Installer.bat` again. To put the large files on another drive, run instead `python tools\install.py --iso "<path to the ISO>" --work-dir D:\bdr-work` (a path without accents).

**Play:** double-click **`Play.bat`**. Close the window to stop. The console then prints the last lines of the log and its location after `Log:` (normally `C:\bdr-work\local\logs\run.log`). Attach that file when you report a problem.

**Update:** `cd $HOME\BurnoutDominatorReversed`, `git pull`, then run `Installer.bat` again (just press Enter when it asks for the ISO). Only the needed steps are redone: the tools are rebuilt for the changed files, and the game is translated again only if the recompiler, the function map or the game executable changed.

### Linux

```bash
sudo apt install git python3 cmake ninja-build g++ pkg-config libgl-dev libx11-dev libxrandr-dev libxinerama-dev \
  libxcursor-dev libxi-dev libasound2-dev libavcodec-dev libavformat-dev libavutil-dev libswresample-dev libswscale-dev
git clone https://github.com/literalynn/BurnoutDominatorReversed.git && cd BurnoutDominatorReversed
python3 tools/install.py --iso "/path/to/Burnout Dominator.iso"
python3 tools/project.py run
```

The large files go to `~/bdr-work`, or to the folder given with `--work-dir`. As on Windows, the ISO must stay where it is.

### macOS

Not verified yet. The upstream runtime has an SSE-to-NEON path for ARM64; building on macOS, behaviour on Apple Silicon and performance remain to be checked. The Linux commands (with Homebrew equivalents of the packages) are the starting point.

## Development

`tools/install.py` chains the steps below. Running them by hand is useful when working on the recompiler, the runtime or the game integration. Requirements: Python 3.11+, CMake 3.21+, a C++20 compiler (MSVC x64 with the Windows SDK on Windows). The tools install no global configuration.

### Work directory

Commands run from the repository root. Generated and local data (`local/`, `generated/`, `build/`) live in the work directory: the repository itself by default, or the folder given by `BDR_WORK_DIR` or by `work.json` (written by `python tools/project.py workdir <folder>` and by `tools/install.py`). `build/…` paths below are relative to it.

### 1. Extract the disc

```powershell
python tools/project.py extract --iso "D:\path\to\Burnout Dominator (Europe) (En,Fr,De,Es,It).iso" --all --hash-iso
```

`local/disc/` receives the 570 files of the disc, including the executable and the 12 IRX modules. The original ISO is still needed to keep the real sector addresses: `run` reads it at every launch, from the location recorded in `local/paths.json`. The inventory, ELF segments, sections, strings and SHA256 hashes are written to `local/`. These files, saves and `generated/` are ignored by Git.

### 2. Build the tools

```powershell
python tools/project.py configure --generator "Visual Studio 18 2026" --arch x64
python tools/project.py build --jobs 8
```

On Linux/macOS, use `--generator Ninja` and no `--arch`; `configure` adds `-msse4.1` on x86-64. CMake downloads the dependencies at the versions pinned in the project files. This builds `ps2_recomp`, `ps2_analyzer`, the runtime and every test; no game file is needed.

### 3. Translate the game

```powershell
python tools/project.py generate --tool build/ps2xRecomp/Release/ps2_recomp.exe --function-map data/functions.ee.csv --augment
python tools/project.py audit
```

The game has no symbols: the function map comes from a Ghidra analysis and ships in [data/functions.ee.csv](data/README.md) (no game bytes; provenance and how to redo it in [data/README.md](data/README.md)). `--augment` adds the code reached only through pointers. `.data` is also flagged executable on this disc, so function boundaries matter: without them, data gets translated into bogus instructions.

Generation never forces a function to return success and configures no `skip` or arbitrary instruction patch. `local/translation_audit.json` holds the recompiler's real errors and static counts. `--regenerate` keeps the previous sources in `local/backups/`.

### 4. Build and run the game

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

Exit codes: `124` = time limit reached (an observation, not a pass), `3` = missing guest function, `1` = failure. More diagnostics are driven by environment variables (`BDR_STATUS_DETAIL`, `BDR_TRACE_SIF`, `BDR_TRACE_IPU`, `BDR_PROFILE`…), listed in [docs/BOOT.md §10](docs/BOOT.md#10-observation-tools).

Editing `ps2xRuntime/include/ps2_runtime.h` (or another header included by the generated code) rebuilds the ~51,000 generated files; prefer changes in `.cpp` files or in `src/`.

### Tests

```powershell
python -m unittest discover -s tests/python
```

Native test programs are built by `project.py build`: `ps2x_tests` (run it from the repository root), `ps2_iop_emulator_tests`, `ps2_iop_import_tests`, `ps2_iop_compatibility_tests`, `ps2_iop_import_version_tests`, `ps2_iso9660_tests`, `ps2_spu2_tests` and `burnout_overrides_tests`. CI ([`.github/workflows/build.yml`](.github/workflows/build.yml)) runs them with GCC, Clang and MSVC.

### Working without the disc on the build machine

`python tools/cloud_bundle.py --iso <ISO>` (on the machine with the disc) writes `bdr-cloud.zip`: the ELF, SYSTEM.CNF, the IOP modules, the analysis reports, the function map, previous run logs and as many small disc files as fit in 30 MB. Unzip it into a folder `W` elsewhere and use `BDR_WORK_DIR=W` with the steps above. Disc reads beyond the bundled files fail, so this only helps debugging the boot up to the first missing file.

## Repository layout

| Path | Contents |
|---|---|
| `ps2xRecomp/`, `ps2xRuntime/`, `ps2xIOP/`, `ps2xAnalyzer/`, `ps2xTest/`, `ps2xStudio/`, `android/`, `vita/` | PS2Recomp snapshot (commit in [`UPSTREAM.json`](UPSTREAM.json)), with the changes listed in [docs/CHANGES.md](docs/CHANGES.md) |
| `src/` | game integration: runner (`burnout_main.cpp`), SDK bindings by address and ROM0 profile (`burnout_overrides.cpp`), sampling profiler |
| `cmake/Burnout.cmake` | links the translated functions into the `burnout_dominator` executable |
| `tools/` | Python driver (`project.py`, `install.py`), ISO9660/ELF tools, function map augmentation, cloud bundle, Ghidra scripts |
| `data/` | Ghidra function map of the game (names, addresses, sizes; no game bytes) |
| `tests/` | Python tests of the tools, native tests of the bindings |
| `docs/` | status, boot analysis, findings, identified SDK functions, upstream changes |
| `Installer.bat`, `Play.bat` | Windows one-click install and launch |

## Documentation

- [docs/STATUS.md](docs/STATUS.md) — what is verified, what works, what is left
- [docs/BOOT.md](docs/BOOT.md) — boot sequence, IOP modules, run history, diagnostics
- [docs/SDK_FUNCTIONS.md](docs/SDK_FUNCTIONS.md) — Sony SDK functions identified in the executable and their evidence
- [docs/FINDINGS.md](docs/FINDINGS.md) — findings log with the limits of each conclusion
- [docs/CHANGES.md](docs/CHANGES.md) — changes to the PS2Recomp snapshot
- [docs/PS2Recomp-UPSTREAM.md](docs/PS2Recomp-UPSTREAM.md) — upstream PS2Recomp README

## Contributing

Contributions are welcome. Read [CONTRIBUTING.md](CONTRIBUTING.md) first: the project has strict rules (no faked success, evidence for every SDK binding, every upstream change documented).

## License and credits

Built on [PS2Recomp](https://github.com/ran-j/PS2Recomp) by ran-j and contributors. The included PS2Recomp sources keep their GPLv3 license ([LICENSE](LICENSE)); the integration code added here is under the same license. Game files and the C++ derived from them are not covered by this license, are never committed and stay on your machine.

Burnout Dominator is a trademark of Electronic Arts. This project is not affiliated with or endorsed by Electronic Arts, Criterion Games or Sony Interactive Entertainment.
