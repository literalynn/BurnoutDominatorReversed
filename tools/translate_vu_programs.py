#!/usr/bin/env python3
"""Translate whole VU1 microprograms into C++ for runtime/ps2_vu1_native.h.

A program runs from the MSCAL or MSCNT address that starts it to its E bit. The translation
follows VU1Interpreter (ps2_vu1_exec.inl, ps2_vu1_core.cpp) cycle by cycle at translation time.
A pair waits until the VF and VI values it reads are written, so register values follow program
order and the C++ writes them at once; what does not wait is modelled: MAC and clip flags land
4 cycles after their instruction, Q 7 or 13 cycles after its DIV, SQRT or RSQRT, P 12 or 18
cycles after its EFU instruction, and a conditional branch right after an integer instruction
that wrote its register reads the old value. A register jump goes to the targets the start
values and the program's own stores allow; the C++ checks those start values first. A block of code is
translated once per pipeline state it can be entered with. The one timing the translation
cannot know is how long an XGKICK waits for the previous packet: the C++ counts cycles and
picks the block for the wait it finds.

VU0 programs translate the same way: their data memory is 4 KiB, their code addresses wrap at
4 KiB, and the VU1-only instructions (XGKICK, MFP, the EFU) are reserved there.

Inputs are the code images of compile_vu_programs.py (vu1-<hash>.code and vu0-<hash>.code in
profile directories) and data/vu_native_programs.txt, which names the programs to translate by
the checksum of their code. The output is generated from game microcode and must stay local.

  translate_vu_programs.py --programs <list> --output <cpp> [--shards N] <profile dir>...
  translate_vu_programs.py --describe <name> <image> <entry pc, hex>   (prints a list line;
                                                                      a 4 KiB image is VU0's)
"""
import argparse
import re
import struct
import sys
from pathlib import Path

FMAC_LATENCY = 4
# MSVC's memory to optimize one function (/d2OptimizeHugeFunctions) grows faster than its length:
# 3.5 GB for gen_adc's 9,400 lines, 16.5 GB for gt_im3d's 21,000 (GitHub issue 11). Translations
# longer than SPLIT_LINES are cut into parts of about PART_LINES lines (7.6 GB for gt_im3d's three);
# each part costs a call, so shorter programs stay whole.
SPLIT_LINES = 12000
PART_LINES = 6000


class TranslationError(Exception):
    pass


def lanes_of(mask):
    return [lane for lane in range(4) if mask & (8 >> lane)]


def lane_bit(lane):
    return 8 >> lane


def s11(value):
    return value - 0x800 if value & 0x400 else value


def normalized_bits(word):
    exponent = (word >> 23) & 0xFF
    if exponent == 0:
        return word & 0x80000000
    if exponent == 0xFF:
        return (word & 0x80000000) | 0x7F7FFFFF
    return word


def vf(reg):
    return f"f{reg}"


def vi(reg):
    return "0" if reg == 0 else f"vi{reg}"


class Upper:
    """An upper instruction, decoded as decodeUpperUsage and execUpper do."""

    def __init__(self, word):
        self.word = word
        dest = self.dest = (word >> 21) & 15
        ft, fs, fd = (word >> 16) & 31, (word >> 11) & 31, (word >> 6) & 31
        self.fs, self.ft = fs, ft
        self.reads = []  # (vf register, lanes)
        self.out = None  # ("vf", register) or ("acc",)
        self.kind = "nop"  # nop arith max min itof ftoi abs clip
        self.op = None  # arith: add sub mul madd msub
        self.second = None  # ("bc", lane) | ("q",) | ("i",) | ("vec",)
        self.outer = False  # OPMULA, OPMSUB: fs.yzx with ft.zxy
        self.shift = 0
        op = word & 0x3F
        if op < 0x3C:
            if op > 0x2F:
                raise TranslationError(f"reserved upper {word:08x}")
            self.reads.append((fs, dest))
            self.out = ("vf", fd)
            if op <= 0x1B:
                self.reads.append((ft, lane_bit(op & 3)))
                self.second = ("bc", op & 3)
                kind = ["add", "sub", "madd", "msub", "max", "min", "mul"][op >> 2]
            elif op <= 0x27:
                kind, second = {0x1C: ("mul", "q"), 0x1D: ("max", "i"), 0x1E: ("mul", "i"), 0x1F: ("min", "i"),
                                0x20: ("add", "q"), 0x21: ("madd", "q"), 0x22: ("add", "i"), 0x23: ("madd", "i"),
                                0x24: ("sub", "q"), 0x25: ("msub", "q"), 0x26: ("sub", "i"), 0x27: ("msub", "i")}[op]
                self.second = (second,)
            else:
                kind = ["add", "madd", "mul", "max", "sub", "msub", "msub", "min"][op - 0x28]
                self.reads.append((ft, 0xE if op == 0x2E else dest))
                self.second = ("vec",)
                self.outer = op == 0x2E
            self.set(kind)
        else:
            special = (word & 3) | ((word >> 4) & 0x7C)
            writes_acc = (special <= 0x0F or 0x18 <= special <= 0x1C or special == 0x1E or
                          0x20 <= special <= 0x2A or 0x2C <= special <= 0x2E)
            if writes_acc:
                self.reads.append((fs, dest))
                self.out = ("acc",)
                if special <= 0x0F or 0x18 <= special <= 0x1B:
                    self.reads.append((ft, lane_bit(special & 3)))
                    self.second = ("bc", special & 3)
                    kind = {0: "add", 1: "sub", 2: "madd", 3: "msub", 6: "mul"}[special >> 2]
                elif special in (0x1C, 0x1E):
                    kind, self.second = "mul", (("q",) if special == 0x1C else ("i",))
                elif special <= 0x27:
                    kind = ["add", "madd", "add", "madd", "sub", "msub", "sub", "msub"][special - 0x20]
                    self.second = ("q",) if special in (0x20, 0x21, 0x24, 0x25) else ("i",)
                else:
                    kind = {0x28: "add", 0x29: "madd", 0x2A: "mul", 0x2C: "sub", 0x2D: "msub", 0x2E: "mul"}[special]
                    self.reads.append((ft, 0xE if special == 0x2E else dest))
                    self.second = ("vec",)
                    self.outer = special == 0x2E
                self.set(kind)
            elif 0x10 <= special <= 0x17:
                self.kind = "itof" if special < 0x14 else "ftoi"
                self.shift = [0, 4, 12, 15][special & 3]
                self.reads.append((fs, dest))
                self.out = ("vf", ft)
            elif special == 0x1D:
                self.kind = "abs"
                self.reads.append((fs, dest))
                self.out = ("vf", ft)
            elif special == 0x1F:
                self.kind = "clip"
                self.reads += [(fs, 0xE), (ft, 0x1)]
            elif special not in (0x2F, 0x30):
                raise TranslationError(f"reserved upper {word:08x}")
        if self.outer and dest & 1:
            raise TranslationError(f"OPMULA/OPMSUB with w in {word:08x}")
        if self.out == ("vf", 0):
            self.out = None  # an upper write to VF0 is discarded
        # updateFmacFlags: FMAC arithmetic with a destination queues MAC flags.
        self.mac = self.kind == "arith" and dest != 0
        self.reads = [(reg, lanes) for reg, lanes in self.reads if reg != 0 and lanes]

    def set(self, kind):
        if kind in ("max", "min"):
            self.kind = kind
        else:
            self.kind, self.op = "arith", kind

    def operands(self):
        a = vf(self.fs)
        second = self.second
        if second[0] == "bc":
            b = f"bc<{second[1]}>({vf(self.ft)})"
        elif second[0] == "q":
            b = "splat(q)"
        elif second[0] == "i":
            b = "iN" if self.kind == "arith" else "iR"  # MAXi and MINIi compare the raw bits
        else:
            b = vf(self.ft)
        if self.outer:
            a, b = f"outerLeft({a})", f"outerRight({b})"
        return a, b

    def result(self):
        k = self.kind
        if k in ("itof", "ftoi"):
            return f"{k}<{self.shift}>({vf(self.fs)})"
        if k == "abs":
            return f"abs({vf(self.fs)})"
        if k in ("max", "min"):
            a, b = self.operands()
            return f"{k}({a}, {b})"
        if k == "arith":
            a, b = self.operands()
            if self.op in ("madd", "msub"):
                return f"{self.op}<{self.dest}>(acc, {a}, {b})"
            return f"{self.op}({a}, {b})"
        return None

    def flags(self, result):
        a, b = self.operands()
        op = {"add": "Add", "sub": "Sub", "mul": "Mul", "madd": "MAdd", "msub": "MSub"}[self.op]
        return f"flags<Arith::{op}, {self.dest}>(acc, {a}, {b}, {result})"


