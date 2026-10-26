#!/usr/bin/env python3
"""Writes a linked image's string catalog json, once: the strings generate_string_constants.py found
before the link (`--strings`, its `<target>_string_constants.prelink.json`), then the image's call sites
(REMOTE_FMT_SITE, src/remote_fmt/catalog.hpp): each site's `{"id", "fmt", "args"}` entry (descriptor.py
catalog_entry) to "StringConstants" and its function's signature to "Sites": {"<id>": "<signature>"};
"FrameCheck"; the image's type entries (remote_fmt::detail::typeTag, descriptor.hpp) to "Types":
{"<16 hex>": descriptor}. A site's string is `fmt 0x1e descriptors`, split into one descriptor per
argument; the link fails on a site without descriptors, a descriptor that does not split, a type key
that two texts share, a type reference without its entry and a site at 0xFFFF (the inline id). The
catalog (`--json`) is replaced whole, only once everything is known: a failed step leaves the previous
one as it was, and no reader ever sees half a catalog.
"""

import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys

# a build runs this: importing the sibling modules writes no tools/__pycache__
sys.dont_write_bytecode = True
import descriptor  # noqa: E402
from rf_symbols import (Error, find_demangler, invalid_utf8, read_symbols as read_elf_symbols,  # noqa: E402
                        string_constant_bytes)

SECTION = "remote_fmt_sites"
ANCHOR = "_ZN10remote_fmt6detail10siteAnchorE"
SITE_TAG_MANGLED = "19REMOTE_FMT_SITE_TAG"
# `_ZZZ<enclosing>E NK <lambda> clI<sc::StringConstant<...>> ... E19REMOTE_FMT_SITE_TAG`, the lambda being
# `UlTy..._` (gcc, clang) or clang's `<len>$_<n>` in a function of internal linkage. Split by structure: the
# string is read from its literals and only the enclosing function goes to the demangler (GNU c++filt
# reads no `$_<n>` and gives up on long names). `sc::StringConstant` is a substitution (`S<n>_`) when the
# enclosing function's own template arguments already named it (a StringConstant template parameter).
SITE = re.compile(
    r"ENK(UlTy(?:RK)?(?:T_|S[0-9A-Z]*_)E[0-9]*_|([0-9]+)\$_([0-9]+))"
    r"clIN(?:2sc14StringConstant|S[0-9A-Z]*_)IJ((?:Lcn?[0-9]+E)*)EE")
CLANG_LOCAL_LAMBDA = re.compile(r"(?<![0-9])([0-9]+)\$_([0-9]+)")
SHF_ALLOC = 0x2
FIRST_SITE_ID = 0x8000
ID_LIMIT = 1 << 16
INLINE_ID = 0xFFFF   # remote_fmt::protocol::InlineId: no site may sit there
SEPARATOR = descriptor.SEPARATOR
TYPE_TAG_MANGLED = "19REMOTE_FMT_TYPE_TAG"


def read_symbols(image):
    """(64-bit, site section indices with their flags, symbols as (name, value, section)): read once per image, the
    `elf` that frame_check_of, types_of, config_conflicts and sites_of take."""
    return read_elf_symbols(image, lambda name: name == SECTION or name.startswith(SECTION + "."))


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
    text = string_constant_bytes(match[4])
    return "_Z" + mangled[len("_ZZZ"):match.start()], text


# Neither demangler reads every name, so what one cannot read goes to the other (measured on 911
# enclosing functions of 300 images):
# - llvm-cxxfilt (22) reads no template parameter (`T_`) in the parameters of a member template of
#   a local class: `main::$_7::operator()<X>(X const&)`, a generic lambda in main (2 of 911);
# - GNU c++filt (binutils 2.47) gives up on names past ~3.7 KB (489 of 911) and reads no `Tn<type>`
#   or `Tk<concept>`, clang's mark on a `template<auto>` or constrained parameter's argument --
#   before a builtin type it is given the name without the mark (without_builtin_tn()).
def demanglers(tool):
    """`tool`, then the other demangler if there is one. find_demangler() prefers llvm-cxxfilt
    wherever it looks, so the other one is c++filt, which it only looks for on PATH too."""
    other = shutil.which(
        "c++filt" if "llvm-cxxfilt" in os.path.basename(tool) else "llvm-cxxfilt")
    return [tool] if other is None else [tool, other]


