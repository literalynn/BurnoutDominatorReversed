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
| 7 | + ISO réelle (Windows) | 124 ; machine d’état 0x207D08 à l’état 9 (0x29DA28 attend l’ouverture du flux `sound_generic.awd`) ; l’écran LOADING ne bouge plus | RWA reçoit et répond aux commandes SIF, mais son moteur est cadencé par les IRQ du SPU2, que l’IOP émulé ne générait pas (§8). |
| 8 | + SPU2 (§8) | 124 ; EE en boucle à 0x3B1DA0 (appelant 0x3B1D1C) | Tout `sound_generic.awd` est transféré (commandes 0x32/0x33). Puis 0x396A78 appelle 0x3B1EA8 (sceSifSearchModuleByName) : 0x3B1CE0 réessaie sans fin le bind du SID loadfile 0x80000006, qu’aucun serveur ne sert (loadfile est dans l’IOPRP300.IMG, non exécuté). |
| 9 | + sceSifSearchModuleByName (29 liaisons), IOP et timers avancés par lots | 124 ; boucle principale (0x208390, sous-état 7), état de démarrage 0x1C, écran LOADING à environ 93 % ; 25 à 50 images par seconde | `0x207D08` a terminé. La scène de chargement attend `TRACKS/EATRAX1.RWS` : blocage actuel (§9). Les lectures `FE/FEMAIN.BIN` et `TRACKS/US/S7_V1/STATIC.DAT` ont abouti. |
| 10 | + lectures CD au-delà d’un bloc alloué (§9) | 124 ; le chargement se termine (sous-état 5) puis le thread principal dort à 0x3ACF08 (`SleepThread` appelé par 0x1E7328), plus aucune image | 0x1E7328 attend une alarme (`SetAlarm(6, 0x1E7308, thread)`) qui ne se déclenche jamais : le correctif noyau de libkernel a remplacé les appels système d’alarme (§11). |
| 11 | + appels système remplacés par du code non recompilé servis par le runtime | 124 ; écran noir, EE en boucle à 0x38A2C8 | Les alarmes fonctionnent ; le jeu lance la vidéo d’introduction et son analyseur MPEG-2 attend des données de l’IPU (§12). |

Les lancements 4 à 6 ont été faits sous Linux à partir du paquet `tools/cloud_bundle.py`, sans l’ISO (`--disc` seul, image virtuelle). La machine à états de démarrage est `0x207D08`, état à 0x51B920 : 1 = modules IOP, 2 = police, 3 et 4 = `Data/Global%c.txd`, puis la suite.

Pendant les lancements 1 et 2, le compteur VBlank avance de 60 par seconde : le jeu n’a pas encore appelé SetGsCrt, qui fait passer le runtime à 50 Hz en PAL.

## 7. Blocages suivants

1. Lecture de `rom0:ROMVER` : **réglé** (liaisons fileio, profil ROM0 de 16 octets).
2. Chargement physique des 11 IRX : **réglé** (imports du §5).
3. Client GTFS (SID « GTS ») : **réglé** sur l’ISO réelle ; le jeu lit la police, `FE/FEMAIN.BIN` et `TRACKS/US/S7_V1/STATIC.DAT` (lancement 9).
4. Lecture de `TRACKS/EATRAX0.RWS` / `EATRAX1.RWS` : **réglé** (§9).
5. Alarmes EE remplacées par le correctif noyau de libkernel : **réglé** (§11).
6. Vidéo d’introduction (IPU) : **ouvert**, voir §12.
7. Manettes et carte mémoire : absence de modèle SIO2 (§4), **ouvert** ; cela bloquera dès que le jeu attendra une entrée.

## 8. SPU2 et audio RenderWare (RWA)

**Modèle SPU2** (`ps2xIOP/src/emulator/core/iop_spu2.cpp`, accès 16 bits depuis `iop_memory.cpp`).

