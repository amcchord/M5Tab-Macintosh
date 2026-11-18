#!/usr/bin/env python3
"""Ahead-of-time translation of hot 68k code into C++.

The interpreter executes one generated handler per 68k instruction. For code
whose bytes are known in advance the same work can be done at build time:
this tool takes the instructions a profiling build saw executed
(tools/aot/aot_profile.py), groups them into regions and writes one C++
function per region. Each instruction's body is the text of the interpreter's
own handler (generated/cpuemu.cpp) with the opcode, extension words and PC
folded to constants, so translated code computes exactly what the interpreter
computes. Branches inside a region become gotos; flags live in locals, which
lets the compiler drop the flag work the 68k code never reads.

Two kinds of code are translated:

  ROM   Immutable and at a fixed address. Regions are installed at start-up
        after the ROM bytes they were translated from have been checked.
  RAM   System and application code, in "modules" of adjacent instructions
        that move together. A module is position independent; the firmware
        installs it where it sees that code executing, after checking the
        bytes, and checks them again whenever the guest has flushed the
        instruction cache.

Nothing is generated or made executable at run time. Installing means
replacing the first opcode of each entry point with an otherwise illegal
opcode whose handler is the region function. Anything this tool does not
translate stays with the interpreter.

    aot_translate.py --rom PATCHED_ROM --optable optable.tsv --hot counts.txt \
        [--ram-profile ram.txt] --out aot_rom_generated.cpp

Only addresses the profile proves to be instruction starts are translated or
patched. Sampled PCs are not good enough: handlers advance the PC while they
run, so a sample can point into the middle of an instruction.

See tools/aot/README.md.
"""

from __future__ import annotations

import argparse
import json
import re
import zlib
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
GENERATED = ROOT / "src/basilisk/uae_cpu/generated"

# Addressing modes as numbered by readcpu.h.
(DREG, AREG, AIND, AIPI, APDI, AD16, AD8R, ABSW, ABSL, PC16, PC8R, IMM) = range(12)

# Opcode values no 68040 instruction, Mac trap or Basilisk EMUL_OP uses. Entry
# points are patched with these. 0x71xx is Basilisk's EMUL_OP range.
PATCH_OPCODES = ([0x7300 + i for i in range(256)] + [0x7500 + i for i in range(256)] +
                 [0x7700 + i for i in range(256)] + [0x7900 + i for i in range(256)] +
                 [0x7B00 + i for i in range(256)] + [0x7D00 + i for i in range(256)] +
                 [0x7F00 + i for i in range(256)] + [0xF800 + i for i in range(0x800)])

# Calls a reused handler body may make. Anything else ends the region.
ALLOWED_CALLS = {
    "get_long", "get_word", "get_byte", "put_long", "put_word", "put_byte",
    "get_disp_ea_020", "cctrue", "m68k_dreg", "m68k_areg",
    "SET_CFLG", "SET_ZFLG", "SET_NFLG", "SET_VFLG", "SET_XFLG",
    "SET_CFLG_ALWAYS", "SET_NFLG_ALWAYS", "if", "while", "for", "switch",
    "AOT_MULL", "AOT_EXCEPTION", "AOT_PC",
}
FORBIDDEN_WORDS = re.compile(r"\b(goto|return|regs\s*\.\s*(?!regs\b)\w+|regflags|currprefs|abort)\b")
CALL = re.compile(r"\b([A-Za-z_]\w*)\s*\(")
ANCHOR_BYTES = 8


@dataclass
class OpInfo:
    handler: int
    mnemo: str
    size: int
    smode: int
    sreg: int
    suse: int
    dmode: int
    dreg: int
    duse: int
    cc: int
    plev: int
    clev: int
    cflow: int


@dataclass
class Insn:
    off: int                      # offset in its space (ROM offset, or RAM address)
    opcode: int
    length: int
    text: str                     # the handler's description
    kind: str                     # normal, bcc, bra, dbcc, bsr, jsr, jmp, rts, dyn
    code: str = ""                # C++ for kinds that fall through or compute a target
    target: int | None = None     # static target as a guest address
    relative: bool = True         # the target moves with the code (PC-relative)
    cc: int = 0
    reg: int = 0
    count: int = 0
    uses: frozenset | None = frozenset()     # guest registers touched (0-7 D, 8-15 A); None if unknown
    writes: frozenset | None = frozenset()

    @property
    def falls_through(self) -> bool:
        return self.kind in ("normal", "bcc", "dbcc")

    @property
    def ends_block(self) -> bool:
        return self.kind != "normal"


class Unsupported(Exception):
    """Raised for an instruction that stays with the interpreter."""

    def __init__(self, reason: str, length: int | None = None):
        super().__init__(reason)
        self.length = length


@dataclass
class Skipped:
    reason: str
    length: int | None        # None when the instruction's size is unknown


def swap16(value: int) -> int:
    return ((value & 0xFF) << 8) | (value >> 8)


def sum16(data: bytes, start: int = 0x811C9DC5) -> int:
    """The firmware's module checksum (aot_sum16): FNV-1a over 16-bit units."""
    value = start
    for position in range(0, len(data) - 1, 2):
        value ^= data[position] | (data[position + 1] << 8)
        value = (value * 0x01000193) & 0xFFFFFFFF
    return value


def load_optable(path: Path) -> dict[int, OpInfo]:
    table = {}
    for line in path.read_text().splitlines():
        f = line.split("\t")
        table[int(f[0], 16)] = OpInfo(int(f[1], 16), f[2], *(int(x) for x in f[3:]))
    return table


def parse_cpuemu(path: Path) -> dict[str, tuple[str, str]]:
    """Return {handler name: (comment, body)} with the unswapped-opcode branch selected."""
    handlers = {}
    header = re.compile(r"^(?:IRAM_ATTR |CPU_TCM_ATTR )?void REGPARAM2 CPUFUNC(?:_FF)?\((op_[0-9a-f]+_\d+)\)"
                        r"\(uae_u32 opcode\) /\* (.*?) \*/$")
    name = comment = None
    body: list[str] = []
    skipping = 0   # 0: emit, 1: inside #else of the opcode-layout conditional
    for line in path.read_text().splitlines():
        if name is None:
            m = header.match(line)
            if m:
                name, comment, body, skipping = m.group(1), m.group(2), [], 0
            continue
        if line == "}":
            handlers[name] = (comment, "\n".join(body[1:]))   # drop the opening brace
            name = None
            continue
        if line.startswith("#ifdef HAVE_GET_WORD_UNSWAPPED"):
            skipping = 0
            continue
        if line.startswith("#else"):
            skipping = 1
            continue
        if line.startswith("#endif"):
            skipping = 0
            continue
        if line.startswith("#"):
            raise SystemExit(f"unexpected preprocessor line in {name}: {line}")
        if not skipping:
            body.append(line)
    return handlers


def parse_cpustbl(path: Path, table: str = "op_smalltbl_0") -> list[tuple[str, int, int]]:
    text = path.read_text()
    start = text.index(f"CPUFUNC({table})[]")
    end = text.index("};", start)
    rows = re.findall(r"\{ CPUFUNC(?:_FF)?\((op_[0-9a-f]+_\d+)\), (\d+), (\d+) \}", text[start:end])
    return [(name, int(specific), int(opcode)) for name, specific, opcode in rows]


