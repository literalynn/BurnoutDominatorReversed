<div align="center">

# Burnout Dominator Reversed

**A native PC port of Burnout Dominator (PlayStation 2), made by static recompilation.**

[![build](https://github.com/literalynn/BurnoutDominatorReversed/actions/workflows/build.yml/badge.svg)](https://github.com/literalynn/BurnoutDominatorReversed/actions/workflows/build.yml)
[![release](https://img.shields.io/github/v/release/literalynn/BurnoutDominatorReversed)](https://github.com/literalynn/BurnoutDominatorReversed/releases/latest)
![platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-blue)
![graphics](https://img.shields.io/badge/GPU-Vulkan-red)
![license](https://img.shields.io/badge/license-GPLv3-green)

<img src="docs/screenshots/banner.jpg" width="100%" alt="Boosting down a mountain road at 148 mph">

</div>

The game's PS2 code is translated ahead of time into C++ and compiled for the PC; the rest of the console (graphics chip, sound, I/O processor) runs next to it. The result is a normal PC program: the whole game is playable, from the intro videos to the last World Tour event.

> [!IMPORTANT]
> **No game data is included.** You need your own disc image of the European release (`SLES-54627`). The game is built on your PC from that image; nothing of it is distributed here.

## Features

- **The complete game**: World Tour, every event type, crashes and Impact Time, videos, music and sound effects.
- **60 frames per second**, or more: 120, 144, 240 and up to 500, simulated for real, not interpolated.
- **Higher resolution**: up to four times the PS2's, 1 to 16 samples per pixel, FXAA, progressive scan.
- **Real 16:9**: the game draws a wider view; nothing is stretched.
- **Controllers**: Xbox, PlayStation and other SDL gamepads with vibration; keyboard and mouse with keys you can change.
- **Saves**: the memory card, save states (F5 / F7), and save profiles kept apart.
- **A launcher** that installs the game, starts it, keeps it up to date and edits its settings.

## Screenshots

<table>
<tr>
<td width="50%"><img src="docs/screenshots/race-mountain.jpg" alt="Mountain road"></td>
<td width="50%"><img src="docs/screenshots/race-factory-sun.jpg" alt="Industrial track in the sun"></td>
</tr>
<tr>
<td><img src="docs/screenshots/race-city-boost.jpg" alt="Boosting through the city"></td>
<td><img src="docs/screenshots/crash.jpg" alt="Crash with Impact Time"></td>
</tr>
<tr>
<td><img src="docs/screenshots/car-select.jpg" alt="Car select"></td>
<td><img src="docs/screenshots/medal-table.jpg" alt="Burnout Challenge medal table"></td>
</tr>
</table>

<sub>Captured at four times the PS2's resolution with 16 samples per pixel.</sub>

## Requirements

| | |
|---|---|
| **Game** | your own Burnout Dominator ISO, European release (`SLES-54627`). Other releases are not supported. |
| **System** | Windows 10 or 11, 64-bit. Linux builds from source. macOS is not verified. |
| **Processor** | x86-64, four threads or more; single-thread speed matters most. FMA3 (Intel since 2013, AMD since 2012) is used when present. |
| **Graphics** | a card with Vulkan. Two and four times the PS2's resolution need a dedicated card. |
| **Disk** | 25 GB free. |
| **Build tools** | Windows: Visual Studio 2022 or 2026 (Community or Build Tools) with *Desktop development with C++*. Linux: see below. |

## Installation

The first installation translates and compiles the game on your PC: 30 to 90 minutes.

### Windows

1. Install the C++ build tools, if Visual Studio is not there yet:

   ```powershell
   winget install Microsoft.VisualStudio.2022.BuildTools --override "--passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
   ```

2. Download **[`BurnoutDominator.exe`](https://github.com/literalynn/BurnoutDominatorReversed/releases/latest/download/BurnoutDominator.exe)** into an empty folder and start it. It downloads the project into that folder, with a Python and a Git of its own (official builds, checked by SHA-256). The file is not signed: if Windows SmartScreen stops it, choose *More info*, then *Run anyway*.
3. In the launcher, choose your ISO, press **Generate the code and build the game**, then **Play**.

<img src="docs/screenshots/launcher.jpg" width="80%" alt="The launcher: ISO, installation steps, save profile, Play">

Everything stays in that one folder: the project (`source`), the game's files (`work`), your saves (`save`) and `settings.ini`. The folder can be moved or copied to another drive; the first update after a move builds everything again. To uninstall, delete the folder. Keep the ISO where it is: the game reads it at every launch.

### Linux

```bash
sudo apt install git python3 cmake ninja-build g++ pkg-config libgl-dev libx11-dev libxrandr-dev libxinerama-dev \
  libxcursor-dev libxi-dev libasound2-dev libavcodec-dev libavformat-dev libavutil-dev libswresample-dev libswscale-dev
```

Then download [`BurnoutDominator.py`](https://github.com/literalynn/BurnoutDominatorReversed/releases/latest/download/BurnoutDominator.py) into an empty folder and run `python3 BurnoutDominator.py`. It works like the Windows file; at its first start and after each update it builds the launcher's window (a few minutes the first time). If that cannot be done, it says why and the launcher opens in your browser instead.

### From a Git checkout

```bash
git clone https://github.com/literalynn/BurnoutDominatorReversed.git
cd BurnoutDominatorReversed
python tools/launcher.py        # Windows: Launcher.bat
```

The game's files then go to a work directory outside the checkout (`C:\bdr-work` on Windows, `~/bdr-work` elsewhere). Without the launcher: `python tools/install.py --iso <ISO>`, then `python tools/project.py run`. [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) has the details.

## The launcher

A window of its own ([tools/launcher/](tools/launcher/), drawn with raylib like the game) over [tools/launcher.py](tools/launcher.py), which does the work. Closing the window does not stop a running game; starting the launcher while its window is open brings that window to the front. From a Git checkout, `python tools/launcher.py` shows the same tabs as a page ([tools/launcher.html](tools/launcher.html)).

| Tab | |
|---|---|
| **Play** | the ISO, the installation steps, the save profile to play with, and Play. |
| **Console** | what the installation and the game print; no console window stays open. |
| **Settings** | picture, sound, gamepad, keys. Changes are saved at once. |
| **Updates** | checks GitHub, shows what a new version brings ([CHANGELOG.md](CHANGELOG.md)) and installs it; only what changed is built again. |

**Save profiles** each have their own memory card and save states; the first one is *Default*. They are the folders of `save` in the installation folder (in a Git checkout, of the work directory's `local`). *New* creates one, empty or with a copy of the saves in another folder (another installation's, for example).

## Controls

| PS2 | Keyboard and mouse | Gamepad |
|---|---|---|
| ✕ accelerate, select | W · left click | A / ✕ |
| □ brake | Space | X / □ |
| R1 boost, Impact Time | Left Ctrl | RB / R1 |
| D-pad: steer, menus | A · D · S, arrows, mouse wheel | D-pad, left stick |
| △ back | V · Escape · right click | Y / △ |
| ○ | C · keypad 1 | B / ○ |
| L1 · L2 · R2 | Left Shift · E · Q | LB · LT · RT |
| Start · Select | Enter · Tab | Start · Back |

Each PS2 button takes up to three keys, mouse buttons or wheel directions: change them in the launcher's Settings tab or in the game's **F1** menu.

| Key | |
|---|---|
| **F1** | settings menu (the game pauses) |
| **F11** · **Alt+Enter** | full screen |
| **F5** · **F7** | save · load state |
| **F9** | mark a glitch: notes the moment in the log and saves a screenshot and a save state in `marks/` |

## Settings

The launcher's Settings tab, the game's **F1** menu and `settings.ini` (in the installation folder; at the repository root in a Git checkout) change the same settings.

<details>
<summary><code>settings.ini</code> reference</summary>

| Setting | Default | |
|---|---|---|
| `window_width`, `window_height` | 1280 × 960 | start-up window size |
| `fullscreen` | false | borderless full screen |
| `vsync`, `fps_limit` | true, 0 | display sync and cap |
| `aspect` | 4:3 | `4:3`, `16:9` or `stretch`. With `16:9` the game draws a real widescreen picture from its next start, unless its own Widescreen option was set otherwise |
| `smooth_scaling` | true | bilinear enlargement |
| `super_sampling` | 4 | samples per pixel: 1, 2, 4, 8 or 16 |
| `high_resolution` | true | a higher resolution built from the samples: 2x (1280 pixels wide) with 4 or 8 samples, 4x (2560) with 16 |
| `fxaa` | false | FXAA on the finished picture |
| `hud_smoothing` | true | the race HUD smooth instead of blocky at 2x and 4x |
| `progressive_scan` | true | whole frames, no interlacing lines |
| `gs_thread` | true | the PS2's graphics units on a second processor core |
| `fast_loading` | true | loading screens run faster than real time |
| `keep_speed` | true | when the graphics cannot keep up, the game draws fewer frames instead of slowing down (from the next race) |
| `refresh_rate` | 60 | `50` or `60` as on a console; `120`, `144`, `240`... up to `500` runs the whole game that many frames a second at the same speed, with more processor work |
| `stick_deadzone` | 12 | percent |
| `vibration` | true | the game's pad vibration on your gamepad |
| `gamepad_mappings` | — | an extra `gamecontrollerdb.txt` |
| `volume` | 100 | percent |
| `show_fps` | false | frames per second in the corner |
| `key_cross`, `key_l1`... | see Controls | up to three inputs per button (`W`, `LEFT_SHIFT`, `MOUSE_LEFT`, `MOUSE_WHEEL_UP`...), or `NONE` |

</details>

## FAQ

**Why is there no ready-to-play download?**
The executable is built from the game's own code, translated from your disc: distributing it would be distributing the game. The releases contain the installer and this project's source only, and the build happens on your PC.

**Which version of the game do I need?**
The European PlayStation 2 release, `SLES-54627`. The installer checks the disc image. The American, Japanese and PSP releases are different programs and are not supported.

**Is it an emulator?**
Partly. The game's main program is not interpreted: it is translated to C++ before the build, like its vertex programs. The graphics chip, the sound chip and the I/O processor are emulated.

**Can I bring my saves?**
A save that is a folder (`BESLES-54627...` with its `icon.sys`, as PCSX2's *folder* memory cards keep it) can be copied into a new save profile from the launcher. Memory card image files and `.psu` or `.max` files are not read.

**Linux, macOS, Android?**
Windows is the tested platform. Linux builds in CI and from source. There is no macOS or Android build.

## How it works

| PS2 | Here |
|---|---|
| **EE**, the main MIPS R5900 processor that runs the game's code | translated ahead of time into C++ by [PS2Recomp](https://github.com/ran-j/PS2Recomp) (about 51,000 functions), then compiled |
| **VU0** and **VU1**, the vector units that transform and light every vertex | their microprograms are translated to C++ as well, with an interpreter for the rest |
| **GS**, the graphics chip | [paraLLEl-GS](https://github.com/Arntzen-Software/parallel-gs), on the GPU through Vulkan |
| **IOP**, the I/O processor with its sound, disc and pad modules | emulated, running the game's own modules |
| **SPU2**, **IPU**, DMA, timers | emulated: sound voices and mixing, the MPEG video decoder |

Nothing in the game is skipped or faked: a console library function is replaced by native code only where its identity is proven ([docs/SDK_FUNCTIONS.md](docs/SDK_FUNCTIONS.md)). The repository holds a snapshot of PS2Recomp with this project's changes ([docs/CHANGES.md](docs/CHANGES.md)), the runner, the installer and the launcher. Game files and the code generated from them stay in the work directory and are never committed.

## Documentation

| | |
|---|---|
| [CHANGELOG](CHANGELOG.md) | what each version changes for players |
| [STATUS](docs/STATUS.md) | what works and what is left |
| [DEVELOPMENT](docs/DEVELOPMENT.md) | build steps, run options, tests, repository layout |
| [BOOT](docs/BOOT.md) | boot sequence, IOP modules, diagnostics |
| [NATIVE_RENDERER](docs/NATIVE_RENDERER.md) | the translated VU programs and where the time goes |
| [CHANGES](docs/CHANGES.md) | every change to the PS2Recomp snapshot |
| [SDK_FUNCTIONS](docs/SDK_FUNCTIONS.md) · [FINDINGS](docs/FINDINGS.md) | reverse-engineering evidence |

## Contributing

Bug reports and pull requests are welcome; read [CONTRIBUTING.md](CONTRIBUTING.md) first (no faked success, evidence for every SDK binding, every upstream change documented). A useful bug report says what you saw, when, on which hardware, and comes with a save state (F5) taken just before if you can. Areas where help counts most:

- **Speed** on smaller PCs: the game thread and the graphics submission thread ([docs/NATIVE_RENDERER.md](docs/NATIVE_RENDERER.md)).
- **Linux and macOS**: builds, controllers, packaging.
- **Sound**: the SPU2 ([`iop_spu2.cpp`](ps2xIOP/src/emulator/core/iop_spu2.cpp)) needs testing by ear, its reverb especially.
- **Modding**: [`disc_patches.cpp`](ps2xRuntime/src/lib/disc_patches.cpp) changes disc files as the game reads them, without touching the ISO.

## Credits

- [PS2Recomp](https://github.com/ran-j/PS2Recomp) by ran-j and contributors: the recompiler and the runtime this project builds on (GPLv3).
- [paraLLEl-GS](https://github.com/Arntzen-Software/parallel-gs) by Arntzen Software: the GS renderer (LGPLv3).
- [ps2autotests](https://github.com/unknownbrackets/ps2autotests) (ISC): the console results the EE's float instructions are checked against.
- [PCSX2](https://github.com/PCSX2/pcsx2) (GPLv3 or later): the reference for the EE divider.

## License

GPLv3, see [LICENSE](LICENSE).

Burnout Dominator is a trademark of Electronic Arts. This project is not affiliated with or endorsed by Electronic Arts, Criterion Games or Sony Interactive Entertainment. It ships no game code or game data; screenshots are of the game running in this project.