# clang's `Tn` before a builtin type (`TnDa`: a `template<auto>` argument). A builtin type is no
# substitution candidate, so dropping the mark moves no `S<n>_` and GNU c++filt reads the rest --
# which is what reads a name with both (a closure from a generic lambda passed to a
# `template<auto>`, where llvm-cxxfilt reads no `T_` either). A class type after `Tn` is one, so
# that mark stays.
CLANG_TN_BUILTIN = re.compile(r"Tn(?:[vwbcahstijlmxynofdegz]|D[acnisudfeh])")


def without_builtin_tn(mangled):
    """`mangled` without clang's `Tn` marks before builtin types, as GNU c++filt reads it."""
    return CLANG_TN_BUILTIN.sub("", mangled)


def demangle(tools, names):
    """Each name demangled by the first of `tools` that reads it; one none reads stays `_Z...`.
    GNU c++filt's closing `> >` is written `>>` as llvm-cxxfilt writes it, so the catalog does not
    depend on the demangler that read a name (uc_log shows the class's arguments)."""
    out = list(names)
    for tool in tools:
        gnu = "llvm-cxxfilt" not in os.path.basename(tool)
        todo = [i for i, name in enumerate(out) if name.startswith("_Z")]
        given = [without_builtin_tn(out[i]) if gnu else out[i] for i in todo]
        for i, name in zip(todo, run_demangler(tool, given)):
            if gnu and name.startswith("_Z"):
                continue   # unread: the name as it was, not the one without its marks
            while gnu and "> >" in name:
                name = name.replace("> >", ">>")
            out[i] = name
    return out


def run_demangler(tool, names):
    if not names:
        return []
    out = subprocess.run([tool], input="\n".join(names) + "\n",
                         check=True, capture_output=True, text=True).stdout.splitlines()
    if len(out) != len(names):
        raise Error(f"{tool} gave {len(out)} lines for {len(names)} names")
    return out


# The catalog is loaded by every decoder (a hub, a TUI that has it built in), and one signature
# whose template arguments spell out a board's whole device configuration ran to 1.9 MB -- 170 MB
# for one image (0.5 MB now). So template argument lists are abbreviated,
# innermost first, measured with their qualifiers dropped as uc_log's qualifiedFunction reads them.
# It shows the arguments of the class a function belongs to, and nothing else:
# - that list (`App<...>::run`) keeps its arguments up to MAX_ARGS, more than uc_log's 100, and is
#   then cut after an argument: `<A, B, …>`;
# - every other list (a nested one, uc_log's `<>`; a function template's; a parameter's or the
#   return type's) longer than MAX_NESTED_ARGS is written "<…>";
# - a template operator's list is shown as it is (`X::operator()<Y<int>>::lambda`, up to the name's
#   192 characters), so it stays whole up to MAX_OPERATOR_ARGS.
# So the module and short name uc_log derives stay the same (checked against real catalogs with
# namesOfDemangled). The nested limit is the catalog's size: that image's 547 KB at 40, 498 KB at 24.
MAX_NESTED_ARGS = 24
MAX_ARGS = 200
MAX_OPERATOR_ARGS = 256
QUALIFIER = re.compile(r"\w+::")
# A name neither demangler could read (see demangle()) is kept mangled, cut to this length.
MAX_MANGLED = 256
OPERATOR_CHARS = "<>=-!+*/%^&|~,"
# Longest first: `operator<<<int>` (llvm-cxxfilt) is `operator<<` and its arguments `<int>`.
OPERATORS = sorted(["<=>", "<<=", ">>=", "->*", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||", "++",
                    "--", "->", "+=", "-=", "*=", "/=", "%=", "^=", "&=", "|=", "<", ">", "=", "!",
                    "+", "-", "*", "/", "%", "^", "&", "|", "~", ","], key=len, reverse=True)


