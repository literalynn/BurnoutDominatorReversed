# Project data

`functions.ee.csv`: function map of the PAL ELF `SLES_546.27` (SHA256 in `project.json`). Each line gives a Ghidra-generated name (`FUN_…`, `entry_…`, `caseD_…`), a start address, an end address and a size. It contains no game bytes.

Provenance: analysis of `SLES_546.27` by Ghidra 12.1.4 with the Emotion Engine Reloaded 2.1.38 extension (processor `r5900:LE:32:default`; archives and hashes in `tools/ghidra/docs/toolchain.json`), exported by the upstream script `ps2xRecomp/tools/ghidra/ExportPS2Functions.java` (method: `ps2xAnalyzer/Readme.md`, section "Ghidra Integration"). Only the `.text` records (`0x00100000` to `0x003BEB28`) are kept: 48,969 lines, i.e. 8,860 functions and 40,109 resume labels. The 65 records outside `.text` are dropped (`docs/FINDINGS.md`, E003 and E004).

`tools/install.py` uses it in place, unless the work directory contains `local/analysis/ghidra/functions.ee.csv` (a newer export); `--function-map` picks another map. `project.py generate --augment` writes the map completed by `tools/augment_function_map.py` into `local/analysis/ghidra/` of the work directory, never here.

To redo it: analyse `SLES_546.27` in Ghidra with these versions, run `ExportPS2Functions.java`, keep the lines whose start address is in `.text` and save the result as `functions.ee.csv`. `tools/ghidra/README.md` describes the pseudocode export and the project's analysis scripts, not this map.

SHA256: `449eb932a36f348a6e032e023c8dd1b1f5a3996e0e5129ddbf12773df8011138`
