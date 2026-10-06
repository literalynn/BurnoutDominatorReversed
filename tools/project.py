#!/usr/bin/env python3
"""Portable, identity-locked build/generation workflow. Python 3.11+.

This script never treats a successful native build as proof of a playable port.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
LOCK = json.loads((ROOT / "project.json").read_text(encoding="utf-8"))


def work_root() -> Path:
    """Heavy local data (disc, analysis, generated C++, builds) may live outside
    the repository, e.g. when the checkout sits in a cloud-synced folder.
    Priority: BDR_WORK_DIR, then work.json (ignored by Git), then the checkout."""
    if os.environ.get("BDR_WORK_DIR"):
        return Path(os.environ["BDR_WORK_DIR"]).resolve()
    config = ROOT / "work.json"
    if config.is_file():
        return Path(json.loads(config.read_text(encoding="utf-8"))["work_dir"]).resolve()
    return ROOT


WORK = work_root()
LOCAL = WORK / "local"
GENERATED = WORK / "generated"


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def write_json(path: Path, data) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def checked_elf() -> Path:
    elf = LOCAL / "disc" / LOCK["boot_path"]
    if not elf.is_file():
        raise RuntimeError("Extract the supplied ISO first: python tools/project.py extract --iso <path>")
    if elf.stat().st_size != LOCK["elf_size"] or sha256(elf) != LOCK["elf_sha256"]:
        raise RuntimeError("Wrong ELF build: this project is locked to PAL SLES_546.27 with the SHA256 in project.json")
    return elf


def run(command: list[str], log_name: str, *, timeout: int | None = None) -> None:
    log = LOCAL / "logs" / log_name
    log.parent.mkdir(parents=True, exist_ok=True)
    # Windows environment names are case-insensitive; inherited duplicate
    # Path/PATH entries break MSBuild before the compiler is started.
    env = {key.upper(): value for key, value in os.environ.items()} if os.name == "nt" else os.environ.copy()
    if os.name == "nt":
        count = int(env.get("GIT_CONFIG_COUNT", "0"))
        env.update({"GIT_CONFIG_COUNT": str(count + 1), f"GIT_CONFIG_KEY_{count}": "core.longpaths", f"GIT_CONFIG_VALUE_{count}": "true"})
    print("Running:", subprocess.list2cmdline(command), flush=True)
    with log.open("w", encoding="utf-8") as f:
        result = subprocess.run(command, cwd=ROOT, env=env, stdout=f, stderr=subprocess.STDOUT, timeout=timeout)
    print("Log:", log, flush=True)
    if result.returncode:
        print(log.read_text(encoding="utf-8", errors="replace")[-12000:])
        raise RuntimeError(f"Command failed with exit code {result.returncode}")


def extract(args) -> None:
    from analyze_disc import run as inventory
    metadata = inventory(args.iso, LOCAL, hash_iso=args.hash_iso, extract_all_disc=args.all)
    checked_elf()
    write_json(LOCAL / "paths.json", {"iso": str(args.iso.resolve()), "disc": str(LOCAL / "disc")})
    print(f"Extracted {len(metadata['binaries'])} binaries; inventoried {metadata['file_count']} files.")


def toml_string(value: str) -> str:
    return json.dumps(value, ensure_ascii=False)


def generate(args) -> None:
    elf = checked_elf()
    output = GENERATED
    if output.exists() and any(output.iterdir()):
        if not args.regenerate:
            raise RuntimeError("generated/ is nonempty. Use --regenerate to preserve it in local/backups/ before generating again.")
        backup = LOCAL / "backups" / ("generated-" + datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S-%f"))
        backup.parent.mkdir(parents=True, exist_ok=True)
        output.rename(backup)
    output.mkdir(parents=True, exist_ok=True)
    map_path = args.function_map.resolve() if args.function_map else None
    if map_path and not map_path.is_file():
        raise RuntimeError(f"Missing function map: {map_path}")
    augmentation = None
    if map_path and args.augment:
        # Ghidra misses code reached only through pointers (vtables, callbacks).
        augmented = map_path.with_name(map_path.stem + ".augmented.csv")
        report = augmented.with_suffix(".json")
        run([sys.executable, str(ROOT / "tools" / "augment_function_map.py"), str(elf), str(map_path),
             str(LOCAL / "analysis" / (LOCK["boot_path"] + ".json")), str(augmented), str(report)],
            "augment-function-map.log")
        augmentation = {key: value for key, value in json.loads(report.read_text(encoding="utf-8")).items()
                        if key not in ("entries", "rejected")}
        augmentation["report"] = str(report)
        map_path = augmented
    cfg = LOCAL / "burnout.toml"
    cfg.parent.mkdir(parents=True, exist_ok=True)
    cfg.write_text(
        "# Literal guest translation: no forced success returns or skipped functions.\n[general]\n"
        f"input = {toml_string(elf.as_posix())}\n"
        f"ghidra_output = {toml_string(map_path.as_posix() if map_path else '')}\n"
        f"output = {toml_string(output.as_posix())}\n"
        "single_file_output = false\nlow_memory_mode = true\noutput_worker_threads = 1\n"
        "patch_syscalls = false\npatch_cop0 = false\npatch_cache = false\n"
        "stubs = []\nskip = []\n"
        f"entry_points = [{toml_string('entry@' + LOCK['entry'])}]\n",
        encoding="utf-8",
    )
    run([str(args.tool.resolve()), str(cfg)], "recompile.log")
    write_json(output / "generation.json", {
        "elf_sha256": LOCK["elf_sha256"], "ps2recomp_commit": LOCK["ps2recomp_commit"],
        "function_map": str(map_path) if map_path else None,
        "function_map_sha256": sha256(map_path) if map_path else None,
        "boundary_method": ("Ghidra export + pointer/gap entry points" if augmentation else "Ghidra export")
                           if map_path else "unverified native heuristics",
        "augmentation": augmentation,
        "tool_sha256": sha256(args.tool), "skip": [], "stubs": [],
        "patch_syscalls": False, "patch_cop0": False, "patch_cache": False,
        "playability": "unverified",
    })
    audit(args)


def audit(args) -> None:
    checked_elf()
    files = sorted(GENERATED.glob("*.cpp"))
    if not files:
        raise RuntimeError("No generated C++ to audit")
    addresses: set[int] = set()
    ranges = []
    blockers = []
    comments = re.compile(r"//\s*(?:Delay slot:\s*)?0x([0-9a-fA-F]+):\s*0x([0-9a-fA-F]+)")
    range_pattern = re.compile(r"// Address: 0x([0-9a-fA-F]+) - 0x([0-9a-fA-F]+)")
    for path in files:
        with path.open(encoding="utf-8", errors="replace") as f:
            for number, line in enumerate(f, 1):
                found = comments.search(line)
                if found:
                    addresses.add(int(found[1], 16))
                found = range_pattern.search(line)
                if found:
                    ranges.append({"start": int(found[1], 16), "end": int(found[2], 16), "source": path.name})
                if re.search(r"TODO_NAMED|Unsupported instruction|Unknown instruction|Unhandled .*instruction|Unhandled opcode|unimplemented instruction", line, re.IGNORECASE):
                    blockers.append({"source": path.name, "line": number, "text": line.strip()[:400]})
    report = json.loads((LOCAL / "analysis" / (LOCK["boot_path"] + ".json")).read_text(encoding="utf-8"))
    sections = []
    for section in report["code_sections"]:
        start, size = section["address"], section["size"]
        observed = sum(start <= address < start + size for address in addresses)
        sections.append({"name": section["name"], "address": start, "size": size,
                         "instruction_comments": observed, "aligned_words": size // 4,
                         "comment_coverage_percent": round(observed / (size // 4) * 100, 4) if size >= 4 else None})
    log = LOCAL / "logs" / "recompile.log"
    log_errors = []
    summary = {}
    if log.is_file():
        for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
            if "[error]" in line:
                log_errors.append(line.strip())
            for field, pattern in [("reported_unhandled_instructions", r"Unhandled instructions: (\d+)"),
                                   ("reported_functions", r"Functions discovered: (\d+)"),
                                   ("reported_stubs", r"recompiled: \d+, stubs: (\d+)"),
                                   ("reported_skipped", r"stubs: \d+, skipped: (\d+)"),
                                   ("reported_warnings", r"Warnings: (\d+)"),
                                   ("reported_errors", r"Warnings: \d+, errors: (\d+)")]:
                found = re.search(pattern, line)
                if found:
                    summary[field] = int(found[1])
    write_json(LOCAL / "translation_audit.json", {
        "elf_sha256": LOCK["elf_sha256"], "cpp_files": len(files), "function_ranges": len(ranges),
        "unique_instruction_comment_addresses": len(addresses), "sections": sections,
        "possible_blockers": blockers, "recompiler_summary": summary,
        "recompiler_errors": log_errors, "ranges": ranges,
        "limitations": ["Counts are static output observations, not proof of semantic correctness or execution",
                        "Delay-slot and inline emission can omit instruction comments",
                        "SHF_EXECINSTR on .data does not establish that every data word is code",
                        "VU overlays and IOP modules are not EE C++ functions",
                        "Unknown indirect targets, runtime hardware behavior and playability remain separate checks"],
    })
    print(f"Generated {len(files)} C++ files, {len(ranges)} function ranges; {len(blockers)} unsupported/TODO markers; {len(log_errors)} reported errors.")


def configure(args) -> None:
    command = [args.cmake, "-S", str(ROOT), "-B", str(args.build_dir.resolve()),
               "-DPS2X_BUILD_STUDIO=OFF", "-DPS2X_ENABLE_DEBUG_UI=OFF", "-DPS2X_BUILD_TEST=ON",
               "-DPS2X_IOP_BUILD_TESTS=ON", "-DPS2X_ENABLE_AGRESSIVE_LOGS=OFF",
               "-DPS2X_ENABLE_FFMPEG=" + ("OFF" if args.no_ffmpeg else "ON"),
               "-DCMAKE_BUILD_TYPE=Release", "-DBDR_BUILD_GAME=" + ("ON" if args.game else "OFF"),
               # LTO turns the link of ~49k generated functions into a very long
               # single step; keep it for distribution builds only.
               "-DPS2X_ENABLE_LTO=" + ("ON" if args.lto else "OFF"),
               "-DPS2X_SHOW_WINDOWS_CONSOLE=ON"]
    command.append("-DBDR_GENERATED_DIR=" + GENERATED.as_posix())
    if os.name != "nt" and platform.machine().lower() in ("x86_64", "amd64"):
        # The runtime uses SSE4.1 intrinsics (_mm_extract_epi64...). MSVC accepts
        # them without flags; GCC and Clang need the target feature, as in
        # upstream's Linux CI.
        command += ["-DCMAKE_C_FLAGS=-msse4.1", "-DCMAKE_CXX_FLAGS=-msse4.1"]
    # Optional offline dependency cache: <work>/deps-src/<name>-src.
    deps = WORK / "deps-src"
    for name in ["elfio", "toml11", "fmt", "libdwarf", "rabbitizer", "nlohmann_json", "raylib"]:
        if (deps / f"{name}-src").is_dir():
            command.append(f"-DFETCHCONTENT_SOURCE_DIR_{name.upper()}={(deps / f'{name}-src').as_posix()}")
    if args.generator:
        command += ["-G", args.generator]
    if args.arch:
        command += ["-A", args.arch]
    run(command, "configure.log")


def set_work_dir(args) -> None:
    target = args.path.resolve()
    target.mkdir(parents=True, exist_ok=True)
    write_json(ROOT / "work.json", {"work_dir": str(target)})
    print(f"Work directory: {target}")


def build(args) -> None:
    cache = args.build_dir / "CMakeCache.txt"
    if os.name == "nt" and cache.is_file() and "CMAKE_GENERATOR:INTERNAL=Visual Studio" in cache.read_text(encoding="utf-8", errors="replace"):
        pwsh = shutil.which("pwsh")
        if not pwsh:
            raise RuntimeError("PowerShell 7 is required for the MSBuild environment wrapper (scripts/Invoke-MSBuild.ps1)")
        targets = ["ps2EntryRunner"] if args.game else [
            "ps2_recomp", "ps2_analyzer", "ps2x_tests", "ps2_iop_emulator_tests", "ps2_iop_import_tests",
            "ps2_iop_compatibility_tests", "ps2_iop_import_version_tests", "ps2_iso9660_tests", "burnout_overrides_tests"]
        for target in targets:
            run([pwsh, "-NoProfile", "-File", str(ROOT / "scripts" / "Invoke-MSBuild.ps1"),
                 "-BuildDirectory", str(args.build_dir.resolve()), "-Target", target,
                 "-Jobs", str(args.jobs), "-LogPath", str(LOCAL / "logs" / ("msbuild-" + target + ".log"))], "native-build-" + target + ".log")
        return
    command = [args.cmake, "--build", str(args.build_dir.resolve()), "--config", "Release", "--parallel", str(args.jobs), "--target"]
    command += ["ps2EntryRunner"] if args.game else ["ps2_recomp", "ps2_analyzer", "ps2x_tests", "ps2_iop_emulator_tests", "ps2_iop_import_tests", "ps2_iop_compatibility_tests", "ps2_iop_import_version_tests", "ps2_iso9660_tests", "burnout_overrides_tests"]
    run(command, "native-build.log")


def verified_iso() -> Path:
    paths = json.loads((LOCAL / "paths.json").read_text(encoding="utf-8"))
    iso = Path(paths["iso"])
    if not iso.is_file():
        raise RuntimeError("Original ISO is missing; run extract --iso <new location> to update its path")
    stat = iso.stat()
    stamp = {"path": str(iso), "size": stat.st_size, "mtime_ns": stat.st_mtime_ns}
    cache = LOCAL / "iso_verified.json"
    # Hashing 4.6 GB on every launch is slow: reuse a verification of the same file.
    if cache.is_file() and json.loads(cache.read_text(encoding="utf-8")) == {**stamp, "sha256": LOCK["iso_sha256"]}:
        return iso
    if stat.st_size != LOCK["iso_size"] or sha256(iso) != LOCK["iso_sha256"]:
        raise RuntimeError("Original disc does not match the ISO SHA256 in project.json")
    write_json(cache, {**stamp, "sha256": LOCK["iso_sha256"]})
    return iso


def default_exe() -> Path:
    for candidate in [WORK / "build" / "ps2xRuntime" / "Release" / "burnout_dominator.exe",
                      WORK / "build" / "ps2xRuntime" / "burnout_dominator"]:
        if candidate.is_file():
            return candidate
    raise RuntimeError("Game executable not found; build it with: project.py build --game")


def launch(args) -> None:
    elf = checked_elf()
    if not (GENERATED / "generation.json").is_file():
        raise RuntimeError("Generate code before launching")
    iso = verified_iso()
    exe = args.exe.resolve() if args.exe else default_exe()
    command = [str(exe), str(elf), "--iso", str(iso), "--disc", str(LOCAL / "disc"), "--save", str(LOCAL / "saves")]
    seconds = args.seconds or args.smoke_seconds
    if args.headless or args.smoke_seconds:
        command.append("--headless")
    if seconds:
        command += ["--seconds", str(seconds)]
    if args.status_ms:
        command += ["--status-ms", str(args.status_ms)]
    if args.dump_frames:
        command += ["--dump-frames", str(args.dump_frames.resolve()), "--dump-every-ms", str(args.dump_every_ms)]
    log = LOCAL / "logs" / (args.log or ("smoke.log" if "--headless" in command else "run.log"))
    log.parent.mkdir(parents=True, exist_ok=True)
    print("Running:", subprocess.list2cmdline(command), flush=True)
    with log.open("w", encoding="utf-8") as f:
        try:
            result = subprocess.run(command, cwd=WORK, stdout=f, stderr=subprocess.STDOUT,
                                    timeout=seconds + 60 if seconds else None)
            code = result.returncode
        except subprocess.TimeoutExpired:
            code = "host-timeout"
    tail = log.read_text(encoding="utf-8", errors="replace").splitlines()[-args.tail:]
    print("\n".join(tail))
    print(f"Log: {log}\nExit code: {code}")
    if code not in (0, 124):
        raise RuntimeError(f"Game exited with {code} (124 = time limit, 3 = missing function)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    p = sub.add_parser("workdir", help="Store local data outside the checkout (writes work.json)")
    p.add_argument("path", type=Path)
    p.set_defaults(func=set_work_dir)
    p = sub.add_parser("extract")
    p.add_argument("--iso", required=True, type=Path)
    p.add_argument("--all", action="store_true", help="Extract all disc files for runtime path opens")
    p.add_argument("--hash-iso", action="store_true")
    p.set_defaults(func=extract)
    p = sub.add_parser("generate")
    p.add_argument("--tool", required=True, type=Path)
    p.add_argument("--function-map", type=Path)
    p.add_argument("--augment", action="store_true",
                   help="Add pointer/gap entry points missing from the map (tools/augment_function_map.py)")
    p.add_argument("--regenerate", action="store_true")
    p.set_defaults(func=generate)
    p = sub.add_parser("audit")
    p.set_defaults(func=audit)
    for name, function in [("configure", configure), ("build", build)]:
        p = sub.add_parser(name)
        p.add_argument("--cmake", default="cmake")
        p.add_argument("--build-dir", default=WORK / "build", type=Path)
        p.add_argument("--game", action="store_true")
        if name == "configure":
            p.add_argument("--generator")
            p.add_argument("--arch")
            p.add_argument("--no-ffmpeg", action="store_true", help="Diagnostic build only; video becomes stub frames")
            p.add_argument("--lto", action="store_true", help="Whole-program optimization (slow link; distribution builds)")
        else:
            p.add_argument("--jobs", default=4, type=int)
        p.set_defaults(func=function)
    p = sub.add_parser("run")
    p.add_argument("--exe", type=Path, help="Defaults to the game executable in <work>/build")
    p.add_argument("--headless", action="store_true", help="No window, audio device or presentation loop")
    p.add_argument("--seconds", default=0, type=int, help="Stop after N seconds (exit code 124)")
    p.add_argument("--smoke-seconds", default=0, type=int, help="Same as --headless --seconds N")
    p.add_argument("--status-ms", default=0, type=int, help="Print a scheduler/IOP status line every N ms")
    p.add_argument("--dump-frames", type=Path, help="Save the presented GS frame as PNG periodically")
    p.add_argument("--dump-every-ms", default=1000, type=int)
    p.add_argument("--log", help="Log file name under <work>/local/logs")
    p.add_argument("--tail", default=40, type=int, help="Log lines printed after the run")
    p.set_defaults(func=launch)
    args = parser.parse_args()
    try:
        args.func(args)
        return 0
    except (RuntimeError, OSError, ValueError, subprocess.TimeoutExpired) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
