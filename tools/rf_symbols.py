"""What both build steps read from the toolchain's symbols: the demangler to use, a string constant's
bytes from a mangled name, the ELF symbol table, the UTF-8 rule. generate_string_constants.py (before
the link) and extract_sites.py (after it) import it; both are LINK_DEPENDS of every image, and so is
this file (CMakeLists.txt target_generate_string_constants).
"""

import os
import re
import shutil
import struct

SHT_SYMTAB = 2
STT_OBJECT = 1

# a `char` template argument of sc::StringConstant<...>: `Lc84E`, negative where char is signed `Lcn61E`
CHAR_LITERAL = re.compile(r"Lc(n?[0-9]+)E")


class Error(Exception):
    pass


def find_demangler(nm=None):
    """llvm-cxxfilt next to nm (llvm-nm-19 -> llvm-cxxfilt-19, also through a symlink), else on PATH,
    else c++filt (which gives up on long symbols)."""
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


def string_constant_bytes(mangled):
    """The bytes of the sc::StringConstant<...> in a mangled name, from its `Lc<n>E` literals."""
    return bytes(-int(n[1:]) & 0xFF if n.startswith("n") else int(n)
                 for n in CHAR_LITERAL.findall(mangled))


def invalid_utf8(texts):
    """The strings that are not UTF-8: a cataloged string may hold any byte >= 0x80, but the host
    reads the json as UTF-8 and would show a replacement character (a Latin-1 source file)."""
    bad = []
    for text in texts:
        try:
            text.decode("utf-8")
        except UnicodeDecodeError:
            bad.append(text)
    return bad


def read_symbols(image, sections_named):
    """(64-bit, {index: flags} of the sections whose name `sections_named(name)` accepts, the object
    symbols as (name, value, section index)) of an ELF image."""
    if image[:4] != b"\x7fELF":
        raise Error("not an ELF file")
    elf_class, data = image[4], image[5]
    if data != 1:
        raise Error("big endian ELF is not supported")
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

    chosen = {}
    symbols = []
    for index, (name, sh_type, flags, _, offset, size, link, *_) in enumerate(raw):
        if sections_named(string(shstrndx, name)):
            chosen[index] = flags
        if sh_type == SHT_SYMTAB:
            for i in range(size // symbol.size):
                sym_name, value, info, shndx = fields(
                    symbol.unpack_from(image, offset + i * symbol.size))
                if sym_name and info & 0xF == STT_OBJECT:
                    symbols.append((string(link, sym_name), value, shndx))
    return elf_class == 2, chosen, symbols
