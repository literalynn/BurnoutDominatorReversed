#!/usr/bin/env python3
"""Translate whole VU1 microprograms into C++ for runtime/ps2_vu1_native.h.

A program runs from the MSCAL address that starts it to its E bit. The translation follows
VU1Interpreter (ps2_vu1_exec.inl, ps2_vu1_core.cpp) cycle by cycle at translation time. A pair
waits until the VF and VI values it reads are written, so register values follow program order
and the C++ writes them at once; what does not wait is modelled: MAC and clip flags land 4
cycles after their instruction, Q 7 cycles after its DIV, and a conditional branch right after
an integer instruction that wrote its register reads the old value. A block of code is
translated once per pipeline state it can be entered with. The one timing the translation
cannot know is how long an XGKICK waits for the previous packet: the C++ counts cycles and
picks the block for the wait it finds.

Inputs are the code images of compile_vu_programs.py (vu1-<hash>.code in profile directories)
and data/vu_native_programs.txt, which names the programs to translate by the checksum of their
code. The output is generated from game microcode and must stay local.

  translate_vu_programs.py --programs <list> --output <cpp> <profile dir>...
  translate_vu_programs.py --describe <name> <image> <entry pc, hex>   (prints a list line)
"""
import argparse
import struct
import sys
from pathlib import Path

FMAC_LATENCY = 4
DIV_LATENCY = 7


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

    def __init__(self, word, pc):
        self.word = word
        self.op = "nop"
        self.vf_reads = []  # (vf register, lanes)
        self.vi_reads = []
        self.vf_write = None  # (register, lanes), latency 4
        self.vi_write = None  # (register, latency)
        self.delays_branch = False
        self.branch = None  # ("b"|"bal"|"cond", target) or ("jr"|"jalr", register)
        self.fdiv = False
        self.waitq = False
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

        target = (pc + 8 + self.imm * 8) & 0x3FFF
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
                    self.op = "div"; self.fdiv = True
                    rvf(is_, lane_bit(self.fsf)); rvf(it, lane_bit(self.ftf))
                elif special == 0x3B:
                    self.op = "waitq"; self.waitq = True
                elif special == 0x3C:
                    self.op = "mtir"; rvf(is_, lane_bit(self.fsf)); wvi(self.vit, 1); self.delays_branch = True
                elif special == 0x3D:
                    self.op = "mfir"; rvi(self.vis); wvf(it, d)
                elif special == 0x6C:
                    self.op = "xgkick"; rvi(self.vis)
                else:
                    raise TranslationError(f"unsupported lower {word:08x} at {pc:04x}")
            else:
                raise TranslationError(f"unsupported lower {word:08x} at {pc:04x}")
        else:
            raise TranslationError(f"unsupported lower {word:08x} at {pc:04x}")


class Pair:
    def __init__(self, code, pc):
        lower, upper = struct.unpack_from("<II", code, pc)
        self.pc = pc
        self.ibit = bool(upper >> 31 & 1)
        self.ebit = bool(upper >> 30 & 1)
        if upper >> 27 & 3:
            raise TranslationError(f"D or T bit at {pc:04x}")
        self.upper = Upper(upper)
        self.imm = lower if self.ibit else None
        self.lower = Lower(0 if self.ibit else lower, pc)
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
        self.pending = ()  # (time, "mac"|"clip"|"q", producer pc), in issue order
        self.backup = 0  # VI register the last pair wrote, read old by a branch right after it

    def key(self):
        return (tuple(sorted(self.vf.items())), tuple(sorted(self.vi.items())), self.pending, self.backup)

    def copy(self):
        other = State()
        other.vf, other.vi, other.pending, other.backup = dict(self.vf), dict(self.vi), self.pending, self.backup
        return other

    def latest(self):
        return max([0] + list(self.vf.values()) + list(self.vi.values()) + [t for t, _, _ in self.pending])


class Block:
    def __init__(self, label):
        self.label = label
        # str, or ("macflags", producer, code) / ("maccommit", producer) / ("macread",)
        self.lines = [f"{label}:"]
        self.successors = []


