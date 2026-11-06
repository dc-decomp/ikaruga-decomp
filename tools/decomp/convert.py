#!/usr/bin/env python3
"""Turn Ghidra's C for Ikaruga into C++ that compiles against the port's runtime (port/include).

    tools/decomp/convert.py [--ghidra ghidra/out] [--out decomp]

Reads what tools/ghidra/analyze.sh exports: c/<address>.c (the decompiled C), c/<address>.json
(each function's prototype, locals and globals with their types) and types.json (every structure,
union, typedef and enum with field offsets). Writes into the output directory:

    gen/include/types.h      the game's and the SDK's types, laid out as in guest memory
    gen/include/functions.h  a declaration for every function
    gen/include/globals.h    every named global, as a reference into guest memory
    gen/src/*.cpp            the functions, grouped by library and by address
    gen/src/dispatch.cpp     the table that calls through code pointers use
    gen/report.txt           what the conversion could not resolve

Every guest pointer becomes gptr<T> and every dereference a gref<T> (port/include/guest.h). A local
whose address is taken gets a slot on the guest stack at the offset Ghidra found on the console.

Names come from game/symbols.tsv and game/globals.tsv as Ghidra applied them. A user row in
symbols.tsv also names its function here, before an analysis has applied it, so that a name needs
no new analysis. The comment column of both files goes above each function, its declaration and
each global.

A function defined in fix/*.cpp (hand-corrected decompiled code) or listed in port/replaced.txt
(native code in the port) is not generated. A function listed in port/hooked.txt is generated as
NAME_game, and the port defines NAME, which wraps it.

The output is derived from the game. It stays out of this repository: the output directory is
ignored by git and is its own local repository.
"""

from __future__ import annotations

import argparse
import collections
import json
import re
import struct
import sys
import textwrap
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# The end of the code: the literal pool of the function at 0x8C0CA2B4. The SDK's data follows.
# dream-translate puts the end at 0x8C0C0574, but the rankings run code after it
# (tools/ghidra/DropDataFunctions.java).
CODE_END = 0x8C0CA2F4

# Ghidra's names for primitive types, as the port's runtime spells them (guest.h typedefs most).
PRIMITIVE = {
    "void": "void", "char": "char", "short": "short", "int": "int", "long": "int32_t",
    "uchar": "uchar", "ushort": "ushort", "uint": "uint", "ulong": "uint32_t", "float": "float",
    "double": "double", "bool": "bool", "byte": "byte", "word": "word", "dword": "dword",
    "qword": "qword", "longlong": "longlong", "ulonglong": "ulonglong", "sbyte": "int8_t",
    "schar": "signed char", "wchar_t": "wchar16", "wchar16": "wchar16", "float10": "float10",
    "undefined": "undefined", "undefined1": "undefined1", "undefined2": "undefined2",
    "undefined3": "undefined3", "undefined4": "undefined4", "undefined5": "undefined5",
    "undefined6": "undefined6", "undefined7": "undefined7", "undefined8": "undefined8",
    "code": "code", "pointer": "gptr<void>", "string": "char", "TerminatedCString": "char",
    "unicode": "wchar16", "uint3": "uint3", "int3": "int3", "uint5": "uint5", "uint6": "uint6",
    "uint7": "uint7", "unsigned int": "uint", "unsigned char": "uchar", "unsigned short": "ushort",
    "long long": "longlong", "unsigned long": "ulong", "signed char": "signed char",
    "ImageBaseOffset32": "uint", "Void": "void", "VOID": "void", "KMVOID": "void",
}
FLOAT_TYPES = {"float", "Float", "KMFLOAT"}
# C++'s keywords, which no name may be.
CPP_KEYWORDS = {
    "alignas", "alignof", "and", "asm", "auto", "bool", "break", "case", "catch", "char", "class",
    "const", "constexpr", "continue", "decltype", "default", "delete", "do", "double", "else",
    "enum", "explicit", "export", "extern", "false", "float", "for", "friend", "goto", "if",
    "inline", "int", "long", "mutable", "namespace", "new", "noexcept", "not", "nullptr",
    "operator", "or", "private", "protected", "public", "register", "return", "short", "signed",
    "sizeof", "static", "struct", "switch", "template", "this", "throw", "true", "try", "typedef",
    "typeid", "typename", "union", "unsigned", "using", "virtual", "void", "volatile", "while",
    "xor",
}
KEYWORDS = {
    "if", "else", "while", "for", "do", "switch", "case", "default", "return", "break",
    "continue", "goto", "sizeof", "typedef", "struct", "union", "enum", "const", "volatile",
    "static", "extern", "unsigned", "signed",
}


def doc_lines(comment: str) -> list[str]:
    """A comment from the names' files, as lines of C++ comment."""
    return ["// " + line for line in textwrap.wrap(comment, 97)] if comment else []


def cpp_ident(name: str) -> str:
    # MinGW's errno is (*_errno()), and the game has a global called _errno. Rename it so the
    # generated headers do not replace the CRT function with a guest-memory reference.
    if name == "_errno":
        return "ikaruga__errno"
    out = re.sub(r"[^A-Za-z0-9_]", "_", name)
    return "_" + out if out[:1].isdigit() else out


class Types:
    """Ghidra type names to C++ type expressions."""

    def __init__(self, types: list[dict]):
        self.kind: dict[str, dict] = {}
        self.funcdefs: set[str] = set()
        for t in types:
            if t["kind"] == "funcdef":
                self.funcdefs.add(t["name"])
                continue
            # A typedef of itself says nothing: game/sdk.h declares Ghidra's uchar as unsigned char,
            # and Ghidra keeps it as uchar.
            if t["kind"] == "typedef" and t["base"] == t["name"]:
                continue
            # First definition of a name wins; later duplicates come from other headers.
            self.kind.setdefault(t["name"], t)

    def parse(self, s: str) -> tuple[str, list]:
        """'float[4] *' -> ('float', [('arr', 4), ('ptr',)]), applied left to right."""
        s = s.strip()
        ops: list = []
        m = re.match(r"^(.*?)((?:\s*\*|\s*\[\d+\])*)$", s)
        base, suffix = m.group(1).strip(), m.group(2)
        for tok in re.findall(r"\*|\[\d+\]", suffix):
            ops.append(("ptr",) if tok == "*" else ("arr", int(tok[1:-1])))
        return base, ops

    def base_cpp(self, base: str) -> str:
        if base in PRIMITIVE:
            return PRIMITIVE[base]
        if base.startswith(("struct ", "union ", "enum ")):
            base = base.split(" ", 1)[1]
        if base in self.kind:
            return cpp_ident(base)
        if base in self.funcdefs:
            return "code"
        return "undefined4" if base else "void"

    def cpp(self, s: str) -> tuple[str, str]:
        """(type without trailing arrays, trailing array suffix) for a declaration."""
        base, ops = self.parse(s)
        t = self.base_cpp(base)
        if t == "code" and not any(o[0] == "ptr" for o in ops):
            t = "undefined1"  # Ghidra types a byte of data as code at times; a function is no value
        # Arrays applied after the last pointer form the declarator suffix; everything up to the
        # last pointer is inside the gptr.
        last_ptr = max((i for i, o in enumerate(ops) if o[0] == "ptr"), default=-1)
        for o in ops[: last_ptr + 1]:
            if o[0] == "ptr":
                t = f"gptr<{t}>"
            else:
                t = f"{t}[{o[1]}]" if False else self._array_type(t, o[1])
        suffix = "".join(f"[{o[1]}]" for o in ops[last_ptr + 1:])
        return t, suffix

    @staticmethod
    def _array_type(t: str, n: int) -> str:
        # An array inside a pointer: gptr<float[4]> needs the array as a type, which C++ spells
        # with the element first. Nested arrays stack in the same order as declarators.
        m = re.match(r"^(.*?)((?:\[\d+\])*)$", t)
        return f"{m.group(1)}[{n}]{m.group(2)}"

    def decl(self, s: str, name: str) -> str:
        t, suffix = self.cpp(s)
        return f"{t} {name}{suffix}"

    def type_only(self, s: str) -> str:
        t, suffix = self.cpp(s)
        return t + suffix


# ---- types.h -------------------------------------------------------------------------------------