class Lower:
    """A lower instruction, decoded as decodeLowerUsage and execLower do."""

    def __init__(self, word, pc, unit=1):
        self.word = word
        self.op = "nop"
        self.vf_reads = []  # (vf register, lanes)
        self.vi_reads = []
        self.vf_write = None  # (register, lanes), latency 4
        self.vi_write = None  # (register, latency)
        self.delays_branch = False
        self.branch = None  # ("b"|"bal"|"cond", target) or ("jr"|"jalr", register)
        self.fdiv = 0  # FDIV latency of DIV, SQRT, RSQRT
        self.waitq = False
        self.efu = 0  # EFU latency
        self.waitp = False
        if word in (0, 0x8000033C):
            return
        hi = word >> 25
        it, is_ = (word >> 16) & 31, (word >> 11) & 31
        self.it, self.is_ = it, is_
        self.vit, self.vis, self.vid = it & 15, is_ & 15, (word >> 6) & 15
        self.dest = (word >> 21) & 15
        self.imm = s11(word & 0x7FF)

        def rvi(reg):
            if reg:
                self.vi_reads.append(reg)

        def wvi(reg, latency):
            if reg:
                self.vi_write = (reg, latency)

        def rvf(reg, lanes):
            if reg and lanes:
                self.vf_reads.append((reg, lanes))

        def wvf(reg, lanes):
            if reg and lanes:
                self.vf_write = (reg, lanes)

        target = (pc + 8 + self.imm * 8) & (0x3FFF if unit else 0x0FFF)
        if hi == 0x00:
            self.op = "lq"; rvi(self.vis); wvf(it, self.dest)
        elif hi == 0x01:
            self.op = "sq"; rvi(self.vit); rvf(is_, self.dest)
        elif hi == 0x04:
            self.op = "ilw"; rvi(self.vis); wvi(self.vit, 4)
        elif hi == 0x05:
            self.op = "isw"; rvi(self.vis); rvi(self.vit)
        elif hi in (0x08, 0x09):
            self.op = "iaddiu" if hi == 0x08 else "isubiu"
            self.imm15 = (word & 0x7FF) | ((word >> 10) & 0x7800)
            rvi(self.vis); wvi(self.vit, 1); self.delays_branch = True
        elif hi in (0x10, 0x12, 0x13):
            self.op = {0x10: "fceq", 0x12: "fcand", 0x13: "fcor"}[hi]
            self.imm24 = word & 0xFFFFFF
            wvi(1, 1)
        elif hi == 0x11:
            self.op = "fcset"; self.imm24 = word & 0xFFFFFF
        elif hi in (0x18, 0x1A, 0x1B):
            self.op = {0x18: "fmeq", 0x1A: "fmand", 0x1B: "fmor"}[hi]
            rvi(self.vis); wvi(self.vit, 1)
        elif hi == 0x1C:
            self.op = "fcget"; wvi(self.vit, 1)
        elif hi == 0x20:
            self.op = "b"; self.branch = ("b", target)
        elif hi == 0x21:
            self.op = "bal"; self.branch = ("bal", target); wvi(self.vit, 1)
        elif hi == 0x24:
            self.op = "jr"; self.branch = ("jr", self.vis); rvi(self.vis)
        elif hi == 0x25:
            self.op = "jalr"; self.branch = ("jalr", self.vis); rvi(self.vis); wvi(self.vit, 1)
        elif hi in (0x28, 0x29, 0x2C, 0x2D, 0x2E, 0x2F):
            self.op = {0x28: "ibeq", 0x29: "ibne", 0x2C: "ibltz", 0x2D: "ibgtz", 0x2E: "iblez", 0x2F: "ibgez"}[hi]
            self.branch = ("cond", target)
            rvi(self.vis)
            if hi in (0x28, 0x29):
                rvi(self.vit)
        elif hi == 0x40:
            funct = word & 0x3F
            if funct in (0x30, 0x31, 0x34, 0x35):
                self.op = {0x30: "iadd", 0x31: "isub", 0x34: "iand", 0x35: "ior"}[funct]
                rvi(self.vis); rvi(self.vit); wvi(self.vid, 1); self.delays_branch = True
            elif funct == 0x32:
                self.op = "iaddi"
                value = (word >> 6) & 31
                self.imm5 = value - 32 if value & 16 else value
                rvi(self.vis); wvi(self.vit, 1); self.delays_branch = True
            elif funct >= 0x3C:
                special = (word & 3) | ((word >> 4) & 0x7C)
                self.fsf, self.ftf = (word >> 21) & 3, (word >> 23) & 3
                d = self.dest
                if special == 0x30:
                    self.op = "move"; rvf(is_, d); wvf(it, d)
                elif special == 0x31:
                    self.op = "mr32"; rvf(is_, (d >> 1) | ((d & 1) << 3)); wvf(it, d)
                elif special in (0x34, 0x36):
                    self.op = "lqi" if special == 0x34 else "lqd"
                    rvi(self.vis); wvi(self.vis, 1); wvf(it, d); self.delays_branch = True
                elif special in (0x35, 0x37):
                    self.op = "sqi" if special == 0x35 else "sqd"
                    rvi(self.vit); wvi(self.vit, 1); rvf(is_, d); self.delays_branch = True
                elif special == 0x38:
                    self.op = "div"; self.fdiv = 7
                    rvf(is_, lane_bit(self.fsf)); rvf(it, lane_bit(self.ftf))
                elif special == 0x39:
                    self.op = "sqrt"; self.fdiv = 7
                    rvf(it, lane_bit(self.ftf))
                elif special == 0x3A:
                    self.op = "rsqrt"; self.fdiv = 13
                    rvf(is_, lane_bit(self.fsf)); rvf(it, lane_bit(self.ftf))
                elif special == 0x3B:
                    self.op = "waitq"; self.waitq = True
                elif special == 0x3C:
                    self.op = "mtir"; rvf(is_, lane_bit(self.fsf)); wvi(self.vit, 1); self.delays_branch = True
                elif special == 0x3D:
                    self.op = "mfir"; rvi(self.vis); wvf(it, d)
                elif special == 0x64:
                    self.op = "mfp"; wvf(it, d)
                elif special in (0x78, 0x79, 0x7A):
                    self.op = {0x78: "esqrt", 0x79: "ersqrt", 0x7A: "ercpr"}[special]
                    self.efu = {0x78: 12, 0x79: 18, 0x7A: 12}[special]
                    rvf(is_, lane_bit(self.fsf))
                elif special == 0x7B:
                    self.op = "waitp"; self.waitp = True
                elif special in (0x68, 0x69):
                    self.op = "xtop" if special == 0x68 else "xitop"; wvi(self.vit, 1)
                elif special == 0x6C:
                    self.op = "xgkick"; rvi(self.vis)
                else:
                    raise TranslationError(f"unsupported lower {word:08x} at {pc:04x}")
                if not unit and (self.op in ("mfp", "waitp", "xgkick") or self.efu):
                    raise TranslationError(f"VU1 instruction {word:08x} at {pc:04x} in a VU0 program")
            else:
                raise TranslationError(f"unsupported lower {word:08x} at {pc:04x}")
        else:
            raise TranslationError(f"unsupported lower {word:08x} at {pc:04x}")


