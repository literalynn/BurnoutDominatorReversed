# Changelog

What changes for players, newest first. The launcher shows the entries an update brings before installing it, and each version is a release on GitHub. Technical details are in `docs/CHANGES.md` and the commit log.

## 1.1.0 (November 10, 2026)

- **One file to install.** Download `BurnoutDominator.exe` (Windows) or `BurnoutDominator.py` (Linux, macOS) from the latest release, put it in an empty folder and start it. It downloads what it needs into that folder (on Windows a Python and a Git of its own) and everything stays there: the project, the game's files, the settings and the saves. The folder can be moved.
- **A new launcher**: a sober, dark window of its own, with tabs for the game, its console, the settings and updates.
- **Less to install first.** PowerShell 7 is no longer needed; on Windows only the C++ compiler (Visual Studio or its Build Tools) has to be there.
- **Updates without Git.** The launcher downloads the new version's files and the build only compiles what changed.

## 1.0.0 (November 10, 2026)

- **Launcher** (`Launcher.bat`): one window to choose the ISO, generate the code and build the game, pick a save profile, play, read the console and change the settings. No console window stays open while you play.
- **Save profiles**: each profile has its own memory card and save states. The saves you already have are the profile "Default".
- **Updates**: the launcher's Updates tab checks GitHub, shows what the new version brings and installs it.
- **Real 16:9**: with `aspect = 16:9` the game draws a true widescreen picture by itself, because the console now tells it that the screen is 16:9. The game's own Widescreen option still has the last word once you have changed it.