- Registres à 0x1F900000 : voix de 0x10 octets (VOLL, VOLR, PITCH, ADSR1, ADSR2, ENVX, VOLX), adresses de voix SSA/LSAX/NAX, registres de cœur à +0x400 par cœur (PMON, NON, VMIXL/R, MMIX, ATTR, IRQA, KON, KOFF, TSA, DATA, ADMAS, ENDX, STATX), volumes maîtres à 0x760 + 0x28 par cœur, SPDIF_IRQINFO à 0x7C2.
- ATTR : 0x8000 active le cœur, 0x40 les IRQ, les bits 5:4 choisissent le transfert (1 PIO en écriture, 2 DMA en écriture, 3 DMA en lecture). STATX vaut 0 au repos, 0x400 pendant un transfert et 0x80 une fois terminé tant que le mode de transfert reste actif. libsd (exécuté physiquement) attend `(STATX & 0x7FF) == 0` : un STATX forcé à 0x80 au repos bloquait `sceSdInit`.
- DMA SPU : canal 4 (cœur 0, CHCR 0x1F8010C8, IRQ 0x24) et canal 8 (cœur 1, CHCR 0x1F801508, IRQ 0x28) ; CHCR 0x01000201 = démarrage RAM → SPU, BCR = (blocs << 16) | 16 mots. Une IRQ d’adresse (IRQA atteinte par une voix ou un transfert) lève l’IRQ IOP 9 et SPDIF_IRQINFO.
- Horloge : à chaque échantillon (48 kHz), un cœur activé écrit les tampons de sortie du mélangeur en RAM SPU2 (voix 1, voix 3, mélange gauche et droite, 0x200 demi-mots chacun ; pour le cœur 0, les demi-mots 0x400 à 0x5FF reçoivent la voix 1) et teste IRQA sur les deux cœurs. RWA place IRQA dans l’un de ces tampons (demi-mots 0x400 puis 0x500) pour recevoir une IRQ tous les 256 échantillons : c’est l’horloge de son moteur, sans laquelle l’ouverture d’un flux ne se termine jamais.
- Limites : pas de réverbération, de bruit, de modulation de hauteur, d’ADMA ni de balayages de volume ; interpolation linéaire ; aucune sortie vers une carte son de l’hôte.

**Protocole RWA** (commandes SIF, cf. aussi SDK_FUNCTIONS.md).

- EE → IOP : commande 0, type dans le mot 4 du paquet (0x49, 1, 2, 0x11, 0x13, 0x15, 0x4E, 0x30, 0x32, 5, 0x14 au démarrage). IOP → EE : commande 1, avec son propre type de réponse (observé : 1 → 9, 2 → 0xA, 0x11 → 0x1A avec l’adresse IOP 0x12C030, 0x13 → 0x1B avec 0xC25B0 et 0x100000, 0x15 → 0x1C avec 0x5A850, 0x30 → 0x31, 0x32 → 0x33, 0x4E → 0x4F, 5 → 0xD, 0x14 → 0x14).
- 0x32 : requête de tranche de flux ; le descripteur de 0x70 octets part vers l’IOP 0x12C030. La réponse 0x33 porte la fonction de fin 0x3721C0, qui efface l’indicateur « en attente » (0x10) du descripteur et appelle le rappel de l’emplacement de flux. 0x372110 attend ce bit.
- EE : gestionnaire de la commande 1 à 0x3719D8 (file de 8 entrées à 0x1F64740, `iSignalSema` du sémaphore stocké en 0x432D78), thread 2 (entrée 0x362F50). IOP : gestionnaire de la commande 0 à RWA+0x3D70 (file de 8 entrées et SignalSema), thread T11 (RWA+0x4044, 0x38B0), transferts SPU par 0x4568 puis 0xA44C (liste à 0xC300 + 8 × indice), pompes de canal T12 et T13, threads d’ISR de priorité 9.
- Régime établi : à chaque image, commande 0x14 (0x680 octets vers l’IOP 0x5A850, avec un compteur croissant) et réponse 0x14.

## 9. Lecture CD refusée (lancement 9, réglé au lancement 10)

La machine d’état de 0x207D08 est terminée (état 0x1C à 0x51B920) et `main` (0x21B3F8) boucle sur 0x208390, dont le sous-état à 0x51B950 reste à 7 : tant que 0x208E50 ne renvoie rien, la scène de chargement (objet 0x4C1B38, état 8 à +0x1C ; sous-objet 0x1CEDB00, état 7) attend la requête de fichier `tracks/eatrax1.rws` (objet 0x1CF55C0, état de poignée 2 à +0x24C).

Observé :

