# Modifications par rapport à PS2Recomp

Base complète conservée depuis `ran-j/PS2Recomp`, commit indiqué dans `UPSTREAM.json`, sous sa licence GPLv3. Les fichiers de jeu et leur traduction restent dans des répertoires ignorés par Git.

- `tools/` : extraction ISO9660, analyse ELF32, contrôle SHA256 du PAL SLES_546.27, génération et audit de traduction.
- `cmake/Burnout.cmake` : liaison des fonctions traduites dans `burnout_dominator`, sans modifier les sources upstream avec du code de jeu.
- `src/burnout_main.cpp` : montage explicite de l’ISO d’origine et du disque extrait, sauvegardes locales, arrêt strict sur fonction absente, diagnostic sans fenêtre avec délai borné.
- `ps2_runtime.h` : accesseur de l’indicateur existant de fonction manquante pour que le diagnostic retourne un échec réel.
- `CMakeLists.txt` : inclusion de l’intégration Burnout.
- `ps2xIOP/.../iso9660.h`, `iso9660.cpp` et les deux routes CDVD EE/IOP : recherche des fichiers avec leur LBA réelle sur l’ISO d’origine, lectures bornées en 64 bits et échec explicite si cette image est invalide. Le comportement upstream reste disponible lorsque aucune image n’est configurée.
- Tests natifs ISO : données malformées, formats non pris en charge, chemins et offsets au-delà de 4 Gio ; les secteurs réels de SYSTEM.CNF et du binaire PAL ont été vérifiés.

Les appels matériels du runtime upstream restent expérimentaux. Aucune fonction du jeu n’est remplacée par `ret0`, `ret1`, une réussite fictive ou un `skip` pour faire passer un test de démarrage.

Le moteur choisit automatiquement 8 adaptateurs SIF reconnus malgré la liste `stubs=[]` de la configuration. Le rapport et l’en-tête généré permettent de les vérifier séparément ; les comptes de corps C++ ne doivent pas être confondus avec le nombre de fonctions réelles du jeu.
