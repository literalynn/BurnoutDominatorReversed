"""Read the primary ISO9660 tree without mounting or dumping disc assets.

Only ordinary, non-interleaved, single-extent ISO9660 files are supported.
Joliet/Rock Ridge names are deliberately not substituted for the primary names.
All offsets and lengths are checked before any read or extraction.
"""
from __future__ import annotations

from dataclasses import asdict, dataclass
from pathlib import Path, PurePosixPath
import re
import struct
from typing import BinaryIO

SECTOR_SIZE = 2048


class DiscError(ValueError):
    pass


class UnsupportedDiscError(DiscError):
    pass


def both16(data: bytes, offset: int) -> int:
    if offset < 0 or offset + 4 > len(data):
        raise DiscError("Truncated ISO9660 dual-endian 16-bit field")
    little = struct.unpack_from("<H", data, offset)[0]
    big = struct.unpack_from(">H", data, offset + 2)[0]
    if little != big:
        raise DiscError("ISO9660 dual-endian 16-bit field disagrees")
    return little


def both32(data: bytes, offset: int) -> int:
    if offset < 0 or offset + 8 > len(data):
        raise DiscError("Truncated ISO9660 dual-endian 32-bit field")
    little = struct.unpack_from("<I", data, offset)[0]
    big = struct.unpack_from(">I", data, offset + 4)[0]
    if little != big:
        raise DiscError("ISO9660 dual-endian 32-bit field disagrees")
    return little


def clean_identifier(identifier: bytes) -> str:
    try:
        name = identifier.decode("ascii")
    except UnicodeDecodeError as exc:
        raise UnsupportedDiscError("Primary ISO9660 identifier is not ASCII") from exc
    name = re.sub(r";[0-9]+$", "", name)
    if name.endswith("."):
        name = name[:-1]
    _validate_component(name)
    return name


def _validate_component(name: str) -> None:
    # Apply the same portable restrictions on every host. In particular, Windows
    # device names must not be opened as output files even inside a valid root.
    if (not name or name in (".", "..") or any(ord(c) < 32 or c in '/\\:<>"|?*' for c in name)
            or name.endswith((" ", "."))
            or re.match(r"(?i)^(?:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$)", name)):
        raise DiscError(f"Unsafe portable path component: {name!r}")


def safe_destination(root: Path, iso_path: str) -> Path:
    """Reject traversal and platform-specific absolute paths before resolving."""
    if not iso_path or "\\" in iso_path or ":" in iso_path or "\x00" in iso_path:
        raise DiscError(f"Unsafe extraction path: {iso_path!r}")
    relative = PurePosixPath(iso_path)
    if relative.is_absolute() or any(p in ("", ".", "..") for p in iso_path.split("/")):
        raise DiscError(f"Unsafe extraction path: {iso_path!r}")
    for component in relative.parts:
        _validate_component(component)
    root = root.resolve()
    destination = root.joinpath(*relative.parts).resolve()
    try:
        destination.relative_to(root)
    except ValueError as exc:
        raise DiscError("Extraction path escapes destination") from exc
    return destination


@dataclass(frozen=True)
class DiscEntry:
    path: str
    lba: int
    offset: int
    size: int
    is_directory: bool
    flags: int
    raw_identifier: str
    extended_attribute_blocks: int = 0

    def to_dict(self) -> dict:
        return asdict(self)