- `BDR_TRACE_CDVD=1` : environ 560 `sceCdRead` par seconde, toujours `lsn=2007487` (début de `TRACKS/EATRAX1.RWS`), 4 secteurs, vers l’IOP 0x12C210 (tampon de RWA).
- `BDR_IOP_PROFILE=1` : près de 100 % des instructions IOP sont dans GTFSCDVD.IRX, 0x1110–0x1150 et les stubs cdvdman 0x2168 (`sceCdRead`) et 0x2180/0x2188 : c’est la boucle interne de 0x10CC, `do { sceCdDiskReady(1) ; … ; sceCdRead(…) } while (retour == 0)`. `sceCdRead` renvoie donc 0 à chaque fois et la boucle ne laisse pas le temps aux autres threads IOP, dont la requête RWA 0x32 reste sans réponse 0x33.

Cause, vérifiée en journalisant le refus : le jeu lit 4 secteurs (0x2000 octets) au début d’un bloc de 0x1840 octets alloué à 0x12C210 ; les 0x7C0 derniers octets tombent dans de la mémoire libre (premier octet libre 0x12DA50). `readSectors` (`iop_cdvd.cpp`, upstream) refusait toute lecture dont la destination n’était pas entièrement allouée ou écrite (`IopMemory::ownsRamRange`) et renvoyait 0, que GTFSCDVD réessayait sans fin. Le DMA CD de la console écrit les secteurs entiers là où on le lui demande : `readSectors` ne vérifie plus que les bornes de la RAM IOP et signale le débordement (au plus 4 fois). La même lecture vise `EATRAX0.RWS` (LSN 1911758) ou `EATRAX1.RWS` selon le morceau tiré au hasard.

## 10. Outils d’observation

Variables d’environnement, à poser avant `python tools/project.py run …` (toutes désactivées par défaut) :

| Variable | Effet |
|---|---|
| `BDR_STATUS_DETAIL=1` | à chaque ligne `[status]` : threads EE (entrée, pc, ra, priorité, attente), sémaphores, drapeaux, threads et objets du noyau IOP, journal de ses opérations |
| `BDR_STATUS_WATCH=0xA,0xB+N,@0xC+OFF` | mots de la mémoire invitée ajoutés à l’état : un mot, N mots, ou le mot situé à OFF du pointeur stocké en 0xC |
| `BDR_TRACE_SIF=1` | commandes SIF dans les deux sens (1 200 premières, 80 pour les commandes 4 et 5 de GTFS) |
| `BDR_TRACE_CDVD=1` | chaque `sceCdRead` (LBA, secteurs, destination) ; le répertoire ISO9660 de l’ISO permet de retrouver le fichier |
| `BDR_IOP_PROFILE=1` | où l’IOP passe ses instructions (par tranches de 16 octets, module + offset) |
| `BDR_TRACE_IOPCALLS`, `BDR_TRACE_IOPHW`, `PS2X_IOP_TRACE_SPU`, `BDR_TRACE_GS` | appels de fonctions IOP choisies, écritures matérielles IOP, SPU2, GS |
| `BDR_TRACE_KERNEL=1` | `SetSyscall` (numéro, gestionnaire, code recompilé ou non), `SetAlarm`, `CancelAlarm` et expiration des alarmes |
| `BDR_PROFILE=1` | échantillonneur du thread invité (EE, IOP et GS y tournent) ; affiche les fonctions les plus fréquentes à la fin du lancement. Windows seulement ; il faut le PDB, produit par `/DEBUG` (`cmake/Burnout.cmake`) |

Avec ces outils, le profil a montré que le jeu tournait à 7 % du temps réel parce que l’IOP et les timers étaient avancés tous les 8 cycles EE ; ils le sont maintenant par lots de 2048 cycles (CHANGES.md).

## 11. Correctif noyau de libkernel (lancement 10)

Au démarrage, libkernel installe deux correctifs du noyau EE, comme sur console (trace `BDR_TRACE_KERNEL=1`) :