def build_handler_map(optable: dict[int, OpInfo], rows) -> dict[int, str]:
    """Mirror build_cpufunctbl() for the 68040 table."""
    direct = {}
    for name, specific, opcode in rows:
        if not specific:
            direct[opcode] = name
    result = dict(direct)
    for opcode, info in optable.items():
        if info.clev > 4:
            continue
        if info.handler != opcode:
            if info.handler not in direct:
                raise SystemExit(f"no handler for {opcode:04x} -> {info.handler:04x}")
            result[opcode] = direct[info.handler]
    for name, specific, opcode in rows:
        if specific:
            result[opcode] = name
    return result


REGISTER_REF = re.compile(r"m68k_(d|a)reg\s*\(\s*regs\s*,")
ASSIGNMENT = re.compile(r"\s*(=(?!=)|\+=|-=|\|=|&=|\^=|<<=|>>=|\+\+|--)")
DEFINITION = re.compile(r"\b(?:uae_[us](?:8|16|32)|int|unsigned int)\s+(\w+)\s*=\s*([^;,]+);")
CAST = re.compile(r"\(\s*(?:uae_[us](?:8|16|32)|uaecptr|int|unsigned int)\s*\)")
IMM8_TABLE = [8, 1, 2, 3, 4, 5, 6, 7]


def evaluate(expression: str, names: dict[str, int]) -> int | None:
    """Value of a C integer expression over known names, or None."""
    text = CAST.sub("", expression)
    text = re.sub(r"\b(0x[0-9a-fA-F]+|\d+)[uU]?[lL]?\b", r"\1", text)
    text = text.replace("imm8_table", "IMM8")
    if not re.fullmatch(r"[\w\s()\[\]+\-*&|^<>~]+", text):
        return None
    for name in re.findall(r"\b[A-Za-z_]\w*\b", text):
        if name != "IMM8" and name not in names:
            return None
    try:
        return int(eval(text, {"__builtins__": {}}, {"IMM8": IMM8_TABLE, **names}))
    except Exception:
        return None


def register_use(code: str) -> tuple[frozenset, frozenset] | None:
    """Guest registers a translated instruction touches and may write."""
    names: dict[str, int] = {}
    for m in DEFINITION.finditer(code):
        value = evaluate(m.group(2), names)
        if value is not None:
            names[m.group(1)] = value & 0xFFFFFFFF
    for name in list(names):
        # A name that is assigned again has no single value (MOVE16 sets
        # its destination register after declaring it).
        if len(re.findall(rf"\b{name}\s*(?:[-+|&^]|<<|>>)?=(?!=)", code)) != 1:
            del names[name]
    uses, writes = set(), set()
    for m in REGISTER_REF.finditer(code):
        depth, position = 1, m.end()
        while depth and position < len(code):
            depth += {"(": 1, ")": -1}.get(code[position], 0)
            position += 1
        index = evaluate(code[m.end():position - 1], names)
        if index is None or not 0 <= index <= 7:
            return None
        register = index + (8 if m.group(1) == "a" else 0)
        uses.add(register)
        if ASSIGNMENT.match(code, position):
            writes.add(register)
    for m in re.finditer(r"AOT_REGS\[", code):
        depth, position = 1, m.end()
        while depth and position < len(code):
            depth += {"[": 1, "]": -1}.get(code[position], 0)
            position += 1
        index = evaluate(code[m.end():position - 1], names)
        if index is None or not 0 <= index <= 15:
            return None
        uses.add(index)
        if ASSIGNMENT.match(code, position):
            writes.add(index)
    # Index registers of indexed addressing modes.
    for m in re.finditer(r"get_disp_ea_020\s*\(", code):
        tail = code[m.end():]
        extension = re.search(r",\s*\(\(uae_u32\)0x([0-9a-f]{4})u\)", tail)
        if extension is None:
            return None
        uses.add((int(extension.group(1), 16) >> 12) & 15)
    return frozenset(uses), frozenset(writes)


