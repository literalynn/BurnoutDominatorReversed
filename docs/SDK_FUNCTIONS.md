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
| 0x3AFBA8 | sceSifAddCmdHandler | `sceSifAddCmdHandler` | `(cid, handler, data)` : un cid négatif indexe la table système (+0xC des données 0x1F6F158), sinon la table utilisateur (+0x14) ; entrée de 12 octets `{handler, data, gp}`. Appelants : RWA 0x371900 (cid 1), GTFS 0x1E2390/0x1E23A4 (cid 4 et 5), 0x203228 (cid 6), libcdvd 0x377A88, fileio 0x3B11E8/0x3B1200, sceSifInitRpc invité. |
| 0x3AFC20 | sceSifRemoveCmdHandler | `sceSifRemoveCmdHandler` | Même indexation, efface `handler`. Appelants : RWA 0x371950, libcdvd 0x3779D0. |
| 0x3AFB90 | sceSifSetCmdBuffer | `sceSifSetCmdBuffer` | `(table, count)` : écrit la table utilisateur (+0x14) et son nombre (+0x18), renvoie l’ancienne. Seul appelant : RWA 0x3718EC. |
| 0x3AFDA8 | sceSifSendCmd | `sceSifSendCmd` (liste des appels système, qui délègue à la version de `ps2_stubs`) | `(cid, packet, psize, src, dest, size)`, arguments 5 et 6 dans `$t0`/`$t1` ; appelle le cœur 0x3AFC70 avec le mode 0. RWA envoie la commande 0 à 0x371548, 0x371600, 0x371810 ; GTFS à 0x1E2DB4. |
| 0x3AFDE8 | isceSifSendCmd | `sceSifSendCmd` | Même enveloppe avec le mode 1 (contexte d’interruption). Le runtime termine ses transferts SIF dans l’appel : la variante d’interruption est le même appel. Seuls appelants : les gestionnaires RPC côté EE (0x3B03F0–0x3B0604), installés par sceSifInitRpc invité. |
| 0x3AFB58 | sceSifExitCmd | `sceSifExitCmd` | `DisableDmac(5)`, `RemoveDmacHandler(5, …)`, remise à zéro de la garde 0x3E1F88. Seul appelant 0x3B01C8, atteint par sceSifRebootIop invité. |
| 0x3AF8B0 | sceSifGetSreg | `sceSifGetSreg` | Lit le mot `index` de la table 0x1F6F300. Seul appelant : sceSifInitRpc invité (0x3B0178). |
| 0x3B2180 | sceSifLoadModule | `sceSifLoadModule` (liste des appels système) | Enveloppe de `_SifLoadModule(path, argc, argv, &res, 0)` ; client loadfile, SID 0x80000006 ; données 0x1F725C0–0x1F727E8. |
| 0x3B1EA8 | sceSifSearchModuleByName | `sceSifSearchModuleByName` | Client loadfile (SID 0x80000006), RPC 9 : envoi `{nom}` (copié sur 0xFC octets, dernier octet forcé à 0), réponse `{identifiant du module}`. Elle passe d’abord par 0x3B1CE0 (lie le SID puis appelle la RPC 0xFF d’initialisation) et par 0x3B1DE0 (contrôle de la version renvoyée). Seul appelant : 0x396A78, qui formate un message « Error: IOP module missing: %s. You must load this module before initilizing the memcard library. » (VA 0x42FDD8) quand le résultat est négatif. Le runtime renvoie l’identifiant du module IOP chargé dont le nom interne (en-tête IOPMOD de l’IRX : `mcman`, `mcserv`, `sio2man`…) est identique, sinon −1. Sans cette liaison, 0x3B1CE0 réessayait indéfiniment le bind du SID 0x80000006, que l’IOP émulé ne sert pas : loadfile fait partie de l’IOPRP300.IMG, qu’il n’exécute pas (lancement 8 de [BOOT.md](BOOT.md)). |
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
| 0x3AFC70 | Cœur de sceSifSendCmd (`mode` en a1) | Seuls appelants : 0x3AFDA8 et 0x3AFDE8, liés. Il lit l’adresse du tampon IOP posée par sceSifInitCmd invité, qui ne s’exécute plus. |
| 0x3AFE28, 0x3AF880, 0x3AF8A0 | Gestionnaire d’interruption DMAC 5 et gestionnaires système « set sreg » / « change saddr » de libsif | Installés seulement par sceSifInitCmd invité (lié) : inaccessibles. |
| 0x3B0A48 | sceSifCheckStatRpc | Vérifiée au désassemblage. |
| 0x3AFF70 | sceSifWriteBackDCache | Déduite. |
| 0x3B2960 / 0x3B29B0 | DI / EI | Déduites. |
| 0x3AF780 | scePrintf | Déduite. |
| 0x3ADDF0 | DelayThread | Déduite. |
| 0x377870, 0x377958, 0x377A50, 0x3779F8, 0x377B48 | Sémaphores, sortie, gestionnaire d’extinction et verrou S-cmd de libcdvd | Inaccessibles une fois les points d’entrée publics liés. |
| 0x38AFE0 | Lecture de 14 octets de `rom0:ROMVER` vers 0x3E0A88 (libscf) | Passe par fileio, désormais lié. |
| 0x379908 | Lecture de `rom0:ROMVER` jusqu’au NUL ; date des 9 octets précédents comparée à 20010608 | Exige un ROMVER de 16 octets terminé par `\n\0` : profil ROM0 de `src/burnout_overrides.cpp`. |
| 0x396A78 | Vérification des modules IOP par nom (appelée par 0x395A30 pour sio2man, mcman, mcserv, multitap_manager) | Appelle 0x3B1EA8 (lié) ; si le résultat est négatif, formate un message d’erreur dans un tampon local de 512 octets, sans autre effet visible dans le pseudocode. MTAPMAN n’étant pas sur le disque, un résultat négatif pour multitap_manager est attendu. Non liée. |

## À identifier

- libdbc/libpad2 (marqueurs ci-dessus) : client du service DBCMAN (SID 0x80001300), chargé physiquement par l’IOP.
- libmc 3020 : client MCSERV (SID 0x80000400).
- Client GTFS de Criterion : `FUN_001E2278(0x561D18, 1, …, 0x419368, 3, 0x20)` lie le SID 0x475453 (« GTS ») à 0x1E23C8 ; RPC 1 (init), 3 (ouverture), 5 (lecture par blocs de 0xA800 octets). GTFSCDVD.IRX copie les secteurs vers l’EE par `sceSifSetDmaIntr` (sifman:32), puis signale la fin par la commande SIF 4, dont le gestionnaire EE 0x1E2DF0 fait `iSignalSema`. Servi par l’IRX dans l’IOP émulé : ne pas le lier.
- Client audio RenderWare (RWA.IRX / B4ROUTE.IRX), 0x371630 (chaîne « EE  RwaRPCTransfer : ERROR! DMA command queue FULL! » à VA 0x42C9B0) : commandes envoyées à l’IOP par `sceSifSendCmd(0, …)` vers le gestionnaire RWA+0x3D70, enregistré par `sceSifAddCmdHandler(0, …)` dans une table de 8 entrées (`sceSifSetCmdBuffer`). Réponses par la commande 1 vers 0x3719D8 ; l’EE attend la réponse 0x12 (drapeau gp−0x7A9C, boucle 0x372BC8). Servi par l’IRX : ne pas le lier.