def emit_types(ty: Types) -> str:
    out = ["// Generated by tools/decomp/convert.py from Ghidra's type export. Do not edit.",
           "#pragma once", '#include "guest.h"', "", "namespace game {", "",
           "#pragma pack(push, 1)", ""]
    structs = [t for t in ty.kind.values() if t["kind"] in ("struct", "union")]
    typedefs = [t for t in ty.kind.values() if t["kind"] == "typedef"]
    enums = [t for t in ty.kind.values() if t["kind"] == "enum"]

    for t in structs:
        out.append(f"{'struct' if t['kind'] == 'struct' else 'union'} {cpp_ident(t['name'])};")
    out.append("")
    seen_consts: set[str] = set()
    for t in enums:
        size = t["size"]
        it = {1: "uint8_t", 2: "uint16_t", 8: "uint64_t"}.get(size, "uint32_t")
        out.append(f"typedef {it} {cpp_ident(t['name'])};")
        for k, v in t["values"].items():
            if k in seen_consts or not re.match(r"^[A-Za-z_]\w*$", k):
                continue
            seen_consts.add(k)
            # Ghidra makes an enum of each #define in the headers; a host macro of the same name
            # (NULL, EOF, RAND_MAX) wins.
            out.append(f"#ifndef {k}")
            out.append(f"constexpr {it} {k} = {v & ((1 << (8 * size)) - 1)}u;")
            out.append("#endif")
    out.append("")

    # A pointer to a type needs only its declaration; a value of it needs the complete type.
    # Structures are declared up front; a typedef is declared once it is emitted.
    by_name = {cpp_ident(t["name"]): t for t in structs + typedefs}
    declared: set[str] = {cpp_ident(t["name"]) for t in structs}
    complete: set[str] = set()
    busy: set[str] = set()
    body: list[str] = []

    def ref(s: str, by_value: bool) -> None:
        base, ops = ty.parse(s)
        b = base.split(" ", 1)[1] if base.startswith(("struct ", "union ")) else base
        b = cpp_ident(b)
        if b not in by_name:
            return
        first_ptr = next((i for i, o in enumerate(ops) if o[0] == "ptr"), None)
        if by_value and first_ptr is None:
            need_complete(b)
        else:
            need_declared(b)

    def need_declared(n: str) -> None:
        t = by_name[n]
        if t["kind"] != "typedef" or n in declared or n in busy:
            return
        busy.add(n)
        base, ops = ty.parse(t["base"])
        if not (base == t["name"] and ops and ops[0][0] == "ptr"):
            arrays = any(o[0] == "arr" for o in ops)
            ref(t["base"], by_value=arrays)
        busy.discard(n)
        emit_typedef(t)
        declared.add(n)

    def need_complete(n: str) -> None:
        if n in complete or n in busy:
            return
        t = by_name[n]
        if t["kind"] == "typedef":
            need_declared(n)
            base, ops = ty.parse(t["base"])
            if not any(o[0] == "ptr" for o in ops):
                ref(t["base"], by_value=True)
            complete.add(n)
            return
        busy.add(n)
        for f in t["fields"]:
            ref(f["type"], by_value=True)
        busy.discard(n)
        emit_struct(t)
        complete.add(n)

    def emit_typedef(t: dict) -> None:
        name = cpp_ident(t["name"])
        if t["base"] == t["name"] or cpp_ident(t["base"]) == name:
            return
        base, ops = ty.parse(t["base"])
        if base == t["name"] and ops and ops[0][0] == "ptr":
            body.append(f"typedef gptr<code> {name};")  # pointer to the function type of this name
            return
        tt, suffix = ty.cpp(t["base"])
        if tt == name:
            return
        body.append(f"typedef {tt} {name}{suffix};")

    def emit_struct(t: dict) -> None:
        name = cpp_ident(t["name"])
        kw = "struct" if t["kind"] == "struct" else "union"
        body.append(f"{kw} {name} {{")
        pos = 0
        used: set[str] = set()
        for f in sorted(t["fields"], key=lambda f: f["offset"]):
            fname = cpp_ident(f["name"])
            while fname in used:
                fname += "_"
            used.add(fname)
            if kw == "struct":
                if f["offset"] < pos:
                    continue  # overlapping field: Ghidra allows it, a C++ struct does not
                if f["offset"] > pos:
                    body.append(f"    uint8_t _pad_{pos:x}[{f['offset'] - pos}];")
                pos = f["offset"] + f["size"]
            tt, suffix = ty.cpp(f["type"])
            for n in reversed(re.findall(r"\[(\d+)\]", suffix)):
                tt = f"inarray<{tt}, {n}>"
            body.append(f"    {tt} {fname};")
        if not t["fields"]:
            if t["size"] > 0:
                body.append(f"    uint8_t _data[{t['size']}];")
        elif kw == "struct" and t["size"] > pos:
            body.append(f"    uint8_t _pad_{pos:x}[{t['size'] - pos}];")
        body.append("};")
        if t["size"] > 0:
            body.append(f"static_assert(sizeof({name}) == {t['size']}, \"{name}\");")

    for n in sorted(by_name):
        need_complete(n)
    out += body
    out += ["", "#pragma pack(pop)", "", "}  // namespace game", ""]
    return "\n".join(out)


# ---- functions -------------------------------------------------------------------------------------

SIG = re.compile(r"^(?P<ret>.*?)\s*\b(?P<name>[A-Za-z_]\w*)\s*\((?P<params>.*)\)\s*;?\s*$", re.S)


class Function:
    def __init__(self, addr: str, c: str, side: dict, ty: Types):
        self.addr = addr
        self.c = c
        self.side = side
        self.comment = ""  # from game/symbols.tsv
        self.name = side["name"]
        self.ident = cpp_ident(self.name)
        sig = side["signature"].strip()
        m = SIG.match(sig)
        if not m:
            raise ValueError(f"cannot parse signature {sig!r}")
        self.ret = m.group("ret").strip() or "void"
        for cc in ("__stdcall", "__cdecl", "__thiscall", "__fastcall"):
            self.ret = self.ret.replace(cc, "").strip()
        # The conventions that keep some of r1-r3 across a call (tools/ghidra/KeptRegisters.java).
        self.ret = re.sub(r"\b__keep_r\w+", "", self.ret).strip()
        # Ghidra's default for a function it never decompiled.
        if self.ret == "undefined":
            self.ret = "void"
        self.params: list[tuple[str, str]] = []
        self.varargs = False
        ps = m.group("params").strip()
        if ps and ps != "void":
            for p in split_params(ps):
                p = p.strip()
                if p == "...":
                    self.varargs = True
                    continue
                pm = re.match(r"^(.*?)([A-Za-z_]\w*)\s*((?:\[\d+\])*)$", p)
                if not pm:
                    raise ValueError(f"cannot parse parameter {p!r} in {sig!r}")
                # An array parameter is a pointer in C.
                ptype = pm.group(1).strip() + (" *" if pm.group(3) else "")
                self.params.append((ptype, pm.group(2)))
        # A parameter's type follows its register: Ghidra prints a word in r5 that the function uses
        # as a float as `float`, and a float type would move it to fr4 in the C++ calls and thunks.
        # A word in an FPU register that the function uses as an integer stays a word: it is its
        # bits. Such a word comes from fr in the thunk (self.from_fr).
        regs = {l["param"]: l.get("register", "") for l in side.get("locals", []) if l.get("param", -1) >= 0}
        self.from_fr = set()
        for i, (ptype, pname) in enumerate(self.params):
            reg = regs.get(i, "")
            if ptype.strip() in FLOAT_TYPES and re.fullmatch(r"r\d+", reg):
                self.params[i] = ("undefined4", pname)
            elif re.fullmatch(r"fr\d+", reg) and ptype.strip() not in FLOAT_TYPES and "*" not in ptype:
                self.params[i] = ("undefined4", pname)
                self.from_fr.add(i)
        self.ty = ty

    def prototype(self, with_names: bool = True, defaults_from: int | None = None, ident: str | None = None) -> str:
        ret = self.ty.type_only(self.ret)
        ps = []
        for i, (t, n) in enumerate(self.params):
            p = self.ty.decl(t, n if with_names else f"p{i}") if with_names else self.ty.type_only(t)
            if defaults_from is not None and i >= defaults_from:
                p += " = {}"
            ps.append(p)
        if self.varargs:
            ps.append("...")
        return f"{ret} {ident or self.ident}({', '.join(ps)})"


def split_params(s: str) -> list[str]:
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur)
    return out


# A pointer cast or declaration: TYPE followed by one or more stars, inside parentheses for a cast.
TYPE_NAME = r"(?:unsigned\s+(?:int|char|short|long)|long\s+long|[A-Za-z_]\w*)"
CAST = re.compile(r"\(\s*(" + TYPE_NAME + r")\s*((?:\*\s*)+)\)")
CAST_ARRAY_PTR = re.compile(r"\(\s*(" + TYPE_NAME + r")\s*\(\s*\*\s*\)\s*((?:\[\d+\])+)\s*\)")


def convert_casts(text: str, ty: Types) -> str:
    text = re.sub(r"\(code\)(?=\s*[\w(*&~!'-])", "(undefined1)", text)

    def cast(m: re.Match) -> str:
        base, stars = m.group(1), m.group(2).count("*")
        if base in KEYWORDS:
            return m.group(0)
        t = ty.base_cpp(base)
        for _ in range(stars):
            t = f"gptr<{t}>"
        return f"({t})"

    def cast_array_ptr(m: re.Match) -> str:
        return f"(gptr<{ty.base_cpp(m.group(1))}{m.group(2)}>)"

    def pun(m: re.Match) -> str:
        base, stars = m.group(1).strip(), m.group(2).count("*")
        t = ty.base_cpp(base)
        for _ in range(stars):
            t = f"gptr<{t}>"
        return f"pun<{t}>("

    text = re.sub(r"\bpun<([^<>()*]+?)\s*(\**)>\(", pun, text)
    text = CAST_ARRAY_PTR.sub(cast_array_ptr, text)
    return CAST.sub(cast, text)


IDENT = re.compile(r"\b[A-Za-z_]\w*\b")


