# Changelog

What changes for players, newest first. The launcher shows the entries an update brings before installing it, and each version is a release on GitHub. Technical details are in `docs/CHANGES.md` and the commit log.

## 1.3.0 (November 13, 2026)

- **No Visual Studio needed on Windows**: when no C++ compiler is installed, the launcher offers to download Microsoft's C++ Build Tools, CMake and Ninja into the game's folder (about 500 MB, 1.25 GB once unpacked) once you accept Microsoft's licence. Nothing is installed on the system and no administrator rights are needed; deleting the folder removes them. A Visual Studio you already have is still used.
- **Skip any video**: while a video plays, hold Start (or the key bound to it) for a second and it ends at once, the way it ends when you watch it to the end, including the Basic Training video the game shows once per profile and does not let you skip. One hold skips all three videos at start-up. Setting "Skip videos".
- **Button prompts for your controller**: the ✕ ○ □ △ and L1/R1 symbols become A B X Y and LB RB LT RT on Xbox and other gamepads, or the key you bound on the keyboard, and they change as soon as you use another device. Setting "Button prompts": Auto, PlayStation, Xbox or Keyboard (a PlayStation pad that shows up as an Xbox pad, through DSX for example, needs "PlayStation").
- **Brighter car lights**, optional: "Car lights" at 150 % or 200 % makes headlights and tail lights brighter, to see traffic sooner. It brightens every car, yours and the rivals' included. Off by default.
- **A new F1 menu** in the game's own font, art and sounds, usable with a gamepad: hold Select (or press the Guide button) to open it. It has a **Quit game** entry, which asks first and waits for a save in progress to finish before closing the game.
- **16:9 without stretching**: the window now always shows the picture in the shape the game draws. Switching "Picture shape" while playing no longer stretches the 4:3 view; the game draws 16:9 from its next start. New installations start in 16:9, in a 1280 × 720 window.
- Videos play with half the processor time they took.
- **Builds on PCs with less memory**: the installation no longer stops with "compiler is out of heap space". It compiles fewer files at once when the PC has little memory.
- **Free up disk space**: once the game is built, a small button in the launcher's Installation card deletes the files only building needs (about 12 GB). The game still plays, but the next update has to build the whole game again (30 to 90 minutes) instead of only what changed.
- Above 60 frames a second, the menus' animations keep the console's speed: the scrolling text of the World Tour pages no longer runs four times too fast at 240.

## 1.2.3 (November 11, 2026)

- **Browse no longer closes the launcher**: choosing the ISO, or a folder of saves, in the Browse window closed the launcher's window as soon as the choice was made.

## 1.2.2 (November 10, 2026)

- Linux: the launcher's window also builds with the Clang compiler (it fell back to the page).

## 1.2.1 (November 10, 2026)

- The launcher shows the red B again; only its icon in the taskbar and the title bar is grey.

## 1.2.0 (November 10, 2026)

- **The launcher has a window of its own**, drawn by the program itself instead of a page in Edge: same tabs, same settings, nothing to translate. It is the same on Windows, Linux and macOS. On Windows it is inside `BurnoutDominator.exe`; on Linux and macOS `BurnoutDominator.py` builds it the first time (about a minute) and after each update. If it cannot start, the launcher opens as a page like before.
- `BurnoutDominator.exe` now updates itself with the rest: after this update, the next start of the launcher opens the new window.

## 1.1.4 (November 10, 2026)

- **Sharper HUD and menus in high resolution**: the race display (speed, laps, positions, the names over the rivals) and the menus' text and art are now smooth at 2x and 4x instead of showing big square pixels. The setting is "Smooth HUD", on by default.
- **16:9 explained**: the game reads the screen shape when it starts. After changing "Picture shape" to or from 16:9, restart the game to get the wider view; a save state keeps the view it was saved with. "Stretch" fills the window with the 4:3 view.
- The launcher's icon is now a grey B, so it is not mistaken for the game, which keeps the red one.

## 1.1.3 (November 10, 2026)

- **A `save` folder**: in an installation made with `BurnoutDominator.exe`, your saves are now in the `save` folder next to it, one folder per profile (`save\Default`...). The saves you already have move there by themselves at the launcher's next start.
- The game's window and its executable carry the game's icon, and the window is titled "Burnout Dominator".

## 1.1.2 (November 10, 2026)

- **Browse buttons fixed**: the window to choose the ISO or a folder of saves opened behind the launcher, so nothing seemed to happen. It now opens in front of it.

## 1.1.1 (November 10, 2026)

- **Settings tab redone**: the settings are grouped by subject, with switches, sliders and choices, and a line that says what each one does. A change is saved at once. To change a key, press + and then the key.
- **Saves from elsewhere**: a new save profile can start with a copy of the saves in a folder you give, another installation's for example.
- The launcher's window opens at 1280 × 720 (16:9).
- Edge no longer offers to translate the launcher's window.

## 1.1.0 (November 10, 2026)

- **One file to install.** Download `BurnoutDominator.exe` (Windows) or `BurnoutDominator.py` (Linux, macOS) from the latest release, put it in an empty folder and start it. It downloads what it needs into that folder (on Windows a Python and a Git of its own) and everything stays there: the project, the game's files, the settings and the saves. The folder can be moved.
- **A new launcher**: a sober, dark window of its own, with tabs for the game, its console, the settings and updates.
- **Less to install first.** PowerShell 7 is no longer needed; on Windows only the C++ compiler (Visual Studio or its Build Tools) has to be there.
- **Updates without Git.** The launcher downloads the new version's files and the build only compiles what changed.

## 1.0.0 (November 10, 2026)

- **Launcher** (`Launcher.bat`): one window to choose the ISO, generate the code and build the game, pick a save profile, play, read the console and change the settings. No console window stays open while you play.
- **Save profiles**: each profile has its own memory card and save states. The saves you already have are the profile "Default".
- **Updates**: the launcher's Updates tab checks GitHub, shows what the new version brings and installs it.
- **Real 16:9**: with `aspect = 16:9` the game draws a true widescreen picture by itself, because the console now tells it that the screen is 16:9. The game reads it when it starts.
