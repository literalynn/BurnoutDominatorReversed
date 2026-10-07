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

- SPU2 (`ps2_spu2_tests`) : décodage ADPCM, registres, transferts PIO et DMA, minuterie d’IRQ des voix, IRQ de transfert, horloge du mélangeur, fin de boucle, sortie audio : réussis.
- Après le lancement 11 (Windows, MSVC) : `ps2x_tests` 443/443, les cinq suites ps2xIOP, `ps2_spu2_tests` et `burnout_overrides_tests` (29 liaisons) réussissent.
- Linux (Ubuntu 24.04, GCC 13, Ninja) : `ps2_recomp`, `ps2_analyzer`, le runtime et tous les tests compilent ; 438/438 tests du moteur, les 5 suites ps2xIOP et `burnout_overrides_tests` réussissent. Le jeu compile et se lance aussi sous Linux (session cloud, `-O1`, à partir du paquet `tools/cloud_bundle.py`, sans l’ISO).

## Démarrage du jeu

Détails et preuves : [BOOT.md](BOOT.md) et [SDK_FUNCTIONS.md](SDK_FUNCTIONS.md).

- Premier exécutable Windows : 134 Mo, compilé sans LTO en 9 min 20 s.
- Lancements bornés 1 à 3 : le jeu passe crt0, `main`, l’initialisation SIF et libcdvd, puis s’arrête au troisième lancement sur un appel indirect vers 0x1E5020, code absent de la carte Ghidra.
- `tools/augment_function_map.py` ajoute 2 191 points d’entrée atteints par pointeur ; la régénération (51 161 fichiers, 0 erreur) et la recompilation complète ont réussi.
- Depuis : 29 fonctions SDK liées au runtime (libsif avec les commandes SIF, loadfile, iopheap, fileio, libcdvd), ROMVER européen de 16 octets, `sceCdReadClock` et `GetSystemTimeLow` côté IOP, avertissement sur tout import IOP intégré inconnu.
- Lancements 4 à 6 (Linux, sans l’ISO) :
  - `RwEngineInit` réussit une fois corrigée la fin du tas ;
  - la mémoire propre du runtime est déplacée sous l’image du jeu ;
  - les commandes SIF entre l’EE et les IRX (audio RWA, système de fichiers GTFS) et `sceSifSetDmaIntr` fonctionnent ;
  - le démarrage lit la police `Language/Fonts/dirtyEra.bin`, puis attend `Data/GlobalE.txd`, absent du paquet cloud ;
  - le jeu envoie une image GIF par boucle d’attente.

- Lancements 7 à 9 (Windows, avec l’ISO) :
  - un SPU2 réel côté IOP donne à RWA l’horloge qu’il attend : tout `sound_generic.awd` est alors transféré ;
  - `sceSifSearchModuleByName` (0x3B1EA8) est liée : le jeu vérifie la présence de modules IOP (`sio2man`, `mcman`, `mcserv`, `multitap_manager`) avant d’initialiser la bibliothèque de carte mémoire ;
  - la machine d’état de démarrage se termine (état 0x1C), le jeu entre dans sa boucle principale et dessine l’écran LOADING (barre orange) ;
  - l’IOP et les timers EE avancent par lots : de 3,6 à 25–50 images par seconde.

- Lancements 10 et 11 :
  - les lectures CD qui débordent d’un bloc IOP alloué sont faites comme sur console : le chargement de l’écran LOADING se termine ;
  - les appels système remplacés par le correctif noyau de libkernel (alarmes, drapeaux d’événement), dont le code copié en mémoire noyau ne peut pas s’exécuter, sont servis par le runtime : `SetAlarm` réveille de nouveau le thread principal.

Blocage actuel (lancement 11) : écran noir ; la vidéo d’introduction démarre et son analyseur MPEG-2 attend des données de l’IPU. Détails : [BOOT.md](BOOT.md) §12.

