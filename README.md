# Burnout Dominator Reversed

Projet de recompilation statique de **Burnout Dominator PS2, version européenne SLES_546.27**, pour Windows. L’architecture C++20/CMake prépare Linux et macOS, qui doivent être validés séparément.

**Projet en cours : la rétro-ingénierie intégrale et le jeu jouable ne sont pas encore établis.** La génération C++ et une compilation native ne prouvent pas la fidélité des graphismes, de l’audio, de la physique, des sauvegardes ou de tous les chemins d’exécution. Les résultats réels et les blocages sont consignés dans [docs/STATUS.md](docs/STATUS.md).

La base est un instantané de [PS2Recomp](https://github.com/ran-j/PS2Recomp), identifié dans `UPSTREAM.json`. [REA](https://github.com/morluto/rea) sert de méthode d’investigation ; son fournisseur Ghidra Windows x86-64 PE ne prend pas en charge ce binaire PS2. Le jeu est un ELF MIPS R5900 dépourvu de symboles. Son identité exacte est verrouillée dans `project.json`.

## Installation simple (Windows)

> **Où en est le jeu :** il se compile et démarre, mais n’affiche encore aucune image et ne va pas jusqu’au menu. Installer aujourd’hui sert à tester et à envoyer le journal de lancement.

**Il te faut :**

- ta propre copie du jeu : l’ISO européenne de Burnout Dominator (SLES-54627). Le script vérifie que c’est bien elle (taille et empreinte SHA256). **Laisse-la ensuite à sa place, sans la renommer ni la supprimer** : le jeu la relit à chaque lancement. Si tu la déplaces, glisse-la de nouveau sur `Installer.bat` ;
- environ 20 Go libres sur le disque `C:` : les gros fichiers (disque extrait, code traduit, compilation) vont dans `C:\bdr-work` ;
- ces quatre programmes, à installer une seule fois. Ouvre l’application « Terminal » et tape :
  1. **Python** : `winget install Python.Python.3.12` (ou <https://www.python.org/downloads/>, en cochant **« Add python.exe to PATH »** pendant l’installation).
  2. **Git** : `winget install Git.Git` (ou <https://git-scm.com/download/win>, options par défaut).
  3. **PowerShell 7** : `winget install Microsoft.PowerShell`.
  4. **Visual Studio 2026 Community** : <https://visualstudio.microsoft.com/fr/downloads/>. Dans l’installateur, coche **« Développement Desktop en C++ »**.

  Ferme ensuite le Terminal et rouvre-le pour qu’il trouve ces programmes.

**Installer.** Dans « Terminal », colle ces trois lignes :

```powershell
cd $HOME
git clone https://github.com/literalynn/BurnoutDominatorReversed.git
explorer BurnoutDominatorReversed
```

Si le dépôt est privé, il faut un compte GitHub qui y a accès : au premier `git clone`, Git ouvre une fenêtre de connexion à GitHub (Git Credential Manager). Connecte-toi, la commande continue ensuite toute seule.

Dans le dossier qui s’ouvre, **glisse ton fichier ISO sur `Installer.bat`**. La première fois, compte 30 à 90 minutes. S’il manque quelque chose, le message dit quoi installer : installe-le, puis relance `Installer.bat`. Pour placer les gros fichiers sur un autre disque, lance plutôt dans le Terminal `python tools\install.py --iso "<chemin de l’ISO>" --work-dir D:\bdr-work` (un chemin sans accents).

**Jouer.** Double-clique sur **`Jouer.bat`**. Aujourd’hui, une fenêtre s’ouvre mais reste noire : le lancement s’arrête de lui-même quand le jeu atteint du code pas encore pris en charge (`Exit code: 3`, fonction manquante) ; sinon, ferme la fenêtre. La console affiche ensuite les dernières lignes du journal et son emplacement après `Log:`, normalement `C:\bdr-work\local\logs\run.log`. C’est ce fichier qu’il faut envoyer.

**Mettre à jour.** Dans le Terminal : `cd $HOME\BurnoutDominatorReversed` puis `git pull`, et relance `Installer.bat` (appuie juste sur Entrée quand il demande l’ISO). Seules les étapes nécessaires sont refaites : les outils sont recompilés pour les fichiers modifiés, et le jeu n’est retraduit que si le recompilateur, la carte des fonctions ou l’exécutable du jeu ont changé.

### Linux

```bash
sudo apt install git python3 cmake ninja-build g++ pkg-config libgl-dev libx11-dev libxrandr-dev libxinerama-dev \
  libxcursor-dev libxi-dev libasound2-dev libavcodec-dev libavformat-dev libavutil-dev libswresample-dev libswscale-dev
git clone https://github.com/literalynn/BurnoutDominatorReversed.git && cd BurnoutDominatorReversed
python3 tools/install.py --iso "/chemin/vers/Burnout Dominator.iso"
python3 tools/project.py run
```

`tools/install.py` enchaîne les étapes détaillées ci-dessous et place les gros fichiers dans `~/bdr-work` (`C:\bdr-work` sous Windows), ou dans le dossier passé avec `--work-dir`. Comme sous Windows, l’ISO doit rester à sa place.

## Préparer les fichiers locaux

Python 3.11+, CMake 3.21+, compilateur C++20. Sur Windows : MSVC x64 avec SDK Windows ; la construction initiale utilise Visual Studio 2026. Les outils n’installent aucune configuration globale.

Les commandes se lancent depuis la racine du dépôt. `local/`, `generated/` et `build/` sont dans le dossier de travail : le dépôt lui-même par défaut, ou le dossier donné par `BDR_WORK_DIR` ou par `work.json` (écrit par `python tools/project.py workdir <dossier>` et par `tools/install.py`). Dans ce cas, les chemins `build/…` passés en argument ci-dessous commencent par ce dossier.

```powershell
python tools/project.py extract --iso "D:\ISO EMU\PS2\Burnout Dominator (Europe) (En,Fr,De,Es,It).iso" --all --hash-iso
python -m unittest discover -s tests/python -v
```

`local/disc/` reçoit les 570 fichiers du disque, dont l’exécutable et les 12 IRX ; l’ISO d’origine reste requise pour conserver les adresses de secteurs : `run` la relit à chaque lancement, à l’emplacement noté dans `local/paths.json`. L’inventaire complet, les segments ELF, les sections, les chaînes et les SHA256 sont dans `local/`. Ces fichiers, les sauvegardes et `generated/` sont ignorés par Git.

## Construire les outils et traduire

```powershell
python tools/project.py configure --generator "Visual Studio 18 2026" --arch x64
python tools/project.py build --jobs 4
```

Les dépendances sont téléchargées par CMake aux versions fixées dans les fichiers du projet. Les outils compilés sont `ps2_recomp` et `ps2_analyzer`. Les chemins exacts varient selon le générateur ; avec MSVC ils sont sous `build/ps2xRecomp/Release/` et `build/ps2xAnalyzer/Release/`.

Pour ce jeu sans symboles, la carte des fonctions issue de Ghidra est fournie dans [data/functions.ee.csv](data/README.md) ; `generate --augment` y ajoute le code atteint seulement par pointeur. Sa provenance et la façon de la refaire avec Ghidra et `ps2xRecomp/tools/ghidra/ExportPS2Functions.java` sont décrites dans [data/README.md](data/README.md). La section `.data` porte aussi le drapeau exécutable dans ce disque : les limites de fonctions doivent être examinées, faute de quoi des données sont traduites en fausses instructions.

```powershell
python tools/project.py generate --tool build/ps2xRecomp/Release/ps2_recomp.exe --function-map data/functions.ee.csv --augment
python tools/project.py audit
```

La génération ne force aucune fonction à retourner une réussite et ne configure ni `skip`, ni remplacement arbitraire d’instructions. `local/translation_audit.json` contient les erreurs réelles du recompileur et les comptes statiques. Le mode sans `--function-map` est une expérience heuristique ; il ne justifie pas une couverture complète. `--regenerate` conserve les sources précédentes dans `local/backups/` ; `tools/install.py` n’y garde que la plus récente.

## Compiler et essayer le jeu

```powershell
python tools/project.py configure --game --generator "Visual Studio 18 2026" --arch x64
python tools/project.py build --game --jobs 4
python tools/project.py run --smoke-seconds 10
python tools/project.py run
```

`run` trouve le programme dans `build/` du dossier de travail ; `--exe` en désigne un autre. Options de diagnostic : `--trace-calls 0xADRESSE,…` journalise les appels et retours de ces fonctions, `--trace-watch 0xADRESSE,…` ajoute ces mots mémoire à chaque ligne de trace, `--no-iso` lit les secteurs dans une image virtuelle des fichiers extraits au lieu de l’ISO (numéros de secteurs différents de l’original).

Le diagnostic sans fenêtre s’arrête sur une fonction manquante. Le code 124 indique que le délai a été atteint ; il ne constitue pas une validation de jouabilité. Les journaux sont sous `local/logs/`. FFmpeg est activé pour le décodage vidéo ; `--no-ffmpeg` sert uniquement à un diagnostic limité et remplace les vidéos par des images factices.

## Linux et macOS

Utiliser les mêmes commandes Python/CMake sans `--arch` ; `--generator Ninja` est conseillé. `configure` ajoute `-msse4.1` sur x86-64. Le programme se situe alors sous `build/ps2xRuntime/burnout_dominator`. Les chemins du disque sont configurés à l’extraction ; aucune lettre de lecteur n’est intégrée dans le code C++.

Paquets Ubuntu 24.04 utilisés pour la vérification : `ninja-build libgl-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libasound2-dev pkg-config libavcodec-dev libavformat-dev libavutil-dev libswresample-dev libswscale-dev`. Avec GCC 13, les outils, le runtime et tous les tests compilent et réussissent (lancer `ps2x_tests` depuis la racine du dépôt). Le jeu n’y a pas encore été compilé.

Le runtime upstream possède un chemin SSE vers NEON pour ARM64. La compilation macOS, le comportement sur Apple Silicon et les performances du jeu restent à vérifier sur ces systèmes.

## Licence et provenance

Les sources PS2Recomp incluses conservent leur licence GPLv3, dans `LICENSE`. Le code d’intégration ajouté est sous la même licence. Les fichiers extraits du jeu et le C++ dérivé ne reçoivent pas une licence open source par cette opération et restent locaux. [docs/CHANGES.md](docs/CHANGES.md) décrit les modifications par rapport à l’instantané upstream.
