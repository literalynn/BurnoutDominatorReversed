# Données du projet

`functions.ee.csv` : carte des fonctions de l’ELF PAL `SLES_546.27` (SHA256 dans `project.json`). Chaque ligne donne un nom généré par Ghidra (`FUN_…`, `entry_…`, `caseD_…`), une adresse de début, une adresse de fin et une taille. Elle ne contient aucun octet du jeu.

Provenance : analyse de `SLES_546.27` par Ghidra 12.1.4 avec l’extension Emotion Engine Reloaded 2.1.38 (processeur `r5900:LE:32:default` ; archives et empreintes dans `tools/ghidra/docs/toolchain.json`), exportée par le script upstream `ps2xRecomp/tools/ghidra/ExportPS2Functions.java` (méthode : `ps2xAnalyzer/Readme.md`, section « Ghidra Integration »). Seuls les enregistrements de `.text` (`0x00100000` à `0x003BEB28`) sont gardés : 48 969 lignes, soit 8 860 fonctions et 40 109 labels de reprise. Les 65 enregistrements hors de `.text` sont écartés (`docs/FINDINGS.md`, E003 et E004).

`tools/install.py` l’utilise sur place, sauf si le dossier de travail contient `local/analysis/ghidra/functions.ee.csv` (un export plus récent) ; `--function-map` choisit une autre carte. `project.py generate --augment` écrit la carte complétée par `tools/augment_function_map.py` dans `local/analysis/ghidra/` du dossier de travail, jamais ici.

Pour la refaire : analyser `SLES_546.27` dans Ghidra avec ces versions, lancer `ExportPS2Functions.java`, garder les lignes dont l’adresse de début est dans `.text` et enregistrer le résultat sous `functions.ee.csv`. `tools/ghidra/README.md` décrit l’export du pseudocode et les scripts d’analyse du projet, pas cette carte.

SHA256 : `449eb932a36f348a6e032e023c8dd1b1f5a3996e0e5129ddbf12773df8011138`