class ISO9660:
    def __init__(self, path: Path | str):
        self.path = Path(path)
        self.size = self.path.stat().st_size
        self.file: BinaryIO = self.path.open("rb")
        self.warnings: list[str] = []
        self.volume: dict = {}
        self.entries: list[DiscEntry] = []
        self._by_path: dict[str, DiscEntry] = {}
        try:
            self._read_volume()
            self._walk(self.root, "", set())
        except Exception:
            self.file.close()
            raise

    def __enter__(self) -> "ISO9660":
        return self

    def __exit__(self, *_args) -> None:
        self.file.close()

    def _read_at(self, offset: int, size: int) -> bytes:
        if offset < 0 or size < 0 or offset > self.size or size > self.size - offset:
            raise DiscError(f"Disc read out of bounds: offset={offset}, size={size}")
        self.file.seek(offset)
        data = self.file.read(size)
        if len(data) != size:
            raise DiscError("Short read from disc image")
        return data

    def _read_volume(self) -> None:
        primary = None
        terminated = False
        joliet = False
        for sector in range(16, 16 + 256):
            descriptor = self._read_at(sector * SECTOR_SIZE, SECTOR_SIZE)
            if descriptor[1:6] != b"CD001" or descriptor[6] != 1:
                raise DiscError(f"Invalid ISO9660 volume descriptor at sector {sector}")
            kind = descriptor[0]
            if kind == 1 and primary is None:
                primary = descriptor
            elif kind == 2:
                joliet = joliet or descriptor[88:91] in (b"%/@", b"%/C", b"%/E")
            elif kind == 255:
                terminated = True
                break
        if not terminated:
            raise DiscError("ISO9660 volume descriptor sequence has no terminator")
        if primary is None:
            raise UnsupportedDiscError("No primary ISO9660 volume descriptor")
        block_size = both16(primary, 128)
        if block_size != SECTOR_SIZE:
            raise UnsupportedDiscError(f"Logical block size {block_size}; only 2048-byte ISO images supported")
        volume_blocks = both32(primary, 80)
        if volume_blocks * block_size > self.size:
            raise DiscError("Declared ISO9660 volume extends beyond image")
        self.volume_size = volume_blocks * block_size
        self.volume = {
            "system_identifier": primary[8:40].decode("ascii", errors="replace").rstrip(),
            "volume_identifier": primary[40:72].decode("ascii", errors="replace").rstrip(),
            "logical_block_size": block_size,
            "volume_blocks": volume_blocks,
            "volume_size": self.volume_size,
            "image_size": self.size,
            "joliet_present": joliet,
            "namespace": "primary ISO9660",
        }
        if joliet:
            self.warnings.append("Joliet supplementary names ignored; primary ISO9660 namespace inventoried")
        self.warnings.append("Rock Ridge extensions are not interpreted; primary ISO9660 names are retained")
        record_size = primary[156]
        if record_size < 34 or 156 + record_size > len(primary):
            raise DiscError("Invalid ISO9660 root directory record")
        self.root = self._record(primary[156:156 + record_size], "")
        if not self.root.is_directory:
            raise DiscError("Root directory record is not a directory")

    def _record(self, record: bytes, path: str) -> DiscEntry:
        if len(record) < 34 or record[0] != len(record):
            raise DiscError("Truncated ISO9660 directory record")
        name_size = record[32]
        if name_size < 1 or 33 + name_size > len(record):
            raise DiscError("Invalid ISO9660 identifier length")
        flags = record[25]
        if flags & 0x80:
            raise UnsupportedDiscError(f"Multi-extent ISO9660 file is unsupported: {path}")
        if record[26] or record[27]:
            raise UnsupportedDiscError(f"Interleaved ISO9660 file is unsupported: {path}")
        if both16(record, 28) != 1:
            raise UnsupportedDiscError("Multi-volume ISO9660 file is unsupported")
        lba = both32(record, 2)
        size = both32(record, 10)
        ea = record[1]
        offset = (lba + ea) * SECTOR_SIZE
        if offset > self.volume_size or size > self.volume_size - offset:
            raise DiscError(f"ISO9660 extent is outside volume: {path}")
        return DiscEntry(path, lba, offset, size, bool(flags & 2), flags,
                         record[33:33 + name_size].decode("ascii", errors="backslashreplace"), ea)

    def _walk(self, directory: DiscEntry, prefix: str, ancestors: set[int]) -> None:
        if directory.offset in ancestors:
            raise DiscError(f"Directory extent cycle: {prefix}")
        if len(ancestors) >= 64:
            raise DiscError("ISO9660 directory nesting exceeds 64 levels")
        ancestors = ancestors | {directory.offset}
        # Read one sector at a time, keeping malformed directory sizes bounded.
        cursor = 0
        children: list[DiscEntry] = []
        while cursor < directory.size:
            sector_size = min(SECTOR_SIZE, directory.size - cursor)
            sector = self._read_at(directory.offset + cursor, sector_size)
            position = 0
            while position < len(sector):
                length = sector[position]
                if length == 0:
                    break
                if length < 34 or position + length > len(sector):
                    raise DiscError(f"Directory record crosses logical block or is truncated: {prefix}")
                record = sector[position:position + length]
                position += length
                if 33 + record[32] > len(record):
                    raise DiscError("Directory identifier exceeds record")
                identifier = record[33:33 + record[32]]
                if identifier in (b"\x00", b"\x01"):
                    continue
                name = clean_identifier(identifier)
                path = f"{prefix}/{name}" if prefix else name
                entry = self._record(record, path)
                key = path.casefold()
                if key in self._by_path:
                    raise DiscError(f"Ambiguous duplicate normalized ISO path: {path}")
                self._by_path[key] = entry
                self.entries.append(entry)
                if entry.is_directory:
                    children.append(entry)
            cursor += sector_size
        for child in children:
            self._walk(child, child.path, ancestors)

    def lookup(self, path: str) -> DiscEntry:
        key = re.sub(r";[0-9]+$", "", path.replace("\\", "/").lstrip("/")).casefold()
        try:
            return self._by_path[key]
        except KeyError as exc:
            raise DiscError(f"File not found in primary ISO9660 tree: {path}") from exc

    def read(self, entry: DiscEntry, limit: int | None = None) -> bytes:
        if entry.is_directory:
            raise DiscError(f"Cannot read directory as a file: {entry.path}")
        return self._read_at(entry.offset, entry.size if limit is None else min(entry.size, limit))

    def extract(self, entry: DiscEntry, root: Path) -> Path:
        if entry.is_directory:
            raise DiscError("Cannot extract a directory as a file")
        destination = safe_destination(root, entry.path)
        destination.parent.mkdir(parents=True, exist_ok=True)
        remaining = entry.size
        self.file.seek(entry.offset)
        with destination.open("wb") as out:
            while remaining:
                chunk = self.file.read(min(1024 * 1024, remaining))
                if not chunk:
                    raise DiscError("Short read while extracting file")
                out.write(chunk)
                remaining -= len(chunk)
        return destination


def parse_boot2(system_cnf: bytes) -> str:
    text = system_cnf.decode("ascii", errors="replace")
    match = re.search(r"(?im)^\s*BOOT2\s*=\s*cdrom\d*:\\?([^\r\n]+)", text)
    if match is None:
        raise DiscError("SYSTEM.CNF does not contain a supported BOOT2=cdrom path")
    path = match.group(1).strip().replace("\\", "/").lstrip("/")
    path = re.sub(r";[0-9]+$", "", path)
    # The extraction validator also rejects drive names and path traversal here.
    safe_destination(Path.cwd(), path)
    return path