class Pair:
    def __init__(self, code, pc, unit=1):
        lower, upper = struct.unpack_from("<II", code, pc)
        self.pc = pc
        self.ibit = bool(upper >> 31 & 1)
        self.ebit = bool(upper >> 30 & 1)
        if upper >> 27 & 3:
            raise TranslationError(f"D or T bit at {pc:04x}")
        self.upper = Upper(upper)
        self.imm = lower if self.ibit else None
        self.lower = Lower(0 if self.ibit else lower, pc, unit)
        u, l = self.upper, self.lower
        if u.kind == "clip" and l.op == "fcset":
            raise TranslationError(f"CLIP with FCSET at {pc:04x}")
        # decodeInstructionWords: a lower write to the register the upper writes is dropped.
        if u.out and u.out[0] == "vf" and l.vf_write and l.vf_write[0] == u.out[1]:
            self.lower_vf_write = None
        else:
            self.lower_vf_write = l.vf_write

    def vf_reads(self):
        return self.upper.reads + self.lower.vf_reads


class State:
    """The pipeline between two pairs. Times count cycles from the next issue cycle: a result
    with time t is visible to a pair issued t cycles or more after it (always t >= 1)."""

    def __init__(self):
        self.vf = {}  # (register, lane) -> time its value is written
        self.vi = {}  # register -> time
        self.pending = ()  # (time, "mac"|"clip"|"q"|"p", producer pc), in issue order
        self.backup = 0  # VI register the last pair wrote, read old by a branch right after it
        self.efu = 0  # time until the EFU takes another instruction

    def key(self):
        return (tuple(sorted(self.vf.items())), tuple(sorted(self.vi.items())), self.pending, self.backup, self.efu)

    def copy(self):
        other = State()
        other.vf, other.vi, other.pending, other.backup = dict(self.vf), dict(self.vi), self.pending, self.backup
        other.efu = self.efu
        return other

    def latest(self, efu=True):
        """When everything in flight has landed; flushPipelines does not wait for the EFU itself."""
        return max([0, self.efu if efu else 0] + list(self.vf.values()) + list(self.vi.values()) +
                   [t for t, _, _ in self.pending])