def operator_end(signature, i):
    """End of the operator name at `i` (`operator<`, `operator()`), None if there is none: its
    `<`, `>`, `(` are no brackets. The longest symbol that is followed by what may follow a
    name: llvm-cxxfilt's `operator<<int>` is `operator<` and `<int>`, `operator<<<int>` is
    `operator<<` and `<int>`."""
    if not signature.startswith("operator", i) or (i != 0 and signature[i - 1] not in ": "):
        return None
    j = i + len("operator")
    if signature.startswith(("()", "[]"), j):
        return j + 2
    run = j
    while run < len(signature) and signature[run] in OPERATOR_CHARS:
        run += 1
    for op in OPERATORS:
        end = j + len(op)
        if signature.startswith(op, j, run) and (end == len(signature) or signature[end] in "(<> :,)"):
            return end
    return None


ARGUMENTS = re.compile(r"[<>(),]")
# What abbreviated() stops at; the text between is copied whole (a signature runs to megabytes).
STOPS = re.compile(r"[<>()]|operator")


def nested_list(inner, keep):
    """`<inner>`, or "<…>" if it is longer than `keep` without its qualifiers."""
    too_long = len(inner) > keep and len(QUALIFIER.sub("", inner)) > keep
    return "<…>" if too_long else f"<{inner}>"


def name_list(inner, keep):
    """`<inner>`, or its leading arguments that fit in `keep` and ", …". Counted as uc_log shows
    them: no qualifiers, and a nested list's content not at all. Inside parentheses `<` and `>`
    are operators (abbreviated() closed every list in them already)."""
    if len(inner) <= keep:
        return f"<{inner}>"
    angle, paren, shown, pos, cut = 0, 0, 0, 0, None
    for match in ARGUMENTS.finditer(inner):
        if angle == 0:
            shown += len(QUALIFIER.sub("", inner[pos:match.start()])) + 1
        pos = match.end()
        c = match[0]
        if shown > keep:
            break
        if c == "<" and paren == 0:
            angle += 1
        elif c == ">" and paren == 0:
            angle -= 1
        elif c == "(":
            paren += 1
        elif c == ")":
            paren -= 1
        elif angle == 0 and paren == 0:
            cut = match.start()
    else:
        if angle == 0:
            shown += len(QUALIFIER.sub("", inner[pos:]))
        if shown <= keep:
            return f"<{inner}>"
    return "<…>" if cut is None else f"<{inner[:cut]}, …>"


# A nested list at least this long is remembered by abbreviated(): a board's signature repeats the
# same device types hundreds of times, and a nested list's abbreviation depends on its own text
# only, so a repeat is skipped with one compare (one large image: 64 s -> 0.1 s).
MEMO_MIN = 256
MEMO_KEY = 64