class Translator:
    """Translates single instructions of one address space.

    `image` holds the space's bytes from guest address `base`; `valid` marks
    the bytes that are known (all of them when None).
    """

    def __init__(self, image: bytes, base: int, optable, handlers, handler_map, valid: bytes | None = None):
        self.rom = image
        self.base = base
        self.valid = valid
        self.optable = optable
        self.handlers = handlers
        self.handler_map = handler_map
        self.cache: dict[int, Insn | Skipped] = {}
        self.label_id = 0

    # Image accessors -----------------------------------------------------
    def byte(self, off: int) -> int:
        if off < 0 or off >= len(self.rom) or (self.valid is not None and not self.valid[off]):
            raise Unsupported("outside the image")
        return self.rom[off]

    def word(self, off: int) -> int:
        return (self.byte(off) << 8) | self.byte(off + 1)

    def long(self, off: int) -> int:
        return (self.word(off) << 16) | self.word(off + 2)

    def contains(self, address: int) -> bool:
        return self.base <= address < self.base + len(self.rom)

    # Handler text --------------------------------------------------------
    def fold(self, body: str, off: int) -> tuple[str, int]:
        """Fold PC-relative fetches in a handler body to constants.

        Returns the new text and the PC advance the handler makes. Handler
        bodies are straight-line where the PC is concerned, so textual order
        is execution order. The PC itself becomes AOT_PC(address), which the
        generated file defines per address space.
        """
        body = body.replace("cpuop_begin();", "").replace("cpuop_end();", "")
        token = re.compile(
            r"m68k_incpc\s*\(\s*(\d+)\s*\)\s*;"
            r"|get_ibyte\s*\(\s*(\d+)\s*\)"
            r"|get_iword\s*\(\s*(\d+)\s*\)"
            r"|get_ilong\s*\(\s*(\d+)\s*\)"
            r"|(next_iword)\s*\(\s*\)"
            r"|(next_ilong)\s*\(\s*\)"
            r"|(m68k_getpc)\s*\(\s*\)")
        advance = 0
        out = []
        last = 0
        for m in token.finditer(body):
            out.append(body[last:m.start()])
            last = m.end()
            if m.group(1) is not None:
                advance += int(m.group(1))
            elif m.group(2) is not None:
                out.append(f"((uae_u32)0x{self.byte(off + advance + int(m.group(2)) + 1):02x}u)")
            elif m.group(3) is not None:
                out.append(f"((uae_u32)0x{self.word(off + advance + int(m.group(3))):04x}u)")
            elif m.group(4) is not None:
                out.append(f"((uae_u32)0x{self.long(off + advance + int(m.group(4))):08x}u)")
            elif m.group(5) is not None:
                value = self.word(off + advance)
                advance += 2
                out.append(f"((uae_u32)0x{value:04x}u)")
                if value & 0x100 and re.search(r"get_disp_ea_020\s*\([^;]*,\s*$", body[:m.start()]):
                    # A full extension word is followed by its base and
                    # outer displacements, which get_disp_ea_020_full()
                    # reads through the PC. Pass them to the four-argument
                    # form in aot_codegen.h instead.
                    displacements = []
                    for size in ((value >> 4) & 3, value & 3):
                        if size == 2:
                            word = self.word(off + advance)
                            advance += 2
                            displacements.append(word - 0x10000 if word & 0x8000 else word)
                        elif size == 3:
                            displacements.append(self.long(off + advance))
                            advance += 4
                        else:
                            displacements.append(0)
                    out.append(f", (uae_s32)0x{displacements[0] & 0xFFFFFFFF:08x}u"
                               f", (uae_s32)0x{displacements[1] & 0xFFFFFFFF:08x}u")
            elif m.group(6) is not None:
                value = self.long(off + advance)
                advance += 4
                out.append(f"((uae_u32)0x{value:08x}u)")
            else:
                out.append(f"AOT_PC(0x{self.base + off + advance:08x}u)")
        out.append(body[last:])
        text = "".join(out)
        # A few handlers index the register array themselves.
        text = re.sub(r"\bregs\s*\.\s*regs\s*\[", "AOT_REGS[", text)
        if re.search(r"\b(m68k_incpc|next_ibyte|get_i(byte|word|long)_prefetch|fill_prefetch\w*)\b", text):
            raise Unsupported("unfolded PC access")
        # Labels have function scope and a region holds many instructions.
        self.label_id += 1
        text = re.sub(r"\b(endlabel\d+|didnt_jump)\b", rf"\1_{self.label_id}", text)
        return text, advance

    def check(self, text: str, allow: set[str] = frozenset()) -> None:
        m = FORBIDDEN_WORDS.search(text)
        if m:
            raise Unsupported(f"uses {m.group(1).split('.')[0].strip()}")
        for m in CALL.finditer(text):
            if m.group(1) not in ALLOWED_CALLS and m.group(1) not in allow and not m.group(1).startswith("uae_"):
                raise Unsupported(f"calls {m.group(1)}")

    def unroll_movem(self, text: str, off: int) -> str:
        mask = self.word(off + 2)
        low = re.search(r"(\w+) = mask & 0xff", text).group(1)
        values = {low: mask & 0xFF}
        values[re.search(r"(\w+) = \(mask >> 8\) & 0xff", text).group(1)] = mask >> 8

        def expand(m: re.Match) -> str:
            bits = values[m.group(1)]
            body = m.group(2)
            parts = []
            for bit in range(8):
                if bits & (1 << bit):
                    parts.append(body.replace(f"movem_index1[{m.group(1)}]", str(bit))
                                     .replace(f"movem_index2[{m.group(1)}]", str(7 - bit)))
            return "{ " + " ".join(parts) + " }"

        text, count = re.subn(r"while \((\w+)\) \{ (.*?) \1 = movem_next\[\1\]; \}", expand, text)
        if count != 2 or "movem_" in text:
            raise Unsupported("MOVEM shape")
        return text

    # Decoding ------------------------------------------------------------
    def decode(self, off: int) -> Insn | Skipped:
        """Translate one instruction, or say why it stays interpreted."""
        if off in self.cache:
            return self.cache[off]
        try:
            result = self._decode(off)
            stack = frozenset({15})
            if result.kind == "dbcc":
                result.uses = result.writes = frozenset({result.reg})
            elif result.kind in ("rts", "bsr") or (result.kind == "jsr" and result.target is not None):
                result.uses = result.writes = stack
            elif result.code:
                found = register_use(result.code)
                if found is None:
                    result.uses = result.writes = None
                else:
                    extra = stack if result.kind in ("jsr", "dyn") else frozenset()
                    result.uses, result.writes = found[0] | extra, found[1] | extra
        except Unsupported as reason:
            result = Skipped(str(reason), reason.length)
        self.cache[off] = result
        return result

    def ea_length(self, mode: int, off: int) -> int:
        if mode in (AIND, AIPI, APDI, DREG, AREG):
            return 0
        if mode in (AD16, ABSW, PC16):
            return 2
        if mode == ABSL:
            return 4
        if mode in (AD8R, PC8R):
            extension = self.word(off)
            if extension & 0x100:
                sizes = {2: 2, 3: 4}
                return 2 + sizes.get((extension >> 4) & 3, 0) + sizes.get(extension & 3, 0)
            return 2
        raise Unsupported(f"EA mode {mode}")

    def static_target(self, mode: int, off: int) -> tuple[int, bool] | None:
        """Guest address of a control-transfer EA that needs no registers,
        and whether it is PC-relative."""
        if mode == ABSW:
            value = self.word(off + 2)
            return (value - 0x10000 if value & 0x8000 else value) & 0xFFFFFFFF, False
        if mode == ABSL:
            return self.long(off + 2), False
        if mode == PC16:
            value = self.word(off + 2)
            return (self.base + off + 2 + (value - 0x10000 if value & 0x8000 else value)) & 0xFFFFFFFF, True
        return None

    def displacement(self, off: int, opcode: int) -> tuple[int, int]:
        """Displacement and length of a Bcc/BSR."""
        disp = opcode & 0xFF
        if disp == 0:
            disp = self.word(off + 2)
            return (disp - 0x10000 if disp & 0x8000 else disp), 4
        if disp == 0xFF:
            disp = self.long(off + 2)
            return (disp - (1 << 32) if disp & 0x80000000 else disp), 6
        return (disp - 0x100 if disp & 0x80 else disp), 2

    def _decode(self, off: int) -> Insn:
        if off & 1:
            raise Unsupported("odd address")
        opcode = self.word(off)
        info = self.optable.get(opcode)
        if info is None or info.clev > 4:
            if (opcode & 0xF000) == 0xA000:
                raise Unsupported("A-line trap", 2)
            raise Unsupported("illegal opcode")
        if info.mnemo == "EMULOP":
            raise Unsupported("EMUL_OP", 2)
        name = self.handler_map[opcode]
        comment, body = self.handlers[name]
        mnemo = info.mnemo
        here = self.base + off
        const_opcode = f"const uae_u32 opcode = 0x{swap16(opcode):04x}u; (void)opcode;"

        if mnemo == "Bcc":
            cc = (opcode >> 8) & 15
            disp, length = self.displacement(off, opcode)
            return Insn(off, opcode, length, comment, "bra" if cc == 0 else "bcc",
                        target=(here + 2 + disp) & 0xFFFFFFFF, cc=cc)

        if mnemo == "DBcc":
            disp = self.word(off + 2)
            disp = disp - 0x10000 if disp & 0x8000 else disp
            return Insn(off, opcode, 4, comment, "dbcc", target=(here + 2 + disp) & 0xFFFFFFFF,
                        cc=(opcode >> 8) & 15, reg=opcode & 7)

        if mnemo == "BSR":
            disp, length = self.displacement(off, opcode)
            return Insn(off, opcode, length, comment, "bsr", target=(here + 2 + disp) & 0xFFFFFFFF)

        if mnemo in ("JSR", "JMP"):
            length = 2 + self.ea_length(info.smode, off + 2)
            static = self.static_target(info.smode, off)
            kind = mnemo.lower()
            if static is not None:
                return Insn(off, opcode, length, comment, kind, target=static[0], relative=static[1])
            text, _ = self.fold(body, off)
            self.check(text, {"m68k_do_jsr", "m68k_setpc"})
            return Insn(off, opcode, length, comment, kind, code=f"{{ {const_opcode}\n{text}\n}}")

        if mnemo == "RTS":
            return Insn(off, opcode, 2, comment, "rts")

        if mnemo == "RTD":
            text, _ = self.fold(body, off)
            self.check(text, {"m68k_setpc_rte"})
            return Insn(off, opcode, 4, comment, "dyn", code=f"{{ {const_opcode}\n{text}\n}}")

        text, length = self.fold(body, off)
        try:
            return self._normal(off, opcode, mnemo, comment, text, length, const_opcode)
        except Unsupported as reason:
            # The handler text still tells how long the instruction is, unless
            # it sets the PC itself or reads operands through a helper.
            known = length if length >= 2 and not re.search(
                r"m68k_setpc|m68k_do_|fpuop_|Exception|op_illg|m68k_emulop|mmu_op|m68k_divl", text) else None
            raise Unsupported(str(reason), known)

    def _normal(self, off, opcode, mnemo, comment, text, length, const_opcode) -> Insn:
        here = self.base + off
        if mnemo in ("MVMEL", "MVMLE"):
            text = self.unroll_movem(text, off)
        if mnemo == "MULL":
            text = re.sub(r"\bm68k_mull\s*\(", "AOT_MULL(", text)
        if mnemo in ("DIVU", "DIVS"):
            # The handler has already advanced the PC past the instruction
            # when it raises the divide-by-zero exception.
            text, count = re.subn(r"Exception\s*\((\d+),\s*(\w+)\);\s*goto\s+\w+;",
                                  rf"AOT_EXCEPTION(\1, \2, AOT_PC(0x{here + length:08x}u), AOT_REMAINING);", text)
            if count != 1:
                raise Unsupported("DIV shape")
        self.check(text)
        if length < 2:
            raise Unsupported("no PC advance")
        return Insn(off, opcode, length, comment, "normal", code=f"{{ {const_opcode}\n{text}\n}}")