class Translator:
    def __init__(self, code, entry, name):
        self.code = code
        self.entry = entry
        self.name = name
        self.pairs = {}
        self.labels = {}
        self.work = []
        self.blocks = []
        self.leaders = {entry}
        self.links = {}  # register -> link addresses BAL or JALR write
        self.delay_slot_of = {}
        self.ends = set()
        self.clip_producers = set()
        self.mac_producers = set()
        self.wait_blocks = 0

    def pair(self, pc):
        if pc not in self.pairs:
            self.pairs[pc] = Pair(self.code, pc)
        return self.pairs[pc]

    def discover(self):
        seen = set()
        stack = [self.entry]
        jr_regs = set()
        while stack:
            pc = stack.pop()
            while pc not in seen:
                seen.add(pc)
                p = self.pair(pc)
                if p.lower.branch:
                    if p.ebit:
                        raise TranslationError(f"E bit on a branch at {pc:04x}")
                    kind, arg = p.lower.branch
                    delay = self.pair(pc + 8)
                    if delay.lower.branch or delay.ebit:
                        raise TranslationError(f"branch or E bit in the delay slot at {pc + 8:04x}")
                    self.delay_slot_of[pc + 8] = pc
                    seen.add(pc + 8)
                    if kind in ("b", "bal", "cond"):
                        self.leaders.add(arg)
                        stack.append(arg)
                    if kind in ("bal", "jalr"):
                        self.links.setdefault(p.lower.vi_write[0], set()).add(pc + 16)
                    if kind in ("cond", "bal", "jalr"):
                        self.leaders.add(pc + 16)
                        stack.append(pc + 16)
                    if kind in ("jr", "jalr"):
                        jr_regs.add(arg)
                    break
                if p.ebit:
                    delay = self.pair(pc + 8)
                    if delay.lower.branch or delay.ebit:
                        raise TranslationError(f"branch or E bit after the E bit at {pc + 8:04x}")
                    seen.add(pc + 8)
                    self.ends.add(pc)
                    break
                pc += 8
        # A register jump goes back to where a BAL or JALR linked: nothing else may write the
        # register, and every path to the jump links it first.
        for reg in jr_regs:
            if reg not in self.links:
                raise TranslationError(f"JR vi{reg} without a link")
            for p in self.pairs.values():
                w = p.lower.vi_write
                if w and w[0] == reg and p.lower.op not in ("bal", "jalr"):
                    raise TranslationError(f"vi{reg} written at {p.pc:04x} and used by JR")
            self.check_linked(reg)
        for targets in self.links.values():
            self.leaders |= targets

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
            return sorted(self.links[arg])
        return [pc + 8]

    def check_linked(self, reg):
        linked = {self.entry: False}
        stack = [self.entry]
        while stack:
            pc = stack.pop()
            state = linked[pc]
            p = self.pair(pc)
            if p.lower.op in ("jr", "jalr") and p.lower.branch[1] == reg and not state:
                raise TranslationError(f"JR vi{reg} at {pc:04x} may run before a link")
            if p.lower.op in ("bal", "jalr") and p.lower.vi_write[0] == reg:
                state = True
            for succ in self.successors(pc):
                if succ not in linked:
                    linked[succ] = state
                    stack.append(succ)
                elif linked[succ] and not state:
                    linked[succ] = False
                    stack.append(succ)

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
                else:
                    lines.append("    q = qp;")
            else:
                keep.append((t - cycles, kind, producer))
        state.pending = tuple(keep)
        state.vf = {key: t - cycles for key, t in state.vf.items() if t > cycles}
        state.vi = {reg: t - cycles for reg, t in state.vi.items() if t > cycles}

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
            pending.append((DIV_LATENCY, "q", p.pc))
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
        if op in ("nop", "waitq", "b"):
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
            return f"(static_cast<uint32_t>(static_cast<uint16_t>({vi(reg)})) * 8u) & 0x3FFFu"

        if op == "lq":
            out.append(f"    const V lv = lq(mem, {vi(l.vis)} + {l.imm});")
        elif op == "sq":
            out.append(f"    sq<{l.dest}>(mem, {vi(l.vit)} + {l.imm}, {vf(l.is_)});")
        elif op == "ilw":
            if l.vit:
                comp = lanes_of(l.dest)[0] if l.dest else 0
                out.append(f"    {vi(l.vit)} = ilw(mem, {vi(l.vis)} + {l.imm}, {comp});")
        elif op == "isw":
            out.append(f"    isw<{l.dest}>(mem, {vi(l.vis)} + {l.imm}, {vi(l.vit)});")
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
            out.append(f"    const V lv = lq(mem, {vi(l.vis)});")
            if l.vis:
                out.append(f"    {vi(l.vis)} = vi16({vi(l.vis)} + 1);")
        elif op == "lqd":
            if l.vis:
                out.append(f"    {vi(l.vis)} = vi16({vi(l.vis)} - 1);")
            out.append(f"    const V lv = lq(mem, {vi(l.vis)});")
        elif op == "sqi":
            out.append(f"    sq<{l.dest}>(mem, {vi(l.vit)}, {vf(l.is_)});")
            if l.vit:
                out.append(f"    {vi(l.vit)} = vi16({vi(l.vit)} + 1);")
        elif op == "sqd":
            if l.vit:
                out.append(f"    {vi(l.vit)} = vi16({vi(l.vit)} - 1);")
            out.append(f"    sq<{l.dest}>(mem, {vi(l.vit)}, {vf(l.is_)});")
        elif op == "div":
            out.append(f"    qp = div(lane<{l.fsf}>({vf(l.is_)}), lane<{l.ftf}>({vf(l.it)}));")
        elif op == "mtir":
            if l.vit:
                out.append(f"    {vi(l.vit)} = static_cast<int16_t>(laneBits<{l.fsf}>({vf(l.is_)}) & 0xFFFFu);")
        elif op == "mfir":
            out.append(f"    const V lv = opaque(bits(static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>({vi(l.vis)})))));")
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
            return sorted(self.links[pend[1]])
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
                latest = state.latest()
                lines.append(f"    cycle += {cycles + latest};")
                lines.append("    if (kickEnd > cycle) cycle = kickEnd;")
                self.advance(state, latest, lines)
                lines.append(("macread",))
                lines.append(f"    endPc = 0x{(pc + 8) & 0x3FFF:04x}u;")
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
                    for target_pc in sorted(self.links[pend[1]]):
                        target = self.label(target_pc, state)
                        lines.append(f"    case 0x{target_pc:04x}u: goto {target};")
                        block.successors.append(target)
                    lines.append("    default: unexpectedJump(c, jump); return;")
                    lines.append("    }")
                break
            if p.lower.branch:
                kind, arg = p.lower.branch
                pend = ("cond", arg, pc + 16) if kind == "cond" else (kind, arg)
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
        """The C++ function, run_<name>."""
        out = [f"void run_{self.name}(Context &c)", "{",
               "    uint8_t *const mem = c.data;", "    VU1State &s = *c.state;",
               "    const V f0 = opaque(_mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f));"]
        out += [f"    V f{reg} = load(s.vf[{reg}]);" for reg in range(1, 32)]
        out += [f"    int32_t vi{reg} = static_cast<int16_t>(s.vi[{reg}]);" for reg in range(1, 16)]
        out += ["    V acc = load(s.acc);",
                "    float q = s.q, qp = 0.0f;",
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
        out += ["    (void)wait; (void)viOld; (void)taken; (void)jump; (void)qp; (void)clipW; (void)iR; (void)iN;",
                f"    goto {self.entry_label};"]
        for block in sorted(self.blocks, key=lambda b: b.label):
            for line in block.lines:
                if isinstance(line, tuple):
                    if line[0] == "macflags" and line[1] in self.needed:
                        out.append(line[2])
                    elif line[0] == "maccommit" and line[1] in self.needed:
                        out.append(f"    mac = mac_{line[1]:04x};")
                else:
                    out.append(line)
        out.append("finish:")
        out += [f"    store(s.vf[{reg}], f{reg});" for reg in range(1, 32)]
        out += [f"    s.vi[{reg}] = vi{reg};" for reg in range(1, 16)]
        out += ["    store(s.acc, acc);", "    s.q = q;", "    std::memcpy(&s.i, &iBits, sizeof(iBits));",
                "    s.mac = mac;", "    s.clip = clip;", "    s.pc = endPc;", "    c.cycles = cycle;", "}"]
        return out

    def footprint(self):
        return min(self.pairs), max(self.pairs) + 8


def fnv(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return value


def read_programs(path):
    """data/vu_native_programs.txt: <name> <entry> <begin> <end> <fnv64 of code[begin, end)>."""
    programs = []
    for line in Path(path).read_text().splitlines():
        line = line.split("#", 1)[0].split()
        if line:
            name, entry, begin, end, checksum = line
            programs.append((name, int(entry, 16), int(begin, 16), int(end, 16), int(checksum, 16)))
    return programs


def images(profiles):
    for profile in profiles:
        for path in sorted(Path(profile).glob("vu1-*.code")):
            data = path.read_bytes()
            if len(data) == 16384:
                yield path, data


def generate(programs, profiles):
    found = {}
    for path, data in images(profiles):
        for name, entry, begin, end, checksum in programs:
            if name not in found and fnv(data[begin:end]) == checksum:
                found[name] = data
    out = ["// Native VU1 programs, generated by tools/translate_vu_programs.py from the game's microcode.",
           "// Built locally and never distributed.",
           '#include "runtime/ps2_vu1_native.h"', "", "#include <cstring>", "",
           "namespace ps2_vu_native", "{"]
    table = []
    out += ["#if PS2_VU_NATIVE_AVAILABLE", "namespace", "{", "using namespace ops;", ""]
    for name, entry, begin, end, checksum in programs:
        if name not in found:
            print(f"translate_vu_programs: {name}: no code image has it", file=sys.stderr)
            continue
        translator = Translator(found[name], entry, name)
        translator.translate()
        if translator.footprint() != (begin, end):
            raise TranslationError(f"{name} spans {translator.footprint()}, the list says {(begin, end)}")
        out += translator.function() + [""]
        # find() compares the uploaded microcode with these bytes.
        code = found[name][begin:end]
        out.append(f"const uint8_t code_{name}[] = {{")
        out += ["    " + ", ".join(f"0x{byte:02x}" for byte in code[i:i + 16]) + "," for i in range(0, len(code), 16)]
        out += ["};", ""]
        table.append(f'    {{"{name}", 0x{entry:04x}u, 0x{begin:04x}u, 0x{end:04x}u, 0x{checksum:016x}ull, code_{name}, &run_{name}}},')
        print(f"translate_vu_programs: {name}: {len(translator.pairs)} pairs, {len(translator.blocks)} blocks, "
              f"MAC flags of {len(translator.needed)} of {len(translator.mac_producers)} instructions read",
              file=sys.stderr)
    out += ["} // namespace", ""]
    out += ["extern const Program kGeneratedPrograms[];", "extern const uint32_t kGeneratedProgramCount;"]
    out += ["const Program kGeneratedPrograms[] = {"] + (table or ['    {"", 0u, 0u, 0u, 0ull, nullptr, nullptr},']) + ["};"]
    out += [f"const uint32_t kGeneratedProgramCount = {len(table)}u;", "#else",
            "extern const Program kGeneratedPrograms[];", "extern const uint32_t kGeneratedProgramCount;",
            'const Program kGeneratedPrograms[] = {{"", 0u, 0u, 0u, 0ull, nullptr, nullptr}};',
            "const uint32_t kGeneratedProgramCount = 0u;", "#endif", "} // namespace ps2_vu_native", ""]
    return "\n".join(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--programs", type=Path, help="data/vu_native_programs.txt")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--describe", nargs=3, metavar=("NAME", "IMAGE", "ENTRY"))
    parser.add_argument("profiles", type=Path, nargs="*")
    args = parser.parse_args()
    if args.describe:
        name, image, entry = args.describe
        data = Path(image).read_bytes()
        translator = Translator(data, int(entry, 16), name)
        translator.translate()
        begin, end = translator.footprint()
        print(f"{name} {int(entry, 16):04x} {begin:04x} {end:04x} {fnv(data[begin:end]):016x}")
        return
    if not args.programs or not args.output:
        parser.error("--programs and --output are required")
    text = generate(read_programs(args.programs), args.profiles)
    if not args.output.is_file() or args.output.read_text() != text:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text, newline="\n")


if __name__ == "__main__":
    main()
