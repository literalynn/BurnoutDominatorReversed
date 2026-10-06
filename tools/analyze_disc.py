#!/usr/bin/env python3
"""Inventory a local PS2 disc and analyze extracted ELF/IRX binaries.

Usage: python tools/analyze_disc.py --iso /path/game.iso --out-dir local
Use --extract-all-disc to stream all disc files into <out-dir>/disc for runtime use.
The output contains proprietary game files; keep it local and out of Git.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import sys

from elf32 import ELFError, analyze_elf, printable_strings
from iso9660 import DiscError, ISO9660, parse_boot2, safe_destination


def hash_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_json(path: Path, value) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def summarize_elf(path: str, digest: str, result: dict) -> str:
    header = result["header"]
    lines = [f"Disc path: {path}", f"SHA256: {digest}",
             f"ELF32 {header['endian']}-endian, machine={header['machine']}, type={header['type']}",
             f"Entry: 0x{header['entry']:08x}", f"Flags: 0x{header['flags']:08x}",
             f"Program headers: {header['resolved_program_count']}; sections: {header['resolved_section_count']}",
             f"Static symbol table absent (stripped): {result['stripped']}",
             f"Symbols: {result['symbol_count']}; FUNC symbols: {result['function_symbol_count']}; named FUNC symbols: {result['named_function_symbol_count']}",
             "", "Executable sections:"]
    for section in result["code_sections"]:
        lines.append(f"  {section['name'] or '(unnamed)'} address=0x{section['address']:08x} offset=0x{section['offset']:x} size=0x{section['size']:x}")
    lines += ["", "Program segments:"]
    for program in result["programs"]:
        lines.append(f"  [{program['index']}] type={program['type']} vaddr=0x{program['virtual_address']:08x} offset=0x{program['offset']:x} filesz=0x{program['file_size']:x} memsz=0x{program['memory_size']:x} flags=0x{program['flags']:x}")
    if result["entry_location"]:
        lines += ["", "Entry words (raw MIPS instruction encodings):"]
        entry = result["entry_location"]
        for index, word in enumerate(entry["instruction_words"]):
            lines.append(f"  0x{header['entry'] + index * 4:08x}: {word}")
    for warning in result["warnings"]:
        lines.append(f"WARNING: {warning}")
    return "\n".join(lines) + "\n"


def run(iso: Path, out_dir: Path, hash_iso: bool = False, strings_min: int = 6,
        extract_all_disc: bool = False) -> dict:
    iso = iso.resolve()
    out_dir = out_dir.resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    with ISO9660(iso) as disc:
        system = disc.lookup("SYSTEM.CNF")
        if system.size > 64 * 1024:
            raise DiscError("SYSTEM.CNF exceeds the 64 KiB safety limit")
        cnf = disc.read(system)
        boot_path = parse_boot2(cnf)
        boot = disc.lookup(boot_path)
        if boot.is_directory or disc.read(boot, 4) != b"\x7fELF":
            raise DiscError("SYSTEM.CNF BOOT2 file is not an ELF binary")
        system_file = disc.extract(system, out_dir / "disc")
        entries = sorted(disc.entries, key=lambda e: e.path)
        candidates = []
        # Magic-probe every regular file, including executables without suffixes.
        for entry in entries:
            if entry.is_directory:
                continue
            reasons = []
            if entry.path.casefold() == boot.path.casefold():
                reasons.append("BOOT2")
            if Path(entry.path).suffix.upper() in (".ELF", ".IRX"):
                reasons.append("executable suffix")
            if disc.read(entry, 4) == b"\x7fELF":
                reasons.append("ELF magic")
            if reasons:
                candidates.append((entry, reasons))
        metadata = {
            "schema_version": 1, "created_utc": datetime.now(timezone.utc).isoformat(),
            "iso_source": str(iso.resolve()), "iso_sha256": hash_file(iso) if hash_iso else None,
            "iso_hash_status": "computed" if hash_iso else "not requested; use --hash-iso",
            "volume": disc.volume, "warnings": disc.warnings,
            "limitations": ["Single-extent non-interleaved primary ISO9660 only; unsupported layouts fail explicitly",
                            "Joliet and Rock Ridge aliases are not interpreted",
                            "Embedded/compressed executables inside asset archives are not scanned",
                            "ASCII strings only; strings are clues and do not prove function identity",
                            "ELF metadata and raw instruction words do not constitute recovered source code"],
            "boot_path": boot.path, "system_cnf_sha256": hash_file(system_file),
            "file_count": sum(not e.is_directory for e in entries),
            "directory_count": sum(e.is_directory for e in entries),
            "disc_extraction": "all files" if extract_all_disc else "selected executables and SYSTEM.CNF",
            "entries": [e.to_dict() for e in entries], "binaries": [],
        }
        for entry, reasons in candidates:
            if entry.size > 512 * 1024 * 1024:
                raise DiscError(f"Selected binary exceeds 512 MiB extraction/analysis safety limit: {entry.path}")
            artifact = disc.extract(entry, out_dir / "disc")
            digest = hash_file(artifact)
            binary = {"disc_path": entry.path, "size": entry.size, "sha256": digest,
                      "selection_reasons": reasons, "artifact": artifact.relative_to(out_dir).as_posix()}
            try:
                data = artifact.read_bytes()
                result = analyze_elf(data)
                report = safe_destination(out_dir / "analysis", entry.path + ".json")
                write_json(report, result)
                text_report = report.with_suffix(".txt")
                text_report.write_text(summarize_elf(entry.path, digest, result), encoding="utf-8")
                strings = printable_strings(data, strings_min)
                strings_path = report.with_suffix(".strings.txt")
                strings_path.write_text("".join(f"0x{s['offset']:08x}\t{s['text']}\n" for s in strings), encoding="utf-8")
                binary.update({"elf_report": report.relative_to(out_dir).as_posix(),
                               "strings_report": strings_path.relative_to(out_dir).as_posix(),
                               "entry": result["header"]["entry"], "machine": result["header"]["machine"],
                               "stripped": result["stripped"], "function_symbol_count": result["function_symbol_count"],
                               "code_sections": result["code_sections"], "string_count": len(strings)})
            except ELFError as exc:
                binary["analysis_error"] = str(exc)
                if entry.path == boot.path:
                    raise
            metadata["binaries"].append(binary)
        if extract_all_disc:
            # Binaries/SYSTEM.CNF already have verified local copies. Stream all
            # remaining files without loading large assets into memory.
            extracted = {system.path, *(entry.path for entry, _ in candidates)}
            for entry in entries:
                if entry.is_directory:
                    safe_destination(out_dir / "disc", entry.path).mkdir(parents=True, exist_ok=True)
                elif entry.path not in extracted:
                    disc.extract(entry, out_dir / "disc")
        write_json(out_dir / "disc_inventory.json", metadata)
        lines = [f"Volume: {disc.volume['volume_identifier']}", f"ISO image bytes: {disc.size}",
                 f"Files: {metadata['file_count']}; directories: {metadata['directory_count']}",
                 f"BOOT2: {boot.path}", f"Selected binaries: {len(metadata['binaries'])}", ""]
        for binary in metadata["binaries"]:
            lines += [f"{binary['disc_path']} ({binary['size']} bytes)", f"  SHA256 {binary['sha256']}"]
            if "analysis_error" in binary:
                lines.append(f"  Analysis error: {binary['analysis_error']}")
            else:
                lines.append(f"  Entry 0x{binary['entry']:08x}; FUNC symbols {binary['function_symbol_count']}; stripped={binary['stripped']}")
        lines += ["", "Every file and directory offset/size is retained in disc_inventory.json."]
        (out_dir / "disc_inventory.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
        return metadata


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iso", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, default=Path("local"))
    parser.add_argument("--hash-iso", action="store_true", help="Also hash the whole ISO; reads every image byte")
    parser.add_argument("--extract-all-disc", action="store_true", help="Stream every disc file into <out-dir>/disc, including assets")
    parser.add_argument("--strings-min", type=int, default=6)
    args = parser.parse_args()
    if args.strings_min < 1:
        parser.error("--strings-min must be at least 1")
    try:
        metadata = run(args.iso, args.out_dir, args.hash_iso, args.strings_min, args.extract_all_disc)
    except (DiscError, ELFError, OSError, ValueError) as exc:
        print(f"Analysis failed: {exc}", file=sys.stderr)
        return 1
    print(f"Inventoried {metadata['file_count']} files; BOOT2={metadata['boot_path']}")
    print(f"Extracted/analyzed {len(metadata['binaries'])} binaries into {args.out_dir.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