# ---------------------------------------------------------------------------
# Profiles


def load_counts(path: Path) -> Counter:
    counts: Counter = Counter()
    for line in path.read_text().splitlines():
        if line and not line.startswith("#"):
            off, count = line.split()
            counts[int(off, 16)] += int(count)
    return counts


def load_ram_profile(paths: list[Path]) -> tuple[Counter, bytearray, bytearray]:
    """Execution counts and the memory image of a RAM code profile.

    Several profiles may be given when they are runs of one system: a byte
    two profiles disagree about is not used.
    """
    counts: Counter = Counter()
    chunks = []
    top = 0
    for path in paths:
        for line in path.read_text().splitlines():
            if line.startswith("C "):
                _, address, count = line.split()
                counts[int(address, 16)] += int(count)
            elif line.startswith("B "):
                _, address, data = line.split()
                chunks.append((int(address, 16), bytes.fromhex(data)))
                top = max(top, chunks[-1][0] + len(chunks[-1][1]))
    image = bytearray(top)
    valid = bytearray(top)
    for address, data in chunks:
        for index, value in enumerate(data):
            position = address + index
            if valid[position] == 1 and image[position] != value:
                valid[position] = 2       # contradicted
            elif valid[position] == 0:
                image[position] = value
                valid[position] = 1
    for position in range(top):
        if valid[position] == 2:
            valid[position] = 0
    return counts, image, valid


# ---------------------------------------------------------------------------
# Region construction


@dataclass
class Block:
    start: int
    insns: list[Insn]
    taken: int | None = None        # guest address of the static branch target
    fall: int | None = None         # offset of the fallthrough successor
    entry: bool = False
    patched: bool = False
    region: int = -1
    module: int = -1
    count: int = 0

    @property
    def last(self) -> Insn:
        return self.insns[-1]


@dataclass
class Module:
    index: int
    origin: int                     # address of its first byte in the profile
    span: int = 0
    insns: list[Insn] = field(default_factory=list)


def discover(tr: Translator, counts: Counter, min_count: int, check_flow: bool):
    """Translate the executed instructions that are hot enough.

    Returns the translated instructions, the offsets that must not become
    entry points, and the execution counts lost per reason.
    """
    insns: dict[int, Insn] = {}
    failures: Counter = Counter()
    no_entry: set[int] = set()
    decoded: dict[int, Insn | Skipped] = {}
    for off, count in sorted(counts.items()):
        if off & 1 or off + 2 > len(tr.rom):
            continue
        result = tr.decode(off)
        decoded[off] = result
        # Bytes after the first word of an executed instruction belong to it,
        # even where another path executes them as an instruction of their
        # own. Patching an entry opcode there would change this instruction.
        length = result.length
        if length is None:
            # Not translated and of unknown size. Where the next executed
            # address ran exactly as often, control fell through to it and
            # the instruction ends there.
            length = 16
            for following in range(off + 2, off + 16, 2):
                if following in counts:
                    if counts[following] == count:
                        length = following - off
                    break
        for inside in range(off + 2, off + length, 2):
            no_entry.add(inside)
    for off, count in sorted(counts.items()):
        result = decoded.get(off)
        if result is None:
            continue
        if isinstance(result, Skipped):
            failures[result.reason] += count
            continue
        if count < min_count:
            continue
        if check_flow and result.kind == "normal":
            # RAM is read after the run: where an instruction's successor
            # ran far less often than it did, the bytes are no longer the
            # code that was counted.
            if counts.get(off + result.length, 0) * 4 < count:
                failures["memory changed since it ran"] += count
                continue
        result.count = count
        insns[off] = result
    return insns, no_entry, failures


def form_modules(insns: dict[int, Insn], gap: int, min_insns: int) -> list[Module]:
    """Group RAM instructions that lie close enough to move together."""
    modules: list[Module] = []
    end = None
    for off in sorted(insns):
        insn = insns[off]
        if end is None or off - end > gap:
            modules.append(Module(len(modules), off))
        modules[-1].insns.append(insn)
        end = max(end or 0, off + insn.length)
        modules[-1].span = end - modules[-1].origin
    kept = [m for m in modules if len(m.insns) >= min_insns]
    for index, module in enumerate(kept):
        module.index = index
    return kept


def build_blocks(tr: Translator, insns: dict[int, Insn]) -> dict[int, Block]:
    leaders = set()
    for off, insn in insns.items():
        if insn.ends_block and off + insn.length in insns:
            leaders.add(off + insn.length)
        if insn.kind in ("bcc", "bra", "dbcc") and tr.contains(insn.target) and insn.target - tr.base in insns:
            leaders.add(insn.target - tr.base)
    # An instruction is also a leader when no translated instruction falls
    # into it.
    reached = {off + insn.length for off, insn in insns.items() if insn.falls_through}
    for off in insns:
        if off not in reached:
            leaders.add(off)

    blocks: dict[int, Block] = {}
    for start in sorted(leaders):
        off = start
        body = []
        while True:
            insn = insns[off]
            body.append(insn)
            off += insn.length
            if insn.ends_block or off in leaders or off not in insns:
                break
        block = Block(start, body, count=sum(i.count for i in body))
        last = block.last
        if last.kind in ("bcc", "bra", "dbcc"):
            block.taken = last.target
        if last.falls_through:
            block.fall = off
        blocks[start] = block
    return blocks


