# Amorçage EE observé dans le pseudocode

L'entrée PAL est `0x00100008`, dans `local/decompiled/functions/00100008_entry.pseudocode.c`. La décompilation conserve des types inférés tels que `undefined4` et des noms automatiques. Les valeurs associées aux différentes parties du registre zéro sont des artefacts de représentation du décompilateur ; consulter les instructions R5900 avant de modifier une opération mémoire.

Le pseudocode montre cette séquence :

1. `SYNC(0x10)` et initialisation à zéro de la zone `0x00432B00` à `0x01F9BB00`, par écritures alignées de 16 octets puis gestion des bords.
2. Les appels système `0x3C` et `0x3D`. Le répartiteur actuel de PS2Recomp associe ces identifiants à `SetupThread` et `SetupHeap`.
3. Appel à `0x003B2BD0`, qui appelle neuf sous-routines d'initialisation. Leur signification complète reste à identifier.
4. `FlushCache(0)` et `EI()`.
5. Appel à `0x0021B3F8` avec les données globales à `0x00433080` et `0x00433084`.
6. Appel à `0x00322630` après son retour ; cette routine appelle `0x00321E18`, `0x003A2A50`, puis un thunk à `0x003B2DE0`.

Le code à `0x0021B3F8` prépare notamment une chaîne de chemin, utilise `GetThreadId`, puis appelle `ChangeThreadPriority(..., 10)`. Il répète une séquence jusqu'à ce que `FUN_00207D08(0x00491540)` retourne une valeur non nulle, puis attend que l'octet global `DAT_0051B910` devienne non nul. Ces adresses servent de points d'observation pour distinguer une progression de l'initialisation d'une attente. Elles ne justifient pas de forcer ces résultats ni de remplacer les boucles par des retours fictifs.

Fichiers utiles :

- `00100008_entry.pseudocode.c`
- `003B2BD0_FUN_003b2bd0.pseudocode.c`
- `0021B3F8_FUN_0021b3f8.pseudocode.c`
- `00322630_FUN_00322630.pseudocode.c`

Les plages de corps exactes et tentatives d'export figurent dans `local/decompiled/manifest.json`. Aucun nom du projet Ghidra ni octet de l'ELF original n'a été modifié lors de cet export.
