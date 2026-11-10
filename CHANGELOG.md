# Changelog

What changes for players, newest first. The launcher shows the entries an update brings before installing it, and each version is a release on GitHub. Technical details are in `docs/CHANGES.md` and the commit log.

## 1.0.0 (November 10, 2026)

- **Launcher** (`Launcher.bat`): one window to choose the ISO, generate the code and build the game, pick a save profile, play, read the console and change the settings. No console window stays open while you play.
- **Save profiles**: each profile has its own memory card and save states. The saves you already have are the profile "Default".
- **Updates**: the launcher's Updates tab checks GitHub, shows what the new version brings and installs it.
- **Real 16:9**: with `aspect = 16:9` the game draws a true widescreen picture by itself, because the console now tells it that the screen is 16:9. The game's own Widescreen option still has the last word once you have changed it.
