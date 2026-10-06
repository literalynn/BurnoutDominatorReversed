# État réel du projet

La demande de rétro-ingénierie intégrale n’est pas encore satisfaite. Ce dépôt conserve le travail reproductible, la chaîne de recompilation et les observations vérifiées sur l’ISO fournie. Il ne revendique pas un port jouable ou une récupération des sources originales.

## Identité observée

- ISO : 4 660 166 656 octets, SHA256 `dc4fc4e9700d6a5ae26531618d880541b1ff2d2deaf191f1359b98d3f63f96a5`.
- BOOT2 : `SLES_546.27`, 3 472 132 octets, SHA256 `fe8b4b28e165620a35e55bcf6fb452de16cf7982ea6584ab28dc5f6b4b187301`.
- ELF32 little endian, MIPS R5900, point d’entrée `0x00100008`, 2 segments PT_LOAD, 79 sections, aucune table de symboles.
- `.text` : adresse `0x00100000`, taille 2 878 248 octets. `.data` est aussi marquée exécutable ; cela ne prouve pas que ses mots sont des instructions.
- 64 overlays DVP/VU, accompagnés des tables d’overlays ; 12 modules IOP IRX.
- Disque inventorié : 570 fichiers, 50 répertoires. Tous les fichiers sont extraits localement ; les lectures de secteurs doivent utiliser l’ISO originale.

Preuves conservées : `local/disc_inventory.json`, `local/analysis/`, `local/analysis_validation.json`.

## Vérifications déjà réalisées

- 20 tests Python des parseurs ISO9660/ELF32 réussis : bornes, endianness, symboles, chemins, extraction limitée et complète.
- SHA256 des 13 binaires extraits et de SYSTEM.CNF vérifiés.
- Outils PS2Recomp et PS2Analyzer compilés sous MSVC x64, commit upstream fixé dans `UPSTREAM.json`.
- Diagnostic REA en lecture seule : aucun moteur configuré ; le périmètre Windows documenté ne couvre pas PS2 ELF/R5900. La configuration personnelle de Codex n’a pas été modifiée.
- Premier essai heuristique : 8 514 fonctions générées, 0 fonctions forcées en stub, 0 fonctions ignorées. Le rapport signale **5 216 instructions non traitées** et 2 086 avertissements. Les premières/dernières erreurs observées sont dans `.data`. L’exit code 0 du recompileur ne garantit donc pas une traduction correcte.
- Analyse Ghidra 12.1.4 avec EmotionEngine Reloaded 2.1.38 : 8 860 fonctions identifiées dans `.text`, 40 109 labels exécutables supplémentaires ; les 65 enregistrements hors de `.text` sont conservés séparément.
- Traduction affinée : **0 instruction non traitée, 0 erreur**, 4 158 avertissements relatifs aux branchements indirects ; 48 961 corps traduits et 8 adaptateurs SIF automatiques du moteur, 0 fonction ignorée. Les 8 adaptateurs sont listés dans `generated/ps2_recompiled_stubs.h` ; ils ne sont pas des retours de réussite inventés.
- 437/437 tests upstream du moteur et 45/45 tests CTest du cache GS réussis sur Windows. Ces tests portent sur le moteur ; ils ne valident pas les courses de Burnout.
- Accès au disque corrigé : les recherches EE/IOP rendent maintenant les LBA originales. Cinq exécutables de tests IOP réussissent, dont sept groupes de tests ISO ; offsets de plus de 4 Gio vérifiés. Sur le disque réel : SYSTEM.CNF au secteur 2 265 203 et SLES_546.27 au secteur 2 263 507.

## Travaux en cours et critères restants

L’analyse Ghidra/R5900 affine les limites des fonctions. Les journaux et l’audit de traduction doivent être examinés avant toute affirmation de couverture totale. La compilation du runtime et les essais du binaire exact sont des étapes distinctes.

Restent à établir : code indirect et données exécutées, overlays VU et IOP, démarrage réel, menu, courses, physique, collisions, entrées, graphismes, synchronisation DMA/VIF/GIF, son, vidéos et sauvegardes. Les comportements observés doivent être comparés avec l’exécution PS2/PCSX2. Linux et macOS doivent être compilés et testés sur leurs systèmes.

Le runtime upstream reste expérimental : support matériel partiel, performances VU/GS limitées et implémentations audio incomplètes. Une compilation Windows réussie établira seulement un exécutable natif issu des fonctions traduites.