class Converter:
    def __init__(self, ghidra: Path, out: Path, interpreted: Path):
        self.ghidra = ghidra
        self.out = out
        self.interpreted_list = interpreted
        self.ty = Types(json.loads((ghidra / "types.json").read_text()))
        self.report: list[str] = []
        self.functions: list[Function] = []
        self.by_name: dict[str, Function] = {}
        self.skip: set[str] = set()
        self.data_names: dict[str, int] = {}
        self.dup_names: set[str] = set()
        self.by_addr: dict[str, Function] = {}

    def load_names(self) -> None:
        """The rows of game/symbols.tsv by address, and the comments of game/globals.tsv."""
        self.names: dict[str, tuple[str, str, str]] = {}  # address -> name, source, comment
        for line in (ROOT / "game" / "symbols.tsv").read_text().splitlines():
            if line.strip() and not line.startswith("#"):
                cols = line.split("\t") + ["", "", ""]
                addr = f"{(int(cols[0], 16) & 0x1FFFFFFF) | 0x80000000:08x}"
                self.names[addr] = (cols[1], cols[2], cols[3].strip())
        self.global_comments: dict[int, str] = {}
        for line in (ROOT / "game" / "globals.tsv").read_text().splitlines():
            if line.strip() and not line.startswith("#"):
                cols = line.split("\t") + ["", "", "", ""]
                if cols[4].strip():
                    self.global_comments[(int(cols[0], 16) & 0x1FFFFFFF) | 0x80000000] = cols[4].strip()

    def check_renames(self, texts: list[str]) -> None:
        """A new name must not meet another identifier: a local, a parameter, a field, a global or
        another function. The port's code and lists name some functions too, and a renamed one must
        change there in the same commit."""
        problems = []
        olds = set(self.rename)
        tokens = set()
        for text in texts:
            tokens.update(re.findall(r"\b[A-Za-z_]\w*\b", text))
        for new, count in collections.Counter(self.rename.values()).items():
            if not re.fullmatch(r"[A-Za-z_]\w*", new) or new in CPP_KEYWORDS:
                problems.append(f"{new}: not a C++ identifier")
            elif count > 1:
                problems.append(f"{new}: names {count} functions")
            elif new in tokens and new not in olds:
                problems.append(f"{new}: the code has this identifier already")
        port = [ROOT / "tools" / "decomp" / "spills.txt", *(ROOT / "port").glob("*.txt"),
                *(p for p in (ROOT / "port").rglob("*") if p.suffix in (".cpp", ".h", ".mm") and p.is_file())]
        for path in port:
            used = olds & set(re.findall(r"\b[A-Za-z_]\w*\b", path.read_text(errors="replace")))
            for old in sorted(used):
                problems.append(f"{old} -> {self.rename[old]}: rename it in {path.relative_to(ROOT)} too")
        if problems:
            sys.exit("convert: names in game/symbols.tsv that cannot apply:\n  " + "\n  ".join(problems))

    def load(self) -> None:
        self.load_names()
        index = (self.ghidra / "c" / "index.tsv").read_text().splitlines()[1:]
        rows = [row.split("\t")[:4] for row in index]
        # Each function that a user row names otherwise than Ghidra did is renamed in every
        # function's C and sidecar.
        self.rename: dict[str, str] = {}
        for addr, name, _, _ in rows:
            row = self.names.get(addr)
            if row and row[1] == "user" and row[0] != name:
                self.rename[name] = row[0]
        starts = {addr for addr, _, _, _ in rows}
        for addr, (name, source, _) in sorted(self.names.items()):
            if source == "user" and addr not in starts and int(addr, 16) < CODE_END:
                print(f"convert: game/symbols.tsv names {name} at 0x{addr}, where no function starts")
        texts = {}
        for addr, name, _, status in rows:
            side_path = self.ghidra / "c" / f"{addr}.json"
            if side_path.exists():
                texts[addr] = (side_path.read_text(), (self.ghidra / "c" / f"{addr}.c").read_text())
        if self.rename:
            self.check_renames([c for _, c in texts.values()])
            token = re.compile(r"\b[A-Za-z_]\w*\b")
            for addr, (side, c) in texts.items():
                texts[addr] = tuple(token.sub(lambda m: self.rename.get(m.group(0), m.group(0)), t)
                                    for t in (side, c))
        for addr, name, size, status in rows:
            if status != "ok":
                self.report.append(f"not decompiled: {name} @ {addr}: {status}")
            if addr not in texts:
                continue
            side = json.loads(texts[addr][0])
            c = texts[addr][1]
            try:
                f = Function(addr, c, side, self.ty)
            except ValueError as e:
                self.report.append(f"skipped {name} @ {addr}: {e}")
                continue
            f.comment = self.names.get(addr, ("", "", ""))[2]
            if f.name in self.by_name:
                f.ident = f"{f.ident}_{addr}"
                self.dup_names.add(f.name)
            self.functions.append(f)
            self.by_name.setdefault(f.name, f)
            self.by_addr[addr] = f

    def image_word(self, address: int) -> int:
        if not hasattr(self, "_image"):
            self._image = (ROOT / "game" / "extracted" / "1ST_READ.plain.bin").read_bytes()
        o = address - 0x8C010000
        if o < 0 or o + 4 > len(self._image):
            return 0
        return int.from_bytes(self._image[o:o + 4], "little")

    def drop_data_functions(self) -> None:
        """Ghidra's pointer scan makes functions in the data area. Keep a function only where
        dream-recomp's discovery found one too, where it has a library name, or where a kept
        function calls it."""
        recomp = self.ghidra / "recomp-functions.json"
        if not recomp.exists():
            return
        found = {int(f["entry"], 16) for f in json.loads(recomp.read_text())["functions"]}
        keep = {f.name for f in self.functions
                if int(f.addr, 16) < CODE_END
                and (int(f.addr, 16) in found or not f.name.startswith("FUN_"))}
        names = {f.name: f for f in self.functions}
        work = list(keep)
        while work:
            f = names[work.pop()]
            for ident in set(re.findall(r"\b([A-Za-z_]\w*)\s*\(", f.c)):
                if ident in names and ident not in keep:
                    keep.add(ident)
                    work.append(ident)
        dropped = [f for f in self.functions if f.name not in keep]
        for f in dropped:
            self.report.append(f"dropped as data: {f.name} @ {f.addr}")
        # The C indexes some of them as the tables they are (_ctype): those become byte arrays.
        self.data_names = {f.name: int(f.addr, 16) for f in dropped}
        self.functions = [f for f in self.functions if f.name in keep]
        self.by_name = {}
        for f in self.functions:
            self.by_name.setdefault(f.name, f)

    def tail_call_returns(self) -> None:
        """A function that ends by calling another returns that callee's r0: SHC compiles
        `return f(x)` to a jump, and Ghidra then shows `f(x); return;` in a void function. Callers
        through code pointers (driver tables) read the value. Returning it is exact either way:
        r0 holds the callee's result when the function returns."""
        # A call statement: a direct call by name, or a call through a pointer.
        tail = re.compile(r"\n  ((?:\(\*+[^;\n]*?\)|[A-Za-z_]\w*)\([^;]*\));\n  return;\n}\s*$", re.S)

        def callee_ret(call: str) -> str | None:
            m = re.match(r"([A-Za-z_]\w*)\(", call)
            if m:
                g = self.by_name.get(m.group(1))
                return g.ret if g is not None else None
            m = re.match(r"\(\*PTR_([A-Za-z_]\w*?)_[0-9a-f]{8}\)\(", call)
            if m and m.group(1) in self.by_name:
                return self.by_name[m.group(1)].ret
            return "undefined4"  # through an unknown pointer: r0 as it is

        changed = True
        while changed:
            changed = False
            for f in self.functions:
                if self.ty.type_only(f.ret) != "void" or f.name in self.skip or f.ident in self.skip:
                    continue
                m = tail.search(f.c)
                if not m:
                    continue
                ret = callee_ret(m.group(1))
                if ret is None or self.ty.type_only(ret) == "void":
                    continue
                f.ret = ret
                f.c = f.c[: m.start()] + f"\n  return {m.group(1)};\n}}\n"

                # Other paths: a call right before a return is a tail call too; any other return
                # left r0 as it was, which Ghidra did not keep.
                def inner(mm: re.Match) -> str:
                    before = mm.string[: mm.start()].rstrip()
                    if before and not before.endswith((";", "{", "}", ":", "*/")):
                        return mm.group(0)  # the continuation of a longer statement
                    r = callee_ret(mm.group(2))
                    if r is not None and self.ty.type_only(r) != "void":
                        return f"{mm.group(1)}return {mm.group(2)};"
                    return mm.group(0)
                f.c = re.sub(r"(\n\s+)((?:\(\*+[^;\n]*?\)|[A-Za-z_]\w*)\([^;]*\));\n\s+return;",
                             inner, f.c)
                f.c = re.sub(r"\breturn;", "return {};", f.c)
                self.report.append(f"returns its tail call's result: {f.name}")
                changed = True

    def load_stubs(self) -> None:
        self.stubs: set[str] = set()
        path = ROOT / "port" / "stubbed.txt"
        if path.exists():
            for line in path.read_text().splitlines():
                line = line.split("#", 1)[0].strip()
                if line:
                    self.stubs.add(line)

    def stub_body(self, f: Function) -> str:
        ret = self.ty.type_only(f.ret)
        body = "  return {};" if ret != "void" else "  return;"
        return (f"// {f.name} @ 0x{f.addr}: a stub (port/stubbed.txt)\n{f.prototype()}\n{{\n"
                + "".join(f"  (void){n};\n" for _, n in f.params) + body + "\n}\n")

    def failed_body(self, f: Function) -> str:
        return (f"// {f.name} @ 0x{f.addr}: the decompiler failed on it; write it in port/ or fix/\n"
                f"{f.prototype()}\n{{\n" + "".join(f"  (void){n};\n" for _, n in f.params)
                + f"  hw::unimplemented(\"{f.name} (not decompiled)\");\n}}\n")

    def load_interpreted(self) -> None:
        self.interpreted: set[str] = set()
        path = self.interpreted_list
        if path.exists():
            for line in path.read_text().splitlines():
                line = line.split("#", 1)[0].strip()
                if line:
                    self.interpreted.add(line.split()[0])

    def interpreted_body(self, f: Function) -> str:
        """A function the port runs on its SH-4 interpreter: the arguments go where the sidecar's
        storage puts them, and the result comes from r0 or fr0."""
        storage = {l["param"]: l.get("storage", "") for l in f.side.get("locals", []) if l.get("param", -1) >= 0}
        lines = [f"// {f.name} @ 0x{f.addr}: hand-written SH-4 code; the port interprets it (port/interpreted.txt)",
                 f.prototype(), "{", "  CallArgs a;"]
        for i, (t, n) in enumerate(f.params):
            st = storage.get(i, "")
            m = re.match(r"(f?)r(\d+):", st)
            if m and m.group(1) == "f" and 4 <= int(m.group(2)) <= 11:
                lines.append(f"  a.fr[{int(m.group(2)) - 4}] = pun<float>({n});")
            elif m and not m.group(1) and 4 <= int(m.group(2)) <= 7:
                lines.append(f"  a.r[{int(m.group(2)) - 4}] = to_word({n});")
            elif (sm := re.match(r"Stack\[0x([0-9a-f]+)\]", st)):
                lines.append(f"  a.stack[{int(sm.group(1), 16) // 4}] = to_word({n});")
            elif (cm := re.match(r"r(8|9|1[0-4]):", st)):
                lines.append(f"  cpu::ctx[{cm.group(1)}] = to_word({n});")  # sh4::run starts r8 to r14 from these
            else:
                raise ValueError(f"interpreted function {f.name}: parameter {n} has storage {st!r}")
        nr, nfr, nst = self.arg_counts(f)
        lines.append(f"  a.nr = {nr}; a.nfr = {nfr}; a.nstack = {nst};")
        ret = self.ty.type_only(f.ret)
        if ret == "void":
            lines.append(f"  sh4::run(0x{f.addr}u, a);")
        elif ret in self.WIDE_TYPES and self.returns_fpu_pair(f):
            lines += [f"  const CallResult r = sh4::run(0x{f.addr}u, a);",
                      f"  return ({ret})(((uint64_t)to_word(r.fr0) << 32) | to_word(r.fr1));"]
        else:
            lines.append(f"  return sh4::run(0x{f.addr}u, a);")
        return "\n".join(lines + ["}", ""])

    ARG_INPUT = re.compile(r"in_(r[5-7]|fr(?:[4-9]|1[01])|dr(?:4|6|8|10))")

    def lost_inputs(self) -> list[tuple[str, list[str]]]:
        """The converted functions whose C reads r5 to r7, fr4 to fr11 or a pair of them without a
        parameter: Ghidra's in_r5 and its kind. Interrupt handlers and the interpreted functions
        take what they find there by design, and are listed too."""
        out = []
        for f in self.functions:
            if f.ident in self.interpreted or f.name in self.skip or f.ident in self.skip:
                continue
            regs = sorted({m.group(1) for l in f.side.get("locals", [])
                           if (m := self.ARG_INPUT.fullmatch(l.get("name") or ""))})
            if regs:
                out.append((f.name, regs))
        return out

    @staticmethod
    def arg_counts(f: Function) -> tuple[int, int, int]:
        """The argument registers r4 on, fr4 on, and the stack words that a function's parameters take."""
        storage = [l.get("storage", "") for l in f.side.get("locals", []) if l.get("param", -1) >= 0]
        nr = max([int(m.group(1)) - 3 for st in storage if (m := re.match(r"r(\d+):", st))] + [0])
        nfr = max([int(m.group(1)) - 3 for st in storage if (m := re.match(r"fr(\d+):", st))] + [0])
        nst = max([int(m.group(1), 16) // 4 + 1 for st in storage
                   if (m := re.match(r"Stack\[0x([0-9a-f]+)\]", st))] + [0])
        return nr, nfr, nst

    def load_spills(self) -> None:
        self.spills: dict[str, tuple[str, list[str]]] = {}
        path = ROOT / "tools" / "decomp" / "spills.txt"
        for line in path.read_text().splitlines():
            words = line.split("#", 1)[0].split()
            if words:
                self.spills[words[0]] = (words[1], words[2:])

    def load_skips(self) -> None:
        self.hooked: set[str] = set()
        hooked = ROOT / "port" / "hooked.txt"
        if hooked.exists():
            for line in hooked.read_text().splitlines():
                line = line.split("#", 1)[0].strip()
                if line:
                    self.hooked.add(line.split()[0])
        self.replaced: set[str] = set()
        replaced = ROOT / "port" / "replaced.txt"
        if replaced.exists():
            for line in replaced.read_text().splitlines():
                line = line.split("#", 1)[0].strip()
                if line:
                    self.skip.add(line.split()[0])
                    self.replaced.add(line.split()[0])
        fix = self.out / "fix"
        if fix.exists():
            for p in sorted(fix.glob("*.cpp")):
                for m in re.finditer(r"^\S[^;{]*?\b([A-Za-z_]\w*)\s*\([^;]*\)\s*\{", p.read_text(), re.M):
                    self.skip.add(m.group(1))

    # Globals: one declaration per name with the type most functions give it, and an override in
    # each function that uses it with another type.
    def global_type(self, g: dict) -> str:
        """The variable's type in the function, unless the symbol's own type is a structure, an
        array or a pointer, which the C then reaches with `.`, `[]` or `*` as declared."""
        st = g.get("symtype", g["type"])
        # A string's label: the characters, so that the name decays to their address as in C.
        if st in ("string", "TerminatedCString") and g.get("size", 0) > 1:
            return f"char[{g['size']}]"
        if st == "unicode" and g.get("size", 0) > 2:
            return f"wchar16[{g['size'] // 2}]"
        base, ops = self.ty.parse(st)
        if ops or self.ty.kind.get(base, {}).get("kind") in ("struct", "union"):
            return st
        return g["type"]

    def count_call_args(self) -> None:
        """The fewest arguments any call site passes to each function."""
        self.fewest_args: dict[str, int] = {}
        call = re.compile(r"(?:\(\*PTR_([A-Za-z_]\w*?)_[0-9a-f]{8}\)|\b([A-Za-z_]\w*))\(")
        for f in self.functions:
            c = f.c
            for m in call.finditer(c):
                name = m.group(1) or m.group(2)
                if name not in self.by_name:
                    continue
                depth, i, n, empty = 1, m.end(), 0, True
                while i < len(c) and depth:
                    ch = c[i]
                    if ch in "([":
                        depth += 1
                    elif ch in ")]":
                        depth -= 1
                    elif ch == "," and depth == 1:
                        n += 1
                    elif not ch.isspace() and depth >= 1:
                        empty = False
                    i += 1
                args = 0 if empty else n + 1
                if m.start() > 0 and c[m.start() - 1] in "*&":
                    continue
                self.fewest_args[name] = min(self.fewest_args.get(name, args), args)

    def collect_globals(self) -> None:
        types = collections.defaultdict(collections.Counter)
        addr_of: dict[str, int] = {}
        for f in self.functions:
            for g in f.side["globals"]:
                if not g["name"] or "address" not in g:
                    continue
                if g["name"] in self.by_name or g["name"].startswith("LAB_"):
                    continue
                types[g["name"]][self.global_type(g)] += 1
                addr_of[g["name"]] = int(g["address"], 16)
        self.globals = {n: (c.most_common(1)[0][0], addr_of[n]) for n, c in types.items()}

    def global_def(self, name: str, gtype: str, addr: int) -> str:
        t, suffix = self.ty.cpp(gtype)
        full = t + suffix
        if full == "code":
            return f"#define {name} (gref<code>{{0x{addr:08x}u}})"
        if (addr & 0x1F000000) == 0x0C000000 and addr < 0xE0000000:
            return f"#define {name} (guest::host<{full}>(0x{addr:08x}u))"
        if suffix:
            return f"#define {name} (guest::host<{full}>(0x{addr:08x}u))"
        return f"#define {name} (gref<{full}>{{0x{addr:08x}u}})"

    def emit_globals(self) -> str:
        out = ["// Generated by tools/decomp/convert.py. Do not edit.", "#pragma once",
               '#include "types.h"', ""]
        for name in sorted(self.globals):
            gtype, addr = self.globals[name]
            out += doc_lines(self.global_comments.get((addr & 0x1FFFFFFF) | 0x80000000, ""))
            out.append(self.global_def(cpp_ident(name), gtype, addr))
        return "\n".join(out) + "\n"

    def emit_functions_h(self) -> str:
        out = ["// Generated by tools/decomp/convert.py. Do not edit.", "#pragma once",
               '#include "types.h"', "", "namespace game {", ""]
        for f in self.functions:
            if f.varargs and (f.name in self.skip or f.ident in self.skip):
                continue  # port/include/native.h declares it, as a template that takes the words
            out += doc_lines(f.comment)
            fewest = self.fewest_args.get(f.name)
            if fewest is not None and fewest < len(f.params) and not f.varargs:
                # Some call passes fewer arguments than Ghidra's prototype has. On the console
                # the missing ones were whatever the registers held.
                self.report.append(f"called with {fewest} of {len(f.params)} arguments: {f.name}")
                out.append(f"{f.prototype(defaults_from=fewest)};  // 0x{f.addr}")
            else:
                out.append(f"{f.prototype()};  // 0x{f.addr}")
            if f.ident in self.hooked:
                out.append(f"{f.prototype(ident=f.ident + '_game')};  // the game's body, which {f.ident} wraps")
        out += ["", "}  // namespace game"]
        return "\n".join(out) + "\n"

    def convert_body(self, f: Function) -> str:
        c = f.c
        lines = c.splitlines()
        # Drop the header comment and Ghidra's own prototype; keep warnings as comments.
        body_start = next(i for i, l in enumerate(lines) if l.startswith("{"))
        body = lines[body_start:]
        locals_by_name = {l["name"]: l for l in f.side["locals"] if l["name"]}
        f.printed = {t_n: t for t, t_n in f.params}
        text = "\n".join(body)

        # Which locals need guest addresses: arrays, structures, and anything whose address is taken.
        taken = set(re.findall(r"&\s*([A-Za-z_]\w*)", text))
        # _X is X read at a wider size than it is declared (Ghidra's naming); both share storage.
        wide = {m for m in re.findall(r"\b_([A-Za-z]\w*)\b", text)
                if m in locals_by_name and f"_{m}" not in locals_by_name}
        # A pair store into _X writes X and the word after it.
        wide8 = {m for m in wide if re.search(r"\b_" + m + r"\s*=\s*(?:CONCAT44|\((?:u?longlong|undefined8)\))", text)}
        frame_size = 0
        decl_lines: list[str] = []
        rest_start = 1
        for i in range(1, len(body)):
            line = body[i]
            m = re.match(r"^  (?!return\b)([A-Za-z_][\w ]*?)\s*(\**)\s*([A-Za-z_]\w*)\s*((?:\[\d+\])*)\s*;$", line)
            # A pointer to an array: char (*pacVar4) [32];
            pa = re.match(r"^  ([A-Za-z_][\w ]*?)\s*\(\s*\*\s*([A-Za-z_]\w*)\s*\)\s*((?:\[\d+\])+)\s*;$", line)
            if not m and not pa:
                rest_start = i
                break
            name = m.group(3) if m else pa.group(2)
            loc = locals_by_name.get(name)
            # The declaration Ghidra printed is the type the body uses; the sidecar adds the storage.
            if m:
                gtype = (m.group(1).strip() + (" " + m.group(2) if m.group(2) else "") + m.group(4)).strip()
            else:
                gtype = f"{pa.group(1).strip()}{pa.group(3)} *"
            f.printed[name] = gtype
            t, suffix = self.ty.cpp(gtype)
            base_kind = self.ty.kind.get(self.ty.parse(gtype)[0], {})
            is_array = bool(suffix) or (base_kind.get("kind") == "typedef" and "[" in base_kind.get("base", "")
                                        and not self.ty.parse(gtype)[1])
            is_struct = base_kind.get("kind") in ("struct", "union")
            stack = loc.get("stack") if loc else None
            # Every local with a stack slot lives on the guest stack at that slot: Ghidra splits one
            # buffer into several locals, and a callee writing through a pointer to the first
            # reaches the others only there.
            if stack is not None and stack < 0:
                frame_size = max(frame_size, -stack)
                if is_array:
                    decl_lines.append(f"  garray<{t}{suffix}> {name} = _frame.array<{t}{suffix}>({stack});")
                else:
                    decl_lines.append(f"  {t} &{name} = _frame.at<{t}>({stack});")
                if name in wide:
                    wt = "undefined8" if name in wide8 else "uint32_t"
                    decl_lines.append(f"  {wt} &_{name} = _frame.at<{wt}>({stack});")
            elif re.match(r"^(in_|unaff_|extraout_)", name):
                flag = re.match(r"^(?:in|extraout)_(T|S|IMASK|Q|M|FD|BL|RB|MD)(?:_\d+)?$", name)
                if suffix:
                    decl_lines.append(f"  {t} {name}{suffix} = {{}};")
                elif flag:
                    # SR's fields as Ghidra splits them; the port keeps SR whole.
                    decl_lines.append(f"  {t} {name} = ({t})cpu::sr_field(\"{flag.group(1)}\");")
                elif fpscr := re.match(r"^(?:in|extraout)_FPSCR_(RM|FLAG|ENABLE|CAUSE|DN|PR|SZ|FR)(?:_\d+)?$", name):
                    decl_lines.append(f"  {t} {name} = ({t})cpu::fpscr_field(\"{fpscr.group(1)}\");")
                elif name in ("in_GBR",):
                    decl_lines.append(f"  {t} {name} = ({t})cpu::gbr;")
                elif name in ("in_VBR",):
                    decl_lines.append(f"  {t} {name} = ({t})cpu::vbr;")
                else:
                    decl_lines.append(
                        f"  {t} {name} = ({t})cpu::unknown(\"{f.name}\", \"{name}\");")
            else:
                decl_lines.append(f"  {t} {name}{suffix}{'' if suffix else ' = {}'};")
        else:
            rest_start = len(body)
        for name in sorted(wide):
            loc = locals_by_name[name]
            if loc.get("param", -1) >= 0 and f"_{name}" not in "\n".join(decl_lines):
                decl_lines.append(f"  uint32_t _{name} = to_word({name});")
            elif f"&_{name} " not in "\n".join(decl_lines) and f" _{name} " not in "\n".join(decl_lines):
                decl_lines.append(f"  uint32_t _{name} = {{}};")

        rest = "\n".join(body[rest_start:])
        # References to the caller's frame or to stack space without a variable.
        stack_refs = set(re.findall(r"\b(stack0x[0-9a-f]{8})\b", rest)) | set(
            re.findall(r"\b(in_stack_[0-9a-f]{8})\b", rest))
        for s in sorted(stack_refs):
            off = int(s[-8:], 16)
            off = off - (1 << 32) if off >= 0x80000000 else off
            if off < 0:
                frame_size = max(frame_size, -off)
            declared = any(re.search(r"[\s&]" + s + r"\b", d) for d in decl_lines)
            if declared:
                continue
            if s.startswith("in_stack_"):
                decl_lines.append(f"  uint32_t &{s} = _frame.at<uint32_t>({off});")
            elif s.startswith("stack0x") and re.search(r"\b" + s + r"\s*=\s*(?:CONCAT44|\(ulonglong\))", rest):
                # A pair store: FUN_8c0c357c writes a point's z this way, and as one byte the point
                # kept its old z.
                decl_lines.append(f"  undefined8 &{s} = _frame.at<undefined8>({off});")
            elif s.startswith("stack0x"):
                decl_lines.append(f"  uint8_t &{s} = _frame.at<uint8_t>({off});")
        # A stack argument that the body uses only through its own address. A variadic function
        # passes the address of its first value to vsprintf, and the values are the words from there
        # on in the caller's frame: the address is the argument's place there. Ghidra 12.2 names the
        # place after the argument, 12.1.4 wrote stack0x.
        for name, loc in locals_by_name.items():
            off = loc.get("stack")
            if loc.get("param", -1) < 0 or off is None or off < 0:
                continue
            own_address = r"&\s*" + re.escape(name) + r"\b(?!\s*(?:->|\.|\[))"
            uses = len(re.findall(r"\b" + re.escape(name) + r"\b", rest))
            if uses and len(re.findall(own_address, rest)) == uses:
                place = f"stack0x{off:08x}"
                if place not in stack_refs:
                    decl_lines.append(f"  uint8_t &{place} = _frame.at<uint8_t>({off});")
                rest = re.sub(own_address, "&" + place, rest)
        if frame_size or any("_frame." in d for d in decl_lines):
            decl_lines.insert(0, f"  guest::Frame _frame(0x{frame_size:x});")
        # The address and the arguments are for the differential tests (port/include/diff.h).
        trace_args = [f"0x{f.addr}u"] + [n for _, n in f.params]
        decl_lines.insert(0, f"  TRACE_FUNCTION(\"{f.name}\", {', '.join(trace_args)});")
        # Spills of register arguments that the decompiler dropped (tools/decomp/spills.txt).
        if f.name in self.spills:
            array, params = self.spills[f.name]
            decl_lines += [f"  {array}[{i}] = to_word({p});" for i, p in enumerate(params)]

        # A jmp through a register passes the argument registers on unchanged. Ghidra shows such a
        # tail call with an empty argument list; the function's own parameters are what it passes.
        if f.params:
            args = ", ".join(n for _, n in f.params)
            rest = re.sub(r"(/\* WARNING: Treating indirect jump as call \*/\n[^\n]*?\))\(\)",
                          lambda m: f"{m.group(1)}({args})", rest)
        for c in f.side.get("calls", []):
            target = self.by_addr.get(c["entry"])
            if target is not None and target.ident != c["name"] and c["name"] in self.dup_names:
                rest = re.sub(r"\b" + re.escape(c["name"]) + r"\(", target.ident + "(", rest)
        # A call through a literal-pool slot is a call to the slot's function: the slot is constant.
        # The slot's value in the image names the function, also where two share a name.
        def pool_call(m: re.Match) -> str:
            addr = int(m.group(2), 16)
            if addr >= CODE_END:
                return m.group(0)
            g = self.by_addr.get(f"{self.image_word(addr):08x}")
            if g is None:
                return m.group(0)
            return g.ident + "("
        rest = re.sub(r"\(\*PTR_([A-Za-z_]\w*?)_([0-9a-f]{8})\)\(", pool_call, rest)
        rest = self.trim_extra_args(rest)
        rest = self.convert_expressions(rest, f)
        # A parameter or local with a type's name hides the type inside the function.
        for n in set(f.printed) & {cpp_ident(k) for k in self.ty.kind}:
            rest = re.sub(r"gptr<" + re.escape(n) + r"\b", f"gptr<game::{n}", rest)
        rest, extra_defs, extra_undefs = self.unnamed_globals(rest, f)
        sig = f.prototype(ident=f.ident + "_game" if f.ident in self.hooked else None)
        header = f"// {f.name} @ 0x{f.addr}"
        overrides, undefs = self.global_overrides(f)
        rest = self.byte_compares(rest, overrides + extra_defs)
        return "\n".join([header] + doc_lines(f.comment) + overrides + extra_defs + [sig, "{"] + decl_lines + [rest]
                         + undefs + extra_undefs) + "\n"

    # Ghidra's prefixes for a global it has no symbol for give the type: uRam8c02ebc0 is a uint.
    RAM_PREFIX = {
        "u": "uint", "i": "int", "s": "short", "S": "short", "c": "char", "b": "byte", "f": "float",
        "us": "ushort", "uc": "uchar", "pc": "char *", "pu": "undefined4 *", "pi": "int *",
        "ps": "short *", "pf": "float *", "pp": "undefined4 **", "pb": "byte *", "pv": "void *",
        "d": "double", "l": "longlong", "ul": "ulonglong", "pcVar": "code *", "pus": "ushort *",
        "puc": "uchar *", "pbVar": "byte *", "_DAT_": "undefined4", "DAT_": "undefined4",
    }

    def unnamed_globals(self, text: str, f: Function) -> tuple[str, list[str], list[str]]:
        by_addr = {int(g["address"], 16): g for g in f.side["globals"]
                   if "address" in g and not g["name"]}
        named = {g["name"] for g in f.side["globals"] if g["name"]}
        defs, undefs, seen = [], [], set()
        # Names that are not C++ identifiers (s_%d_8c0d1234): renamed in the body and in globals.h.
        for n in sorted(named, key=len, reverse=True):
            if cpp_ident(n) != n:
                text = text.replace(n, cpp_ident(n))
        # _X for a global X is X read at a wider size, with the type the sidecar gives it.
        for g in f.side["globals"]:
            n = g["name"]
            if n and "address" in g and re.search(r"\b_" + re.escape(cpp_ident(n)) + r"\b", text):
                ident = "_" + cpp_ident(n)
                if ident not in named and ident not in seen:
                    seen.add(ident)
                    defs.append(self.global_def(ident, g["type"], int(g["address"], 16)))
                    undefs.append(f"#undef {ident}")
        for n, addr in self.data_names.items():
            if re.search(r"\b" + re.escape(n) + r"\b", text) and n not in seen:
                seen.add(n)
                defs.append(f"#define {cpp_ident(n)} (garray<undefined1[1]>{{0x{addr:08x}u}})")
                undefs.append(f"#undef {cpp_ident(n)}")
        for m in re.finditer(r"\b(_DAT_|DAT_|LAB_|[a-zA-Z]+Ram|ram0x)([0-9a-f]{8})(_\d+)?\b", text):
            ident, prefix, hexaddr, piece = m.group(0), m.group(1), m.group(2), m.group(3)
            if ident in seen or ident in named or ident in self.globals:
                continue
            if prefix == "LAB_" and not piece:
                continue  # a code label: a goto target or a code address, handled elsewhere
            seen.add(ident)
            addr = int(hexaddr, 16) + (int(piece[1:]) if piece else 0)
            if addr in by_addr:
                gtype = self.global_type(by_addr[addr])
            else:
                p = prefix[:-3] if prefix.endswith("Ram") else prefix
                gtype = self.RAM_PREFIX.get(p, "undefined4")
            defs.append(self.global_def(ident, gtype, addr))
            undefs.append(f"#undef {ident}")
        return text, defs, undefs

    def global_overrides(self, f: Function) -> tuple[list[str], list[str]]:
        defs, undefs = [], []
        # Other names of a global's address, where the C prints one of them.
        text = f.c
        for g in f.side["globals"]:
            if "address" not in g:
                continue
            for alias in g.get("aliases", []):
                ident = cpp_ident(alias)
                if alias == g["name"] or ident in self.globals or alias in self.by_name:
                    continue
                if re.search(r"\b" + re.escape(alias) + r"\b", text):
                    defs.append(f"#undef {ident}")
                    defs.append(self.global_def(ident, self.global_type(g), int(g["address"], 16)))
                    undefs.append(f"#undef {ident}")
        for g in f.side["globals"]:
            n = g["name"]
            if not n or n not in self.globals or "address" not in g:
                continue
            if self.global_type(g) != self.globals[n][0]:
                ident = cpp_ident(n)
                defs.append(f"#undef {ident}")
                defs.append(self.global_def(ident, self.global_type(g), int(g["address"], 16)))
                undefs.append(f"#undef {ident}")
                undefs.append(self.global_def(ident, *self.globals[n]))
        return defs, undefs

    def byte_compares(self, text: str, defs: list[str]) -> str:
        """Ghidra compares an undefined byte global as a byte, and prints 0xff as -1 or '\\xff'.
        In C++ the byte is uint8_t and such a value never matches, so it becomes the byte value."""
        types = {cpp_ident(n): t for n, (t, _) in self.globals.items()}
        for d in defs:
            m = re.match(r"#define (\w+) \((?:guest::host<|gref<)(\w+)>", d)
            if m:
                types[m.group(1)] = m.group(2)
        for n, t in types.items():
            if t not in ("undefined", "undefined1") or n not in text:
                continue
            operand = rf"(\(gaddr\({n}\)\)\[[^\[\]]*\]|\b{n}\b(?!\s*\[))"

            def byte(m: re.Match) -> str:
                v = m.group(3)
                if v == "(char)":
                    v = "(uchar)"
                elif v.startswith("'"):
                    v = "0x" + v[3:5]
                elif int(v[1:], 0) <= 0x80:
                    v = f"0x{256 - int(v[1:], 0):02x}"
                return f"{m.group(1)} {m.group(2)} {v}"
            text = re.sub(operand + r"\s*([!=]=)\s*(-(?:0x[0-9a-f]+|\d+)\b|'\\x[89a-f][0-9a-f]'|\(char\))",
                          byte, text)
        return text

    def trim_extra_args(self, text: str) -> str:
        """Drop arguments beyond a known callee's parameters. The caller set a register the callee
        never reads; C++ does not allow the extra argument."""
        idents = {g.ident: g for g in self.functions}
        out, i = [], 0
        call = re.compile(r"\b([A-Za-z_]\w*)\(")
        while True:
            m = call.search(text, i)
            if not m:
                out.append(text[i:])
                break
            g = idents.get(m.group(1))
            out.append(text[i:m.end()])
            i = m.end()
            if g is None or g.varargs:
                continue
            depth, j, starts = 1, i, [i]
            while j < len(text) and depth:
                ch = text[j]
                if ch in "([{":
                    depth += 1
                elif ch in ")]}":
                    depth -= 1
                elif ch == "," and depth == 1:
                    starts.append(j + 1)
                j += 1
            args_text = text[i:j - 1]
            nargs = 0 if not args_text.strip() else len(starts)
            if nargs > len(g.params):
                keep = text[i:starts[len(g.params)] - 1] if g.params else ""
                out.append(self.trim_extra_args(keep) + ")")
                i = j
        return "".join(out)

    def convert_expressions(self, text: str, f: Function) -> str:
        text = convert_casts(text, self.ty)
        text = self.float_to_uint(text, f)
        # Code addresses used as values: function names not called, and code labels.
        def fn_value(m: re.Match) -> str:
            before, name, after = m.group(1), m.group(2), m.group(3)
            g = self.by_name.get(name)
            if g is not None and not after.lstrip().startswith("("):
                return f"{before.replace('&', '')}gptr<code>(0x{g.addr}u){after}"
            if (re.match(r"LAB_[0-9a-f]{8}$", name) and "goto" not in before
                    and not after.lstrip().startswith(":")):
                return f"{before.replace('&', '')}gptr<code>(0x{name[4:]}u){after}"
            return m.group(0)
        text = re.sub(r"((?:\bgoto\s+)?&?)\b([A-Za-z_]\w*)\b(\s*[(:]?)", fn_value, text)
        # Call sites of functions renamed for uniqueness.
        text = re.sub(r"\bhalt_baddata\(\)", f"hw::bad_data(\"{f.name}\")", text)
        text = re.sub(r"\bhalt_unimplemented\(\)", f"hw::unimplemented(\"{f.name}\")", text)
        text = re.sub(r"\bregister0x0000003c\b", "guest::sp", text)
        # A call to a function declared void whose result is used: void_value() counts it.
        def void_call(m: re.Match) -> str:
            pre, name = m.group(1), m.group(2)
            g = self.by_name.get(name)
            if g is None or self.ty.type_only(g.ret) != "void":
                return m.group(0)
            if re.match(r"^\s*$", pre):
                return m.group(0)  # a statement of its own
            return f"{pre}void_value({g.ident}, \"{name}\")("
        text = re.sub(r"(^[^\n]*?)\b([A-Za-z_]\w*)\(", lambda m: void_call(m) if m.group(1).strip() else m.group(0),
                      text, flags=re.M)
        text = re.sub(r"([=(,!&|+\-*/<>?:]\s*)\b([A-Za-z_]\w*)\(", void_call, text)
        # A call through a variable Ghidra typed as an integer.
        int_locals = {n for n, t in getattr(f, "printed", {}).items() if "*" not in t}
        text = re.sub(r"\(\*([A-Za-z_]\w*)\)\(",
                      lambda m: f"gfunc{{to_word({m.group(1)})}}(" if m.group(1) in int_locals else m.group(0),
                      text)
        # A switch on an undefined word: the word converts to every integer type, so C++ needs one.
        raw_words = {n for n, t in getattr(f, "printed", {}).items() if t in ("undefined4", "undefined8")}
        text = re.sub(r"\bswitch\s*\(([A-Za-z_]\w*)\)",
                      lambda m: f"switch(to_word({m.group(1)}))" if m.group(1) in raw_words else m.group(0), text)
        text = re.sub(r"\bprefetch\(", "hw::prefetch_any(", text)
        text = re.sub(r"\bset(SR|GBR|VBR|FPSCR)\(", lambda m: f"cpu::set_{m.group(1).lower()}_any(", text)
        # fmac's result has no type in the decompiler, which prints it as a bit cast to float.
        text = re.sub(r"\bpun<float>\(fmac\(", "(fmac(", text)
        # A variable that the decompiler typed as an integer can receive fmac's result where Ghidra
        # merged several uses of one register. The register then holds the float's bits:
        # FUN_8c0aa988 stores such a result in its object, and as a C++ conversion -0.0023 became 0.
        integral = {n: t for n, t in getattr(f, "printed", {}).items() if t in ("int", "uint", "Sint32", "Uint32")}
        def keep_fmac_bits(code: str) -> str:
            out, at = [], 0
            for m in re.finditer(r"\b([A-Za-z_]\w*) = \(?fmac\(", code):
                if m.group(1) not in integral or m.start() < at:
                    continue
                start = m.end(1) + 3
                depth, end = 0, start
                while end < len(code) and not (code[end] == ";" and depth == 0):
                    depth += code[end] == "("
                    depth -= code[end] == ")"
                    end += 1
                out.append(code[at:start] + f"pun<{integral[m.group(1)]}>(" + code[start:end] + ")")
                at = end
            return "".join(out) + code[at:]
        text = keep_fmac_bits(text)
        # A denormal literal is an integer in a word that Ghidra typed as a float: FUN_8c06ff30 tests
        # param_1[7] == 0x1130f with cmp/eq. As a float compare under FPSCR.DN, both sides flush to 0.
        def denormal_test(m: re.Match) -> str:
            bits = struct.unpack("<I", struct.pack("<f", float(m.group(3))))[0]
            if bits & 0x7F800000:
                return m.group(0)
            return f"pun<uint32_t>({m.group(1)}) {m.group(2)} 0x{bits:08x}u"
        operand = r"[A-Za-z_]\w*(?:\.[A-Za-z_]\w*|\[[^\[\]]*\]|->[A-Za-z_]\w*)*"
        text = re.sub(r"(" + operand + r")\s*(==|!=)\s*(\d+\.?\d*[eE]-\d+)(?![\w.])", denormal_test, text)
        # SHC's double is single precision, so a floating literal is a float. As a C++ double, 1.0 in
        # x * k + 1.0 kept the sum unrounded, and FUN_8c07be04 differed in the last bit. The C
        # library's functions that switch the FPU to double precision keep their doubles.
        if not re.search(r"\bdouble\b", text):
            def single(code: str) -> str:
                return re.sub(r"(?<![\w.])((?:\d+\.\d*|\.\d+)(?:[eE][+-]?\d+)?|\d+[eE][+-]?\d+)(?![\w.])",
                              r"\1f", code)
            parts = re.split(r"(\"(?:[^\"\\\n]|\\.)*\"|'(?:[^'\\\n]|\\.)*')", text)
            text = "".join(p if i % 2 else single(p) for i, p in enumerate(parts))
        # Unary &: Ghidra writes the binary operator with spaces, the unary one without.
        postfix = r"[A-Za-z_]\w*(?:\.[A-Za-z_]\w*|\[[^\[\]]*\]|->[A-Za-z_]\w*)*"
        text = re.sub(r"(?<![&\w)\]])&(" + postfix + ")", r"gaddr(\1)", text)
        text = re.sub(r"(\))&(" + postfix + ")", r"\1gaddr(\2)", text)
        # The same for a member of a parenthesized operand: (int)&(points->position).y.
        member = r"\((?:[^()]|\([^()]*\))*\)(?:\.[A-Za-z_]\w*|\[[^\[\]]*\]|->[A-Za-z_]\w*)+"
        text = re.sub(r"(?<![&\w)\]])&(" + member + ")", r"gaddr(\1)", text)
        text = re.sub(r"(\))&(" + member + ")", r"\1gaddr(\2)", text)
        # A global array cast to an integer is its guest address, and so is a row of one with more
        # dimensions. The host array would decay to a host pointer: fadeDraw passes (int)fadeQuad to
        # the screen-mode copy, and scriptGroupRemove reads (int)scriptGroups[group].
        for g in f.side["globals"]:
            dims = self.ty.cpp(self.global_type(g))[1].count("[") if g["name"] and "address" in g else 0
            if dims:
                text = re.sub(r"\(((?:u?int|undefined4|dword))\)(" + re.escape(cpp_ident(g["name"]))
                              + r"\b(?:\[[^\[\]]*\]){0," + str(dims - 1) + r"})(?!\s*(?:\[|\.|->|\())",
                              r"(\1)gaddr(\2)", text)
        # Sub-pieces: x._4_4_ is the 4 bytes of x at offset 4.
        fpu = {l["name"] for l in f.side["locals"]
               if l.get("register", "")[:2] in ("dr", "xd", "fr")}

        def piece(m: re.Match) -> str:
            name, off, size, marked = m.group(1), m.group(2), m.group(3), m.group(4)
            is_float = size == "4" and (marked or name in fpu or re.match(r"xd\d+$", name))
            return f"{'PIECE_F' if is_float else 'PIECE'}({name}, {off}, {size})"
        # x._4_4f_: ExportDecompiled marks the pieces that hold a float.
        text = re.sub(r"\b([A-Za-z_]\w*)\._(\d+)_(\d+)(f?)_", piece, text)
        return text

    def module_of(self, f: Function) -> str:
        n = f.name.lstrip("_")
        for prefix, mod in (("nj", "ninja"), ("km", "kamui"), ("sy", "shinobi"), ("sd", "sound"),
                            ("mp", "sound_mp"), ("gd", "gdfs"), ("bu", "backup"), ("vms", "vmsfs"),
                            ("pd", "peripheral"), ("kd", "kd"), ("cv", "cv")):
            if n.startswith(prefix) and not f.name.startswith("FUN_"):
                return mod
        a = int(f.addr, 16)
        return f"game_{(a >> 14) << 14:08x}"

    WIDE_TYPES = ("undefined8", "ulonglong", "longlong", "qword", "double")

    def float_to_uint(self, text: str, f: Function) -> str:
        """A float converted to uint: the SH-4's ftrc keeps a negative value's bits, which a C++
        conversion makes 0 on the host, so cpu::ftrc converts it. The angles from mathAtan2 that
        ch1BossHomingUpdate turns toward are negative."""
        floats = {n for n, t in getattr(f, "printed", {}).items() if t == "float"}
        out, at = [], 0
        for m in re.finditer(r"\(uint\)\s*", text):
            if m.start() < at:
                continue
            rest = text[m.end():]
            if rest.startswith("("):
                inner = rest[1:]
                if not (inner.startswith(("pun<float>(", "(float)"))
                        or (re.match(r"[A-Za-z_]\w*", inner) and re.match(r"[A-Za-z_]\w*", inner).group(0) in floats)):
                    continue
                depth, end = 0, m.end()
                while end < len(text):
                    depth += text[end] == "("
                    depth -= text[end] == ")"
                    end += 1
                    if depth == 0:
                        break
                expr = text[m.end() + 1:end - 1]
                # A comparison yields a bool, not a float.
                top, d = [], 0
                for ch in expr:
                    d += ch == "("
                    d -= ch == ")"
                    top.append(ch if d == 0 else " ")
                if re.search(r"[<>]|==|!=|&&|\|\|", re.sub(r"\b\w+<[^<>()]*>", "", "".join(top))):
                    continue
                out.append(text[at:m.start()] + "cpu::ftrc(" + expr + ")")
                at = end
            else:
                n = re.match(r"[A-Za-z_]\w*", rest)
                if n is None or n.group(0) not in floats or rest[n.end():].lstrip().startswith(("(", "[", ".", "->")):
                    continue
                out.append(text[at:m.start()] + f"cpu::ftrc({n.group(0)})")
                at = m.end() + n.end()
        return "".join(out) + text[at:]

    def is_float_param(self, t: str) -> bool:
        ct = self.ty.type_only(t)
        return ct in ("float", "Float", "double") or (
            self.ty.base_cpp(self.ty.parse(t)[0]) in ("float",) and "*" not in t)

    def layout(self, f: Function) -> list[int] | None:
        """Where each parameter is passed, when that is not where the convention's order puts it:
        0 to 3 for r4 to r7, 4 to 11 for fr4 to fr11, 16 + k for the stack's word k, 32 + n for rn
        from r8 to r14. None when the convention's order is right, or when a parameter's storage is
        not one of these."""
        storage = {l["param"]: l.get("storage", "") for l in f.side.get("locals", []) if l.get("param", -1) >= 0}
        slots = []
        for i in range(len(f.params)):
            st = storage.get(i, "")
            if (m := re.fullmatch(r"r([4-7]):\d+", st)):
                slots.append(int(m.group(1)) - 4)
            elif (m := re.fullmatch(r"r(8|9|1[0-4]):\d+", st)):
                slots.append(32 + int(m.group(1)))  # a callee-saved register (ContextParams)
            elif (m := re.fullmatch(r"fr(\d+):4", st)) and 4 <= int(m.group(1)) <= 11:
                slots.append(int(m.group(1)))
            elif (m := re.fullmatch(r"Stack\[0x([0-9a-f]+)\]:\d+", st)) and int(m.group(1), 16) % 4 == 0:
                slots.append(16 + int(m.group(1), 16) // 4)
            else:
                return None
        convention = self.convention(f)
        return None if slots == convention else slots

    def convention(self, f: Function) -> list[int]:
        """Where the convention's order puts each parameter, in layout() numbers."""
        slots = []
        nr = nfr = 0
        for i, (t, _) in enumerate(f.params):
            if i not in f.from_fr and self.is_float_param(t) and nfr < 8:
                slots.append(4 + nfr)
                nfr += 1
            else:
                slots.append(nr if nr < 4 else 16 + nr - 4)
                nr += 2 if self.ty.type_only(t) in self.WIDE_TYPES else 1
        return slots

    # SHC passes the last named argument of a variadic function and the variable ones on the stack.
    # Every call of one has a fixed prototype in the C (OverrideIndirectCalls): the four word
    # registers, then the stack words its format takes, at most eight, in the convention's order.
    # A thunk cannot know the format, so it passes eight; the callee uses the ones it needs.
    VARARG_WORDS = 8

    def thunk(self, f: Function) -> str:
        if f.varargs:
            return self.thunk_from(f, self.convention(f), self.VARARG_WORDS)  # r4 to r7, then the stack
        if (slots := self.layout(f)) is not None:
            return self.thunk_from(f, slots)
        args = []
        nr = nfr = 0
        for i, (t, _) in enumerate(f.params):
            ct = self.ty.type_only(t)
            if i in f.from_fr:
                args.append(f"pun<{ct}>(a.fr[{nfr}])")
                nfr += 1
                continue
            if ct in ("float", "Float", "double") or self.ty.base_cpp(self.ty.parse(t)[0]) in ("float",) and "*" not in t:
                args.append(f"({ct})a.fr[{nfr}]")
                nfr += 1
                continue
            src = f"a.r[{nr}]" if nr < 4 else f"a.stack[{nr - 4}]"
            nr += 1
            if ct.startswith("gptr<"):
                args.append(f"{ct}({src})")
            elif ct in ("undefined8", "ulonglong", "longlong", "qword", "double"):
                hi = f"a.r[{nr}]" if nr < 4 else f"a.stack[{nr - 4}]"
                nr += 1
                args.append(f"({ct})(((uint64_t){hi} << 32) | {src})")
            elif self.ty.kind.get(self.ty.parse(t)[0], {}).get("kind") in ("struct", "union") and "*" not in t:
                args.append(f"guest::host<{ct}>({src})")
            else:
                args.append(f"({ct}){src}")
        return self.thunk_body(f, args)

    def thunk_from(self, f: Function, slots: list[int], stack_words: int = 0) -> str:
        """A thunk that takes each argument from where the function's storage puts it."""
        args = []
        for (t, _), slot in zip(f.params, slots):
            ct = self.ty.type_only(t)
            if slot >= 32:
                args.append(f"({ct})cpu::ctx[{slot - 32}]")  # the caller's register, from the interpreter
                continue
            if 4 <= slot < 16:
                args.append(f"({ct})a.fr[{slot - 4}]" if self.is_float_param(t) else f"pun<{ct}>(a.fr[{slot - 4}])")
                continue
            def word(k: int) -> str:
                return f"a.r[{k}]" if k < 4 else f"a.stack[{k - 16}]"
            src = word(slot)
            if ct.startswith("gptr<"):
                args.append(f"{ct}({src})")
            elif ct in self.WIDE_TYPES:
                args.append(f"({ct})(((uint64_t){word(slot + 1)} << 32) | {src})")
            elif self.ty.kind.get(self.ty.parse(t)[0], {}).get("kind") in ("struct", "union") and "*" not in t:
                args.append(f"pun<{ct}>({src})")
            else:
                args.append(f"({ct}){src}")
        if stack_words:
            args += [f"a.r[{k}]" for k in range(sum(1 for v in slots if v < 4), 4)]
            args += [f"a.stack[{k}]" for k in range(stack_words)]
        return self.thunk_body(f, args)

    @staticmethod
    def returns_fpu_pair(f: Function) -> bool:
        """Whether a 64-bit result comes back in fr0 and fr1 (Ghidra's dr0, or xd0 after the pair
        moves' language patch), not in r0 and r1."""
        return bool(re.match(r"(dr|xd|fr)0:8", f.side.get("return_storage", "")))

    def thunk_body(self, f: Function, args: list[str]) -> str:
        call = f"{f.ident}({', '.join(args)})"
        ret = self.ty.type_only(f.ret)
        if ret == "void":
            body = f"{call}; return {{}};"
        elif ret in ("float", "Float"):
            body = f"CallResult r; r.fr0 = {call}; return r;"
        elif ret in ("undefined8", "ulonglong", "longlong", "qword"):
            body = f"CallResult r; uint64_t v = (uint64_t){call}; r.r0 = (uint32_t)v; r.r1 = (uint32_t)(v >> 32);"
            if self.returns_fpu_pair(f):
                # The pair's high word is fr0: a caller in machine code reads the result there.
                body += " r.fr0 = pun<float>(r.r1); r.fr1 = pun<float>(r.r0);"
            body += " return r;"
        elif ret == "double":
            body = f"CallResult r; double d = {call}; uint64_t v; std::memcpy(&v, &d, 8); r.r0 = (uint32_t)v; r.r1 = (uint32_t)(v >> 32); return r;"
        else:
            body = f"CallResult r; r.r0 = to_word({call}); return r;"
        return f"static CallResult thunk_{f.ident}(const CallArgs& a) {{ (void)a; {body} }}"

    def run(self) -> int:
        self.load()
        self.drop_data_functions()
        self.load_skips()
        self.load_spills()
        self.load_interpreted()
        self.load_stubs()
        self.tail_call_returns()
        self.collect_globals()
        self.count_call_args()
        gen = self.out / "gen"
        (gen / "include").mkdir(parents=True, exist_ok=True)
        (gen / "src").mkdir(parents=True, exist_ok=True)
        for old in (gen / "src").glob("*.cpp"):
            old.unlink()
        (gen / "include" / "types.h").write_text(emit_types(self.ty))
        (gen / "include" / "globals.h").write_text(self.emit_globals())
        (gen / "include" / "functions.h").write_text(self.emit_functions_h())

        modules: dict[str, list[str]] = collections.defaultdict(list)
        wrapped: set[str] = set()
        for f in self.functions:
            if f.name in self.skip or f.ident in self.skip:
                continue
            try:
                if f.side.get("failed"):
                    modules["undecompiled"].append(self.failed_body(f))
                    continue
                if f.name in self.stubs or f.ident in self.stubs:
                    modules["stubs"].append(self.stub_body(f))
                    continue
                # By identifier: a name the game has twice names only its first function.
                if f.ident in self.interpreted:
                    modules["interpreted"].append(self.interpreted_body(f))
                    continue
                modules[self.module_of(f)].append(self.convert_body(f))
                wrapped.add(f.ident)
            except Exception as e:  # noqa: BLE001 - one bad function must not stop the rest
                self.report.append(f"conversion failed: {f.name} @ {f.addr}: {e}")
        # A hooked function that is not converted leaves its wrapper nothing to call.
        for name in sorted(self.hooked - wrapped):
            self.report.append(f"hooked but not converted: {name}")
            print(f"convert: {name} is in port/hooked.txt but not converted")
        prelude = ["// Generated by tools/decomp/convert.py from Ghidra's output. Do not edit;",
                   "// copy a function into fix/ to change it.",
                   '#include "decomp.h"', "", "namespace game {", ""]
        for mod, bodies in modules.items():
            (gen / "src" / f"{mod}.cpp").write_text(
                "\n".join(prelude + bodies + ["}  // namespace game", ""]))

        disp = ["// Generated by tools/decomp/convert.py. Do not edit.", '#include "decomp.h"', "",
                "namespace game {", ""]
        for f in self.functions:
            disp.append(self.thunk(f))
        disp += ["", "const DispatchEntry kDispatch[] = {"]
        for f in sorted(self.functions, key=lambda f: int(f.addr, 16)):
            disp.append(f"    {{0x{f.addr}u, thunk_{f.ident}}},")
        disp += ["};", "", "}  // namespace game", "",
                 "const DispatchEntry* const kDispatch = game::kDispatch;",
                 "const size_t kDispatchCount = sizeof(game::kDispatch) / sizeof(game::kDispatch[0]);", ""]
        interp = sorted((f for f in self.functions if f.ident in self.interpreted), key=lambda f: int(f.addr, 16))
        disp += ["// The functions the SH-4 interpreter runs, sorted (port/interpreted.txt).",
                 "const uint32_t kInterpreted[] = {" + ", ".join(f"0x{f.addr}u" for f in interp) + ("" if interp else "0u") + "};",
                 f"const size_t kInterpretedCount = {len(interp)};",
                 "// Their argument registers from r4, from fr4 and their stack words, for the differential tests.",
                 "const uint8_t kInterpretedArgs[][3] = {" + ", ".join("{%d, %d, %d}" % self.arg_counts(f) for f in interp)
                 + ("" if interp else "{0, 0, 0}") + "};", ""]
        native_only = sorted(int(f.addr, 16) for f in self.functions
                             if f.name in self.replaced and f.ident not in self.interpreted)
        disp += ["// The functions the port replaces with its own C++ (port/replaced.txt), sorted.",
                 "const uint32_t kNativeOnly[] = {" + ", ".join(f"0x{a:08x}u" for a in native_only)
                 + ("" if native_only else "0u") + "};",
                 f"const size_t kNativeOnlyCount = {len(native_only)};", ""]
        layouts = []
        for f in sorted(self.functions, key=lambda f: int(f.addr, 16)):
            if not f.varargs and (slots := self.layout(f)) is not None:
                layouts.append(f"    {{0x{f.addr}u, {len(slots)}, \"" + "".join(f"\\x{v:02x}" for v in slots) + f"\"}},  // {f.name}")
        disp += ["// The functions whose arguments are not where the convention's order puts them: each",
                 "// argument's place, 0 to 3 for r4 to r7, 4 to 11 for fr4 to fr11, 16 + k for stack word k.",
                 "const ArgLayout kArgLayouts[] = {"] + (layouts or ["    {0u, 0, \"\"},"]) + ["};",
                 f"const size_t kArgLayoutCount = {len(layouts)};", ""]
        (gen / "src" / "dispatch.cpp").write_text("\n".join(disp))
        (gen / "include" / "decomp.h").write_text(
            "// Generated by tools/decomp/convert.py. Do not edit.\n#pragma once\n"
            '#include "types.h"\n#include "functions.h"\n#include "native.h"\n#include "globals.h"\n'
            '#include "port.h"\n')
        # The defects that the differential tests cannot find: a function that reads an argument
        # register it has no parameter for gets no value there from its C++ callers.
        lost = self.lost_inputs()
        self.report += [f"reads argument registers it has no parameter for: {name} ({', '.join(regs)})"
                        for name, regs in lost]
        (gen / "report.txt").write_text("\n".join(self.report) + "\n")
        n = sum(len(b) for b in modules.values())
        print(f"convert: {n} functions in {len(modules)} files, {len(self.globals)} globals, "
              f"{len(self.skip)} skipped as fixed or replaced, {len(self.report)} report lines -> {gen}")
        print(f"convert: {len(lost)} functions read argument registers they have no parameter for "
              f"(gen/report.txt)")
        return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ghidra", type=Path, default=ROOT / "ghidra" / "out")
    ap.add_argument("--out", type=Path, default=ROOT / "decomp")
    ap.add_argument("--interpreted", type=Path, default=ROOT / "port" / "interpreted.txt",
                    help="the functions the port interprets, one name per line")
    a = ap.parse_args()
    return Converter(a.ghidra, a.out, a.interpreted).run()


if __name__ == "__main__":
    sys.exit(main())