class Values:
    """Sets of values for the VI registers and the integer words at fixed VU1 addresses,
    for register_jumps. A set holds int16 values and start values: (token, facts), where the
    token is ("vi", reg) or ("mem", qword, lane), what the start finds there, and the facts
    are what the path to the pair has found about start values, (token, "eq0"|"ne0") from an
    IBEQ or IBNE against vi0. None is any value. A state is (registers, words, lanes stored at
    unknown addresses, facts of every path to the pair)."""

    LIMIT = 64

    def __init__(self, words=0x3FF):
        self.words = words  # qword address mask of the data memory

    def start(self):
        regs = [frozenset({0})] + [frozenset({(("vi", reg), frozenset())}) for reg in range(1, 16)]
        return tuple(regs), (), frozenset(), frozenset()

    @staticmethod
    def reg(state, reg):
        return state[0][reg]

    @staticmethod
    def read(state, token, words=None):
        if token[0] == "vi":
            return state[0][token[1]]
        words = dict(state[1]) if words is None else words
        key = (token[1], token[2])
        if key in words:
            return words[key]
        return None if token[2] in state[2] else frozenset({(token, state[3])})

    def merge(self, a, b):
        regs = tuple(None if x is None or y is None else self.limit(x | y) for x, y in zip(a[0], b[0]))
        wa, wb = dict(a[1]), dict(b[1])
        words = []
        for key in sorted(wa.keys() | wb.keys()):
            x, y = self.read(a, ("mem",) + key, wa), self.read(b, ("mem",) + key, wb)
            words.append((key, None if x is None or y is None else self.limit(x | y)))
        return regs, tuple(words), a[2] | b[2], a[3] & b[3]

    @staticmethod
    def assume(state, fact):
        """The state on a branch edge that tells fact about a start value."""
        opposite = (fact[0], "ne0" if fact[1] == "eq0" else "eq0")

        def tag(values):
            if values is None:
                return None
            return frozenset(v if isinstance(v, int) else (v[0], v[1] | {fact})
                             for v in values if isinstance(v, int) or opposite not in v[1])
        return (tuple(tag(r) for r in state[0]), tuple((k, tag(v)) for k, v in state[1]), state[2],
                state[3] | {fact})

    def limit(self, values):
        return None if len(values) > self.LIMIT else frozenset(values)

    def arith(self, a, b, operation):
        if a is None or b is None or not all(isinstance(v, int) for v in a | b):
            return None
        return self.limit({s16(operation(x, y)) for x in a for y in b})

    def transfer(self, p, state):
        l = p.lower
        regs, words, clobbered = list(state[0]), dict(state[1]), set(state[2])

        def get(reg):
            return regs[reg]

        def put(reg, value):
            if reg:
                regs[reg] = value

        def address(reg, offset):
            held = get(reg)
            if held is not None and len(held) == 1:
                (value,) = held
                if isinstance(value, int):
                    return (value + offset) & self.words
            return None

        def store(qword, lanes, value):
            for lane in lanes:
                if qword is None:
                    clobbered.add(lane)
                    for key in list(words):
                        if key[1] == lane:
                            words[key] = None
                else:
                    words[(qword, lane)] = value

        op = l.op
        lanes = lanes_of(l.dest) if op in ("sq", "isw", "sqi", "sqd") else []
        if op == "ilw":
            qword = address(l.vis, l.imm)
            lane = lanes_of(l.dest)[0] if l.dest else 0
            put(l.vit, None if qword is None else self.read((regs, (), clobbered, state[3]), ("mem", qword, lane), words))
        elif op == "isw":
            store(address(l.vis, l.imm), lanes, get(l.vit))
        elif op == "sq":
            store(address(l.vit, l.imm), lanes, None)
        elif op in ("sqi", "sqd"):
            if op == "sqd":
                put(l.vit, self.arith(get(l.vit), frozenset({1}), lambda x, y: x - y))
            store(address(l.vit, 0), lanes, None)
            if op == "sqi":
                put(l.vit, self.arith(get(l.vit), frozenset({1}), lambda x, y: x + y))
        elif op in ("lqi", "lqd"):
            put(l.vis, self.arith(get(l.vis), frozenset({1}), (lambda x, y: x + y) if op == "lqi" else (lambda x, y: x - y)))
        elif op in ("iaddiu", "isubiu"):
            if l.imm15 == 0:
                put(l.vit, get(l.vis))
            else:
                sign = 1 if op == "iaddiu" else -1
                put(l.vit, self.arith(get(l.vis), frozenset({sign * l.imm15}), lambda x, y: x + y))
        elif op == "iaddi":
            put(l.vit, get(l.vis) if l.imm5 == 0 else self.arith(get(l.vis), frozenset({l.imm5}), lambda x, y: x + y))
        elif op in ("iadd", "isub", "iand", "ior"):
            a, b = get(l.vis), get(l.vit)
            if op in ("iadd", "ior") and l.vis == 0:
                put(l.vid, b)
            elif op in ("iadd", "isub", "ior") and l.vit == 0:
                put(l.vid, a)
            else:
                operation = {"iadd": lambda x, y: x + y, "isub": lambda x, y: x - y,
                             "iand": lambda x, y: x & y, "ior": lambda x, y: x | y}[op]
                put(l.vid, self.arith(a, b, operation))
        elif op in ("fceq", "fcand", "fcor"):
            put(1, frozenset({0, 1}))
        elif op == "fmeq":
            put(l.vit, frozenset({0, 1}))
        elif op in ("bal", "jalr"):
            put(l.vit, frozenset({(p.pc + 16) // 8}))
        elif l.vi_write:
            put(l.vi_write[0], None)
        return tuple(regs), tuple(sorted(words.items())), frozenset(clobbered), state[3]


def s16(value):
    value &= 0xFFFF
    return value - 0x10000 if value & 0x8000 else value


class Block:
    def __init__(self, label):
        self.label = label
        # str, or ("macflags", producer, code) / ("maccommit", producer) / ("macread",)
        self.lines = [f"{label}:"]
        self.successors = []


class Translator:
    def __init__(self, code, entry, name, unit=1):
        self.code = code
        self.entry = entry
        self.name = name
        self.unit = unit
        self.code_mask = 0x3FFF if unit else 0x0FFF  # byte addresses
        self.words = 0x3FF if unit else 0xFF  # qword addresses of the data memory
        # Memory helpers' template arguments: VU1's are the defaults.
        self.lq_args = "" if unit else "<0xFF>"
        self.sq_args = "" if unit else ", 0xFF"
        self.pairs = {}
        self.labels = {}
        self.work = []
        self.blocks = []
        self.leaders = {entry}
        self.jr_targets = {}  # JR or JALR address -> the addresses it can jump to
        self.entry_checks = {}  # (start token, facts) -> values the start must hold when the facts are true
        self.delay_slot_of = {}
        self.ends = set()
        self.clip_producers = set()
        self.mac_producers = set()
        self.efu_producers = set()
        self.wait_blocks = 0

    def pair(self, pc):
        if pc not in self.pairs:
            self.pairs[pc] = Pair(self.code, pc, self.unit)
        return self.pairs[pc]

    def discover(self):
        """The pairs the program can run, with the targets of its register jumps: walk the
        code with the targets known so far, bound the registers, again until nothing new."""
        while True:
            self.walk()
            targets, checks = self.register_jumps()
            if targets == self.jr_targets:
                self.entry_checks = checks
                return
            self.jr_targets = targets

    def walk(self):
        self.seen = set()
        stack = [self.entry]
        while stack:
            pc = stack.pop()
            while pc not in self.seen:
                self.seen.add(pc)
                p = self.pair(pc)
                if p.lower.branch:
                    if p.ebit:
                        raise TranslationError(f"E bit on a branch at {pc:04x}")
                    kind, arg = p.lower.branch
                    delay = self.pair(pc + 8)
                    if delay.lower.branch or delay.ebit:
                        raise TranslationError(f"branch or E bit in the delay slot at {pc + 8:04x}")
                    self.delay_slot_of[pc + 8] = pc
                    self.seen.add(pc + 8)
                    if kind in ("b", "bal", "cond"):
                        self.leaders.add(arg)
                        stack.append(arg)
                    if kind in ("cond", "bal", "jalr"):
                        self.leaders.add(pc + 16)
                        stack.append(pc + 16)
                    if kind in ("jr", "jalr"):
                        self.leaders |= self.jr_targets.get(pc, set())
                        stack += sorted(self.jr_targets.get(pc, ()))
                    break
                if p.ebit:
                    delay = self.pair(pc + 8)
                    if delay.lower.branch or delay.ebit:
                        raise TranslationError(f"branch or E bit after the E bit at {pc + 8:04x}")
                    self.seen.add(pc + 8)
                    self.ends.add(pc)
                    break
                pc += 8

    def successors(self, pc):
        """Static successors of the pair at pc."""
        p = self.pair(pc)
        if p.lower.branch or p.ebit:
            return [pc + 8]
        if pc - 8 in self.ends:
            return []
        branch_pc = self.delay_slot_of.get(pc)
        if branch_pc is not None:
            kind, arg = self.pair(branch_pc).lower.branch
            if kind in ("b", "bal"):
                return [arg]
            if kind == "cond":
                return [arg, pc + 8]
            return sorted(self.jr_targets.get(branch_pc, ()))
        return [pc + 8]

    def register_jumps(self):
        """Where each JR and JALR can go. The VI registers and the integer words at fixed
        VU1 addresses get the sets of values they can hold, in program order (a pair waits
        for the VI values it reads); a value the program finds at its start is a token,
        which the C++ checks before it runs against the values the program leaves at its
        end, so that the next start resumes at an address the translation has."""
        if not any(self.pair(pc).lower.op in ("jr", "jalr") for pc in self.seen):
            return {}, {}
        values = Values(self.words)
        entering = {self.entry: values.start()}
        work = [self.entry]
        while work:
            pc = work.pop()
            after = values.transfer(self.pair(pc), entering[pc])
            for succ in self.successors(pc):
                fact = self.branch_fact(pc, succ, entering, values)
                out = values.assume(after, fact) if fact else after
                merged = out if succ not in entering else values.merge(entering[succ], out)
                if succ not in entering or merged != entering[succ]:
                    entering[succ] = merged
                    work.append(succ)
        # What the next start finds: the registers at the ends, and at the E bit right before
        # the entry, where the previous run stopped when the game continues with MSCNT; the
        # words the program stores at fixed addresses. The checks make any guess safe.
        ends = [values.transfer(self.pair(end + 8), entering[end + 8]) for end in self.ends if end + 8 in entering]
        if self.entry >= 16 and Pair(self.code, self.entry - 16, self.unit).ebit:  # a guess: not part of the translation
            before = values.transfer(Pair(self.code, self.entry - 16, self.unit), values.start())
            ends.append(values.transfer(Pair(self.code, self.entry - 8, self.unit), before))
        stored = {}
        for pc, state in entering.items():
            l = self.pair(pc).lower
            held = values.reg(state, l.vit) if l.op == "isw" else None
            base = values.reg(state, l.vis) if l.op == "isw" else None
            if held is not None and base is not None and len(base) == 1 and isinstance(next(iter(base)), int):
                qword = (next(iter(base)) + l.imm) & self.words
                for lane in lanes_of(l.dest):
                    stored.setdefault((qword, lane), set()).update(v for v in held if isinstance(v, int))

        def allowed(token):
            if token[0] == "vi":
                final = set().union(*(state[0][token[1]] or () for state in ends)) if ends else set()
            else:
                final = stored.get(token[1:], set())
            numbers = {v for v in final if isinstance(v, int)}
            if not numbers:
                raise TranslationError(f"the program leaves no known value in {token} for its next start")
            return numbers

        targets, facts = {}, {}
        for pc in sorted(self.seen):
            p = self.pair(pc)
            if p.lower.op not in ("jr", "jalr") or pc not in entering:
                continue
            reg = p.lower.branch[1]
            held = values.reg(entering[pc], reg)
            if held is None:
                raise TranslationError(f"JR vi{reg} at {pc:04x} to an address the translator cannot bound")
            numbers = set()
            for value in held:
                if isinstance(value, int):
                    numbers.add(value)
                else:
                    token = value[0]
                    # One check a start value, under what every path that jumps through it found.
                    facts[token] = facts[token] & value[1] if token in facts else value[1]
                    numbers |= allowed(token)
            targets[pc] = {(value & 0xFFFF) * 8 & self.code_mask for value in numbers}
        return targets, {(token, found): allowed(token) for token, found in facts.items()}

    def branch_fact(self, pc, succ, entering, values):
        """What the edge from the delay slot at pc to succ tells about a start value: an IBEQ or
        IBNE of a start value against vi0, reached only from the pair before it (a branch right
        after an integer write of its register reads the old value: no fact then)."""
        branch_pc = self.delay_slot_of.get(pc)
        if branch_pc is None or branch_pc in self.leaders or branch_pc < 8 or branch_pc not in entering:
            return None
        l = self.pair(branch_pc).lower
        if l.op not in ("ibeq", "ibne") or (l.vis and l.vit) or l.branch[1] == branch_pc + 16:
            return None
        reg = l.vis or l.vit
        before = self.pair(branch_pc - 8).lower
        if not reg or (before.delays_branch and before.vi_write and before.vi_write[0] == reg):
            return None
        held = values.reg(entering[branch_pc], reg)
        if held is None or len(held) != 1 or isinstance(next(iter(held)), int):
            return None
        equal = (succ == l.branch[1]) == (l.op == "ibeq")
        return next(iter(held))[0], "eq0" if equal else "ne0"

    def label(self, pc, state, pend=None, resolved=False):
        key = (pc, state.key(), pend, resolved)
        if key not in self.labels:
            label = f"L{pc:04x}_{sum(1 for k in self.labels if k[0] == pc)}"
            self.labels[key] = label
            self.work.append((label, pc, state.copy(), pend, resolved))
        return self.labels[key]

    # -- the cycle model ----------------------------------------------------------------
    def advance(self, state, cycles, lines):
        """cycles pass: what lands meanwhile commits, in issue order."""
        if cycles <= 0:
            return
        keep = []
        for t, kind, producer in state.pending:
            if t <= cycles:
                if kind == "mac":
                    lines.append(("maccommit", producer))
                elif kind == "clip":
                    lines.append(f"    clip = clip_{producer:04x};")
                elif kind == "p":
                    lines.append(f"    p = p_{producer:04x};")
                else:
                    lines.append("    q = qp;")
            else:
                keep.append((t - cycles, kind, producer))
        state.pending = tuple(keep)
        state.vf = {key: t - cycles for key, t in state.vf.items() if t > cycles}
        state.vi = {reg: t - cycles for reg, t in state.vi.items() if t > cycles}
        state.efu = max(0, state.efu - cycles)

    def stall(self, p, state):
        ready = 0
        for reg, lanes in p.vf_reads():
            for lane in lanes_of(lanes):
                ready = max(ready, state.vf.get((reg, lane), 0))
        for reg in p.lower.vi_reads:
            ready = max(ready, state.vi.get(reg, 0))
        if p.lower.fdiv or p.lower.waitq:
            for t, kind, _ in state.pending:
                if kind == "q":
                    ready = max(ready, t)
        if p.lower.efu or p.lower.waitp:
            ready = max(ready, state.efu)
        if p.lower.waitp:
            for t, kind, _ in state.pending:
                if kind == "p":
                    ready = max(ready, t)
        return ready

    def issued(self, p, state):
        """p issues at time 0: what it puts in the pipeline."""
        u, l = p.upper, p.lower
        for t, kind, producer in state.pending:
            if producer == p.pc and kind != "q":
                raise TranslationError(f"{p.pc:04x} issues again before its flags land")
        if u.out and u.out[0] == "vf":
            for lane in lanes_of(u.dest):
                state.vf[(u.out[1], lane)] = FMAC_LATENCY
        if p.lower_vf_write:
            reg, lanes = p.lower_vf_write
            for lane in lanes_of(lanes):
                state.vf[(reg, lane)] = 4
        if l.vi_write:
            reg, latency = l.vi_write
            state.vi[reg] = latency
        pending = list(state.pending)
        if u.mac:
            pending.append((FMAC_LATENCY, "mac", p.pc))
            self.mac_producers.add(p.pc)
        if u.kind == "clip" or l.op == "fcset":
            pending.append((FMAC_LATENCY, "clip", p.pc))
            self.clip_producers.add(p.pc)
        if l.fdiv:
            pending.append((l.fdiv, "q", p.pc))
        if l.efu:
            if sum(1 for _, kind, _ in pending if kind == "p") >= 2:
                raise TranslationError(f"a third EFU result in flight at {p.pc:04x}")
            pending.append((l.efu, "p", p.pc))
            self.efu_producers.add(p.pc)
            state.efu = l.efu - 1
        state.pending = tuple(pending)
        state.backup = l.vi_write[0] if l.delays_branch and l.vi_write else 0

    # -- C++ for one pair ---------------------------------------------------------------
    def emit_pair(self, p, state, lines, successors):
        u = p.upper
        lines.append(f"    {{ // {p.pc:04x}")
        result = u.result()
        if result is not None:
            lines.append(f"    const V u = {result};")
            if u.mac:
                lines.append(("macflags", p.pc, f"    mac_{p.pc:04x} = {u.flags('u')};"))
        if u.kind == "clip":
            lines.append(f"    clipW = ((clipW << 6) | ops::clip({vf(u.fs)}, {vf(u.ft)})) & 0xFFFFFFu;")
            lines.append(f"    clip_{p.pc:04x} = clipW;")
        lines += self.lower_code(p, state, successors)
        if result is not None and u.out:
            target = "acc" if u.out[0] == "acc" else vf(u.out[1])
            lines.append(f"    {target} = blend<{u.dest}>({target}, u);")
        if p.lower_vf_write:
            reg, lanes = p.lower_vf_write
            lines.append(f"    {vf(reg)} = blend<{lanes}>({vf(reg)}, lv);")
        if p.ibit:
            lines.append(f"    iBits = 0x{p.imm:08x}u; iR = opaque(bits(0x{p.imm:08x}u)); iN = opaque(bits(0x{normalized_bits(p.imm):08x}u));")
        lines.append("    }")

    def lower_code(self, p, state, successors):
        l = p.lower
        out = []
        op = l.op
        if op in ("nop", "waitq", "waitp", "b"):
            return out
        # A conditional branch right after this pair reads the register's old value.
        if l.delays_branch and l.vi_write:
            reg = l.vi_write[0]
            for succ in successors:
                s = self.pair(succ)
                if s.lower.branch and s.lower.branch[0] == "cond" and reg in s.lower.vi_reads:
                    out.append(f"    viOld = {vi(reg)};")
                    break

        def branch_vi(reg):
            if reg == 0:
                return "0"
            return "viOld" if state.backup == reg else vi(reg)

        def address(reg):
            return f"(static_cast<uint32_t>(static_cast<uint16_t>({vi(reg)})) * 8u) & 0x{self.code_mask:X}u"

        if op == "lq":
            out.append(f"    const V lv = lq{self.lq_args}(mem, {vi(l.vis)} + {l.imm});")
        elif op == "sq":
            out.append(f"    sq<{l.dest}{self.sq_args}>(mem, {vi(l.vit)} + {l.imm}, {vf(l.is_)});")
        elif op == "ilw":
            if l.vit:
                comp = lanes_of(l.dest)[0] if l.dest else 0
                out.append(f"    {vi(l.vit)} = ilw{self.lq_args}(mem, {vi(l.vis)} + {l.imm}, {comp});")
        elif op == "isw":
            out.append(f"    isw<{l.dest}{self.sq_args}>(mem, {vi(l.vis)} + {l.imm}, {vi(l.vit)});")
        elif op in ("iaddiu", "isubiu"):
            if l.vit:
                sign = "+" if op == "iaddiu" else "-"
                out.append(f"    {vi(l.vit)} = vi16({vi(l.vis)} {sign} {l.imm15});")
        elif op in ("iadd", "isub", "iand", "ior"):
            if l.vid:
                sym = {"iadd": "+", "isub": "-", "iand": "&", "ior": "|"}[op]
                out.append(f"    {vi(l.vid)} = vi16({vi(l.vis)} {sym} {vi(l.vit)});")
        elif op == "iaddi":
            if l.vit:
                out.append(f"    {vi(l.vit)} = vi16({vi(l.vis)} + {l.imm5});")
        elif op == "fcand":
            out.append(f"    vi1 = (clip & 0x{l.imm24:06x}u) != 0u ? 1 : 0;")
        elif op == "fcor":
            out.append(f"    vi1 = (clip | 0x{l.imm24:06x}u) == 0xFFFFFFu ? 1 : 0;")
        elif op == "fceq":
            out.append(f"    vi1 = (clip & 0xFFFFFFu) == 0x{l.imm24:06x}u ? 1 : 0;")
        elif op == "fcget":
            if l.vit:
                out.append(f"    {vi(l.vit)} = static_cast<int32_t>(clip & 0xFFFu);")
        elif op == "fcset":
            out.append(f"    clipW = 0x{l.imm24:06x}u;")
            out.append(f"    clip_{p.pc:04x} = clipW;")
        elif op in ("fmand", "fmor", "fmeq"):
            out.append(("macread",))
            if l.vit:
                mask = f"static_cast<uint32_t>(static_cast<uint16_t>({vi(l.vis)}))"
                expr = {"fmand": f"vi16(static_cast<int32_t>(mac & {mask}))",
                        "fmor": f"vi16(static_cast<int32_t>(mac | {mask}))",
                        "fmeq": f"(mac & 0xFFFFu) == {mask} ? 1 : 0"}[op]
                out.append(f"    {vi(l.vit)} = {expr};")
        elif op == "bal":
            out.append(f"    {vi(l.vit)} = {(p.pc + 16) // 8};")
        elif op == "jalr":
            out.append(f"    jump = {address(l.vis)};")
            out.append(f"    {vi(l.vit)} = {(p.pc + 16) // 8};")
        elif op == "jr":
            out.append(f"    jump = {address(l.vis)};")
        elif op in ("ibeq", "ibne", "ibltz", "ibgtz", "iblez", "ibgez"):
            a = f"static_cast<int16_t>({branch_vi(l.vis)})"
            b = f"static_cast<int16_t>({branch_vi(l.vit)})"
            cond = {"ibeq": f"{a} == {b}", "ibne": f"{a} != {b}", "ibltz": f"{a} < 0",
                    "ibgtz": f"{a} > 0", "iblez": f"{a} <= 0", "ibgez": f"{a} >= 0"}[op]
            out.append(f"    taken = {cond};")
        elif op == "move":
            out.append(f"    const V lv = {vf(l.is_)};")
        elif op == "mr32":
            out.append(f"    const V lv = _mm_shuffle_ps({vf(l.is_)}, {vf(l.is_)}, _MM_SHUFFLE(0, 3, 2, 1));")
        elif op == "lqi":
            out.append(f"    const V lv = lq{self.lq_args}(mem, {vi(l.vis)});")
            if l.vis:
                out.append(f"    {vi(l.vis)} = vi16({vi(l.vis)} + 1);")
        elif op == "lqd":
            if l.vis:
                out.append(f"    {vi(l.vis)} = vi16({vi(l.vis)} - 1);")
            out.append(f"    const V lv = lq{self.lq_args}(mem, {vi(l.vis)});")
        elif op == "sqi":
            out.append(f"    sq<{l.dest}{self.sq_args}>(mem, {vi(l.vit)}, {vf(l.is_)});")
            if l.vit:
                out.append(f"    {vi(l.vit)} = vi16({vi(l.vit)} + 1);")
        elif op == "sqd":
            if l.vit:
                out.append(f"    {vi(l.vit)} = vi16({vi(l.vit)} - 1);")
            out.append(f"    sq<{l.dest}{self.sq_args}>(mem, {vi(l.vit)}, {vf(l.is_)});")
        elif op == "div":
            out.append(f"    qp = div(lane<{l.fsf}>({vf(l.is_)}), lane<{l.ftf}>({vf(l.it)}));")
        elif op == "sqrt":
            out.append(f"    qp = sqrtQ(lane<{l.ftf}>({vf(l.it)}));")
        elif op == "rsqrt":
            out.append(f"    qp = rsqrtQ(lane<{l.fsf}>({vf(l.is_)}), lane<{l.ftf}>({vf(l.it)}));")
        elif op in ("esqrt", "ersqrt", "ercpr"):
            out.append(f"    p_{p.pc:04x} = {op}(lane<{l.fsf}>({vf(l.is_)}));")
        elif op == "mfp":
            out.append("    const V lv = splat(p);")
        elif op == "mtir":
            if l.vit:
                out.append(f"    {vi(l.vit)} = static_cast<int16_t>(laneBits<{l.fsf}>({vf(l.is_)}) & 0xFFFFu);")
        elif op == "mfir":
            out.append(f"    const V lv = opaque(bits(static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>({vi(l.vis)})))));")
        elif op in ("xtop", "xitop"):
            if l.vit:
                out.append(f"    {vi(l.vit)} = static_cast<int32_t>(s.{op[1:]} & 0x3FFu);")
        elif op == "xgkick":
            out.append(f"    kickEnd = xgkick(c, {vi(l.vis)}, cycle);")
        else:
            raise TranslationError(op)
        return out

    # -- blocks -------------------------------------------------------------------------
    def transfer_successors(self, pend):
        kind = pend[0]
        if kind in ("b", "bal"):
            return [pend[1]]
        if kind == "cond":
            return [pend[1], pend[2]]
        if kind in ("jr", "jalr"):
            return sorted(self.jr_targets[pend[2]])
        return []

    def translate_block(self, label, pc, state, pend, resolved):
        block = Block(label)
        lines = block.lines
        cycles = 0
        while True:
            p = self.pair(pc)
            stall = self.stall(p, state)
            if resolved and stall:
                raise TranslationError(f"stall after the XGKICK wait at {pc:04x}")
            self.advance(state, stall, lines)
            cycles += stall
            if p.lower.op == "xgkick" and not resolved:
                # PATH1 may still send the last packet: the pair waits for it, cycle by cycle.
                latest = state.latest()
                lines.append(f"    cycle += {cycles};")
                lines.append("    wait = kickEnd > cycle ? kickEnd - cycle : 0u;")
                lines.append("    cycle += wait;")
                if latest == 0:
                    # Nothing in flight: the wait changes only the cycle count.
                    target = self.label(pc, state, pend, True)
                    lines.append(f"    goto {target};")
                    block.successors.append(target)
                    break
                lines.append(f"    switch (wait < {latest}u ? wait : {latest}u) {{")
                for waited in range(latest + 1):
                    after = state.copy()
                    commits = []
                    self.advance(after, waited, commits)
                    target = self.label(pc, after, pend, True)
                    case = "default" if waited == latest else f"case {waited}u"
                    if commits:
                        # What lands during the wait commits on its own path.
                        self.wait_blocks += 1
                        wait_block = Block(f"W{pc:04x}_{self.wait_blocks}")
                        wait_block.lines += commits + [f"    goto {target};"]
                        wait_block.successors.append(target)
                        self.blocks.append(wait_block)
                        lines.append(f"    {case}: goto {wait_block.label};")
                        block.successors.append(wait_block.label)
                    else:
                        lines.append(f"    {case}: goto {target};")
                        block.successors.append(target)
                lines.append("    }")
                break
            resolved = False
            if pend and pend[0] == "end":
                successors = []
            elif pend:
                successors = self.transfer_successors(pend)
            else:
                successors = [pc + 8]
            self.emit_pair(p, state, lines, successors)
            self.issued(p, state)
            self.advance(state, 1, lines)
            cycles += 1
            if pend and pend[0] == "end":
                # flushPipelines: the cycles until everything lands, or PATH1 is done.
                latest = state.latest(efu=False)
                lines.append(f"    cycle += {cycles + latest};")
                lines.append("    if (kickEnd > cycle) cycle = kickEnd;")
                self.advance(state, latest, lines)
                lines.append(("macread",))
                lines.append(f"    endPc = 0x{(pc + 8) & self.code_mask:04x}u;")
                lines.append("    goto finish;")
                break
            if pend:
                lines.append(f"    cycle += {cycles};")
                kind = pend[0]
                if kind in ("b", "bal"):
                    target = self.label(pend[1], state)
                    lines.append(f"    goto {target};")
                    block.successors.append(target)
                elif kind == "cond":
                    taken, fallthrough = self.label(pend[1], state), self.label(pend[2], state)
                    lines.append(f"    if (taken) goto {taken};")
                    lines.append(f"    goto {fallthrough};")
                    block.successors += [taken, fallthrough]
                else:
                    lines.append("    switch (jump) {")
                    for target_pc in sorted(self.jr_targets[pend[2]]):
                        target = self.label(target_pc, state)
                        lines.append(f"    case 0x{target_pc:04x}u: goto {target};")
                        block.successors.append(target)
                    lines.append("    default: unexpectedJump(c, jump); return true;")
                    lines.append("    }")
                break
            if p.lower.branch:
                kind, arg = p.lower.branch
                pend = ("cond", arg, pc + 16) if kind == "cond" else (kind, arg, pc)
                pc += 8
                continue
            if p.ebit:
                pend = ("end",)
                pc += 8
                continue
            pc += 8
            if pc in self.leaders:
                lines.append(f"    cycle += {cycles};")
                target = self.label(pc, state)
                lines.append(f"    goto {target};")
                block.successors.append(target)
                break
        self.blocks.append(block)

    def translate(self):
        self.discover()
        self.entry_label = self.label(self.entry, State())
        while self.work:
            if len(self.labels) > 20000:
                raise TranslationError("too many pipeline states")
            self.translate_block(*self.work.pop())
        self.needed_flags()

    def needed_flags(self):
        """The MAC producers whose flags an FMxxx or the end of the program can see."""
        by_label = {b.label: b for b in self.blocks}
        entering = {b.label: set() for b in self.blocks}
        entering[self.entry_label] = {"init"}
        needed = set()
        work = [self.entry_label]
        while work:
            block = by_label[work.pop()]
            current = set(entering[block.label])
            for line in block.lines:
                if isinstance(line, tuple):
                    if line[0] == "maccommit":
                        current = {line[1]}
                    elif line[0] == "macread":
                        needed |= current
            for succ in block.successors:
                if not current <= entering[succ]:
                    entering[succ] |= current
                    work.append(succ)
        self.needed = needed - {"init"}

    def function(self):
        """The C++ function, run_<name>, after the parts it calls when the translation is long."""
        head = ["    uint8_t *const mem = c.data;", "    VU1State &s = *c.state;",
                "    const V f0 = opaque(_mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f));"]
        out = []
        out += [f"    V f{reg} = load(s.vf[{reg}]);" for reg in range(1, 32)]
        out += [f"    int32_t vi{reg} = static_cast<int16_t>(s.vi[{reg}]);" for reg in range(1, 16)]
        out += ["    V acc = load(s.acc);",
                "    float q = s.q, qp = 0.0f, p = s.p;",
                "    uint32_t iBits;",
                "    std::memcpy(&iBits, &s.i, sizeof(iBits));",
                "    V iR = bits(iBits), iN = normalize(iR);",
                "    uint32_t mac = s.mac, clip = s.clip, clipW = s.clip;",
                "    uint64_t cycle = 0, kickEnd = 0, wait = 0;",
                "    int32_t viOld = 0;",
                "    bool taken = false;",
                "    uint32_t jump = 0, endPc = 0;"]
        out += [f"    uint32_t mac_{producer:04x} = 0;" for producer in sorted(self.needed)]
        out += [f"    uint32_t clip_{producer:04x} = 0;" for producer in sorted(self.clip_producers)]
        out += [f"    float p_{producer:04x} = 0.0f;" for producer in sorted(self.efu_producers)]
        out += ["    (void)wait; (void)viOld; (void)taken; (void)jump; (void)qp; (void)clipW; (void)iR; (void)iN; (void)p;"]
        # What the program finds at its start and jumps through: other values, the microcode runs.
        def start_value(token):
            return vi(token[1]) if token[0] == "vi" else f"ilw{self.lq_args}(mem, {token[1]}, {token[2]})"
        for (token, facts), numbers in sorted(self.entry_checks.items(), key=lambda item: (item[0][0], sorted(item[0][1]))):
            test = [f"{start_value(t)} {'==' if kind == 'eq0' else '!='} 0" for t, kind in sorted(facts)]
            test += [f"{start_value(token)} != {number}" for number in sorted(numbers)]
            out.append(f"    if ({' && '.join(test)})")
            out.append("        return false;")
        blocks = []
        for block in sorted(self.blocks, key=lambda b: b.label):
            lines = []
            for line in block.lines:
                if isinstance(line, tuple):
                    if line[0] == "macflags" and line[1] in self.needed:
                        lines.append(line[2])
                    elif line[0] == "maccommit" and line[1] in self.needed:
                        lines.append(f"    mac = mac_{line[1]:04x};")
                else:
                    lines.append(line)
            blocks.append((block.label, lines))
        finish = [f"    store(s.vf[{reg}], f{reg});" for reg in range(1, 32)]
        finish += [f"    s.vi[{reg}] = vi{reg};" for reg in range(1, 16)]
        finish += ["    store(s.acc, acc);", "    s.q = q;", "    std::memcpy(&s.i, &iBits, sizeof(iBits));",
                   "    s.p = p;", "    s.mac = mac;", "    s.clip = clip;", "    s.pc = endPc;", "    c.cycles = cycle;", "    return true;"]
        if sum(len(lines) for _, lines in blocks) > SPLIT_LINES:
            return self.parts(head, out, blocks, finish)
        return ([f"bool run_{self.name}(Context &c)", "{"] + head + out + [f"    goto {self.entry_label};"] +
                [line for _, lines in blocks for line in lines] + ["finish:"] + finish + ["}"])

    def loops(self):
        """Each block's loop: the strongly connected component of the block graph it is in (Tarjan)."""
        successors = {block.label: block.successors for block in self.blocks}
        index, low, stack, on, loop = {}, {}, [], set(), {}
        for root in sorted(successors):
            if root in index:
                continue
            index[root] = low[root] = len(index)
            stack.append(root)
            on.add(root)
            work = [(root, iter(successors[root]))]
            while work:
                label, rest = work[-1]
                for succ in rest:
                    if succ not in index:
                        index[succ] = low[succ] = len(index)
                        stack.append(succ)
                        on.add(succ)
                        work.append((succ, iter(successors[succ])))
                        break
                    if succ in on:
                        low[label] = min(low[label], index[succ])
                else:
                    work.pop()
                    if work:
                        low[work[-1][0]] = min(low[work[-1][0]], low[label])
                    if low[label] == index[label]:
                        while True:
                            member = stack.pop()
                            on.discard(member)
                            loop[member] = label
                            if member == label:
                                break
        return loop

    def parts(self, head, start, blocks, finish):
        """A long translation as parts of about PART_LINES lines, each a function that takes the registers
        from a struct and hands them back when it jumps to another part's label. A loop stays in one part
        (it can be longer), so a part changes only on entering or leaving a loop."""
        variables = ([("V", f"f{reg}") for reg in range(1, 32)] + [("int32_t", f"vi{reg}") for reg in range(1, 16)] +
                     [("V", "acc"), ("float", "q"), ("float", "qp"), ("float", "p"), ("uint32_t", "iBits"), ("V", "iR"),
                      ("V", "iN"), ("uint32_t", "mac"), ("uint32_t", "clip"), ("uint32_t", "clipW"), ("uint64_t", "cycle"),
                      ("uint64_t", "kickEnd"), ("uint64_t", "wait"), ("int32_t", "viOld"), ("bool", "taken"),
                      ("uint32_t", "jump"), ("uint32_t", "endPc")] +
                     [("uint32_t", f"mac_{producer:04x}") for producer in sorted(self.needed)] +
                     [("uint32_t", f"clip_{producer:04x}") for producer in sorted(self.clip_producers)] +
                     [("float", f"p_{producer:04x}") for producer in sorted(self.efu_producers)])
        loop, units = self.loops(), {}
        for label, lines in blocks:
            units.setdefault(loop[label], []).append((label, lines))
        groups, size = [[]], 0
        for unit in units.values():
            length = sum(len(lines) for _, lines in unit)
            if groups[-1] and size + length > PART_LINES:
                groups.append([])
                size = 0
            groups[-1] += unit
            size += length
        # Numbered part after part: run_<name> sends a label to its part by range.
        ids = {label: index for index, (label, _) in enumerate(entry for group in groups for entry in group)}
        group_of = {label: index for index, group in enumerate(groups) for label, _ in group}
        finish_id, abort_id = len(blocks), len(blocks) + 1
        entered = [set() for _ in groups]
        entered[group_of[self.entry_label]].add(self.entry_label)
        bodies = []
        for index, group in enumerate(groups):
            def leave(match, index=index):
                target = match.group(1)
                if target == "finish":
                    return f"{{ at = {finish_id}u; goto leave; }}"
                if group_of[target] == index:
                    return match.group(0)
                entered[group_of[target]].add(target)
                return f"{{ at = {ids[target]}u; goto leave; }}"
            # The one return in a block follows unexpectedJump: run_<name> returns at once, as it did.
            bodies.append([re.sub(r"goto (\w+);", leave, line).replace("return true;", f"return {abort_id}u;")
                           for _, lines in group for line in lines])
        name = self.name
        out = [f"struct Regs_{name}", "{"] + [f"    {kind} {n};" for kind, n in variables] + ["};", ""]
        for index, group in enumerate(groups):
            # A part takes and hands back only the registers it names; the others wait in r.
            words = set(re.findall(r"\w+", "\n".join(bodies[index])))
            used = [(kind, n) for kind, n in variables if n in words]
            out += [f"static uint32_t run_{name}_{index}(Context &c, Regs_{name} &r, uint32_t at)", "{"] + head
            out += [f"    {kind} {n} = r.{n};" for kind, n in used]
            out += ["    (void)mem; (void)s; (void)f0;", "    switch (at)", "    {"]
            out += [f"    case {ids[label]}u: goto {label};" for label in sorted(entered[index], key=ids.get)]
            out += [f"    default: return {abort_id}u; // never: each part is only sent the labels it is entered at", "    }"]
            out += bodies[index] + ["leave:"] + [f"    r.{n} = {n};" for _, n in used] + ["    return at;", "}", ""]
        out += [f"bool run_{name}(Context &c)", "{"] + head + start + [f"    Regs_{name} r;"]
        out += [f"    r.{n} = {n};" for _, n in variables]
        out += [f"    uint32_t at = {ids[self.entry_label]}u;", f"    while (at < {finish_id}u)", "    {"]
        for index, group in enumerate(groups):
            bound = ids[groups[index + 1][0][0]] if index + 1 < len(groups) else None
            test = f"if (at < {bound}u) " if bound is not None else ""
            out.append(f"        {'' if index == 0 else 'else '}{test}at = run_{name}_{index}(c, r, at);")
        out += ["    }", f"    if (at == {abort_id}u)", "        return true;"]
        out += [f"    {n} = r.{n};" for _, n in variables]
        return out + finish + ["}"]

    def footprint(self):
        return min(self.pairs), max(self.pairs) + 8


def fnv(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return value


def read_programs(path):
    """data/vu_native_programs.txt: <name> <entry> <begin> <end> <fnv64 of code[begin, end)>
    [vu0], a VU1 program unless the line ends with vu0."""
    programs = []
    for line in Path(path).read_text().splitlines():
        line = line.split("#", 1)[0].split()
        if line:
            name, entry, begin, end, checksum = line[:5]
            unit = 0 if line[5:] == ["vu0"] else 1
            if line[5:] not in ([], ["vu0"]):
                raise TranslationError(f"{name}: unknown unit {' '.join(line[5:])}")
            programs.append((name, int(entry, 16), int(begin, 16), int(end, 16), int(checksum, 16), unit))
    return programs


def images(profiles):
    """(unit, code image) of each vu1-*.code and vu0-*.code recording."""
    for profile in profiles:
        for unit, size in ((1, 16384), (0, 4096)):
            for path in sorted(Path(profile).glob(f"vu{unit}-*.code")):
                data = path.read_bytes()
                if len(data) == size:
                    yield unit, data


def generate(programs, profiles, shards=1):
    """The C++ files: the table, then the translations spread over shards files, the longest
    programs first, each into the file with the fewest pairs so far, compiled in parallel."""
    found = {}
    for unit, data in images(profiles):
        for name, entry, begin, end, checksum, program_unit in programs:
            if name not in found and unit == program_unit and fnv(data[begin:end]) == checksum:
                found[name] = data
    head = ["// Native VU1 programs, generated by tools/translate_vu_programs.py from the game's microcode.",
            "// Built locally and never distributed.",
            '#include "runtime/ps2_vu1_native_ops.h"', "", "#include <cstring>", "",
            "namespace ps2_vu_native", "{", "#if PS2_VU_NATIVE_AVAILABLE", "namespace generated", "{"]
    table, declarations, translated = [], [], []
    for name, entry, begin, end, checksum, unit in programs:
        if name not in found:
            print(f"translate_vu_programs: {name}: no code image has it", file=sys.stderr)
            continue
        translator = Translator(found[name], entry, name, unit)
        translator.translate()
        if translator.footprint() != (begin, end):
            raise TranslationError(f"{name} spans {translator.footprint()}, the list says {(begin, end)}")
        lines = translator.function() + [""]
        # find() compares the uploaded microcode with these bytes.
        code = found[name][begin:end]
        lines.append(f"extern const uint8_t code_{name}[] = {{")
        lines += ["    " + ", ".join(f"0x{byte:02x}" for byte in code[i:i + 16]) + "," for i in range(0, len(code), 16)]
        lines += ["};", ""]
        translated.append((len(translator.pairs), name, lines))
        declarations += [f"bool run_{name}(Context &c);", f"extern const uint8_t code_{name}[];"]
        table.append(f'    {{"{name}", 0x{entry:04x}u, 0x{begin:04x}u, 0x{end:04x}u, 0x{checksum:016x}ull, '
                     f'generated::code_{name}, &generated::run_{name}, {unit}u}},')
        print(f"translate_vu_programs: {name}: {len(translator.pairs)} pairs, {len(translator.blocks)} blocks, "
              f"MAC flags of {len(translator.needed)} of {len(translator.mac_producers)} instructions read",
              file=sys.stderr)
    bodies, load = [[] for _ in range(shards)], [0] * shards
    for size, name, lines in sorted(translated, key=lambda item: (-item[0], item[1])):
        index = min(range(shards), key=lambda k: (load[k], k))
        load[index] += size
        bodies[index] += lines
    tail = ["} // namespace generated", "#endif", "} // namespace ps2_vu_native", ""]
    files = ["\n".join(head + declarations + ["} // namespace generated", "",
                                               "extern const Program kGeneratedPrograms[];",
                                               "extern const uint32_t kGeneratedProgramCount;",
                                               "const Program kGeneratedPrograms[] = {"] +
                       (table or ['    {"", 0u, 0u, 0u, 0ull, nullptr, nullptr, 1u},']) + ["};",
                       f"const uint32_t kGeneratedProgramCount = {len(table)}u;", "#else",
                       "extern const Program kGeneratedPrograms[];", "extern const uint32_t kGeneratedProgramCount;",
                       'const Program kGeneratedPrograms[] = {{"", 0u, 0u, 0u, 0ull, nullptr, nullptr, 1u}};',
                       "const uint32_t kGeneratedProgramCount = 0u;", "#endif", "} // namespace ps2_vu_native", ""])]
    files += ["\n".join(head + ["using namespace ops;", ""] + body + tail) for body in bodies]
    return files


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--programs", type=Path, help="data/vu_native_programs.txt")
    parser.add_argument("--output", type=Path, help="the table; the translations go to <output stem>_<n>.cpp")
    parser.add_argument("--shards", type=int, default=1, help="files the translations are spread over")
    parser.add_argument("--describe", nargs=3, metavar=("NAME", "IMAGE", "ENTRY"))
    parser.add_argument("profiles", type=Path, nargs="*")
    args = parser.parse_args()
    if args.describe:
        name, image, entry = args.describe
        data = Path(image).read_bytes()
        unit = 0 if len(data) == 4096 else 1
        translator = Translator(data, int(entry, 16), name, unit)
        translator.translate()
        begin, end = translator.footprint()
        print(f"{name} {int(entry, 16):04x} {begin:04x} {end:04x} {fnv(data[begin:end]):016x}" + ("" if unit else " vu0"))
        return
    if not args.programs or not args.output:
        parser.error("--programs and --output are required")
    files = generate(read_programs(args.programs), args.profiles, max(1, args.shards))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    for index, text in enumerate(files):
        path = args.output if index == 0 else args.output.with_name(f"{args.output.stem}_{index - 1}{args.output.suffix}")
        # Unchanged files keep their date and are not compiled again.
        if not path.is_file() or path.read_text() != text:
            path.write_text(text, newline="\n")


if __name__ == "__main__":
    main()
