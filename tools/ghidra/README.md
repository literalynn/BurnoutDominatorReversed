# Ghidra analysis of Burnout Dominator PAL

These scripts use the saved project `local/analysis/ghidra/project/BurnoutDominatorPAL.gpr`. They open `SLES_546.27` read-only, without re-importing or re-analysing it. The processor is `r5900:LE:32:default` and the compiler `default`.

The files `local/decompiled/functions/*.pseudocode.c` contain **Ghidra decompiler pseudocode**, with inferred types and names. This pseudocode is neither recovered original source nor directly compilable C. Export coverage does not prove that the game is recompiled, playable or correctly represented by every decompiler expression.

## Re-exporting the pseudocode

Use Ghidra 12.1.4, the `ghidra-emotionengine-reloaded` 2.1.38 extension built for that version, and JDK 21 or later. The first pass was made with Java 22.0.1. Links and hashes of the verified archives are in `docs/toolchain.json`.

From the repository:

```powershell
& ./tools/ghidra/export-pseudocode.ps1 -GhidraInstall 'C:\path\to\ghidra_12.1.4_PUBLIC'
```

The script uses four independent decompiler instances, a 10-second timeout per function and a global budget of 900 seconds. The project is left unchanged. The Java options put preferences, caches and temporary files under `local/analysis/ghidra/export-state`.

To retry only the functions whose first pass timed out:

```powershell
& ./tools/ghidra/export-pseudocode.ps1 -GhidraInstall 'C:\path\to\ghidra_12.1.4_PUBLIC' -RetryTimeouts
```

The retry uses a 60-second timeout per function. It keeps `manifest.first-pass.json`, `manifest.first-pass.jsonl`, the counts and durations of the first pass, and adds the attempts to the merged manifest. A retry already recorded is not run again implicitly.

`manifest.json` and `manifest.jsonl` describe each function, its address, its exclusive end, its body ranges, its result, its warnings and its attempts. Individual files are prefixed with the address. A failure keeps a status file and its manifest entry. `all_pseudocode_exported` is true only when every function produced pseudocode; check the warnings too.

The command returns an error if Ghidra fails or produces no manifest, and code 2 if an individual export stays incomplete. Logs and the exact argument arrays are kept in `local/decompiled`.

## Auxiliary analysis scripts

- `ExportBurnoutPALAnalysis.java`: tested export of the real Ghidra functions in `.text`, distinct from resume labels; checks the ELF identity.
- `BurnoutPALAnalysisSetup.java`: preparation for a future import. **Not run in the shipped saved analysis.** It checks the ELF hash and removes the execute permission from sections other than `.text` in the Ghidra project. It does not modify the original binary.

The PS2 linker marks `.data` executable in this ELF. The exports therefore limit EE functions to `[0x00100000, 0x003BEB28)` and keep the entry `0x00100008`. VU overlays are data for another processing unit; EE pseudocode does not replace them.

## Kept result

All **8,860 real EE functions** produced pseudocode. The initial pass got 8,858 results in 28.519 seconds and two timeouts. The targeted retry got the two remaining results in 32.747 seconds, with a 60-second timeout per function. The initial files and counts remain available.

**455 files contain Ghidra `WARNING:` comments**, kept in the files and the manifest. These warnings concern among others blocks considered unreachable or overlapping global symbols. A `success` status means Ghidra returned pseudocode; it does not validate equivalence with the binary's behaviour.

## Parallelism API used

The first pass follows the official pooling of `DecompInterface` instances through `DecompilerCallback`, with `ParallelDecompiler` and a pool limited to four workers. The retry uses a single instance and only handles the timeouts.

[Official ParallelDecompiler](https://github.com/NationalSecurityAgency/ghidra/blob/Ghidra_12.1.4_build/Ghidra/Features/Decompiler/src/main/java/ghidra/app/decompiler/parallel/ParallelDecompiler.java), [official DecompilerCallback](https://github.com/NationalSecurityAgency/ghidra/blob/Ghidra_12.1.4_build/Ghidra/Features/Decompiler/src/main/java/ghidra/app/decompiler/parallel/DecompilerCallback.java).
