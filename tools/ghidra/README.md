# Analyse Ghidra de Burnout Dominator PAL

Ces scripts utilisent le projet sauvegardé `local/analysis/ghidra/project/BurnoutDominatorPAL.gpr`. Ils ouvrent `SLES_546.27` en lecture seule, sans réimportation ni nouvelle analyse. Le processeur est `r5900:LE:32:default` et le compilateur `default`.

Les fichiers `local/decompiled/functions/*.pseudocode.c` contiennent du **pseudocode de décompilation Ghidra**, avec des types et noms inférés. Ce pseudocode n'est ni le source original récupéré ni du C directement compilable. La couverture des exports ne prouve pas que le jeu est recompilé, jouable ou correctement représenté par toutes les expressions du décompilateur.

## Réexporter le pseudocode

Utiliser Ghidra 12.1.4, l'extension `ghidra-emotionengine-reloaded` 2.1.38 compilée pour cette version, et un JDK 21 ou supérieur. Le premier passage a été effectué avec Java 22.0.1. Les liens et empreintes des archives vérifiées sont dans `docs/toolchain.json`.

Depuis le dépôt :

```powershell
& ./tools/ghidra/export-pseudocode.ps1 -GhidraInstall 'C:\chemin\ghidra_12.1.4_PUBLIC'
```

Le script utilise quatre instances indépendantes du décompilateur, un délai de 10 secondes par fonction et un budget global de 900 secondes. Le projet reste inchangé. Les paramètres Java placent les préférences, caches et fichiers temporaires sous `local/analysis/ghidra/export-state`.

Pour retenter seulement les fonctions dont le premier passage a expiré :

```powershell
& ./tools/ghidra/export-pseudocode.ps1 -GhidraInstall 'C:\chemin\ghidra_12.1.4_PUBLIC' -RetryTimeouts
```

La reprise applique un délai de 60 secondes par fonction. Elle conserve `manifest.first-pass.json`, `manifest.first-pass.jsonl`, les comptes et durées du premier passage, et ajoute les tentatives au manifeste fusionné. Une reprise déjà enregistrée n'est pas relancée implicitement.

`manifest.json` et `manifest.jsonl` décrivent chaque fonction, son adresse, sa fin exclusive, ses plages de corps, son résultat, ses avertissements et ses tentatives. Les fichiers individuels sont préfixés par l'adresse. Un échec conserve un fichier de statut et son entrée au manifeste. `all_pseudocode_exported` est vrai uniquement lorsque chaque fonction a produit du pseudocode ; consulter aussi les avertissements.

La commande retourne une erreur si Ghidra échoue ou ne produit pas de manifeste, et le code 2 si un export individuel reste incomplet. Les journaux et tableaux d'arguments exacts sont conservés dans `local/decompiled`.

## Scripts d'analyse auxiliaires

- `ExportBurnoutPALAnalysis.java` : export testé des vraies fonctions Ghidra dans `.text`, distinctes des labels de reprise ; vérifie l'identité de l'ELF.
- `BurnoutPALAnalysisSetup.java` : préparation d'une future importation. **Non exécuté dans l'analyse sauvegardée livrée.** Il vérifie l'empreinte de l'ELF et retire le droit d'exécution des sections autres que `.text` dans le projet Ghidra. Il ne modifie pas le binaire original.

Le lieur PS2 marque `.data` exécutable dans cet ELF. Les exports limitent donc les fonctions EE à `[0x00100000, 0x003BEB28)` et conservent l'entrée `0x00100008`. Les overlays VU sont des données d'une autre unité de calcul ; le pseudocode EE ne les remplace pas.

## Résultat conservé

Les **8 860 fonctions EE réelles** ont toutes produit du pseudocode. Le passage initial a obtenu 8 858 résultats en 28,519 secondes et deux expirations. La reprise ciblée a obtenu les deux résultats restants en 32,747 secondes, avec un délai de 60 secondes par fonction. Les fichiers et comptes initiaux restent consultables.

**455 fichiers comportent des commentaires `WARNING:` de Ghidra**, conservés dans les fichiers et le manifeste. Ces avertissements portent notamment sur des blocs considérés inaccessibles ou des symboles globaux qui se chevauchent. Un statut `success` signifie que Ghidra a retourné du pseudocode ; il ne valide pas l'équivalence avec le comportement du binaire.

## API de parallélisme utilisée

Le premier passage suit la mise en commun officielle d'instances `DecompInterface` par `DecompilerCallback`, avec `ParallelDecompiler` et un pool limité à quatre travailleurs. La reprise utilise une seule instance et traite uniquement les expirations.

[ParallelDecompiler officiel](https://github.com/NationalSecurityAgency/ghidra/blob/Ghidra_12.1.4_build/Ghidra/Features/Decompiler/src/main/java/ghidra/app/decompiler/parallel/ParallelDecompiler.java), [DecompilerCallback officiel](https://github.com/NationalSecurityAgency/ghidra/blob/Ghidra_12.1.4_build/Ghidra/Features/Decompiler/src/main/java/ghidra/app/decompiler/parallel/DecompilerCallback.java).