def abbreviated(signature, keep=MAX_NESTED_ARGS, keep_names=MAX_ARGS, memo=None):
    """The signature with its template argument lists abbreviated (see MAX_NESTED_ARGS).
    Parentheses fence the brackets in: a `>` right inside them is an operator (`A<(N > 2)>`), and
    a `<` still open at their `)` was a less-than (`A<((3)<(2))>`); so is one still open at the
    end (llvm-cxxfilt's `A<3 < 2>`): those stay as they were. `memo` carries the long nested
    lists from one call to the next (same `keep` only)."""
    # a list's first MEMO_KEY characters -> [(list, result)]
    memo = {} if memo is None else memo
    # the text outside, then per open bracket: [kind, where it starts, its text so far]
    stack = [[None, 0, []]]
    lists = 0      # open "<" on the stack
    parens = 0     # open "(" on the stack
    operator = -1  # where the last operator name ends
    i = 0
    while (match := STOPS.search(signature, i)) is not None:
        stop = match.start()
        out = stack[-1][2]
        out.append(signature[i:stop])
        i = stop + 1
        c = match[0]
        if c == "operator":
            end = operator_end(signature, stop)
            i = stop + len("operator") if end is None else end
            out.append(signature[stop:i])
            operator = i
        elif c == "<":
            if lists:
                for text, result in memo.get(signature[stop:stop + MEMO_KEY], ()):
                    if signature.startswith(text, stop):
                        out.append(result)
                        i = stop + len(text)
                        break
                else:
                    stack.append(["<", stop, []])
                    lists += 1
            else:
                # a function operator's own list: `operator()<int>`, GNU's `operator< <int>`
                of_operator = stop == operator or (
                    stop == operator + 1 and signature[operator] == " ")
                stack.append(
                    ["<" if not of_operator else "<operator", stop, []])
                lists += 1
        elif c == "(":
            stack.append(["(", stop, []])
            parens += 1
        elif c == ">" and (stack[-1][0] or "").startswith("<"):
            kind, start, parts = stack.pop()
            lists -= 1
            inner = "".join(parts)
            if kind == "<operator" and i - start <= MAX_OPERATOR_ARGS:
                stack[-1][2].append(signature[start:i])
                continue
            if lists == 0 and signature.startswith("::", i):
                stack[-1][2].append(name_list(inner, keep_names))
                continue
            result = nested_list(inner, keep)
            stack[-1][2].append(result)
            if i - start >= MEMO_MIN:
                memo.setdefault(signature[start:start + MEMO_KEY], []).append(
                    (signature[start:i], result))
        elif c == ")" and parens:
            while stack[-1][0].startswith("<"):   # less-thans
                _, _, parts = stack.pop()
                lists -= 1
                stack[-1][2].append("<" + "".join(parts))
            _, _, parts = stack.pop()
            parens -= 1
            stack[-1][2].append("(" + "".join(parts) + ")")
        else:
            out.append(c)
    stack[-1][2].append(signature[i:])
    while len(stack) > 1:
        kind, _, parts = stack.pop()
        stack[-1][2].append(kind[0] + "".join(parts))
    return "".join(stack[0][2])


def abbreviated_or_cut(signature, memo):
    """abbreviated(), or a signature it could not read cut to MAX_MANGLED: the catalog's size is
    no reason to fail a firmware build."""
    try:
        return abbreviated(signature, memo=memo)
    except Exception as e:  # noqa: BLE001 - any bug in the abbreviation
        print(f"extract_sites: cannot abbreviate {signature[:120]} ({type(e).__name__}: {e}), "
              f"cut instead", file=sys.stderr)
        return signature if len(signature) <= MAX_MANGLED else signature[:MAX_MANGLED] + "…"


def demangle_sites(names, demangler):
    """(signature, string bytes) or None per site tag name; a function neither demangler reads
    keeps its mangled name."""
    splits = [split_site(name) for name in names]
    found = [split for split in splits if split is not None]
    tools = demanglers(demangler)
    readable = demangle(tools, [local_lambda_named(enclosing)
                        for enclosing, _ in found])
    signatures = iter(readable)
    memo, done, unreadable = {}, {}, set()
    parts = []
    for split in splits:
        if split is None:
            parts.append(None)
            continue
        enclosing, text = split
        signature = next(signatures)
        if signature.startswith("_Z"):
            if enclosing not in unreadable:
                unreadable.add(enclosing)
                tried = ", ".join(os.path.basename(tool) for tool in tools)
                print(f"extract_sites: {tried} cannot read {enclosing[:120]}, kept mangled",
                      file=sys.stderr)
            signature = enclosing if len(
                enclosing) <= MAX_MANGLED else enclosing[:MAX_MANGLED] + "…"
        else:
            if signature not in done:
                done[signature] = abbreviated_or_cut(signature, memo)
            signature = done[signature]
        parts.append((signature, text))
    return parts


