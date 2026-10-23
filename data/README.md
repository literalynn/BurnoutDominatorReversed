# Project data

`functions.ee.csv`: function map of the PAL ELF `SLES_546.27` (SHA256 in `project.json`). Each line gives a Ghidra-generated name (`FUN_…`, `entry_…`, `caseD_…`), a start address, an end address and a size. It contains no game bytes.

Provenance: analysis of `SLES_546.27` by Ghidra 12.1.4 with the Emotion Engine Reloaded 2.1.38 extension (processor `r5900:LE:32:default`; archives and hashes in `tools/ghidra/docs/toolchain.json`), exported by the upstream script `ps2xRecomp/tools/ghidra/ExportPS2Functions.java` (method: `ps2xAnalyzer/Readme.md`, section "Ghidra Integration"). Only the `.text` records (`0x00100000` to `0x003BEB28`) are kept: 48,969 lines, i.e. 8,860 functions and 40,109 resume labels. The 65 records outside `.text` are dropped (`docs/FINDINGS.md`, E003 and E004).

`tools/install.py` uses it in place, unless the work directory contains `local/analysis/ghidra/functions.ee.csv` (a newer export); `--function-map` picks another map. `project.py generate --augment` writes the map completed by `tools/augment_function_map.py` into `local/analysis/ghidra/` of the work directory, never here.

To redo it: analyse `SLES_546.27` in Ghidra with these versions, run `ExportPS2Functions.java`, keep the lines whose start address is in `.text` and save the result as `functions.ee.csv`. `tools/ghidra/README.md` describes the pseudocode export and the project's analysis scripts, not this map.

SHA256: `449eb932a36f348a6e032e023c8dd1b1f5a3996e0e5129ddbf12773df8011138`

## vu_programs.manifest

Where the game's VU0 and VU1 microprograms sit in `SLES_546.27`, for `tools/compile_vu_programs.py`. Each `image` line gives the entry points of one recorded code image, an FNV-1a 64-bit checksum over all the code it is rebuilt from, and the unit; each `run` line a range of instruction pairs and its offset in the ELF. It contains no game bytes, and a single checksum per image gives none of them away (`tools/vu_program_manifest.py`).

`project.py configure` uses it when the work directory has no recordings (`local/vu-programs/`): it rebuilds the code images from the extracted ELF into `local/vu-programs-shipped/`, and the runtime compiles them. Without them the VU interpreter runs everything, about 20 times slower in a race.

Provenance: 28 code images recorded with `project.py run --vu-capture` in the menus and in races (23 once identical ones are merged), then `python tools/vu_program_manifest.py make --output data/vu_programs.manifest --game-root <work>/local/disc --game <work>/local/disc/SLES_546.27 <work>/local/vu-programs`. The C++ compiled from its expansion is byte for byte the C++ compiled from the recordings.

SHA256 (LF line endings, as committed): `c2eb954427e8e38c7201766a0d961264d469c7d0a3a55c057fbbcedd723973d5`
