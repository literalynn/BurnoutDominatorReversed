# Registre d’observations

Les adresses ci-dessous concernent uniquement le PAL SLES_546.27 dont le SHA256 est fixé dans `project.json`. Les noms `FUN_*` et certains noms de bibliothèques viennent de l’analyse ; ils ne sont pas des symboles originaux retrouvés.

| ID | Observation et preuve locale | Limite de la conclusion |
|---|---|---|
| E001 | `local/disc_inventory.json` : ISO original identifié par SHA256, 570 fichiers et 12 IRX, BOOT2 SLES_546.27. | Les contenus compressés ou imbriqués dans les archives de données ne sont pas tous analysés. |
| E002 | `local/analysis/SLES_546.27.json` : ELF32 LE MIPS, entrée 0x00100008, aucune table de symboles, 64 overlays DVP. | Les limites de fonctions et les types doivent être inférés. Les overlays VU ne sont pas des fonctions EE. |
| E003 | `.data` porte SHF_EXECINSTR ; le premier essai heuristique traite des données en instructions et signale 5 216 erreurs. Les enregistrements Ghidra hors de `.text` restent dans `functions.excluded.csv`. | Ne pas conclure que des octets arbitraires sont du code uniquement à partir des permissions ELF. Un éventuel code exécuté hors de `.text` reste à vérifier dynamiquement. |
| E004 | `local/analysis/ghidra/ghidra-program-summary.json` et `functions.actual-ee.csv` : 8 860 fonctions Ghidra dans `.text` ; carte complète avec 40 109 labels de reprise. | Une fonction Ghidra est une identification statique ; ce compte n’est pas un inventaire prouvé de toutes les fonctions originales. |
| E005 | `local/logs/recompile.log` : 0 instruction non traitée, 0 erreur, 4 158 avertissements de branchement indirect, 48 961 corps traduits et 8 adaptateurs SIF automatiques. | L’absence d’erreur de génération ne vérifie pas les destinations indirectes, la fidélité des instructions ni l’exécution. |
| E006 | `local/decompiled/manifest.json` et fichiers par adresse : pseudocode de chaque fonction identifiée, tentatives et avertissements conservés. | Le pseudocode peut contenir des expressions incorrectes ou des types incomplets ; il n’est pas du source original ni du C à compiler directement. |
| E007 | `local/iso_native_validation.json` : SYSTEM.CNF et SLES_546.27 se trouvent après 4 Gio dans l’image. Les recherches EE/IOP utilisent désormais les mêmes secteurs d’origine que les lectures. | Les tests du lecteur et des routes CDVD ne couvrent pas toutes les opérations du système de fichiers propriétaire du jeu. |
| E008 | Tests du runtime upstream : 437 tests du moteur et 45 tests du cache GS réussis. | Ces tests génériques ne démontrent pas que Burnout démarre, affiche ses courses ou conserve sa physique. |

## Première chaîne d’initialisation à examiner

Le pseudocode de `entry` à `0x00100008` montre l’effacement de la mémoire globale dans `0x00432B00..0x01F9BB00`, les appels système 0x3C/0x3D de préparation du thread et du tas, puis le groupe d’initialisation à `0x003B2BD0`, FlushCache, EI et l’appel à `0x0021B3F8`.

Cette dernière routine contient une attente liée à `0x00207D08` et à un octet global situé à `0x0051B910`. C’est une piste pour comparer les traces de démarrage du runtime avec PS2/PCSX2. Cette lecture statique ne prouve ni l’exécution du chemin, ni la raison de cette attente.

Un démarrage borné doit conserver le PC final, toute destination manquante et le premier échec matériel. Les modifications suivantes doivent être motivées par ces traces, puis comparées au comportement du jeu original.