FRAME_CHECK_MARKER = "remote_fmt_cfg_FRAME_CHECK_1_"


def frame_check_of(elf):
    """Whether the image was built with REMOTE_FMT_FRAME_CHECK: config.hpp's marker of that value in the site section
    (kept by the linker script's KEEP; config_conflicts refuses an image with both), so a reader of the catalog knows -
    never guesses - whether the image's frames carry the CRC."""
    _, sections, symbols = elf
    return any(name == FRAME_CHECK_MARKER and shndx in sections for name, _, shndx in symbols)


def fnv1a64(data):
    """descriptor.hpp desc::fnv1a64: the key of a type entry."""
    h = 0xcbf29ce484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def type_key(text):
    return f"{fnv1a64(text):016x}"


def types_of(elf):
    """{key: text} of the image's type entries; an Error when two texts share a key."""
    _, sections, symbols = elf
    types = {}
    for name, _, shndx in symbols:
        if shndx in sections and TYPE_TAG_MANGLED in name:
            text = string_constant_bytes(name.split(".")[0])
            key = type_key(text)
            if types.setdefault(key, text) != text:
                raise Error(f"type entries {types[key][:60]!r} and {text[:60]!r} share the key {key}: "
                            f"rename one of the types")
    return types


def type_refs(text):
    """The type keys a described string's descriptors refer to."""
    return descriptor.type_refs(text.partition(SEPARATOR)[2])


def missing_types(strings, types):
    """[(where, key)] of the type references (of the strings and of the entries themselves) without an entry."""
    missing = []
    for where, text in strings:
        for key in type_refs(text):
            if key not in types:
                missing.append((where, key))
    for key, text in types.items():
        for ref in descriptor.type_refs(text):
            if ref not in types:
                missing.append((f"type {key}", ref))
    return missing


CONFIG_MARKER = re.compile(r"remote_fmt_cfg_([A-Z_]+?)_([0-9]+)_")


def config_conflicts(elf):
    """The switches of src/remote_fmt/config.hpp that the image's TUs disagree on, as {switch: [values]} (switch:
    REMOTE_FMT_<switch>): each TU that includes remote_fmt puts one marker per switch and value into the site
    section."""
    _, sections, symbols = elf
    values = {}
    for name, _, shndx in symbols:
        match = CONFIG_MARKER.fullmatch(name)
        if match and shndx in sections:
            values.setdefault(match.group(1), set()).add(int(match.group(2)))
    return {switch: sorted(seen) for switch, seen in values.items() if len(seen) > 1}


def sites_of(elf, demangler):
    """({id: string bytes}, {id: signature}) of the call sites of an ELF image."""
    # as catalog.hpp siteId() counts: from address 0 with 32-bit pointers, from siteAnchor with 64
    wide, sections, symbols = elf
    for flags in sections.values():
        if not wide and flags & SHF_ALLOC:
            raise Error(f"the site tags are part of the image: the linker script needs "
                        f"`{SECTION} 0 (INFO) : {{ KEEP(*({SECTION} {SECTION}.*)) }}`")
    anchor = None
    if wide:
        anchor = next(
            (value for name, value, _ in symbols if name == ANCHOR), None)
        if anchor is None and sections:
            raise Error(f"no {ANCHOR} in a 64-bit image")

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
        if site_id == INLINE_ID:
            raise Error(
                f"site {mangled[:120]!r} at {site_id:#x}, the inline id: one site more or less moves it")
        if SEPARATOR not in parts[1]:
            raise Error(f"site {mangled[:120]!r} names no descriptors: REMOTE_FMT_SITE()(remote_fmt::described"
                        f"<Args...>(fmt)), not the plain format string")
        sites[site_id], strings[site_id] = parts
    return strings, sites


