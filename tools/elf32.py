"""Bounds-checked ELF32 metadata parser, including MIPS/PS2 executables."""
from __future__ import annotations

from collections import Counter
import struct


class ELFError(ValueError):
    pass


class UnsupportedELFError(ELFError):
    pass


def _bounds(data: bytes, offset: int, size: int, label: str) -> None:
    if offset < 0 or size < 0 or offset > len(data) or size > len(data) - offset:
        raise ELFError(f"{label} exceeds ELF file bounds: offset={offset}, size={size}")


def _cstring(table: bytes, offset: int, label: str) -> str:
    if offset < 0 or offset >= len(table):
        raise ELFError(f"{label} string offset {offset} is out of bounds")
    end = table.find(b"\x00", offset)
    if end < 0:
        raise ELFError(f"{label} string is not NUL-terminated")
    return table[offset:end].decode("utf-8", errors="replace")


def analyze_elf(data: bytes) -> dict:
    _bounds(data, 0, 52, "ELF32 header")
    if data[:4] != b"\x7fELF":
        raise ELFError("File does not begin with ELF magic")
    if data[4] != 1:
        raise UnsupportedELFError(f"ELF class {data[4]}; only ELF32 is supported")
    if data[5] not in (1, 2):
        raise ELFError("Unsupported ELF data encoding")
    if data[6] != 1:
        raise ELFError("Unsupported ELF identification version")
    endian = "<" if data[5] == 1 else ">"
    warnings = []
    values = struct.unpack_from(endian + "HHIIIIIHHHHHH", data, 16)
    names = ("type", "machine", "version", "entry", "program_offset", "section_offset",
             "flags", "header_size", "program_entry_size", "program_count", "section_entry_size",
             "section_count", "section_name_index")
    header = dict(zip(names, values))
    header["endian"] = "little" if endian == "<" else "big"
    header["osabi"] = data[7]
    header["abi_version"] = data[8]
    if header["version"] != 1 or header["header_size"] < 52:
        raise ELFError("Invalid ELF32 header size or version")
    _bounds(data, 0, header["header_size"], "ELF header")

    shoff = header["section_offset"]
    shentsize = header["section_entry_size"]
    shnum = header["section_count"]
    shstrndx = header["section_name_index"]
    phnum = header["program_count"]
    if shoff:
        if shentsize < 40:
            raise ELFError("ELF32 section entry size is less than 40")
        _bounds(data, shoff, shentsize, "ELF section zero")
        section_zero = struct.unpack_from(endian + "IIIIIIIIII", data, shoff)
        if shnum == 0:
            shnum = section_zero[5]
        if shstrndx == 0xFFFF:
            shstrndx = section_zero[6]
        if phnum == 0xFFFF:
            phnum = section_zero[7]
    elif shnum or shstrndx or phnum == 0xFFFF:
        raise ELFError("Section metadata needs a missing section table")
    if shnum:
        _bounds(data, shoff, shnum * shentsize, "ELF section table")
        if shstrndx >= shnum:
            raise ELFError("ELF section-name table index is out of bounds")
    header["resolved_section_count"] = shnum
    header["resolved_program_count"] = phnum
    header["resolved_section_name_index"] = shstrndx

    sections: list[dict] = []
    snames = ("name_offset", "type", "flags", "address", "offset", "size", "link", "info", "alignment", "entry_size")
    for index in range(shnum):
        fields = struct.unpack_from(endian + "IIIIIIIIII", data, shoff + index * shentsize)
        section = dict(zip(snames, fields))
        section["index"] = index
        # SHT_NULL stores extended counts in sh_size; SHT_NOBITS has no bytes.
        if section["type"] not in (0, 8):
            _bounds(data, section["offset"], section["size"], f"ELF section {index}")
        if section["alignment"] and section["alignment"] & (section["alignment"] - 1):
            raise ELFError(f"ELF section {index} alignment is not a power of two")
        sections.append(section)
    if shstrndx:
        section = sections[shstrndx]
        if section["type"] != 3:
            raise ELFError("ELF section-name table is not SHT_STRTAB")
        string_table = data[section["offset"]:section["offset"] + section["size"]]
        for section in sections:
            section["name"] = _cstring(string_table, section["name_offset"], "section name")
    else:
        for section in sections:
            section["name"] = ""

    programs: list[dict] = []
    phoff = header["program_offset"]
    phentsize = header["program_entry_size"]
    if phnum:
        if phoff == 0 or phentsize < 32:
            raise ELFError("Invalid ELF32 program table location or entry size")
        _bounds(data, phoff, phnum * phentsize, "ELF program table")
    pnames = ("type", "offset", "virtual_address", "physical_address", "file_size", "memory_size", "flags", "alignment")
    for index in range(phnum):
        fields = struct.unpack_from(endian + "IIIIIIII", data, phoff + index * phentsize)
        program = dict(zip(pnames, fields))
        program["index"] = index
        _bounds(data, program["offset"], program["file_size"], f"ELF program {index}")
        if program["type"] == 1 and program["file_size"] > program["memory_size"]:
            raise ELFError(f"ELF PT_LOAD {index} file size exceeds memory size")
        if program["virtual_address"] + program["memory_size"] > 0x100000000:
            raise ELFError(f"ELF program {index} virtual address range overflows ELF32")
        if program["alignment"] not in (0, 1):
            if program["alignment"] & (program["alignment"] - 1):
                raise ELFError(f"ELF program {index} alignment is not a power of two")
            if program["type"] == 1 and (program["virtual_address"] - program["offset"]) % program["alignment"]:
                # Retail PS2 ELF files may violate generic ELF page alignment.
                # Keep the bounded segment usable and record this anomaly.
                warnings.append(f"ELF PT_LOAD {index} offset/address alignment disagrees with p_align")
        if program["flags"] & 1:
            count = min(program["file_size"] // 4, 16)
            program["first_instruction_words"] = [
                {"address": program["virtual_address"] + n * 4,
                 "file_offset": program["offset"] + n * 4,
                 "word": f"0x{struct.unpack_from(endian + 'I', data, program['offset'] + n * 4)[0]:08x}"}
                for n in range(count)
            ]
        programs.append(program)

    symbols: list[dict] = []
    table_stats: list[dict] = []
    for section in sections:
        if section["type"] not in (2, 11):
            continue
        if section["entry_size"] < 16 or section["size"] % section["entry_size"]:
            raise ELFError(f"Invalid ELF symbol entry size in section {section['index']}")
        if section["link"] >= len(sections) or sections[section["link"]]["type"] != 3:
            raise ELFError("ELF symbol table links to invalid string table")
        strings = sections[section["link"]]
        string_table = data[strings["offset"]:strings["offset"] + strings["size"]]
        count = section["size"] // section["entry_size"]
        functions = 0
        for index in range(count):
            offset = section["offset"] + index * section["entry_size"]
            name, value, size, info, other, shndx = struct.unpack_from(endian + "IIIBBH", data, offset)
            symbol = {"table": section["name"], "index": index, "name": _cstring(string_table, name, "symbol name"),
                      "value": value, "size": size, "binding": info >> 4, "type": info & 15,
                      "visibility": other & 3, "section_index": shndx}
            if shndx == 0xFFFF:
                raise UnsupportedELFError("SHN_XINDEX symbols need an unsupported SHT_SYMTAB_SHNDX table")
            if 0 < shndx < 0xFF00 and shndx >= len(sections):
                raise ELFError("ELF symbol section index is out of bounds")
            functions += symbol["type"] == 2
            symbols.append(symbol)
        table_stats.append({"name": section["name"], "type": section["type"], "symbol_count": count, "function_count": functions})
    code_sections = [s for s in sections if s["flags"] & 4 and s["type"] != 8]
    entry_location = None
    for program in programs:
        if program["type"] == 1 and program["virtual_address"] <= header["entry"] < program["virtual_address"] + program["file_size"]:
            file_offset = program["offset"] + header["entry"] - program["virtual_address"]
            available = min(16, (program["virtual_address"] + program["file_size"] - header["entry"]) // 4)
            entry_location = {"program_index": program["index"], "file_offset": file_offset,
                              "instruction_words": [f"0x{struct.unpack_from(endian + 'I', data, file_offset + i * 4)[0]:08x}" for i in range(available)]}
            break
    if header["machine"] != 8:
        warnings.append(f"Machine {header['machine']} is not EM_MIPS (8)")
    if header["entry"] and entry_location is None:
        warnings.append("Entry point is not backed by a PT_LOAD file range")
    return {"header": header, "programs": programs, "sections": sections,
            "code_sections": code_sections, "symbols": symbols, "symbol_tables": table_stats,
            "symbol_count": len(symbols), "function_symbol_count": sum(s["type"] == 2 for s in symbols),
            "named_function_symbol_count": sum(s["type"] == 2 and bool(s["name"]) for s in symbols),
            "symbol_types": dict(Counter(str(s["type"]) for s in symbols)),
            "stripped": not any(s["type"] == 2 for s in sections),
            "entry_location": entry_location, "warnings": warnings}


def printable_strings(data: bytes, minimum: int = 6) -> list[dict]:
    if minimum < 1:
        raise ValueError("Minimum string length must be positive")
    result = []
    start = None
    for offset, value in enumerate(data):
        if 32 <= value <= 126 or value == 9:
            if start is None:
                start = offset
        else:
            if start is not None and offset - start >= minimum:
                result.append({"offset": start, "text": data[start:offset].decode("ascii")})
            start = None
    if start is not None and len(data) - start >= minimum:
        result.append({"offset": start, "text": data[start:].decode("ascii")})
    return result