def successors(tr: Translator, blocks: dict[int, Block], block: Block):
    if block.taken is not None and tr.contains(block.taken) and block.taken - tr.base in blocks:
        yield block.taken - tr.base
    if block.fall is not None and block.fall in blocks:
        yield block.fall


def form_regions(tr: Translator, blocks: dict[int, Block], max_blocks: int) -> list[list[Block]]:
    parent = {start: start for start in blocks}

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    for block in blocks.values():
        for successor in successors(tr, blocks, block):
            if blocks[successor].module == block.module:
                parent[find(block.start)] = find(successor)
    groups = defaultdict(list)
    for start in sorted(blocks):
        groups[find(start)].append(blocks[start])
    regions = []
    for members in groups.values():
        for position in range(0, len(members), max_blocks):
            regions.append(members[position:position + max_blocks])
    regions.sort(key=lambda members: members[0].start)
    for index, members in enumerate(regions):
        for block in members:
            block.region = index
    return regions


def choose_entries(tr: Translator, blocks: dict[int, Block], no_entry: set[int]) -> None:
    fall_only = set()
    targeted = set()
    for block in blocks.values():
        if block.taken is not None and tr.contains(block.taken) and block.taken - tr.base in blocks:
            targeted.add(block.taken - tr.base)
        if block.fall is not None and block.fall in blocks:
            if blocks[block.fall].region == block.region:
                fall_only.add(block.fall)
            else:
                targeted.add(block.fall)
    for block in blocks.values():
        block.entry = (block.start in targeted or block.start not in fall_only) and \
            block.start not in no_entry


# ---------------------------------------------------------------------------
# Code generation


class Emitter:
    def __init__(self, tr: Translator, blocks: dict[int, Block]):
        self.tr = tr
        self.blocks = blocks
        self.lines: list[str] = []

    def emit(self, text: str) -> None:
        self.lines.append(text)

    def leave(self, address: int, relative: bool) -> str:
        """Statement leaving the region for a guest address."""
        if relative and self.tr.contains(address) and not address & 1:
            return f"AOT_EXIT_REL(0x{address:08x}u);"
        if relative:
            return f"AOT_EXIT_DYN(AOT_PC(0x{address:08x}u));"
        return f"AOT_EXIT_DYN(0x{address:08x}u);"

    def jump(self, block: Block, address: int, from_off: int, relative: bool) -> str:
        """Statement transferring control to a guest address."""
        tr = self.tr
        if relative and tr.contains(address) and not address & 1:
            off = address - tr.base
            target = self.blocks.get(off)
            if target is not None and target.region == block.region:
                if off <= from_off:
                    return f"AOT_BACK(L{off:x}, 0x{address:08x}u);"
                return f"goto L{off:x};"
        return self.leave(address, relative)

    def block(self, block: Block) -> None:
        tr = self.tr
        self.emit(f"L{block.start:x}:")
        self.emit(f"\taot_icount += {len(block.insns)};")
        for position, insn in enumerate(block.insns):
            here = tr.base + insn.off
            # Instructions of this block not run if this one leaves early.
            remaining = len(block.insns) - 1 - position
            self.emit(f"\t/* {here:08x}: {insn.opcode:04x} {insn.text} */")
            nxt = insn.off + insn.length
            if insn.kind == "normal":
                self.emit(insn.code.replace("AOT_REMAINING", str(remaining)))
            elif insn.kind == "bcc":
                self.emit(f"\tif (cctrue({insn.cc})) {{ {self.jump(block, insn.target, insn.off, True)} }}")
            elif insn.kind == "bra":
                self.emit(f"\t{self.jump(block, insn.target, insn.off, True)}")
            elif insn.kind == "dbcc":
                self.emit(f"\tif (!cctrue({insn.cc})) {{")
                self.emit(f"\t\tuae_s16 aot_count = m68k_dreg(regs, {insn.reg});")
                self.emit(f"\t\tm68k_dreg(regs, {insn.reg}) = (m68k_dreg(regs, {insn.reg}) & ~0xffff)"
                          f" | ((aot_count - 1) & 0xffff);")
                self.emit(f"\t\tif (aot_count) {{ {self.jump(block, insn.target, insn.off, True)} }}")
                self.emit("\t}")
            elif insn.kind == "bsr" or (insn.kind == "jsr" and insn.target is not None):
                self.emit(f"\tAOT_PUSH_RETURN(AOT_PC(0x{tr.base + nxt:08x}u));")
                self.emit(f"\t{self.leave(insn.target, insn.relative)}")
            elif insn.kind == "jmp" and insn.target is not None:
                self.emit(f"\t{self.jump(block, insn.target, insn.off, insn.relative)}")
            elif insn.kind in ("jsr", "jmp", "dyn"):
                self.emit(insn.code)
                self.emit("\t__builtin_unreachable();")
            elif insn.kind == "rts":
                self.emit("\tAOT_RTS();")
            else:
                raise AssertionError(insn.kind)
        if block.last.falls_through:
            successor = self.blocks.get(block.fall)
            if successor is None or successor.region != block.region:
                self.emit(f"\t{self.leave(tr.base + block.fall, True)}")
            else:
                self.emit(f"\tgoto L{block.fall:x};")


def merge_ranges(insns: list[Insn], holes: set[int] = frozenset()) -> list[tuple[int, int]]:
    """Byte ranges the instructions occupy, without the words at `holes`."""
    covered = set()
    for insn in insns:
        covered.update(range(insn.off, insn.off + insn.length, 2))
    covered -= holes
    ranges = []
    for off in sorted(covered):
        if ranges and ranges[-1][0] + ranges[-1][1] == off:
            ranges[-1] = (ranges[-1][0], ranges[-1][1] + 2)
        else:
            ranges.append((off, 2))
    return ranges


class OpcodeAllocator:
    """Hands out entry opcodes so that one region's are consecutive and
    share their high byte. A handler is called with the opcode as it lies in
    memory, where that byte is the low one, so the entry number is one shift
    and one subtraction away."""

    # PATCH_OPCODES as runs of consecutive values with one high byte.
    RUNS = [(0x7300 + 0x200 * i, 256) for i in range(7)] + [(0xF800 + 0x100 * i, 256) for i in range(8)]

    def __init__(self):
        self.run = 0
        self.used = 0
        self.assigned: list[int] = []

    def remaining(self) -> int:
        return sum(size for _, size in self.RUNS[self.run:]) - self.used

    def take(self, count: int) -> int | None:
        """First of `count` consecutive opcodes, or None when none are left."""
        while self.run < len(self.RUNS) and self.RUNS[self.run][1] - self.used < count:
            self.run += 1
            self.used = 0
        if self.run >= len(self.RUNS):
            return None
        first = self.RUNS[self.run][0] + self.used
        self.used += count
        self.assigned += range(first, first + count)
        return first


CACHE_RUN = 16        # cache guest registers in regions that run this long per entry


