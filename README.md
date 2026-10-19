<div align="center">

# Burnout Dominator Reversed

**Burnout Dominator (PS2) recompiled into a native PC game.**

[![build](https://github.com/literalynn/BurnoutDominatorReversed/actions/workflows/build.yml/badge.svg)](https://github.com/literalynn/BurnoutDominatorReversed/actions/workflows/build.yml)
![platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-blue)
![graphics](https://img.shields.io/badge/GPU-Vulkan-red)
![license](https://img.shields.io/badge/license-GPLv3-green)
![status](https://img.shields.io/badge/status-playable-brightgreen)

</div>

The game's MIPS R5900 code is translated to C++ with [PS2Recomp](https://github.com/ran-j/PS2Recomp). The rest of the console (IOP, SPU2, VU0/VU1, IPU) is emulated, and the GS runs on your graphics card through [paraLLEl-GS](https://github.com/Arntzen-Software/parallel-gs).

> **No game data is included.** You need your own European disc image (`SLES-54627`); the installer checks its SHA256.

## Status

**The whole game is playable and stable.** Work now goes into speed and modding.

| | |
|---|---|
| ✅ Boot, intro videos, menus, World Tour, car select | right graphics |
| ✅ Races | cars, damage, reflections, traffic, HUD, crashes, results |
| ✅ Memory card | profile created and saved after every event |
| ✅ Save states | F5 / F7, the whole machine |
| ✅ Gamepads | Xbox, PlayStation 4/5 (also through DS4Windows or DSX) and SDL-compatible pads with vibration, plus keyboard, all usable at any time |
| ✅ Speed | 60 frames per second in six-car races on a fast PC; when frames come late the game runs extra simulation steps instead of slowing down (`keep_speed`) |
| ✅ Graphics | up to 4x the PS2 resolution, supersampling, FXAA, 16:9 |
| ✅ Sound | music, engines, crashes and menus through the emulated SPU2, at the right pitch, even when the game slows down |
| 🟡 Limits | no SPU2 reverb; no frame interpolation, so a slow PC shows fewer frames, not slower ones |

Details: [docs/STATUS.md](docs/STATUS.md).

## System requirements

The CPU's single-core speed matters most: the game code and the VU1 graphics programs each run on one thread. On a Ryzen 7 9800X3D each of them takes about 12 ms of a 16.7 ms frame in a six-car race, and the game keeps 55-60 frames per second with only 2 or 4 CPU threads. A CPU with clearly lower single-core speed will drop under 60 in busy races; `keep_speed` keeps the race at the right speed with fewer frames. Four CPU threads are enough.

The graphics card needs Vulkan. The PS2 resolution with 1 sample a pixel is light; 2x (4 samples) and 4x (16 samples) need a dedicated GPU. Lower `super_sampling` in F1 if the frame rate drops.

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
| ✕ ○ □ △ | X · C · Z · V (△ also Escape: back) | A B X Y / ✕ ○ □ △ |
| L1 · R1 | Q · E | LB · RB |
| L2 · R2 | Left Shift · Right Shift | LT · RT |
| Start · Select | Enter · Tab | Start · Back |
| Analog sticks | — | sticks |

The mouse works in the menus: wheel to move, left click to select, right click to go back. Keys, mouse buttons and the wheel can be changed in the settings menu (**F1**), three per PS2 button.

| Key | Action |
|---|---|
| **F1** | settings menu (the game pauses): volume, resolution, anti-aliasing, 4:3 / 16:9 / stretch, FPS counter, full screen, smoothing, vibration, fast loading, keep speed, stick dead zone, keyboard and mouse controls |
| **Escape** | back in the menus; in a race, asks whether to quit (so does the window's close button) |
| **F11** or **Alt+Enter** | full screen |
| **F5** / **F7** | save / load state |
| **F9** | mark a glitch: notes the moment in `<work>/local/logs/run.log` and saves a screenshot in `<work>/marks/` |

## Settings

The first launch writes **`settings.ini`** next to `Play.bat`; the F1 menu changes it too:

| Setting | Default | |
|---|---|---|
| `window_width`, `window_height` | 1280 × 960 | start-up window size |
| `fullscreen` | false | borderless full screen |
| `vsync`, `fps_limit` | true, 0 | display sync and cap |
| `aspect` | 4:3 | `4:3`, `16:9` (with in-game widescreen) or `stretch` |
| `smooth_scaling` | true | bilinear enlargement |
| `super_sampling` | 4 | samples per pixel: 1, 2, 4, 8 or 16 |
| `high_resolution` | true | a higher resolution built from the samples: 2x the PS2's (1280 pixels wide) with 4 or 8 samples, 4x (2560) with 16 |
| `fxaa` | false | FXAA on the finished picture |
| `hud_smoothing` | true | the race HUD smooth instead of blocky at 2x and 4x |
| `progressive_scan` | true | whole frames, no interlacing lines |
| `fast_loading` | true | loading screens run faster than real time |
| `keep_speed` | true | when the graphics cannot keep up, the game runs more simulation steps per drawn frame instead of slowing down (from the next race) |
| `stick_deadzone` | 12 | percent |
| `vibration` | true | the game's pad vibration on your gamepad |
| `gamepad_mappings` | — | extra `gamecontrollerdb.txt` |
| `volume` | 100 | percent |
| `show_fps` | false | frames per second in the corner |
| `key_cross`, `key_l1`... | see Controls | up to three keys, mouse buttons or wheel directions (`MOUSE_LEFT`, `MOUSE_WHEEL_UP`...) per button, or `NONE` |
| `refresh_rate` | 60 | the game runs at `60` or `50` Hz without asking; `ask` shows its 50/60 Hz question (a state saved at the other rate switches at the next menu page) |

The game runs at 60 Hz: its 50/60 Hz question is skipped (`refresh_rate`).

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

- **Speed**, for smaller PCs: the VU1 microprograms dominate a race frame. They are compiled ahead of time by [`tools/compile_vu_programs.py`](tools/compile_vu_programs.py) into C++ that is cycle-accurate but far from lean. `BDR_STOP_AT_VSYNC` measures a build's CPU time on a replay ([docs/BOOT.md](docs/BOOT.md)).
- **Modding**: [`disc_patches.cpp`](ps2xRuntime/src/lib/disc_patches.cpp) changes disc files as the game reads them (the PC save texts use it), without touching the ISO.
- **Sound**: the SPU2 ([`ps2xIOP/src/emulator/core/iop_spu2.cpp`](ps2xIOP/src/emulator/core/iop_spu2.cpp)) needs reverb and testing by ear.
- **Linux and macOS**: builds, controllers, packaging.
- **Bug reports**: what you saw, when, your hardware, and if you can a save state (F5) taken just before it.

Read [CONTRIBUTING.md](CONTRIBUTING.md) first: no faked success, evidence for every SDK binding, every upstream change documented.

## Credits and license

Built on [PS2Recomp](https://github.com/ran-j/PS2Recomp) by ran-j and contributors (GPLv3, like this project), with [paraLLEl-GS](https://github.com/Arntzen-Software/parallel-gs) by Arntzen Software (LGPLv3). Game files and the code derived from them are never committed and stay on your machine.

Burnout Dominator is a trademark of Electronic Arts. This project is not affiliated with or endorsed by Electronic Arts, Criterion Games or Sony Interactive Entertainment.
