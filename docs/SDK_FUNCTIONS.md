# Fonctions du SDK Sony identifiées dans SLES_546.27

L’ELF est sans symboles. Les fonctions ci-dessous ont été identifiées par analyse statique (désassemblage, pseudocode Ghidra, références de chaînes, SID RPC, appelants). `src/burnout_overrides.cpp` les relie aux implémentations du runtime ; `tests/native/burnout_overrides_tests.cpp` vérifie chaque liaison.

Règles appliquées :

1. Lier seulement une identité établie par des preuves : SID RPC et numéro de fonction, chaînes d’erreur ou noms de sémaphores référencés, marqueur de version de la bibliothèque, arité compatible avec l’API du SDK.
2. Le gestionnaire du runtime doit avoir le même ABI : sens et ordre des arguments, valeur de retour.
3. Lier ensemble tous les points d’entrée d’une bibliothèque qui parlent à l’IOP : une fois certains remplacés, l’état interne de la bibliothèque invitée (clients RPC, sémaphores) n’est plus tenu à jour.

Pourquoi lier : le runtime n’émule pas le matériel SIF. Une bibliothèque invitée qui attend un serveur RPC IOP boucle indéfiniment si personne ne le sert (lancements 1 et 2 de [BOOT.md](BOOT.md)).

> **Adresses des chaînes.** La colonne d’adresse de `local/analysis/SLES_546.27.strings.txt` est un offset de fichier, pas une VA. Segment 0 : VA = offset − 0x1000 + 0x100000. Segment 1 (`.rodata`/`.data`) : VA = offset − 0x306000 + 0x408080. Exemple : offset 0x32ADA0 « Libcdvd bind err %d CD_Init %d » = VA 0x42CE20.
>
> **Pseudocode Ghidra.** Pour 0x378480, 0x378558, 0x3783C0 et 0x3781A0, Ghidra masque la valeur de retour (lecture non cachée `| 0x20000000` du tampon de réponse). Lire le désassemblage.

## Marqueurs de version

| VA | Marqueur |
|---|---|
| 0x3D4CC0 | `PsIIlibcdvd 3000` |
| 0x3D6878 | `PsIIlibdbc  3020` |
| 0x3E0A40 | `PsIIlibpad2 3020` |
| 0x3E0A70 | `PsIIlibscf  3000` |
| 0x3E36C0 | `PsIIlibmc   3020` |

## Fonctions liées au runtime

| Adresse | Fonction | Gestionnaire | Preuve |
|---|---|---|---|
| 0x3AF8D8 | sceSifInitCmd | `sceSifInitCmd` | Garde d’initialisation unique 0x3E1F88 ; initialise les données de commande SIF (0x1F6F150…). Appelée en premier par 0x3B0020. |
| 0x3B0020 | sceSifInitRpc | `sceSifInitRpc` | Garde 0x3E1F8C ; données RPC à 0x1F70B80 ; enregistre les gestionnaires de commandes SIF 0x80000008/9/A/C ; teste `sceSifGetReg(0x80000002)`. Boucle du lancement 1 à 0x3B0178. |
| 0x3B0668 | sceSifBindRpc | `sceSifBindRpc` | Référence « SceSifrpcBind » (VA 0x431CE8) à 0x3B06D4. |
| 0x3B0848 | sceSifCallRpc | `sceSifCallRpc` (liste des appels système) | Référence « SceSifrpcCall » (VA 0x431CF8) à 0x3B0990. |
| 0x3B2320 | sceSifSyncIop | `sceSifSyncIop` | Appelée en boucle après le reboot IOP dans 0x2156B8. |
| 0x3B2370 | sceSifRebootIop | `sceSifRebootIop` | Préfixe « rom0:UDNL » (VA 0x431FD0) puis appelle sceSifResetIop 0x3B21C8. Argument : `cdrom0:\IOP\IOPRP300.IMG;1` (VA 0x419348). |
| 0x3B2180 | sceSifLoadModule | `sceSifLoadModule` (liste des appels système) | Enveloppe de `_SifLoadModule(path, argc, argv, &res, 0)` ; client loadfile, SID 0x80000006 ; données 0x1F725C0–0x1F727E8. |
| 0x3B1B48 | sceSifInitIopHeap | `sceSifInitIopHeap` | Client iopheap, SID 0x80000003. Appelée par 0x215798 avant le chargement des modules. |
| 0x3B1BD0 | sceSifAllocSysMemory | `sceSifAllocSysMemory` | Client iopheap, RPC 4, envoi `{size, mode, addr}` construit depuis `(mode, size, addr)` : appel type `(0, 0x40700, 0)`. Le gestionnaire lit la taille dans a1. Appelants : 0x1FB828, 0x29D608, 0x3629A8, 0x3692C0, 0x372160. |
| 0x3B1C50 | sceSifFreeIopHeap | `sceSifFreeIopHeap` | RPC 2, envoi `{addr}`. Appelants : 0x29DDC8, 0x369110. |
| 0x3B1468 | sceOpen | `sceOpen` | Client fileio, SID 0x80000001 ; référence « SceStdioOpenSema » à 0x3B15B8. Appelée avec `(path, 1)`. Les trois fonctions fileio n’ont que deux appelants, 0x379908 et 0x38AFE0. |
| 0x3B16F8 | sceClose | `sceClose` | Référence « SceStdioCloseSema » à 0x3B1778. |
| 0x3B1878 | sceRead | `sceRead` | Référence « SceStdioReadSema » à 0x3B1910. Appelée avec `(fd, buf, n)`. |
| 0x377CA0 | sceCdInit | `sceCdInit` | Lie le SID 0x80000592 dans 0x1F65BE8 jusqu’à `server != 0` ; chaînes « Libcdvd bind err %d CD_Init %d » et « Libcdvd Exit » (mode 5). Appelée deux fois avec 0 par 0x2156B8, retour ignoré. Boucle du lancement 2 à 0x377E28. |
| 0x3781A0 | sceCdDiskReady | `sceCdDiskReady` | « NEW DiskReady Call » ; SID 0x8000059C ; après 17 échecs, repli sur 0x377F88. |
| 0x377F88 | sceCdDiskReady (ancien protocole, interne) | `sceCdDiskReady` | « OLD DiskReady Call » ; SID 0x8000059A ; seul appelant 0x378360. |
| 0x378458 | sceCdGetDiskType | `sceCdGetDiskType` | Enveloppe de 0x3783C0 qui transforme −1 en 0. L’appelant 0x215730 compare à 0x12 (PS2 CD) et 0x14 (PS2 DVD). Le runtime renvoie 0x14 : l’ISO (4 660 166 656 octets) est un DVD simple couche. |
| 0x3783C0 | GetDiskType, cœur interne | `sceCdGetDiskType` | S-cmd 3 sur le client 0x3D6840 ; seul appelant 0x378460. |
| 0x378480 | sceCdMmode | `sceCdMmode` | S-cmd 0x22 (table initiale `{0x22, 4, 4}` à 0x3D6868, jamais écrite) ; appelée avec 2 (DVD) ou 1 (CD). |
| 0x378558 | sceCdReadClock | `sceCdReadClock` | S-cmd 1, réponse de 16 octets, copie 8 octets depuis rbuf+4. Appelants 0x1E6F18 et 0x1E6F98 : décodage BCD après conversion libscf (0x38B568 = JST → GMT, −540 min). Le runtime renvoie donc l’heure UTC+9, comme l’horloge d’une console. |
| 0x377AD8 | sceCdSyncS | `sceCdSyncS` | « S cmd wait » ; interroge sceSifCheckStatRpc (0x3B0A48) sur le client S-cmd. |

