#!/usr/bin/env python3
"""Adds the call sites (REMOTE_FMT_SITE, src/remote_fmt/catalog.hpp) of a linked image to its
string catalog json: each site's `[id, string]` to "StringConstants" and its function's signature
to "Sites": {"<id>": "<signature>"}.
"""

import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys

SECTION = "remote_fmt_sites"
ANCHOR = "_ZN10remote_fmt6detail10siteAnchorE"
SITE_TAG_MANGLED = "19REMOTE_FMT_SITE_TAG"
# `_ZZZ<enclosing>E NK <lambda> clI<sc::StringConstant<...>> ... E19REMOTE_FMT_SITE_TAG`, the lambda being
# `UlTy..._` (gcc, clang) or clang's `<len>$_<n>` in a function of internal linkage. Split by structure: the
# string is read from its literals and only the enclosing function goes to the demangler (GNU c++filt
# reads no `$_<n>` and gives up on long names).
SITE = re.compile(
    r"ENK(UlTy(?:RK)?(?:T_|S[0-9A-Z]*_)E[0-9]*_|([0-9]+)\$_([0-9]+))"
    r"clIN2sc14StringConstantIJ((?:Lcn?[0-9]+E)*)EE")
CLANG_LOCAL_LAMBDA = re.compile(r"(?<![0-9])([0-9]+)\$_([0-9]+)")
SHF_ALLOC = 0x2
SHT_SYMTAB = 2
STT_OBJECT = 1
EM_ARM = 40
FIRST_SITE_ID = 0x8000
ID_LIMIT = 1 << 16


class Error(Exception):
    pass


