# Modifications par rapport à PS2Recomp

Base complète conservée depuis `ran-j/PS2Recomp`, commit indiqué dans `UPSTREAM.json`, sous sa licence GPLv3. Les fichiers de jeu et leur traduction restent dans des répertoires ignorés par Git.

- `tools/` : extraction ISO9660, analyse ELF32, contrôle SHA256 du PAL SLES_546.27, génération et audit de traduction.
- `cmake/Burnout.cmake` : liaison des fonctions traduites dans `burnout_dominator`, sans modifier les sources upstream avec du code de jeu.
- `src/burnout_main.cpp` : montage explicite de l’ISO d’origine et du disque extrait, sauvegardes locales, arrêt strict sur fonction absente, diagnostic sans fenêtre avec délai borné.
- `ps2_runtime.h` : accesseur de l’indicateur existant de fonction manquante pour que le diagnostic retourne un échec réel.
- `CMakeLists.txt` : inclusion de l’intégration Burnout.
- `ps2xIOP/.../iso9660.h`, `iso9660.cpp` et les deux routes CDVD EE/IOP : recherche des fichiers avec leur LBA réelle sur l’ISO d’origine, lectures bornées en 64 bits et échec explicite si cette image est invalide. Le comportement upstream reste disponible lorsque aucune image n’est configurée.
- Tests natifs ISO : données malformées, formats non pris en charge, chemins et offsets au-delà de 4 Gio ; les secteurs réels de SYSTEM.CNF et du binaire PAL ont été vérifiés.

- `PS2Runtime::run(options)` / `runHeadless(options)` : même préparation que l’exécution normale (réinitialisations SIF/IOP/audio/MPEG), délai, ligne d’état périodique et captures PNG du framebuffer. `run()` sans argument est une surcharge, compatible avec GCC.
- Ordonnanceur EE : VBlank à 50 Hz quand SetGsCrt choisit le mode PAL.
- `PS2X_ENABLE_LTO` : LTO optionnelle (désactivée par `tools/project.py` sauf `--lto`), car `/GL` + `/LTCG` rendaient l’édition de liens de ~50 000 fonctions très longue.
- `sceCdReadClock` (EE) renvoie l’heure JST (UTC+9), comme l’horloge d’une console.
- ps2xIOP : `sceCdReadClock` (cdvdman:24), `GetSystemTimeLow` (thbase:43) et avertissement unique pour tout ordinal inconnu d’une bibliothèque intégrée, avec tests.
- `src/burnout_overrides.cpp` : 28 fonctions SDK liées au runtime par adresse ([SDK_FUNCTIONS.md](SDK_FUNCTIONS.md)) et profil ROM0 avec un ROMVER européen de 16 octets ; vérifiés par `tests/native/burnout_overrides_tests.cpp`.
- Mémoire invitée du runtime (`Kernel/Syscalls/Helpers/State.h`, `Helpers/Runtime.h`, `ps2_runtime.cpp`) : les pools RPC, TLS et boot mode sont relatifs à une base. Elle reste 0x01F00000, sauf si l’image ELF dépasse cette adresse (Burnout : .bss jusqu’à 0x01F9BB00, pile principale 0x01FF0000–0x02000000). Dans ce cas, `loadELF` place pools, tas du runtime (`guestMalloc`) et piles des rappels dans 0x00080000–0x00100000, sous l’image. Avant, le tas du runtime était vide et pools et piles écrasaient le .bss et la pile du jeu (cf. upstream #258).
- `SetupHeap`/`EndOfHeap` (`Syscalls/System.cpp`) : dans ce cas, le tas appartient au malloc du jeu. `SetupHeap` renvoie sa base ; avec une taille −1, `EndOfHeap` renvoie la pile du thread courant, comme le noyau EE. Avant, `EndOfHeap` valait 0x01F00000, sous la base 0x01F9BB00 : tout `sbrk` échouait et `RwEngineInit` aussi.
- Commandes SIF EE → IOP :
  - `ps2_stubs::sceSifSendCmd` lit les arguments 5 et 6 dans `$t0`/`$t1` (ABI EE), remplit l’en-tête du paquet comme le SDK, copie les données annexes en mémoire IOP et remet le paquet au gestionnaire IOP de son numéro de commande. Il renvoie un identifiant DMA. `ps2_syscalls::sceSifSendCmd` lui délègue : il lisait ces arguments sur la pile et copiait vers la RAM EE.
  - `PS2IopTransport::deliverSifCommand` et `IopSubsystem::deliverSifCommand` acheminent ce paquet.
  - ps2xIOP : sifcmd:8–11 (`sceSifSetCmdBuffer`, `sceSifSetSysCmdBuffer`, `sceSifAddCmdHandler`, `sceSifRemoveCmdHandler`) tiennent les tables de gestionnaires (table utilisateur en mémoire IOP, comme sifcmd). Un paquet reçu exécute `handler(paquet, harg)`.
- ps2xIOP, sifman:32 `sceSifSetDmaIntr` : même transfert que `sceSifSetDma`, puis `func(data)` en rappel planifié après le transfert. Avant, l’ordinal renvoyait 0 sans transfert, et le thread de lecture de GTFSCDVD dormait sans fin.
- Tests : livraison d’une commande SIF et `sceSifSetDmaIntr` dans `ps2_iop_emulator_tests` ; image ELF de type Burnout dans `ps2x_tests` (tas, piles de rappel, `SetupHeap(-1)` terminé à la pile).
- `tools/augment_function_map.py` et `project.py generate --augment` : ajoute à la carte Ghidra le code atteint seulement par pointeur.
- `tools/project.py configure` ajoute `-msse4.1` sous Linux/macOS x86-64, comme l’intégration continue Linux d’upstream.

Les appels matériels du runtime upstream restent expérimentaux. Aucune fonction du jeu n’est remplacée par `ret0`, `ret1`, une réussite fictive ou un `skip` pour faire passer un test de démarrage.

Le moteur choisit automatiquement 8 adaptateurs SIF reconnus malgré la liste `stubs=[]` de la configuration. Le rapport et l’en-tête généré permettent de les vérifier séparément ; les comptes de corps C++ ne doivent pas être confondus avec le nombre de fonctions réelles du jeu.