libcdvd n’est liée que partiellement dans l’exécutable : 13 fonctions entre 0x377870 et 0x378614. Les chaînes de sceCdSearchFile, sceCdRead, des commandes N, du thread de rappel et de sceCdSt* existent dans `.rodata`, mais aucun code ne les référence et aucun bind n’existe pour les SID 0x80000595 et 0x80000597. Le jeu lit le disque par le module Criterion GTFSCDVD.IRX, et non par la libcdvd EE.

Les variables internes de libcdvd (0x3D4CD0–0x3D4D20, 0x3D5EC0, 0x3D6300, 0x3D6740, 0x3D6840–0x3D6880, 0x1F659C0–0x1F659E0, 0x1F65BE0–0x1F65C60) ne sont lues par aucun code hors de la bibliothèque. Un balayage de tous les `jal`/`j` et des mots de données ne trouve que 8 appels externes, tous vers les 5 points d’entrée publics.

## Fonctions identifiées, non liées

| Adresse | Identité | Statut |
|---|---|---|
| 0x3B1E70 | Remise à zéro du client loadfile | EE seulement, sans RPC. Appelée par 0x2156B8. |
| 0x3B1430 | Remise à zéro du client fileio | EE seulement, sans RPC. Appelée par 0x2156B8. |
| 0x3AFBA8 / 0x3AFC20 | sceSifAddCmdHandler / sceSifRemoveCmdHandler | Vérifiées au désassemblage ; restent invitées. |
| 0x3AFDA8 | sceSifSendCmd | Enveloppe de 0x3AFC70. |
| 0x3B0A48 | sceSifCheckStatRpc | Vérifiée au désassemblage. |
| 0x3AFF70 | sceSifWriteBackDCache | Déduite. |
| 0x3B2960 / 0x3B29B0 | DI / EI | Déduites. |
| 0x3AF780 | scePrintf | Déduite. |
| 0x3ADDF0 | DelayThread | Déduite. |
| 0x377870, 0x377958, 0x377A50, 0x3779F8, 0x377B48 | Sémaphores, sortie, gestionnaire d’extinction et verrou S-cmd de libcdvd | Inaccessibles une fois les points d’entrée publics liés. |
| 0x38AFE0 | Lecture de 14 octets de `rom0:ROMVER` vers 0x3E0A88 (libscf) | Passe par fileio, désormais lié. |
| 0x379908 | Lecture de `rom0:ROMVER` jusqu’au NUL ; date des 9 octets précédents comparée à 20010608 | Exige un ROMVER de 16 octets terminé par `\n\0` : profil ROM0 de `src/burnout_overrides.cpp`. |
| 0x396A78 | Recherche de module IOP par nom (appelée par 0x395A30 pour sio2man, mcman, mcserv, multitap_manager) | libmc/libmtap ; à examiner. |

## À identifier

- libdbc/libpad2 (marqueurs ci-dessus) : client du service DBCMAN (SID 0x80001300), chargé physiquement par l’IOP.
- libmc 3020 : client MCSERV (SID 0x80000400).
- Client GTFS de Criterion : `FUN_001E2278(0x561D18, 1, …, 0x419368, 3, 0x20)` lie le SID 0x475453 (« GTS ») à 0x1E23C8. Il est servi par GTFSCDVD.IRX exécuté dans l’IOP émulé ; il ne faut pas le lier tant que l’IRX fonctionne.
- Client audio RenderWare (RWA.IRX / B4ROUTE.IRX) : chaîne « EE  RwaRPCTransfer » à VA 0x42C9B0.