def region_run(tr: Translator, blocks: dict[int, Block], members: list[Block], counts: Counter) -> float:
    """Instructions a region executes per entry, from the profile."""
    executed = sum(b.count for b in members)
    exits = 0
    inside = {b.start for b in members}
    for block in members:
        last = block.last
        target = last.target - tr.base if last.target is not None and last.relative and \
            tr.contains(last.target) else None
        if last.kind in ("rts", "jsr", "bsr", "dyn"):
            exits += last.count
        elif last.kind in ("bra", "jmp"):
            if target not in inside:
                exits += last.count
        elif last.kind in ("bcc", "dbcc"):
            fell = min(last.count, counts.get(block.fall, 0))
            if target not in inside:
                exits += last.count - fell
            if block.fall not in inside:
                exits += fell
        elif block.fall not in inside:
            exits += last.count
    return executed / max(exits, 1)


def emit_region(emitter: Emitter, name: str, members: list[Block], entries: list[Block], enter: str,
                counts: Counter) -> None:
    count = sum(len(b.insns) for b in members)
    insns = [i for b in members for i in b.insns]
    run = region_run(emitter.tr, emitter.blocks, members, counts)
    cached = run >= CACHE_RUN and all(i.uses is not None for i in insns)
    uses = sorted(set().union(*(i.uses for i in insns))) if cached else []
    writes = sorted(set().union(*(i.writes for i in insns))) if cached else []
    emitter.emit(f"// {name}: from {emitter.tr.base + members[0].start:08x}, {len(members)} blocks, "
                 f"{count} instructions, {run:.0f} executed per entry")
    emitter.emit("#undef AOT_REGS")
    emitter.emit("#undef AOT_REGS_IN")
    emitter.emit("#undef AOT_REGS_OUT")
    if cached:
        # Guest registers live in locals; leaving the region writes the
        # ones it may have changed.
        emitter.emit("#define AOT_REGS aot_regs")
        emitter.emit("#define AOT_REGS_IN() do { " +
                     " ".join(f"aot_regs[{r}] = regs.regs[{r}];" for r in uses) + " } while (0)")
        emitter.emit("#define AOT_REGS_OUT() do { " +
                     " ".join(f"regs.regs[{r}] = aot_regs[{r}];" for r in writes) + " } while (0)")
    else:
        emitter.emit("#define AOT_REGS regs.regs")
        emitter.emit("#define AOT_REGS_IN() do { } while (0)")
        emitter.emit("#define AOT_REGS_OUT() do { } while (0)")
    emitter.emit(f"AOT_REGION_ATTR static void {name}(uae_u32 opcode)")
    emitter.emit("{")
    emitter.emit(f"\t{enter};")
    if cached:
        emitter.emit("\tuae_u32 aot_regs[16];")
    emitter.emit("\tAOT_REGS_IN();")
    if len(entries) > 1:
        emitter.emit("\tswitch (aot_entry) {")
        for number, block in enumerate(entries):
            emitter.emit(f"\tcase {number}: goto L{block.start:x};")
        emitter.emit("\tdefault: __builtin_unreachable();")
        emitter.emit("\t}")
    else:
        emitter.emit(f"\tgoto L{entries[0].start:x};")
    for block in members:
        emitter.block(block)
    emitter.emit("\tAOT_EPILOGUE;")
    emitter.emit("}")


def generate_rom(out: list[str], tr: Translator, regions, blocks, opcodes: OpcodeAllocator, stats: dict,
                 counts: Counter) -> tuple[int, set[int]]:
    """Return the entry count and instruction offsets in emitted ROM regions."""
    emitted: set[int] = set()
    rom = tr.rom
    all_ranges: list[tuple[int, int]] = []
    region_rows = []
    entry_rows = []
    emitter = Emitter(tr, blocks)
    out.append("// ---- ROM --------------------------------------------------------------")
    out.append("#define AOT_PC(address) ((uaecptr)(address))")
    out.append("#define AOT_EXIT_REL(address) AOT_EXIT_ROM((address) - AOT_ROM_BASE)")
    out.append("")
    for index, members in enumerate(regions):
        entries = [b for b in members if b.entry and b.patched]
        if not entries:
            continue
        first = opcodes.take(len(entries))
        if first is None:
            break
        emitted.update(i.off for b in members for i in b.insns)
        ranges = merge_ranges([i for b in members for i in b.insns])
        crc = 0
        for start, length in ranges:
            crc = zlib.crc32(rom[start:start + length], crc)
        region_rows.append((index, len(all_ranges), len(ranges), crc & 0xFFFFFFFF))
        all_ranges += ranges
        name = f"aot_r{index}"
        emitter.lines = []
        emitter.emit(f"static const AotEntryInfo {name}_entries[] = {{")
        for block in entries:
            emitter.emit(f"\t{{ 0x{block.start:x}u, 0x{swap16(block.insns[0].opcode):04x}u }},")
            entry_rows.append((block.start, block.insns[0].opcode, len(region_rows) - 1, name))
        emitter.emit("};")
        emit_region(emitter, name, members, entries, f"AOT_ROM_ENTER(0x{first:04x}u, {name}_entries)", counts)
        out += emitter.lines
        out.append("")

    out.append("static const AotRange aot_ranges[] = {")
    out += [f"\t{{ 0x{start:x}u, {length}u }}," for start, length in all_ranges] or ["\t{ 0, 0 },"]
    out.append("};")
    out.append("static const AotRegion aot_regions[] = {")
    out += [f"\t{{ {first}u, {count}u, 0x{crc:08x}u }},  // region {index}"
            for index, first, count, crc in region_rows] or ["\t{ 0, 0, 0 },"]
    out.append("};")
    out.append("static const AotEntry aot_entries[] = {")
    out += [f"\t{{ 0x{start:x}u, 0x{original:04x}u, {region}u, {name} }},"
            for start, original, region, name in entry_rows] or ["\t{ 0, 0, 0, NULL },"]
    out.append("};")
    out.append("#undef AOT_PC")
    out.append("#undef AOT_EXIT_REL")
    out.append("")
    stats["rom_regions"] = len(region_rows)
    stats["rom_regions_with_cached_registers"] = sum(1 for line in out if line == "#define AOT_REGS aot_regs")
    stats["rom_entries"] = len(entry_rows)
    stats["rom_verified_bytes"] = sum(length for _, length in all_ranges)
    stats["rom_counts"] = (len(all_ranges), len(region_rows), len(entry_rows))
    return len(entry_rows), emitted


