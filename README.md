<div align="center">

# Burnout Dominator Reversed

**Burnout Dominator (PS2) recompiled into a native PC game.**

[![build](https://github.com/literalynn/BurnoutDominatorReversed/actions/workflows/build.yml/badge.svg)](https://github.com/literalynn/BurnoutDominatorReversed/actions/workflows/build.yml)
![platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-blue)
![graphics](https://img.shields.io/badge/GPU-Vulkan-red)
![license](https://img.shields.io/badge/license-GPLv3-green)
![status](https://img.shields.io/badge/status-work%20in%20progress-orange)

</div>

The game's MIPS R5900 code is translated to C++ with [PS2Recomp](https://github.com/ran-j/PS2Recomp). The rest of the console (IOP, SPU2, VU0/VU1, IPU) is emulated, and the GS runs on your graphics card through [paraLLEl-GS](https://github.com/Arntzen-Software/parallel-gs).

> **No game data is included.** You need your own European disc image (`SLES-54627`); the installer checks its SHA256.

## Status

| | |
|---|---|
| ✅ Boot, intro videos, menus, World Tour, car select | right graphics |
| ✅ Races | cars, damage, reflections, traffic, HUD, crashes, results |
| ✅ Memory card | profile created and saved after every event |
| ✅ Save states | F5 / F7, the whole machine |
| ✅ Gamepads | Xbox, PlayStation 4/5 (also through DS4Windows or DSX) and SDL-compatible pads, plus keyboard, all usable at any time |
| ✅ Speed | full speed (50 frames per second) in races on a fast PC; a slower PC runs the game in slow motion rather than skipping frames |
| 🟡 Known issues | occasional camera jumps, slow loading screens |
| ✅ Sound | music, engines, crashes and menus through the emulated SPU2 (no reverb yet) |

Details: [docs/STATUS.md](docs/STATUS.md).

## Install (Windows)

**You need**
- the ISO of your European Burnout Dominator disc. Keep it where it is: the game reads it at every launch;
- about 20 GB free and a Vulkan graphics driver (any recent NVIDIA, AMD or Intel);
- Python, Git, PowerShell 7 and Visual Studio 2026 Community (or 2022) with **Desktop development with C++**:

```powershell
winget install Python.Python.3.12 Git.Git Microsoft.PowerShell
```

**Then**

```powershell
git clone https://github.com/literalynn/BurnoutDominatorReversed.git
explorer BurnoutDominatorReversed
```

Drop your ISO onto **`Installer.bat`** (30 to 90 minutes the first time), then double-click **`Play.bat`**.

To update: `git pull`, then run `Installer.bat` again and press Enter; only what changed is rebuilt.

<details>
<summary><b>Linux</b></summary>

```bash
sudo apt install git python3 cmake ninja-build g++ pkg-config libgl-dev libx11-dev libxrandr-dev libxinerama-dev \
  libxcursor-dev libxi-dev libasound2-dev libavcodec-dev libavformat-dev libavutil-dev libswresample-dev libswscale-dev
git clone https://github.com/literalynn/BurnoutDominatorReversed.git && cd BurnoutDominatorReversed
python3 tools/install.py --iso "/path/to/Burnout Dominator.iso"
python3 tools/project.py run
```

macOS is not verified yet.
</details>

## Controls

| PS2 | Keyboard | Gamepad |
|---|---|---|
| D-pad | arrows / WASD | D-pad |
| ✕ ○ □ △ | X · C · Z · V | A B X Y / ✕ ○ □ △ |
| L1 · R1 | Q · E | LB · RB |
| L2 · R2 | Left Shift · Right Shift | LT · RT |
| Start · Select | Enter · Tab | Start · Back |
| Analog sticks | — | sticks |

| Key | Action |
|---|---|
| **F11** or **Alt+Enter** | full screen |
| **F5** / **F7** | save / load state |

## Settings

The first launch writes **`settings.ini`** next to `Play.bat`:

| Setting | Default | |
|---|---|---|
| `window_width`, `window_height` | 1280 × 960 | start-up window size |
| `fullscreen` | false | borderless full screen |
| `vsync`, `fps_limit` | true, 0 | display sync and cap |
| `aspect` | 4:3 | `4:3`, `16:9` (with in-game widescreen) or `stretch` |
| `smooth_scaling` | true | bilinear enlargement |
| `super_sampling` | 4 | anti-aliasing: 1, 2, 4, 8 or 16 samples per pixel |
| `progressive_scan` | true | whole frames, no interlacing lines |
| `stick_deadzone` | 12 | percent |
| `gamepad_mappings` | — | extra `gamecontrollerdb.txt` |

The game itself runs at 50 Hz (PAL) or 60 Hz: pick it on the first screen.

## Documentation

| | |
|---|---|
| [STATUS](docs/STATUS.md) | what works and what is left |
| [DEVELOPMENT](docs/DEVELOPMENT.md) | build steps, run options, tests, repository layout |
| [BOOT](docs/BOOT.md) | boot sequence, IOP modules, diagnostics |
| [CHANGES](docs/CHANGES.md) | every change to the PS2Recomp snapshot |
| [SDK_FUNCTIONS](docs/SDK_FUNCTIONS.md) · [FINDINGS](docs/FINDINGS.md) | reverse-engineering evidence |

## Help wanted

Contributions are welcome, from code to bug reports. Where help matters most:

- **Speed**: the VU1 microprograms dominate a race frame. They are compiled ahead of time by [`tools/compile_vu_programs.py`](tools/compile_vu_programs.py) into C++ that is cycle-accurate but far from lean.
- **Sound**: the SPU2 ([`ps2xIOP/src/emulator/core/iop_spu2.cpp`](ps2xIOP/src/emulator/core/iop_spu2.cpp)) needs reverb and testing by ear.
- **Linux and macOS**: builds, controllers, packaging.
- **Bug reports**: what you saw, when, your hardware, and if you can a save state (F5) taken just before it.

Read [CONTRIBUTING.md](CONTRIBUTING.md) first: no faked success, evidence for every SDK binding, every upstream change documented.

## Credits and license

Built on [PS2Recomp](https://github.com/ran-j/PS2Recomp) by ran-j and contributors (GPLv3, like this project), with [paraLLEl-GS](https://github.com/Arntzen-Software/parallel-gs) by Arntzen Software (LGPLv3). Game files and the code derived from them are never committed and stay on your machine.

Burnout Dominator is a trademark of Electronic Arts. This project is not affiliated with or endorsed by Electronic Arts, Criterion Games or Sony Interactive Entertainment.
