# Démarrage de SLES_546.27

Séquence établie par analyse statique (désassemblage, pseudocode Ghidra, graphe d’appels), puis confrontée aux lancements bornés du binaire recompilé. Les fonctions SDK citées sont documentées dans [SDK_FUNCTIONS.md](SDK_FUNCTIONS.md).

## 1. crt0 (`entry` 0x100008)

| Adresse | Action |
|---|---|
| 0x100008–0x100110 | Mise à zéro des GPR/FPR, `sync`. |
| 0x10011C–0x100194 | Mise à zéro du BSS 0x432B00–0x1F9BB00. |
| 0x1001C8 | `syscall 0x3C` SetupThread(gp = 0x43A4F0, pile 0x1FF0000, taille 0x10000, args 0x433080, racine 0x100220). |
| 0x1001E4 | `syscall 0x3D` SetupHeap(0x1F9BB00, −1). |
| 0x1001E8 | `jal 0x3B2BD0` : initialisation de libkernel. |
| 0x1001F0 | FlushCache(0), puis `ei`. |
| 0x100208 | `main(argc = *0x433080, argv = 0x433084)` = 0x21B3F8. |
| 0x100210 | `j 0x322630` : exit(main()). |

## 2. Initialisation (main → 0x207820)

`0x207820` initialise l’état global du jeu, puis appelle notamment :

1. `0x2156B8` : redémarrage de l’IOP (§3) ;
2. `0x215928`, `0x215918`, `0x1E7018`, `0x1E7050`, `0x2160C8(0x561E78)`, `0x214128`, `0x1E6BE0` ;
3. `0x1E68A0(0x3C)` et `0x1E6668(0x41855555, …)` si l’octet 0x432E98 vaut 0, sinon `0x1E68A0(0x32)` et `0x1E6668(0x41A00000, …)` : 60 ou 50 images par seconde, donc NTSC ou PAL (déduit des constantes) ;
4. plus loin, `0x1B48F8` (à 0x207A44) → `0x1E6EB8` → `0x38B0F8` → `0x38B050` → `0x38AFE0` : lecture de `rom0:ROMVER` par fileio.

## 3. Redémarrage de l’IOP (0x2156B8)

```
sceSifInitRpc(0)                                   0x3B0020
sceCdInit(0)                                       0x377CA0
do r = sceSifRebootIop("cdrom0:\IOP\IOPRP300.IMG;1") while (r == 0)   0x3B2370
do r = sceSifSyncIop() while (r == 0)              0x3B2320
sceSifInitRpc(0)
0x3B1E70()   remise à zéro du client loadfile (EE seulement)
0x3B1430()   remise à zéro du client fileio (EE seulement)
sceCdInit(0); sceCdGetDiskType(); sceCdMmode(2 si DVD, 1 si CD); sceCdDiskReady(0)
```

La commande réellement envoyée est `rom0:UDNL cdrom0:\IOP\IOPRP300.IMG;1`. Le runtime traite le reboot comme une remise à zéro de l’IOP émulé ; les modules d’IOPRP300.IMG (CDVDMAN, CDVDFSV, FILEIO, LOADFILE, MODLOAD, SIFCMD…) ne sont pas exécutés. Leurs services EE sont fournis par les liaisons de `src/burnout_overrides.cpp`.

## 4. Chargement des modules IOP (0x215798)

`sceSifInitIopHeap()` (0x3B1B48), puis 11 itérations sur la table de pointeurs 0x3C5268, chacune `0x215688(path, 0, 0)` → `sceSifLoadModule(path, 0, NULL)`. Un retour négatif interrompt la boucle.

| # | Module | Rôle (déduit) |
|---|---|---|
| 1 | `cdrom0:\IOP\SIO2MAN.IRX;1` (VA 0x419230) | Gestionnaire SIO2 |
| 2 | SIO2D.IRX | Pilote SIO2 |
| 3 | DBCMAN.IRX | Gestionnaire de manettes libdbc (SID 0x80001300) |
| 4, 5 | DS2O.IRX (la même entrée 0x419288, chargée deux fois) | Manette DualShock 2 |
| 6 | MCMAN.IRX | Carte mémoire |
| 7 | MCSERV.IRX | Serveur RPC de carte mémoire (SID 0x80000400) |
| 8 | LIBSD.IRX | Son (SPU2) |
| 9 | RWA.IRX | RenderWare Audio |
| 10 | B4ROUTE.IRX | Routage audio de Criterion |
| 11 | GTFSCDVD.IRX | Système de fichiers GTFS de Criterion |

Non chargés : PADMAN.IRX et MC2_S1.IRX (présents sur le disque, aucune référence). MTAPMAN n’est pas sur le disque, mais `0x395A30` cherche un module « multitap_manager » par son nom.

Ensuite : `0x1E2278(0x561D18, 1, …, 0x419368, 3, 0x20)` initialise le client GTFS, qui lie le SID 0x475453 (« GTS ») à 0x1E23C8. Puis `0x2159F0` → `0x1E60E0` initialise les manettes.

Dans le runtime, un module `cdrom0:` est d’abord chargé physiquement dans l’interpréteur R3000 ; le service HLE du même nom (DBCMAN, MCSERV, LIBSD) n’est utilisé que si ce chargement échoue. L’IOP émulé n’a pas de modèle matériel SIO2 : SIO2MAN, DBCMAN, DS2O et MCMAN chargés physiquement ne verront donc probablement ni manette ni carte mémoire (déduit, à vérifier au premier lancement qui atteint cette étape).

## 5. Couverture des imports des IRX par le noyau IOP virtuel

