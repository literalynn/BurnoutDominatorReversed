# Burnout Dominator Reversed

Projet de recompilation statique de **Burnout Dominator PS2, version européenne SLES_546.27**, pour Windows. L’architecture C++20/CMake prépare Linux et macOS, qui doivent être validés séparément.

**Projet en cours : la rétro-ingénierie intégrale et le jeu jouable ne sont pas encore établis.** La génération C++ et une compilation native ne prouvent pas la fidélité des graphismes, de l’audio, de la physique, des sauvegardes ou de tous les chemins d’exécution. Les résultats réels et les blocages sont consignés dans [docs/STATUS.md](docs/STATUS.md).

La base est un instantané de [PS2Recomp](https://github.com/ran-j/PS2Recomp), identifié dans `UPSTREAM.json`. [REA](https://github.com/morluto/rea) sert de méthode d’investigation ; son fournisseur Ghidra Windows x86-64 PE ne prend pas en charge ce binaire PS2. Le jeu est un ELF MIPS R5900 dépourvu de symboles. Son identité exacte est verrouillée dans `project.json`.

## Préparer les fichiers locaux

Python 3.11+, CMake 3.21+, compilateur C++20. Sur Windows : MSVC x64 avec SDK Windows ; la construction initiale utilise Visual Studio 2026. Les outils n’installent aucune configuration globale.

```powershell
python tools/project.py extract --iso "D:\ISO EMU\PS2\Burnout Dominator (Europe) (En,Fr,De,Es,It).iso" --all --hash-iso
python -m unittest discover -s tests/python -v
```

`local/disc/` reçoit les 570 fichiers du disque, dont l’exécutable et les 12 IRX ; l’ISO d’origine reste requise pour conserver les adresses de secteurs. L’inventaire complet, les segments ELF, les sections, les chaînes et les SHA256 sont dans `local/`. Ces fichiers, les sauvegardes et `generated/` sont ignorés par Git.

## Construire les outils et traduire

```powershell
python tools/project.py configure --generator "Visual Studio 18 2026" --arch x64
python tools/project.py build --jobs 4
```

Les dépendances sont téléchargées par CMake aux versions fixées dans les fichiers du projet. Les outils compilés sont `ps2_recomp` et `ps2_analyzer`. Les chemins exacts varient selon le générateur ; avec MSVC ils sont sous `build/ps2xRecomp/Release/` et `build/ps2xAnalyzer/Release/`.

Pour ce jeu sans symboles, utiliser Ghidra avec l’extension Emotion Engine et `ps2xRecomp/tools/ghidra/ExportPS2Functions.java`. Conserver la base d’analyse et exporter la carte CSV. La section `.data` porte aussi le drapeau exécutable dans ce disque : les limites de fonctions doivent être examinées, faute de quoi des données sont traduites en fausses instructions.

```powershell
python tools/project.py generate --tool build/ps2xRecomp/Release/ps2_recomp.exe --function-map local/analysis/ghidra/functions.csv
python tools/project.py audit
```

La génération ne force aucune fonction à retourner une réussite et ne configure ni `skip`, ni remplacement arbitraire d’instructions. `local/translation_audit.json` contient les erreurs réelles du recompileur et les comptes statiques. Le mode sans `--function-map` est une expérience heuristique ; il ne justifie pas une couverture complète. `--regenerate` conserve les sources précédentes dans `local/backups/`.

## Compiler et essayer le jeu

```powershell
python tools/project.py configure --game --generator "Visual Studio 18 2026" --arch x64
python tools/project.py build --game --jobs 4
python tools/project.py run --exe build/ps2xRuntime/Release/burnout_dominator.exe --smoke-seconds 10
python tools/project.py run --exe build/ps2xRuntime/Release/burnout_dominator.exe
```

Le diagnostic sans fenêtre s’arrête sur une fonction manquante. Le code 124 indique que le délai a été atteint ; il ne constitue pas une validation de jouabilité. Les journaux sont sous `local/logs/`. FFmpeg est activé pour le décodage vidéo ; `--no-ffmpeg` sert uniquement à un diagnostic limité et remplace les vidéos par des images factices.

## Linux et macOS

Utiliser les mêmes commandes Python/CMake sans `--arch` ; `--generator Ninja` est conseillé. `configure` ajoute `-msse4.1` sur x86-64. Le programme se situe alors sous `build/ps2xRuntime/burnout_dominator`. Les chemins du disque sont configurés à l’extraction ; aucune lettre de lecteur n’est intégrée dans le code C++.

Paquets Ubuntu 24.04 utilisés pour la vérification : `ninja-build libgl-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libasound2-dev pkg-config libavcodec-dev libavformat-dev libavutil-dev libswresample-dev libswscale-dev`. Avec GCC 13, les outils, le runtime et tous les tests compilent et réussissent (lancer `ps2x_tests` depuis la racine du dépôt). Le jeu n’y a pas encore été compilé.

Le runtime upstream possède un chemin SSE vers NEON pour ARM64. La compilation macOS, le comportement sur Apple Silicon et les performances du jeu restent à vérifier sur ces systèmes.

## Licence et provenance

Les sources PS2Recomp incluses conservent leur licence GPLv3, dans `LICENSE`. Le code d’intégration ajouté est sous la même licence. Les fichiers extraits du jeu et le C++ dérivé ne reçoivent pas une licence open source par cette opération et restent locaux. [docs/CHANGES.md](docs/CHANGES.md) décrit les modifications par rapport à l’instantané upstream.