def read_symbols(image):
    """(machine, catalog section indices with their flags, symbols as (name, value, section))."""
    if image[:4] != b"\x7fELF":
        raise Error("not an ELF file")
    elf_class, data = image[4], image[5]
    if data != 1:
        raise Error("big endian ELF is not supported")
    machine, = struct.unpack_from("<H", image, 18)
    if elf_class == 1:
        shoff, = struct.unpack_from("<I", image, 32)
        shentsize, shnum, shstrndx = struct.unpack_from("<HHH", image, 46)
        section = struct.Struct("<IIIIIIIIII")
        symbol = struct.Struct("<IIIBBH")

        def fields(raw):
            name, value, _, info, _, shndx = raw
            return name, value, info, shndx
    elif elf_class == 2:
        shoff, = struct.unpack_from("<Q", image, 40)
        shentsize, shnum, shstrndx = struct.unpack_from("<HHH", image, 58)
        section = struct.Struct("<IIQQQQIIQQ")
        symbol = struct.Struct("<IBBHQQ")

        def fields(raw):
            name, info, _, shndx, value, _ = raw
            return name, value, info, shndx
    else:
        raise Error(f"unknown ELF class {elf_class}")

    raw = [section.unpack_from(image, shoff + i * shentsize)
           for i in range(shnum)]

    def string(table, offset):
        start = raw[table][4] + offset
        return image[start:image.index(b"\0", start)].decode()

    catalog = {}
    symbols = []
    for index, (name, sh_type, flags, _, offset, size, link, *_) in enumerate(raw):
        section_name = string(shstrndx, name)
        if section_name == SECTION or section_name.startswith(SECTION + "."):
            catalog[index] = flags
        if sh_type == SHT_SYMTAB:
            for i in range(size // symbol.size):
                sym_name, value, info, shndx = fields(
                    symbol.unpack_from(image, offset + i * symbol.size))
                if sym_name and info & 0xF == STT_OBJECT:
                    symbols.append((string(link, sym_name), value, shndx))
    return machine, catalog, symbols


def find_demangler(nm=None):
    """llvm-cxxfilt next to nm, else on PATH, else c++filt (which gives up on long symbols)."""
    candidates = []
    if nm:
        path = shutil.which(nm) or nm
        for d, base in {os.path.split(path), os.path.split(os.path.realpath(path))}:
            if "llvm-nm" in base:
                candidates.append(os.path.join(
                    d, base.replace("llvm-nm", "llvm-cxxfilt")))
            candidates.append(os.path.join(d, "llvm-cxxfilt"))
    candidates += [shutil.which("llvm-cxxfilt"), shutil.which("c++filt")]
    for candidate in candidates:
        if candidate and os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            return candidate
    raise Error("neither llvm-cxxfilt nor c++filt found")


def local_lambda_named(mangled):
    """`$_<n>` renamed `lambda_<n>`, which llvm-cxxfilt reads."""
    def rename(match):
        name = f"$_{match[2]}"
        if int(match[1]) != len(name):
            return match[0]
        new = f"lambda_{match[2]}"
        return f"{len(new)}{new}"
    return CLANG_LOCAL_LAMBDA.sub(rename, mangled)


def split_site(mangled):
    """(enclosing function as `_Z...`, string bytes) of a site tag's symbol, None if it is not one."""
    mangled = mangled.split(".")[0]   # LTO's `.llvm.<n>`, `.0`
    if not mangled.startswith("_ZZZ"):
        return None
    match = None
    for candidate in SITE.finditer(mangled):
        if candidate[2] is None or int(candidate[2]) == len(f"$_{candidate[3]}"):
            match = candidate
    if match is None:
        return None
    text = bytes(-int(n[1:]) & 0xFF if n.startswith("n") else int(n)
                 for n in re.findall(r"Lc(n?[0-9]+)E", match[4]))
    return "_Z" + mangled[len("_ZZZ"):match.start()], text


def run_demangler(tool, names):
    if not names:
        return []
    out = subprocess.run([tool], input="\n".join(names) + "\n",
                         check=True, capture_output=True, text=True).stdout.splitlines()
    if len(out) != len(names):
        raise Error(f"{tool} gave {len(out)} lines for {len(names)} names")
    return out


def demangle_sites(names, demangler):
    """(signature, string bytes) or None per site tag name; a function the demangler cannot read
    keeps its mangled name."""
    splits = [split_site(name) for name in names]
    found = [split for split in splits if split is not None]
    readable = run_demangler(
        demangler, [local_lambda_named(enclosing) for enclosing, _ in found])
    signatures = iter(readable)
    parts = []
    for split in splits:
        if split is None:
            parts.append(None)
            continue
        enclosing, text = split
        signature = next(signatures)
        if signature.startswith("_Z"):
            print(f"extract_sites: {os.path.basename(demangler)} cannot read {enclosing[:120]}, "
                  f"kept mangled", file=sys.stderr)
            signature = enclosing
        parts.append((signature, text))
    return parts


def sites_of(image, demangler):
    """({id: string bytes}, {id: signature}) of the call sites of an ELF image."""
    machine, sections, symbols = read_symbols(image)
    for flags in sections.values():
        if machine == EM_ARM and flags & SHF_ALLOC:
            raise Error(f"the site tags are part of the image: the linker script needs "
                        f"`{SECTION} 0 (INFO) : {{ KEEP(*({SECTION} {SECTION}.*)) }}`")
    anchor = None
    if machine != EM_ARM:
        anchor = next(
            (value for name, value, _ in symbols if name == ANCHOR), None)
        if anchor is None and sections:
            raise Error(f"no {ANCHOR} in a host image")

    # by name: a host's section also holds C runtime objects
    tags = [(name, value) for name, value, shndx in symbols
            if shndx in sections and SITE_TAG_MANGLED in name]
    strings, sites = {}, {}
    for (mangled, value), parts in zip(tags, demangle_sites([n for n, _ in tags], demangler)):
        offset = value if anchor is None else value - anchor
        site_id = FIRST_SITE_ID + offset if 0 <= offset < FIRST_SITE_ID else ID_LIMIT
        if not FIRST_SITE_ID <= site_id < ID_LIMIT:
            raise Error(
                f"site {mangled[:120]!r} at {site_id:#x}: outside 0x8000-0xFFFF")
        if parts is None:
            raise Error(f"not a site tag this can read: {mangled[:300]}")
        sites[site_id], strings[site_id] = parts
    return strings, sites


def invalid_utf8(strings):
    bad = []
    for text in strings.values():
        try:
            text.decode("utf-8")
        except UnicodeDecodeError:
            bad.append(text)
    return bad


def merge(path, strings, sites):
    with open(path) as f:
        data = json.load(f)
    taken = {entry[0] for entry in data.get("StringConstants", [])}
    for site_id in sorted(strings):
        if site_id in taken:
            raise Error(f"site id {site_id:#x} is a string's id too: the generator's ids must stay "
                        f"below {FIRST_SITE_ID:#x}")
        data["StringConstants"].append(
            [site_id, strings[site_id].decode("utf-8")])
    data["Sites"] = {str(site_id): sites[site_id] for site_id in sorted(sites)}
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(data, f, indent=2)
    os.replace(tmp, path)


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Add the call sites of a linked image to its string catalog json.")
    parser.add_argument("--elf", required=True, help="the linked image")
    parser.add_argument("--json", required=True,
                        help="the catalog generate_string_constants.py wrote before the link")
    parser.add_argument(
        "--nm", help="the toolchain's nm: its llvm-cxxfilt is preferred")
    args = parser.parse_args(argv)
    try:
        with open(args.elf, "rb") as f:
            strings, sites = sites_of(f.read(), find_demangler(args.nm))
        bad = invalid_utf8(strings)
        for text in bad:
            print(
                f"extract_sites: string constant is not UTF-8: {text!r}", file=sys.stderr)
        if bad:
            return 1
        merge(args.json, strings, sites)
    except (Error, OSError, ValueError, struct.error, subprocess.CalledProcessError) as e:
        print(f"extract_sites: {args.elf}: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