Classement statique des tables d’import des IRX du disque par rapport aux bibliothèques intégrées de `ps2xIOP/src/emulator`. Seuls les manques sont listés.

| Module | Import manquant | Effet avant correction | État |
|---|---|---|---|
| MCMAN, MC2_S1 | cdvdman:24 sceCdReadClock | avertissement, v0 = 0 | **Implémenté** (heure JST en BCD) |
| RWA | thbase:43 GetSystemTimeLow | retour silencieux, v0 inchangé | **Implémenté** |
| SIO2D, MCMAN | secrman:6 SecrAuthCard | avertissement, v0 = 0 (échec d’authentification) | Laissé tel quel : sans modèle SIO2, aucune carte ne peut répondre |
| RWA | ioman:4–8 open/close/read/write/lseek | avertissement, v0 = 0 | Ouvert. Un appel à 0x7F90 ouvre avec le mode 0x602 (écriture, création, troncature) : sortie de débogage probable |
| RWA, GTFSCDVD | sifcmd:8, 10, 11 (tables de gestionnaires de commandes) | v0 = 0, aucun gestionnaire enregistré : les commandes de l’EE n’arrivaient jamais | **Implémenté** |
| GTFSCDVD, DBCMAN, PADMAN | sifman:32 sceSifSetDmaIntr | v0 = 0, ni transfert ni rappel : le thread de lecture GTFS dort indéfiniment | **Implémenté** |

Les ordinaux inconnus des bibliothèques intégrées sans repli (thbase, thsemap, thevent, sifcmd, sifman, sysclib) retournaient silencieusement. Ils produisent maintenant un avertissement `[IOP] unhandled built-in import`, une fois par import.

## 6. Lancements bornés

Commande : `python tools/project.py run --headless --seconds 20 --status-ms 1000`. Code 124 = délai atteint, 3 = fonction absente.

| # | Liaisons | Résultat | Cause |
|---|---|---|---|
| 1 | aucune | 124 ; PC fixe 0x3B0178, RA 0x2156D4 ; IOP : 0 instruction | sceSifInitRpc invité attend la réponse de l’IOP par SIF matériel, non émulé. |
| 2 | libsif (6) | 124 ; PC fixe 0x377E28, RA 0x377D74 | sceCdInit invité réessaie le bind du SID 0x80000592 : aucun serveur CDVDFSV. |
| 3 | + loadfile, iopheap, libcdvd (15) | 3 ; JALR de 0x1E3910 vers 0x1E5020 | 0x1E5020 n’est atteint que par pointeur (a0 = 0x535BC4, dont le premier mot vaut 0x4027E0) : absent de la carte Ghidra, entre FUN_001E4EE8 (fin 0x1E501C) et FUN_001E5030. |
| — | correction | `tools/augment_function_map.py` : +2 191 points d’entrée (1 684 dans des trous, 507 dans des fonctions), 10 rejetés. Régénération : 51 161 fichiers C++, 0 instruction non traitée, 4 420 avertissements, 0 erreur ; `ptr_001e5020_0x1e5020.cpp` existe. Recompilation complète réussie (10 min 22 s). | |
| 4 | + fileio, libcdvd internes (21), ROMVER PAL | Linux (cloud, sans ISO), 124 ; JALR vers 0 à 0x32C054 | `RwEngineInit` (0x32CBF0) échoue : `EndOfHeap` vaut 0x1F00000, sous la base du tas 0x1F9BB00. Tout `sbrk` (0x3AD6B0) échoue, RenderWare écrit par un pointeur nul et sa table de fonctions (0x1F578E4) reste vide. |
| 5 | + `SetupHeap(-1)` terminé à la pile du thread ; mémoire du runtime sous l’image | 124 ; boucle à 0x372BC8 | `RwEngineInit` réussit et les 11 IRX se chargent. L’EE attend la réponse 0x12 de RWA.IRX : les commandes SIF de l’EE n’atteignaient pas les gestionnaires des IRX. |
| 6 | + libsif commandes (28), sifcmd:8–11 et sifman:32 côté IOP | 124 ; boucle de démarrage 0x21B5B0 à l’état 4 | RWA répond, GTFS lit `Language/Fonts/dirtyEra.bin` (RPC 1, 3, 5, DMA puis commande 4). L’état 3 demande `Data/GlobalE.txd` (808 Ko), absent du paquet cloud de 30 Mo : seul un lancement avec l’ISO peut aller plus loin. Une image GIF par boucle. |

Les lancements 4 à 6 ont été faits sous Linux à partir du paquet `tools/cloud_bundle.py`, sans l’ISO (`--disc` seul, image virtuelle). La machine à états de démarrage est `0x207D08`, état à 0x51B920 : 1 = modules IOP, 2 = police, 3 et 4 = `Data/Global%c.txd`, puis la suite.

Pendant les lancements 1 et 2, le compteur VBlank avance de 60 par seconde : le jeu n’a pas encore appelé SetGsCrt, qui fait passer le runtime à 50 Hz en PAL.

## 7. Blocages suivants prévisibles

1. Lecture de `rom0:ROMVER` (§2, point 4) : sans liaison fileio, boucle sur le bind du SID 0x80000001. Corrigé par les liaisons fileio et un profil ROM0 de 16 octets.
2. Chargement physique des 11 IRX : imports ci-dessus ; DS2O chargé deux fois.
3. Client GTFS (SID « GTS ») : fonctionne avec GTFSCDVD.IRX dans l’IOP émulé (lancement 6). Reste à vérifier sur l’ISO réelle, avec les vrais numéros de secteurs.
4. Manettes et carte mémoire : absence de modèle SIO2 (§4).