def generate_ram(out: list[str], tr: Translator, modules: list[Module], regions, blocks,
                 opcodes: OpcodeAllocator, stats: dict, counts: Counter) -> tuple[int, set[int]]:
    """Return the entry count and instruction offsets in emitted RAM modules."""
    emitted: set[int] = set()
    image = tr.rom
    all_ranges: list[tuple[int, int]] = []
    module_rows = []
    entry_rows = []
    anchor_rows = []
    test_rows = []
    by_module = defaultdict(list)
    for index, members in enumerate(regions):
        by_module[members[0].module].append((index, members))
    emitter = Emitter(tr, blocks)
    out.append("// ---- RAM --------------------------------------------------------------")
    out.append("#define AOT_EXIT_REL(address) AOT_EXIT_RAM(AOT_PC(address))")
    out.append("")
    for module in modules:
        installed = []
        for index, members in by_module.get(module.index, []):
            entries = [b for b in members if b.entry and b.patched]
            if entries:
                installed.append((index, members, entries))
        if not installed or opcodes.remaining() < sum(len(entries) for _, _, entries in installed) + 64:
            continue
        number = len(module_rows)
        insns = [i for _, members, _ in installed for b in members for i in b.insns]
        emitted.update(i.off for i in insns)
        origin = min(i.off for i in insns)
        span = max(i.off + i.length for i in insns) - origin
        entry_offsets = {b.start for _, _, entries in installed for b in entries}
        ranges = merge_ranges(insns, entry_offsets)
        checksum = 0x811C9DC5
        for start, length in ranges:
            checksum = sum16(bytes(image[start:start + length]), checksum)
        first_entry = len(entry_rows)
        out.append("#undef AOT_PC")
        out.append(f"#define AOT_PC(address) ((uaecptr)(aot_base + ((address) - 0x{origin:08x}u)))")
        emitter.lines = []
        for index, members, entries in installed:
            name = f"aot_m{number}_r{index}"
            first = opcodes.take(len(entries))
            emitter.emit(f"static const AotEntryInfo {name}_entries[] = {{")
            for block in entries:
                emitter.emit(f"\t{{ 0x{block.start - origin:x}u, 0x{swap16(block.insns[0].opcode):04x}u }},")
                entry_rows.append((block.start - origin, block.insns[0].opcode, number, name))
            emitter.emit("};")
            emit_region(emitter, name, members, entries,
                        f"AOT_RAM_ENTER({number}u, 0x{first:04x}u, {name}_entries)", counts)
        out += emitter.lines
        out.append("")
        # Anchors: instruction starts whose next bytes hold no entry opcode,
        # so they read the same whether or not the module is installed.
        for insn in sorted(insns, key=lambda i: -i.count):
            window = range(insn.off, insn.off + ANCHOR_BYTES, 2)
            if any(w in entry_offsets for w in window):
                continue
            if insn.off + ANCHOR_BYTES > origin + span or not all(tr.valid[w] and tr.valid[w + 1] for w in window):
                continue
            anchor_rows.append((sum16(bytes(image[insn.off:insn.off + ANCHOR_BYTES])), number,
                                insn.off - origin))
        module_rows.append((len(all_ranges), len(ranges), checksum, first_entry,
                            len(entry_rows) - first_entry, span, origin))
        all_ranges += [(start - origin, length) for start, length in ranges]
        test_rows.append((origin, span))

    anchor_rows.sort()
    out.append("#undef AOT_PC")
    out.append("#undef AOT_EXIT_REL")
    out.append("static const AotRange aot_ram_ranges[] = {")
    out += [f"\t{{ 0x{start:x}u, {length}u }}," for start, length in all_ranges] or ["\t{ 0, 0 },"]
    out.append("};")
    out.append("static const AotModule aot_modules[] = {")
    out += [f"\t{{ {first}u, {count}u, 0x{checksum:08x}u, {first_entry}u, {entries}u, {span}u }},"
            f"  // profiled at {origin:08x}"
            for first, count, checksum, first_entry, entries, span, origin in module_rows] or \
           ["\t{ 0, 0, 0, 0, 0, 0 },"]
    out.append("};")
    out.append("static const AotEntry aot_ram_entries[] = {")
    out += [f"\t{{ 0x{start:x}u, 0x{original:04x}u, {module}u, {name} }},"
            for start, original, module, name in entry_rows] or ["\t{ 0, 0, 0, NULL },"]
    out.append("};")
    out.append("static const AotAnchor aot_anchors[] = {")
    out += [f"\t{{ 0x{value:08x}u, {module}u, 0x{offset:x}u }}," for value, module, offset in anchor_rows] or \
           ["\t{ 0, 0, 0 },"]
    out.append("};")
    out.append(f"AotModuleState aot_module_state[{max(len(module_rows), 1)}];")
    # The self-tests run each module from a copy of its code.
    out.append("#if AOT_SELFTEST || defined(AOT_HOST_TEST)")
    offsets = []
    blob = bytearray()
    for origin, span in test_rows:
        offsets.append(len(blob))
        blob += bytes(image[origin:origin + span])
        if len(blob) & 1:
            blob.append(0)
    out.append("static const uae_u8 aot_module_code[] = {")
    for position in range(0, len(blob), 24):
        out.append("\t" + ",".join(str(value) for value in blob[position:position + 24]) + ",")
    if not blob:
        out.append("\t0,")
    out.append("};")
    out.append("static const uae_u32 aot_module_code_offsets[] = {")
    out.append("\t" + ", ".join(f"{offset}u" for offset in offsets) + ("," if offsets else "0,"))
    out.append("};")
    out.append("#else")
    out.append("#define aot_module_code NULL")
    out.append("#define aot_module_code_offsets NULL")
    out.append("#endif")
    out.append("")
    stats["ram_modules"] = len(module_rows)
    stats["ram_entries"] = len(entry_rows)
    stats["ram_anchors"] = len(anchor_rows)
    stats["ram_verified_bytes"] = sum(length for _, length in all_ranges)
    stats["ram_counts"] = (len(all_ranges), len(module_rows), len(entry_rows), len(anchor_rows))
    return len(entry_rows), emitted


def select_entries(blocks: dict[int, Block], regions, min_region_insns: int, budget: int) -> int:
    """Patch only entries worth a region transition, most executed first."""
    region_size = {index: sum(len(b.insns) for b in members) for index, members in enumerate(regions)}
    candidates = [b for b in blocks.values() if b.entry and region_size[b.region] >= min_region_insns]
    candidates.sort(key=lambda b: -b.insns[0].count)
    for block in candidates[:budget]:
        block.patched = True
    return len(candidates)


def hot_untranslated(tr: Translator, counts: Counter, emitted: set[int],
                     discovered: dict[int, Insn], threshold: int) -> list[dict]:
    """Rank profiled addresses outside the generated code, independent of runtime installation."""
    ranked = sorted((off for off in counts if off not in emitted), key=lambda off: (-counts[off], off))
    rows = []
    for off in ranked[:20]:
        decoded = tr.cache.get(off)
        if decoded is None:
            reason = "outside the captured instruction image"
        elif isinstance(decoded, Skipped):
            reason = decoded.reason
        elif counts[off] < threshold:
            reason = "below execution threshold"
        elif off not in discovered:
            reason = "memory changed since it ran"
        else:
            reason = "no emitted region entry or module"
        rows.append({"guest_pc": f"0x{tr.base + off:08x}", "executions": counts[off], "reason": reason})
    return rows