def catalog(prelink, strings, sites, frame_check=False, types=None):
    """The catalog json's content: the generator's entries (`prelink`, what it wrote to --strings) and the sites'
    entries in "StringConstants", then "Sites", "FrameCheck" and "Types"; an Error for a site on a string's id or a
    type reference without its entry."""
    entries = list(prelink["StringConstants"])
    taken = {entry["id"] for entry in entries}
    types = types or {}
    everything = [
        (f"string {entry['id']}", descriptor.entry_text(entry)) for entry in entries]
    for site_id in sorted(strings):
        if site_id in taken:
            raise Error(f"site id {site_id:#x} is a string's id too: the generator's ids must stay "
                        f"below {FIRST_SITE_ID:#x}")
        entries.append(descriptor.catalog_entry(site_id, strings[site_id]))
    everything += [(f"site {site_id:#x} ({sites.get(site_id, '?')[:80]})", text)
                   for site_id, text in strings.items()]
    missing = missing_types(everything, types)
    if missing:
        where, key = missing[0]
        raise Error(f"{len(missing)} type reference(s) without an entry, first #{key} in {where}: a formatter that "
                    f"returns typeDescriptor<T>() must call typeEntry<T>() in its format()")
    return {
        "StringConstants": entries,
        "Sites": {str(site_id): sites[site_id] for site_id in sorted(sites)},
        # true: every frame of this image starts with 0x57 and carries a CRC-8 (config.hpp REMOTE_FMT_FRAME_CHECK); a
        # reader decodes it with the check required, and refuses an unchecked frame; false: no check
        "FrameCheck": frame_check,
        # the type entries the descriptors refer to by key (a reader resolves them once, when it loads the catalog)
        "Types": {key: types[key].decode("utf-8") for key in sorted(types)},
    }


def write_json(path, data):
    """`data` to `path` in one step (a temporary file renamed over it): a reader sees the old file or the new one."""
    tmp = path + ".tmp"
    # UTF-8, not "\u2026": a signature has an ellipsis in every abbreviated list
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)
    os.replace(tmp, path)


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Write the string catalog json of a linked image: its strings and its call sites.")
    parser.add_argument("--elf", required=True, help="the linked image")
    parser.add_argument("--strings", required=True,
                        help="the strings generate_string_constants.py found before the link (its .prelink.json)")
    parser.add_argument("--json", required=True, help="the catalog to write")
    parser.add_argument(
        "--nm", help="the toolchain's nm: its llvm-cxxfilt is preferred")
    args = parser.parse_args(argv)
    try:
        with open(args.elf, "rb") as f:
            elf = read_symbols(f.read())
        strings, sites = sites_of(elf, find_demangler(args.nm))
        bad = invalid_utf8(strings.values())
        for text in bad:
            print(
                f"extract_sites: string constant is not UTF-8: {text!r}", file=sys.stderr)
        if bad:
            return 1
        conflicts = config_conflicts(elf)
        for switch, seen in sorted(conflicts.items()):
            print(f"extract_sites: {args.elf}: TUs of this image disagree on REMOTE_FMT_{switch} (values "
                  f"{', '.join(map(str, seen))}): set it for the whole image (CMake), never per TU", file=sys.stderr)
        if conflicts:
            return 1
        types = types_of(elf)
        bad = invalid_utf8(types.values())
        for text in bad:
            print(
                f"extract_sites: type entry is not UTF-8: {text!r}", file=sys.stderr)
        if bad:
            return 1
        with open(args.strings, encoding="utf-8") as f:
            prelink = json.load(f)
        write_json(args.json, catalog(prelink, strings,
                   sites, frame_check_of(elf), types))
    except (Error, OSError, ValueError, struct.error, subprocess.CalledProcessError) as e:
        print(f"extract_sites: {args.elf}: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
