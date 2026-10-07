#!/usr/bin/env python3
"""One-command setup: check the prerequisites, verify and extract the disc,
build the tools, translate the game and build it. Finished steps are skipped
when the command is run again.

Usage: python tools/install.py --iso "<path to the original ISO>"
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
PROJECT = ROOT / "tools" / "project.py"
AUGMENTER = ROOT / "tools" / "augment_function_map.py"
SHIPPED_MAP = ROOT / "data" / "functions.ee.csv"
MAP_RELATIVE = Path("analysis") / "ghidra" / "functions.ee.csv"
STATE_NAME = "install-state.json"
ISO_RECORD_NAME = "iso_verified.json"  # same record as project.verified_iso()
GENERATORS = {18: "Visual Studio 18 2026", 17: "Visual Studio 17 2022"}
GIB = 1024 ** 3
EXTRACTION_BYTES = 4_700_000_000  # every file of the disc
TRANSLATION_BYTES = 2 * GIB       # generated C++, about 51,000 files
BUILD_BYTES = 10 * GIB            # tools, runtime and game build tree
# Inputs that project.py generate records in generation.json. The source map
# hash is kept in install-state.json: generation.json hashes the augmented map.
RECORDED_INPUTS = ("elf_sha256", "ps2recomp_commit", "tool_sha256", "augmenter_sha256")
CHANGE_LABELS = {
    "regenerate": "option --regenerate",
    "translation": "aucune traduction complète",
    "augmentation": "traduction faite sans --augment",
    "elf_sha256": "exécutable du jeu modifié",
    "ps2recomp_commit": "nouvelle version de PS2Recomp",
    "tool_sha256": "recompilateur ps2_recomp modifié",
    "augmenter_sha256": "tools/augment_function_map.py modifié",
    "function_map_sha256": "carte des fonctions modifiée",
}
BACKUP_NAME = re.compile(r"generated-\d{8}-\d{6}-\d{6}")  # project.py generate --regenerate


class SetupError(RuntimeError):
    pass


def say(message: str) -> None:
    print(f"\n==> {message}", flush=True)


def sha256(path: Path, progress: bool = False) -> str:
    digest = hashlib.sha256()
    total, done, shown = path.stat().st_size, 0, -1
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            digest.update(block)
            done += len(block)
            if progress and done * 100 // total != shown:
                shown = done * 100 // total
                print(f"\r  {shown} %", end="", flush=True)
    if progress:
        print(flush=True)
    return digest.hexdigest()


def read_json(path: Path) -> dict:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as error:
        raise SetupError(f"{path} est illisible ({error}). Corrige ou supprime ce fichier, puis relance.") from None


def default_jobs(cpu_count: int | None) -> int:
    # scripts/Invoke-MSBuild.ps1 accepts at most 64 jobs.
    return min(cpu_count or 4, 64)


# --- Work directory ---------------------------------------------------------

def choose_work_dir(requested: Path | None, environ: dict, root: Path, home: Path,
                    windows: bool = os.name == "nt") -> tuple[Path, bool]:
    """Return (work directory, whether work.json must be written).

    Same priority as project.work_root(): BDR_WORK_DIR, then work.json. Without
    either: C:\\bdr-work on Windows (the profile path may hold accents and the
    checkout may sit in a synced folder), ~/bdr-work elsewhere. The translated
    game is ~50,000 files.
    """
    if environ.get("BDR_WORK_DIR"):
        work, write = Path(environ["BDR_WORK_DIR"]).resolve(), False
        if requested and requested.resolve() != work:
            raise SetupError(f"BDR_WORK_DIR vaut déjà {work} ; retire cette variable ou n’utilise pas --work-dir.")
    elif requested:
        work, write = requested.resolve(), True
    elif (root / "work.json").is_file():
        work, write = Path(read_json(root / "work.json")["work_dir"]).resolve(), False
    elif windows:
        # os.environ copies have upper-case keys on Windows.
        drive = next((value for key, value in environ.items() if key.upper() == "SYSTEMDRIVE" and value), "C:")
        work, write = Path(drive + "\\", "bdr-work"), True
    else:
        work, write = (home / "bdr-work").resolve(), True
    if windows and not str(work).isascii():
        raise SetupError(f"Le dossier de travail {work} contient des accents ou d’autres caractères spéciaux, "
                         "que MSBuild, CMake et Ninja gèrent mal. Choisis un dossier au nom simple, par exemple "
                         "--work-dir C:\\bdr-work")
    return work, write


# --- Prerequisites ----------------------------------------------------------

def find_visual_studio() -> tuple[int, Path] | None:
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if not vswhere.is_file():
        return None
    result = subprocess.run([str(vswhere), "-latest", "-products", "*", "-requires",
                             "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-format", "json", "-utf8"],
                            capture_output=True, text=True, encoding="utf-8")
    try:
        instances = json.loads(result.stdout or "[]")
    except json.JSONDecodeError:
        return None
    if not instances:
        return None
    return int(instances[0]["installationVersion"].split(".")[0]), Path(instances[0]["installationPath"])


def find_pwsh() -> Path | None:
    found = shutil.which("pwsh")
    if found:
        return Path(found)
    for base in (os.environ.get("ProgramFiles", r"C:\Program Files"), os.environ.get("ProgramW6432", "")):
        candidate = Path(base) / "PowerShell" / "7" / "pwsh.exe" if base else None
        if candidate and candidate.is_file():
            return candidate
    return None


def cache_entry(build_dir: Path, key: str) -> str | None:
    """Value of a CMakeCache.txt entry (KEY:TYPE=value), or None."""
    cache = build_dir / "CMakeCache.txt"
    if not cache.is_file():
        return None
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        name, separator, value = line.partition("=")
        if separator and ":" in name and name.split(":", 1)[0] == key:
            return value
    return None


def foreign_source(build_dir: Path) -> Path | None:
    """The checkout a build tree was configured from, when it is not this one."""
    source = cache_entry(build_dir, "CMAKE_HOME_DIRECTORY")
    if not source or os.path.normcase(os.path.abspath(source)) == os.path.normcase(os.path.abspath(ROOT)):
        return None
    return Path(source)


def cached_cmake(build_dir: Path) -> str | None:
    """The CMake that configured the build tree, while it is still installed."""
    command = cache_entry(build_dir, "CMAKE_COMMAND")
    return command if command and Path(command).is_file() else None


def existing_parent(path: Path) -> Path:
    while not path.exists() and path.parent != path:
        path = path.parent
    return path


def required_space(disc_ready: bool, game_built: bool) -> int:
    """Free bytes the remaining steps need. A translation is always counted: an
    update can require a new one, and the previous one is kept as a backup."""
    return (0 if disc_ready else EXTRACTION_BYTES) + TRANSLATION_BYTES + (0 if game_built else BUILD_BYTES)


def check_prerequisites(work: Path, build_dir: Path, needed: int) -> dict:
    """Return the CMake executable and generator options, or raise with what to install."""
    windows = os.name == "nt"
    problems = []
    if sys.version_info < (3, 11):
        problems.append("Python 3.11 ou plus récent : dans un terminal, tape  winget install Python.Python.3.12"
                        if windows else "Python 3.11 ou plus récent.")
    if not shutil.which("git"):
        problems.append("Git, qui télécharge les dépendances et les mises à jour : dans un terminal, tape  "
                        "winget install Git.Git" if windows else "Git (Ubuntu : sudo apt install git).")
    toolchain = {"cmake": cached_cmake(build_dir), "generator": [], "path": []}
    if windows:
        studio = find_visual_studio()
        if not studio or studio[0] not in GENERATORS:
            problems.append("Visual Studio 2026 (ou 2022) Community avec la charge de travail "
                            "« Développement Desktop en C++ » : https://visualstudio.microsoft.com/fr/downloads/")
        else:
            major, location = studio
            if not toolchain["cmake"]:
                bundled = location / "Common7" / "IDE" / "CommonExtensions" / "Microsoft" / "CMake" / "CMake" / "bin" / "cmake.exe"
                toolchain["cmake"] = str(bundled) if bundled.is_file() else shutil.which("cmake")
                if not toolchain["cmake"]:
                    problems.append("CMake : coche « Outils CMake C++ pour Windows » dans l’installateur de Visual Studio.")
            toolchain["generator"] = ["--generator", GENERATORS[major], "--arch", "x64"]
        pwsh = find_pwsh()
        if not pwsh:
            problems.append("PowerShell 7 : dans un terminal, tape  winget install Microsoft.PowerShell")
        else:
            toolchain["path"].append(str(pwsh.parent))
    else:
        if not toolchain["cmake"]:
            toolchain["cmake"] = shutil.which("cmake")
            if not toolchain["cmake"]:
                problems.append("CMake 3.21+ (Ubuntu : sudo apt install cmake ninja-build).")
        if not any(shutil.which(c) for c in ("c++", "g++", "clang++")):
            problems.append("Un compilateur C++20 (Ubuntu : sudo apt install g++).")
        if shutil.which("ninja"):
            toolchain["generator"] = ["--generator", "Ninja"]
        if sys.platform.startswith("linux"):
            probe = subprocess.run(["pkg-config", "--exists", "libavcodec", "libavformat", "libswscale", "libswresample", "gl", "x11"],
                                   capture_output=True) if shutil.which("pkg-config") else None
            if probe is None or probe.returncode:
                problems.append("Bibliothèques de développement (Ubuntu) : sudo apt install pkg-config libgl-dev libx11-dev "
                                "libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libasound2-dev libavcodec-dev "
                                "libavformat-dev libavutil-dev libswresample-dev libswscale-dev")
    if cache_entry(build_dir, "CMAKE_GENERATOR"):
        # An existing build tree keeps its generator; CMake rejects a different one.
        toolchain["generator"] = []
    free = shutil.disk_usage(existing_parent(work)).free
    if free < needed:
        problems.append(f"Au moins {needed / GIB:.0f} Go libres pour {work} "
                        f"(il en reste {free / GIB:.1f} Go). Libère de la place ou choisis un autre disque avec --work-dir.")
    if problems:
        raise SetupError("Il manque :\n  - " + "\n  - ".join(problems)
                         + "\nInstalle-les, ferme et rouvre le terminal, puis relance la même commande.")
    return toolchain


# --- Disc -------------------------------------------------------------------

def disc_state(local: Path, lock: dict) -> str:
    """'ok', 'partial' (only executables extracted) or 'missing'."""
    elf = local / "disc" / lock["boot_path"]
    if not elf.is_file() or elf.stat().st_size != lock["elf_size"] or sha256(elf) != lock["elf_sha256"]:
        return "missing"
    inventory = local / "disc_inventory.json"
    if not inventory.is_file() or read_json(inventory).get("disc_extraction") != "all files":
        return "partial"
    return "ok"


def recorded_iso(local: Path) -> Path | None:
    paths = local / "paths.json"
    recorded = read_json(paths).get("iso") if paths.is_file() else None
    return Path(recorded) if recorded else None


def iso_hint(windows: bool) -> str:
    if windows:
        return ("Glisse ton fichier ISO sur Installer.bat, ou tape  "
                'python tools\\install.py --iso "D:\\chemin\\vers\\le jeu.iso"')
    return 'Indique-la avec  python3 tools/install.py --iso "/chemin/vers/le jeu.iso"'


def choose_iso(given: Path | None, recorded: Path | None, windows: bool) -> Path:
    """The ISO given now, else the one recorded by the last extraction. The game
    reads it at every launch, so the recorded one must still exist."""
    if given is None and recorded is None:
        raise SetupError("Indique ton ISO de Burnout Dominator (version européenne). " + iso_hint(windows))
    source = given or recorded
    if not source.is_file():
        if given is None:
            raise SetupError(f"L’ISO enregistrée à l’installation précédente est introuvable : {source}\n"
                             "Elle a été déplacée ou supprimée. " + iso_hint(windows))
        raise SetupError(f"ISO introuvable : {source}")
    return source.resolve()


def iso_record(iso: Path, lock: dict) -> dict:
    stat = iso.stat()
    return {"path": str(iso), "size": stat.st_size, "mtime_ns": stat.st_mtime_ns, "sha256": lock["iso_sha256"]}


def verify_iso(iso: Path, local: Path, lock: dict) -> bool:
    """Check the ISO size and SHA256 against project.json. Return False when a
    check of the same file (path, size, modification time) is reused: hashing
    4.6 GB takes minutes. project.py run reads the same record."""
    record, cache = iso_record(iso, lock), local / ISO_RECORD_NAME
    if cache.is_file() and read_json(cache) == record:
        return False
    expected = ("Il faut une copie 1:1 du disque européen de Burnout Dominator (SLES-54627), "
                "celle décrite dans project.json.")
    if record["size"] != lock["iso_size"]:
        raise SetupError(f"{iso} n’est pas l’ISO attendue ({record['size']} octets au lieu de {lock['iso_size']}). " + expected)
    say("Vérification de l’ISO (une seule fois, une à trois minutes)")
    if sha256(iso, progress=True) != lock["iso_sha256"]:
        raise SetupError(f"{iso} n’est pas l’ISO attendue (empreinte SHA256 différente). " + expected)
    local.mkdir(parents=True, exist_ok=True)
    cache.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    return True


# --- Translation ------------------------------------------------------------

def choose_function_map(local: Path, explicit: Path | None, shipped: Path) -> Path:
    """--function-map, else a map kept in the work directory (a newer Ghidra
    export), else the shipped one, used in place: project.py writes the
    augmented map into the work directory, never next to its source."""
    work_map = local / MAP_RELATIVE
    source = explicit.resolve() if explicit else work_map if work_map.is_file() else shipped
    if not source.is_file():
        raise SetupError(f"Carte des fonctions introuvable : {source}. Mets le dépôt à jour (git pull) "
                         "ou passe --function-map.")
    return source


def generation_inputs(lock: dict, recompiler: Path, map_path: Path, augmenter: Path = AUGMENTER) -> dict:
    return {"elf_sha256": lock["elf_sha256"], "ps2recomp_commit": lock["ps2recomp_commit"],
            "tool_sha256": sha256(recompiler), "augmenter_sha256": sha256(augmenter),
            "function_map_sha256": sha256(map_path)}


def generation_changes(generated: Path, local: Path, current: dict) -> list[str]:
    """Why the game must be translated again (keys of CHANGE_LABELS); empty
    when generated/ is up to date.

    Values come from generation.json, else from install-state.json. A value
    neither holds (a translation made before it was recorded) is assumed
    unchanged once and remembered in install-state.json."""
    record = generated / "generation.json"
    if not record.is_file():
        return ["translation"]
    info = read_json(record)
    if not info.get("augmentation"):
        return ["augmentation"]
    state_path = local / STATE_NAME
    state = read_json(state_path) if state_path.is_file() else {}
    changes = []
    for key, value in current.items():
        known = info.get(key) if key in RECORDED_INPUTS and info.get(key) else state.get(key)
        if known is not None and known != value:
            changes.append(key)
    if not changes and state != current:
        write_state(local, current)
    return changes


def write_state(local: Path, inputs: dict) -> None:
    (local / STATE_NAME).write_text(json.dumps(inputs, indent=2) + "\n", encoding="utf-8")


def remove_inside(path: Path, work: Path) -> None:
    """shutil.rmtree limited to a real directory (not a link) inside the work directory."""
    resolved = path.resolve()
    if work.resolve() not in resolved.parents or resolved != path.parent.resolve() / path.name or not resolved.is_dir():
        raise SetupError(f"Suppression refusée : {path} n’est pas un dossier du dossier de travail {work}.")
    shutil.rmtree(resolved)


def prune_backups(backups: Path, work: Path, keep: int = 1) -> list[Path]:
    """Delete the translations project.py saved in local/backups/, except the newest `keep`."""
    if not backups.is_dir():
        return []
    saved = sorted(p for p in backups.iterdir() if p.is_dir() and BACKUP_NAME.fullmatch(p.name))
    removed = saved[:max(len(saved) - keep, 0)]
    for path in removed:
        remove_inside(path, work)
    return removed


# --- Build ------------------------------------------------------------------

def find_recompiler(build_dir: Path) -> Path | None:
    for candidate in (build_dir / "ps2xRecomp" / "Release" / "ps2_recomp.exe",
                      build_dir / "ps2xRecomp" / "ps2_recomp.exe",
                      build_dir / "ps2xRecomp" / "ps2_recomp"):
        if candidate.is_file():
            return candidate
    return None


def find_game(build_dir: Path) -> Path | None:
    for candidate in (build_dir / "ps2xRuntime" / "Release" / "burnout_dominator.exe",
                      build_dir / "ps2xRuntime" / "burnout_dominator"):
        if candidate.is_file():
            return candidate
    return None


def project(env: dict, *arguments: str) -> None:
    command = [sys.executable, str(PROJECT), *arguments]
    if subprocess.run(command, cwd=ROOT, env=env).returncode:
        step = " ".join([arguments[0]] + (["--game"] if "--game" in arguments else []))
        raise SetupError(f"Étape échouée : {step}. Le journal complet est indiqué juste au-dessus.")


def final_message(windows: bool) -> list[str]:
    launch = ("Pour lancer le jeu : double-clique sur Jouer.bat (ou tape  python tools\\project.py run)."
              if windows else "Pour lancer le jeu : python3 tools/project.py run")
    return [launch,
            "Garde l’ISO à son emplacement actuel : le jeu la lit à chaque lancement.",
            "Le jeu ne va pas encore jusqu’au menu : envoie le journal affiché à la fin du lancement pour la suite."]


# --- Main -------------------------------------------------------------------

def setup(args: argparse.Namespace) -> int:
    windows = os.name == "nt"
    given = args.iso.strip().strip('"')
    iso = Path(given).resolve() if given else None
    work, write_config = choose_work_dir(args.work_dir, dict(os.environ), ROOT, Path.home(), windows)
    build_dir, local, generated = work / "build", work / "local", work / "generated"
    say(f"Dossier de travail : {work}")
    lock = read_json(ROOT / "project.json")
    disc = disc_state(local, lock)
    toolchain = check_prerequisites(work, build_dir, required_space(disc == "ok", find_game(build_dir) is not None))
    if args.check:
        say("Tous les prérequis sont présents.")
        return 0
    work.mkdir(parents=True, exist_ok=True)
    if write_config:
        (ROOT / "work.json").write_text(json.dumps({"work_dir": str(work)}, indent=2) + "\n", encoding="utf-8")
    env = dict(os.environ)
    env["PATH"] = os.pathsep.join(toolchain["path"] + [env.get("PATH", "")])
    env["BDR_WORK_DIR"] = str(work)
    cmake = ["--cmake", toolchain["cmake"]]

    known_iso = recorded_iso(local)
    source = choose_iso(iso, known_iso, windows)
    if not verify_iso(source, local, lock):
        say(f"ISO déjà vérifiée : {source}")
    if disc != "ok":
        say("Extraction du disque (environ 4,7 Go, quelques minutes)")
        project(env, "extract", "--iso", str(source), "--all")
    elif source != known_iso:
        (local / "paths.json").write_text(json.dumps({"iso": str(source), "disc": str(local / "disc")}, indent=2) + "\n",
                                          encoding="utf-8")
        say(f"Nouvel emplacement de l’ISO enregistré : {source}")
    else:
        say("Disque déjà extrait")

    map_path = choose_function_map(local, args.function_map, SHIPPED_MAP)
    recompiler = find_recompiler(build_dir)
    foreign = foreign_source(build_dir)
    if foreign:
        say(f"La compilation existante vient d’une autre copie du projet ({foreign}) : "
            "nouvelle configuration, puis recompilation complète")
    if not recompiler or not (build_dir / "CMakeCache.txt").is_file() or foreign:
        say("Configuration des outils (téléchargement des dépendances)")
        project(env, "configure", *cmake, *toolchain["generator"])
        toolchain["generator"] = []
    # Always built: after an update, only the changed sources are compiled again.
    say("Compilation des outils et des tests (10 à 30 minutes la première fois)" if not recompiler
        else "Mise à jour des outils (seuls les fichiers modifiés sont recompilés)")
    project(env, "build", *cmake, "--jobs", str(args.jobs))
    recompiler = find_recompiler(build_dir)
    if not recompiler:
        raise SetupError(f"ps2_recomp introuvable dans {build_dir} après la compilation.")

    current = generation_inputs(lock, recompiler, map_path)
    changes = ["regenerate"] if args.regenerate else generation_changes(generated, local, current)
    if changes:
        say("Traduction du jeu en C++ (quelques minutes) : " + ", ".join(CHANGE_LABELS[c] for c in changes))
        if generated.is_dir() and not (generated / "generation.json").is_file():
            say(f"Suppression d’une traduction interrompue : {generated}")
            remove_inside(generated, work)
        prune_backups(local / "backups", work)
        regenerate = ["--regenerate"] if generated.is_dir() and any(generated.iterdir()) else []
        project(env, "generate", "--tool", str(recompiler), "--function-map", str(map_path), "--augment", *regenerate)
        write_state(local, current)
        prune_backups(local / "backups", work)
    else:
        say("Code du jeu déjà traduit")

    say("Compilation du jeu (10 à 60 minutes selon le processeur)")
    project(env, "configure", "--game", *cmake, *toolchain["generator"])
    project(env, "build", "--game", *cmake, "--jobs", str(args.jobs))
    say("Installation terminée.")
    print("\n".join(final_message(windows)))
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--iso", default="", help="Original ISO (only needed the first time or after moving it)")
    parser.add_argument("--work-dir", type=Path,
                        help="Where extracted files, translated code and builds go (default C:\\bdr-work, ~/bdr-work)")
    parser.add_argument("--function-map", type=Path,
                        help="Ghidra function map (default: <work>/local/analysis/ghidra/functions.ee.csv "
                             "if present, else data/functions.ee.csv)")
    parser.add_argument("--jobs", type=int, default=default_jobs(os.cpu_count()),
                        help="Parallel compile jobs (default: processor count, at most 64)")
    parser.add_argument("--regenerate", action="store_true", help="Translate the game again even if nothing changed")
    parser.add_argument("--check", action="store_true", help="Only check the prerequisites")
    args = parser.parse_args(argv)
    try:
        return setup(args)
    except KeyboardInterrupt:
        print("\nInstallation interrompue. Relance la même commande pour reprendre.", file=sys.stderr)
        return 130
    except SetupError as error:
        message = str(error)
    except subprocess.CalledProcessError as error:
        command = error.cmd if isinstance(error.cmd, str) else subprocess.list2cmdline([str(c) for c in error.cmd])
        message = f"La commande {command} a échoué (code {error.returncode})."
    except json.JSONDecodeError as error:
        message = f"Un fichier JSON est illisible ({error}). Corrige ou supprime ce fichier, puis relance."
    except KeyError as error:
        message = f"Information {error} absente d’un fichier de configuration (work.json, project.json ou local/*.json)."
    except OSError as error:
        message = (f"Accès impossible à un fichier ou un dossier : {error}\n"
                   "Vérifie que le disque est branché, qu’il reste de la place et que le dossier n’est pas en lecture seule.")
    except ValueError as error:
        message = f"Valeur invalide : {error}"
    print(f"\nERREUR : {message}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