def main() -> int:
    global CACHE_RUN
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--rom", type=Path, required=True, help="ROM image as the emulator runs it (patched)")
    parser.add_argument("--rom-base", type=lambda v: int(v, 0), default=0x40800000)
    parser.add_argument("--optable", type=Path, required=True, help="output of dump_optable")
    parser.add_argument("--hot", type=Path, required=True, action="append",
                        help="ROM execution counts from aot_profile.py (repeatable)")
    parser.add_argument("--ram-profile", type=Path, action="append", default=[],
                        help="RAM code profile from aot_profile.py --ram (repeatable)")
    parser.add_argument("--min-count", type=int, default=1000,
                        help="translate instructions executed at least this often in the profile")
    parser.add_argument("--ram-min-count", type=int, default=None)
    parser.add_argument("--max-blocks", type=int, default=64)
    parser.add_argument("--min-region-insns", type=int, default=3)
    parser.add_argument("--module-gap", type=int, default=32,
                        help="RAM instructions further apart than this are separate modules")
    parser.add_argument("--ram-exclude", action="append", default=[], metavar="START:END",
                        help="leave RAM code in this address range (hex) to the interpreter (repeatable)")
    parser.add_argument("--max-rom-entries", type=int, default=2200)
    parser.add_argument("--max-ram-entries", type=int, default=len(PATCH_OPCODES) - 2200)
    parser.add_argument("--cache-run", type=float, default=CACHE_RUN,
                        help="keep guest registers in locals in regions that execute at least this many "
                             "instructions per entry")
    parser.add_argument("--optimize", default="Os", help="GCC optimization level for translated code")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()

    CACHE_RUN = args.cache_run
    rom = args.rom.read_bytes()
    optable = load_optable(args.optable)
    handlers = parse_cpuemu(GENERATED / "cpuemu.cpp")
    handler_map = build_handler_map(optable, parse_cpustbl(GENERATED / "cpustbl.cpp"))
    stats: dict = {}

    # ROM
    tr = Translator(rom, args.rom_base, optable, handlers, handler_map)
    counts: Counter = Counter()
    for path in args.hot:
        counts += load_counts(path)
    insns, no_entry, failures = discover(tr, counts, args.min_count, False)
    blocks = build_blocks(tr, insns)
    regions = form_regions(tr, blocks, args.max_blocks)
    choose_entries(tr, blocks, no_entry)
    candidates = select_entries(blocks, regions, args.min_region_insns,
                                min(args.max_rom_entries, len(PATCH_OPCODES)))

    out: list[str] = []
    out.append("// GENERATED by tools/aot/aot_translate.py. Do not edit and do not commit:")
    out.append("// this file is derived from a Macintosh ROM image and system software.")
    out.append(f"// ROM CRC32 {zlib.crc32(rom) & 0xffffffff:08x}, {len(rom)} bytes, base {tr.base:08x}")
    out.append('#include "aot_runtime.h"')
    out.append("#if AOT_ROM")
    out.append("// Translated code is fetched through the instruction cache; smaller is faster.")
    out.append("#if defined(__GNUC__) && !defined(__clang__)")
    out.append(f'#pragma GCC optimize ("{args.optimize}")')
    out.append("#endif")
    out.append("#define AOT_GENERATED_CODE 1")
    out.append(f"#define AOT_ROM_BASE 0x{tr.base:08x}u")
    out.append('#include "aot_codegen.h"')
    out.append("")
    out.append("AOT_BLOB_MARK(aot_blob_begin);")
    out.append("")
    opcodes = OpcodeAllocator()
    rom_entries, rom_emitted = generate_rom(out, tr, regions, blocks, opcodes, stats, counts)

    # RAM
    ram_counts, image, valid = load_ram_profile(args.ram_profile) if args.ram_profile else (Counter(), b"", b"")
    for item in args.ram_exclude:
        low, high = (int(value, 16) for value in item.split(":"))
        for address in [a for a in ram_counts if low <= a < high]:
            del ram_counts[address]
    ram_tr = Translator(bytes(image), 0, optable, handlers, handler_map, bytes(valid))
    ram_min = args.ram_min_count if args.ram_min_count is not None else args.min_count
    ram_insns, ram_no_entry, ram_failures = discover(ram_tr, ram_counts, ram_min, True)
    modules = form_modules(ram_insns, args.module_gap, args.min_region_insns)
    kept = {}
    for module in modules:
        for insn in module.insns:
            kept[insn.off] = insn
    ram_blocks = build_blocks(ram_tr, kept)
    module_of = {insn.off: module.index for module in modules for insn in module.insns}
    for block in ram_blocks.values():
        block.module = module_of[block.start]
    ram_regions = form_regions(ram_tr, ram_blocks, args.max_blocks)
    choose_entries(ram_tr, ram_blocks, ram_no_entry)
    ram_candidates = select_entries(ram_blocks, ram_regions, args.min_region_insns,
                                    max(0, min(args.max_ram_entries, len(PATCH_OPCODES) - rom_entries)))
    ram_entries, ram_emitted = generate_ram(out, ram_tr, modules, ram_regions, ram_blocks, opcodes, stats, ram_counts)

    out.append("AOT_BLOB_MARK(aot_blob_end);")
    out.append("")
    out.append("static const uae_u16 aot_patch_opcodes[] = {")
    total = rom_entries + ram_entries
    assert total == len(opcodes.assigned)
    for position in range(0, total, 12):
        out.append("\t" + ", ".join(f"0x{value:04x}" for value in opcodes.assigned[position:position + 12]) + ",")
    if not total:
        out.append("\t0,")
    out.append("};")
    rom_counts = stats.pop("rom_counts")
    ram_table = stats.pop("ram_counts")
    out.append("extern const AotImage aot_image = {")
    out.append(f"\t0x{tr.base:08x}u, {len(rom)}u,")
    out.append("\taot_ranges, aot_regions, aot_entries, aot_patch_opcodes,")
    out.append(f"\t{rom_counts[0]}u, {rom_counts[1]}u, {rom_counts[2]}u,")
    out.append("\taot_ram_ranges, aot_modules, aot_ram_entries, aot_anchors, aot_module_state,")
    out.append(f"\t{ram_table[0]}u, {ram_table[1]}u, {ram_table[2]}u, {ram_table[3]}u,")
    out.append("\taot_module_code, aot_module_code_offsets,")
    out.append("};")
    out.append("#endif  // AOT_ROM")
    out.append("")
    args.out.write_text("\n".join(out))

    executed = sum(counts.values())
    translated = sum(insn.count for insn in insns.values())
    ram_executed = sum(ram_counts.values())
    ram_translated = sum(insn.count for insn in kept.values())
    report = {
        "rom_instructions_executed": executed,
        "rom_translated_share": round(translated / max(executed, 1), 4),
        "rom_installed_share": round(sum(counts[off] for off in rom_emitted) / max(executed, 1), 4),
        "rom_instructions": len(insns),
        "rom_blocks": len(blocks),
        "rom_entry_candidates": candidates,
        "ram_instructions_executed": ram_executed,
        "ram_translated_share": round(ram_translated / max(ram_executed, 1), 4),
        "ram_installed_share": round(sum(ram_counts[off] for off in ram_emitted) / max(ram_executed, 1), 4),
        "ram_instructions": len(kept),
        "ram_blocks": len(ram_blocks),
        "ram_entry_candidates": ram_candidates,
        "rom_hot_untranslated": hot_untranslated(tr, counts, rom_emitted, insns, args.min_count),
        "ram_hot_untranslated": hot_untranslated(ram_tr, ram_counts, ram_emitted, ram_insns, ram_min),
        **stats,
        "rom_not_translated_by_executions": failures.most_common(20),
        "ram_not_translated_by_executions": ram_failures.most_common(20),
    }
    summary = json.dumps(report, indent=1)
    if args.report:
        args.report.write_text(summary + "\n")
    print(summary)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