Reprise sur la machine qui possède l’ISO (les variables d’observation sont décrites au §10 de BOOT.md) :

```powershell
python tools/project.py run --headless --seconds 60 --status-ms 2000 --log run12.log --tail 120
```

## Demandes à traiter une fois le jeu jouable

Ces objectifs supposent que le jeu atteigne les courses ; ils seront traités après le démarrage. Pistes déjà établies :

- **Mods de fichiers.** Le jeu lit ses données par GTFSCDVD.IRX, c’est-à-dire par secteurs (`sceCdRead` dans l’IOP émulé), pas par noms de fichiers EE. Une surcouche `mods/<chemin sur le disque>` doit donc agir au niveau des secteurs : table LBA → fichier tirée du répertoire ISO9660, remplacement direct si la taille ne dépasse pas l’original, image virtuelle reconstruite sinon (`iop_cdvd.cpp` contient déjà une construction d’ISO virtuelle). À vérifier : si GTFS relit lui-même les répertoires ISO9660, les nouvelles tailles et LBA lui parviennent sans autre changement.
- **Mods de code.** Toute fonction invitée peut être remplacée par une fonction native (mécanisme de `src/burnout_overrides.cpp`) ; une API chargeant des bibliothèques natives depuis `mods/` en découle directement.
- **Images par seconde.** Au démarrage, `0x207820` fixe la cadence et le pas de simulation selon l’octet 0x432E98 : `0x1E68A0(60)` et `0x1E6668(16,667 ms)`, ou `0x1E68A0(50)` et `0x1E6668(20 ms)`. La physique suppose ce pas fixe : accélérer la boucle la fausserait. 60 Hz est donc disponible nativement ; au-delà (jusqu’à 480 Hz), il faut garder la simulation à 60 Hz et interpoler le rendu (objets, caméra) entre deux pas, ce qui demande d’identifier la boucle de jeu et la scène RenderWare.
- **Résolution 2K.** Le runtime ne rend le GS que sur le CPU (`gs_cpu_backend`), à la résolution PS2. Une résolution interne plus élevée demande un rendu GS sur GPU, derrière l’interface `GSRasterBackend` existante : chantier du runtime, indépendant du jeu.
- **Manettes.** Le runtime lit une manette du PC (Xbox, DualSense…) mais seulement pour `libpad`. Burnout utilise `libdbc`/`libpad2` avec DBCMAN.IRX et DS2O.IRX, qui parlent au port SIO2 : l’IOP émulé n’a pas de modèle SIO2 et le service DBCMAN simplifié ne lit aucune entrée. Il faut soit un SIO2 émulé alimenté par la manette du PC, soit un DBCMAN/libpad2 de haut niveau ; priorité dès que le jeu affiche quelque chose.
- **Chargements.** Lectures disque (latence CDVD simulée : 128 cycles IOP) et carte mémoire (fichiers hôte) sont déjà quasi instantanées. Restent les attentes codées dans le jeu (durée minimale d’un écran de chargement, temporisations de sauvegarde), à repérer une fois ces écrans atteints.

## Travaux en cours et critères restants

L’analyse Ghidra/R5900 affine les limites des fonctions. Les journaux et l’audit de traduction doivent être examinés avant toute affirmation de couverture totale. La compilation du runtime et les essais du binaire exact sont des étapes distinctes.

Restent à établir : code indirect et données exécutées, overlays VU et IOP, démarrage réel, menu, courses, physique, collisions, entrées, graphismes, synchronisation DMA/VIF/GIF, son, vidéos et sauvegardes. Les comportements observés doivent être comparés avec l’exécution PS2/PCSX2. Linux et macOS doivent être compilés et testés sur leurs systèmes.

Le runtime upstream reste expérimental : support matériel partiel, performances VU/GS limitées et implémentations audio incomplètes. Une compilation Windows réussie établira seulement un exécutable natif issu des fonctions traduites.