- 0x3B2AC0 puis 0x3B2E78 (alarmes) : `SetSyscall(0x83, 0x3B2A68)` (recherche d’adresse) et `SetSyscall(0x5A, …)` (copie), copie de code en mémoire noyau, `SetSyscall(0x5B, 0x80076000)`, puis pour 0xFC, 0xFE, 0xFD, 0xFF, 0x12C et 0x8 : `SetSyscall(n, syscall 0x5B(n))`.
- 0x3B24F0 (drapeaux d’événement) : `SetSyscall(0x5A, 0x3B2498)`, copie de 0x330 octets de 0x3E2038 vers 0x80075000, `SetSyscall(0x5B, 0x80075000)`, `SetSyscall(0x54, 0x3B2840)`, puis 0x55 à 0x59.

Le code copié en mémoire noyau n’est pas recompilé : le runtime ne peut pas l’exécuter. Avant, un appel système dont le gestionnaire remplacé n’était pas du code recompilé renvoyait −1 ; `syscall 0x5B` renvoyait donc −1, et `SetAlarm`, `iSetAlarm`, `ReleaseAlarm`, `iReleaseAlarm` ainsi que les drapeaux d’événement 0x55–0x59 échouaient tous. `sleep(6)` (0x1E7328 : `SetAlarm(6, 0x1E7308, thread)` puis `SleepThread`) ne se réveillait jamais. Désormais, un tel appel est servi par l’implémentation du runtime (`dispatchSyscallOverride`, `Syscalls/System.cpp`). `SetSyscall` écrit toujours l’entrée dans la table noyau visible par le jeu, que le correctif utilise aussi pour écrire des mots du noyau par des index signés. Test : « a syscall override that is not recompiled code falls back to the builtin » (`ps2x_tests`).

## 12. Vidéo d'introduction (lancement 11)

Après le chargement, l’écran devient noir et le thread principal tourne dans 0x38A2C8 : l’analyseur de flux MPEG-2 de la bibliothèque vidéo du jeu (codes de début 0x1B3 séquence, 0x1B8 GOP, 0x100 image, 0x1B7 fin) cherche un code de début (`0x388030(…, 0x18)` jusqu’à 1, puis `0x3880C0(…, 8)`), en lisant les bits du décodeur IPU. Les compteurs DMA et GIF ne bougent plus. Il faut vérifier comment le flux arrive à l’IPU (DMA canal 4 vers l’IPU, commandes BCLR/FDEC/IDEC/BDEC) et ce que l’émulation IPU du runtime renvoie.

## 13. IPU et branchements signés (lancements 12 et 13)

Un modèle IPU est maintenant raccordé aux registres et FIFO de `PS2Memory`, ainsi qu'aux DMA 3/4. Les tests vérifient notamment la reprise d'une commande après apport de données, BDEC vers RAW16, la conversion CSC et les chaînes source. BDEC attend la sortie DMA, puis détecte le code de début suivant après les octets de bourrage. Les échantillons intra sont saturés à 0–255 ; les blocs inter conservent leurs résidus signés. IDEC reste non implémentée. Aucun code vidéo invité n'est remplacé ou ignoré.

Lancement 12 (`run12-codex-ipu.log`, 60 s, code 124) : l'écran de chargement se termine, puis l'écran reste noir. Le PC est désormais dans `0x387DC8` (attente de CMD), notamment `0x387EF4`, et le jeu redémarre continuellement le DMA d'entrée. En fin d'essai : 739 491 démarrages DMA et 426 paquets GIF, sans fonction manquante.

Lancement 13 (`run13-codex-ipu-trace.log`, 24 s, `BDR_TRACE_IPU=1`) : le flux arrive bien. Trois FDEC de 8 bits donnent successivement `0x0001B328`, `0x01B32801`, puis `0xB32801E0`. Sur la dernière, BUSY est effacé (`CTRL=0x00800008`) et le FIFO contient 8 qwords, avec QWC d'entrée `0xFF7`. Le jeu reste pourtant dans l'attente : les `BGEZL` à `0x387E08` et `0x387EF8` sont traduits avec `GPR_S32`. Cela teste le bit 31 des données, au lieu du bit 63 qui porte BUSY dans le registre CMD lu par `LD`.

Le recompileur (`control_flow_emitter.cpp`) utilise maintenant `GPR_S64` pour BLEZ/BGTZ/BLTZ/BGEZ et toutes leurs variantes likely/link. Les douze variantes sont couvertes par un test ; les 452 tests runtime/recompileur passent sous MSVC. La comparaison sur le registre 64 bits est aussi celle de l'[interpréteur R5900 de PCSX2](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/Interpreter.cpp). Il faut régénérer les fichiers C++ et recompiler le jeu pour appliquer cette correction.

`BDR_TRACE_IPU=1` trace les 160 premières commandes et démarrages DMA, puis un sur 10 000 : commande, DATA, CTRL, BP, TOP et registres DMA. Désactivée par défaut, la trace reste dans les journaux locaux ignorés par Git.

## 14. Écran PROFILE et carte mémoire (lancements 14 à 18)

Après régénération et recompilation du correctif 64 bits, le lancement 14 (`run14-codex-branch64.log`, 60 s, code 124) franchit l'ancien blocage vidéo. La trace montre plus de 440 000 commandes IPU, dont BDEC ; le jeu affiche l'écran **PROFILE**, avec le message de vérification de carte mémoire. Les captures sont dans `local/diagnostics/run14-codex-branch64/`. Ce constat ne valide pas la fidélité complète des vidéos et du son.

Lancement 15 (`run15-codex-profile-card.log`, 24 s) : l'EE attend dans `sceSifCallRpc` (0x3B0848, appelant 0x3B594C). Le thread SIO2MAN attend le drapeau 0x2000, posé par son gestionnaire d'IRQ 17 à SIO2MAN+0x584. Aucune émulation SIO2 ne produisait cette interruption. MCMAN effectue des transferts par SIO2MAN, avec des délais et sémaphores.

Un transport PIO SIO2 pour ports déconnectés a été ajouté : octets `0xFF`, RECV1 indiquant l'absence de périphérique, IRQ 17 à CTRL START et acquittement INTR. Le comportement de référence est documenté par [Sio2 de PCSX2](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/SIO/Sio2.cpp) et [ses états de ports](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/SIO/SioTypes.h). Lancement 16 (`run16-codex-sio2.log`, 25 s) : SIO2MAN reçoit bien l'interruption et revient à son attente de commande ; la vérification reste bloquée. Les cartes et manettes ne sont pas simulées comme présentes.

La fonction RPC physique s'exécutait par `callFunction` hors d'un thread IOP (T0). Ses attentes ne suspendaient donc pas le serveur. Elle s'exécute maintenant dans un thread temporaire, à pile réutilisable, pendant que l'ordonnanceur fait avancer les autres threads et périphériques. Lancement 17 (`run17-codex-rpc-thread.log`) : cette correction révèle que MCMAN attend son alarme de 100 µs avant de reprendre WaitSema. `thbase:35` SetAlarm retournait jusque-là sans enregistrer de rappel ; le RPC atteint sa limite et produit une erreur explicite.

Les alarmes IOP 35–38 sont maintenant prises en charge par le distributeur d'imports de l'émulateur (identités et ABI dans [thbase.h de PS2SDK](https://github.com/ps2dev/ps2sdk/blob/master/iop/system/threadman/include/thbase.h)). Un module synthétique vérifie DelayThread puis SetAlarm/SleepThread/iWakeupThread et 128 appels RPC successifs, avec une vraie reprise dans le même thread et réutilisation des piles.

Lancement 18 (`run18-codex-iop-alarm.log`, 35 s, code 124) : aucune erreur d'exécution et aucune fonction manquante ; l'écran PROFILE reste sur la vérification de carte mémoire. Le thread de commande SIO2MAN revient à l'attente 0x4155, les serveurs RPC passent leurs attentes de drapeaux, et l'EE reste dans 0x3B0848 pendant le rappel libmc 0x3B57C8. Il reste à vérifier la réponse GetInfo de MCSERV (SID 0x80000400, fonction 1), les données transférées vers l'EE et la fin du rappel libmc, avant d'attribuer cette attente au seul périphérique absent.

Reprise ciblée :

```powershell
$env:BDR_STATUS_DETAIL = '1'
$env:BDR_STATUS_WATCH = '0x1F776C0+4,0x1F76600+8,0x3E36D0+3,0x1F76128+3'
python tools/project.py run --headless --seconds 35 --status-ms 5000 --log run19-libmc.log --tail 120
```

Limites restantes : SIO2 DMA et dmacman, carte mémoire persistante, protocole DualShock 2 alimenté par les entrées du PC, menu et courses. Le transport PIO actuel représente uniquement des ports déconnectés ; aucun résultat de jeu n'est forcé.

