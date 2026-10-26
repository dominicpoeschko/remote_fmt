import argparse
import subprocess
import json
import sys
import os
import re
import shutil

# a build runs this: importing the sibling modules writes no tools/__pycache__
sys.dont_write_bytecode = True
import descriptor  # noqa: E402
from rf_symbols import Error, find_demangler, invalid_utf8, string_constant_bytes  # noqa: E402


def parse_StringConstant(s):
    """Parse a StringConstant template parameter into the bytes of the string."""
    s = s.removeprefix("sc::StringConstant<")
    s = s.partition(">")[0]
    # bytes, negative where char is signed; UTF-8 is decoded for the json only
    literal = bytearray()

    while s:
        s = s.removeprefix("(char)")
        part = s.partition(", ")[0]
        s = s.removeprefix(part)
        s = s.removeprefix(", ")
        try:
            literal.append(int(part) & 0xFF)
        except (ValueError, OverflowError) as e:
            print(
                f"Error parsing character code '{part}': {e}", file=sys.stderr)
            return None

    return bytes(literal)


CATALOG_PREFIX = "unsigned short remote_fmt::catalog<"


def parse_symbol(symbol):
    """Extract the string constant's bytes from a symbol name."""
    symbol = symbol.removeprefix(CATALOG_PREFIX)
    symbol = symbol.removesuffix(">")

    result = parse_StringConstant(symbol)
    if result is None:
        print(f"Failed to parse symbol: {symbol}", file=sys.stderr)
        return None
    return result


# the mangled name of remote_fmt::catalog<SC> (SC = sc::StringConstant<...>): the filter on `nm -u`, before anything
# is demangled
CATALOG_MANGLED = "_ZN10remote_fmt7catalogI"
STRING_CONSTANT_MANGLED = "N2sc14StringConstantIJ"


def demangled_names(nm, names, f):
    """`names` demangled: by llvm-cxxfilt or c++filt (rf_symbols.find_demangler), without either by nm -uC
    of the same object (the same symbols in the same order)."""
    try:
        tool = find_demangler(nm)
    except Error:
        out = subprocess.run([nm, "-uC", f], check=True,
                             capture_output=True, text=True).stdout
        everything = [l.strip().removeprefix("U ")
                      for l in out.splitlines() if l.strip()]
        raw = subprocess.run([nm, "-u", f], check=True,
                             capture_output=True, text=True).stdout
        mangled = [l.split()[-1] for l in raw.splitlines() if l.strip()]
        by_name = dict(zip(mangled, everything))
        return [by_name[n] for n in names]
    dem = subprocess.run([tool], input="\n".join(names) + "\n",
                         check=True, capture_output=True, text=True).stdout.splitlines()
    if len(dem) != len(names):
        raise Error(f"{tool} gave {len(dem)} lines for {len(names)} names")
    return dem


def collect_symbols(nm, objects):
    """(mangled, demangled) of the remote_fmt::catalog<sc::StringConstant<...>> symbols the objects leave
    undefined. Only these are demangled: the demangled text is what the
    generated .cpp specializes, the string's bytes come from the mangled name."""
    symbols = []
    for f in objects:
        if not os.path.exists(f):
            print(f"Error: Object file '{f}' not found.", file=sys.stderr)
            sys.exit(1)
        try:
            raw = subprocess.run([nm, "-u", f], check=True,
                                 capture_output=True, text=True)
            names = [l.split()[-1]
                     for l in raw.stdout.splitlines() if l.strip()]
            names = [n for n in names if n.startswith(CATALOG_MANGLED)]
            if names:
                symbols += zip(names, demangled_names(nm, names, f))
        except (subprocess.CalledProcessError, Error) as e:
            print(f"Error running nm on '{f}': {e}", file=sys.stderr)
            sys.exit(1)
        except FileNotFoundError:
            print("Error: nm command not found.", file=sys.stderr)
            sys.exit(1)
    return symbols


def group_texts(symbols):
    """Map each string's bytes to its demangled symbol. The bytes are read from the mangled name and must agree with
    the demangled one, which the generated .cpp spells: a symbol where they do not, or that is no StringConstant, is
    left out, so the link fails instead of decoding the wrong string."""
    texts = {}
    for mangled, s in sorted(set(symbols), key=lambda pair: pair[1]):
        text = string_constant_bytes(
            mangled) if STRING_CONSTANT_MANGLED in mangled else None
        if text is None or parse_symbol(s) != text:
            print(f"Skipping invalid symbol: {s}", file=sys.stderr)
            continue
        texts.setdefault(text, s)
    return texts


# Ids are hashes of the string, so a new string leaves the others alone - unless it collides and
# pushes a probed neighbour one slot on. Same strings, same ids. A consumer may leave a part of its
# strings out of the hash (--id-key-strip): uc_log its log line's line number, so a line that moves
# keeps its id.
ID_COUNT = 1 << 15   # 16 bits on the wire, the upper half is the call sites'


def id_key_rule(pattern):
    """--id-key-strip's regex, matched at the start of a string: its group 1 is left out of the key,
    and the first number in group 1 orders strings that share a key."""
    rule = re.compile(pattern.encode("utf-8"))
    if rule.groups < 1:
        raise ValueError(
            f"--id-key-strip {pattern!r} has no group 1 to leave out")
    return rule


def id_key(text, rule=None):
    match = rule.match(text) if rule else None
    return text if match is None else text[:match.start(1)] + text[match.end(1):]


def line_number(text, rule=None):
    match = rule.match(text) if rule else None
    number = re.search(rb"[0-9]+", match.group(1)) if match else None
    return int(number.group()) if number else -1


def preferred_id(key):
    h = 0x811C9DC5   # FNV-1a 32, folded to 16 bits
    for b in key:
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return (h ^ (h >> 16)) & (ID_COUNT - 1)


def assign_ids(texts, rule=None):
    """An id per string: its hash's slot, or the next free one on a collision."""
    if len(texts) > ID_COUNT:
        raise ValueError(
            f"{len(texts)} string constants, the catalog has room for {ID_COUNT}")
    taken = set()
    # identical lines in one function share a key: they go by line number, as numbers
    order = sorted((preferred_id(id_key(t, rule)), id_key(t, rule),
                   line_number(t, rule), t) for t in texts)
    ids = {}
    for slot, _, _, text in order:
        while slot in taken:
            slot = (slot + 1) % ID_COUNT
        taken.add(slot)
        ids[text] = slot
    return ids


def prelink_json(out_dir, target_name):
    """The strings' half of the catalog, for extract_sites.py --strings: not the catalog's own name, so no reader takes
    it for a catalog."""
    return os.path.join(out_dir, f"{target_name}_string_constants.prelink.json")


def write_outputs(ids, texts, out_dir, target_name):
    """Write the catalog .cpp and the strings' half of the catalog json, `<target>_string_constants.prelink.json`
    ({"StringConstants": [{"id", "fmt", "args"}, ...]}, descriptor.py catalog_entry), which extract_sites.py turns into
    the catalog after the link; returns the .cpp's path. An Error if a string has no descriptors or they do not split,
    before anything is written."""
    entries = sorted((descriptor.catalog_entry(id, text)
                     for text, id in ids.items()), key=lambda entry: entry["id"])
    outfilename = os.path.join(
        out_dir, f"{target_name}_string_constants.cpp")
    jsonfilename = prelink_json(out_dir, target_name)

    try:
        with open(outfilename, 'w') as outfile:
            outfile.write("#include <remote_fmt/catalog.hpp>\n")
            outfile.write("#include <string_constant/string_constant.hpp>\n")
            for text, id in sorted(ids.items(), key=lambda item: item[1]):
                outfile.write("template<>")
                outfile.write(texts[text])
                outfile.write("{return ")
                outfile.write(str(id))
                outfile.write(";}\n")
            outfile.write("\n")
    except IOError as e:
        print(f"Error writing C++ file '{outfilename}': {e}", file=sys.stderr)
        sys.exit(1)

    try:
        with open(jsonfilename, 'w') as outfile:
            json.dump({"StringConstants": entries}, outfile, indent=2)
    except IOError as e:
        print(
            f"Error writing JSON file '{jsonfilename}': {e}", file=sys.stderr)
        sys.exit(1)

    return outfilename


def compile_catalog(compiler, cpp_file, out_dir, target_name, source_dir, flags):
    command = []
    command.append(compiler)
    command.append("-o")
    command.append(os.path.join(
        out_dir, f"{target_name}_string_constants.obj"))
    command.append("-c")
    command.append(cpp_file)
    command.append("-Wno-old-style-cast")
    command.append(f"-isystem{os.path.join(source_dir, 'src')}")
    command.append(
        f"-isystem{os.path.join(source_dir, 'string_constant', 'src')}")

    for f in flags:
        for a in f.split():
            command.append(a)

    std_defined = False
    for arg in command:
        if (arg.startswith("-std=")):
            std_defined = True

    if (not std_defined):
        command.append("-std=c++26")

    try:
        subprocess.run(command, check=True)
    except subprocess.CalledProcessError as e:
        print(f"Error compiling generated code: {e}", file=sys.stderr)
        sys.exit(1)
    except FileNotFoundError:
        print(f"Error: Compiler '{compiler}' not found.", file=sys.stderr)
        sys.exit(1)


def main(argv=None):
    parser = argparse.ArgumentParser(
        prog='generate_string_constants',
        description='create the object file that defines the remote_fmt::catalog<> ids, and the strings\' half of the '
                    'catalog json (<target>_string_constants.prelink.json) for extract_sites.py')

    parser.add_argument('--target_name', required=True,
                        help='prefix of the generated files')
    parser.add_argument('--out_dir', required=True,
                        help='directory for the .cpp, .prelink.json and .obj')
    parser.add_argument('--source_dir', required=True,
                        help='remote_fmt root, for the includes')
    parser.add_argument('--compiler', required=True,
                        help='compiler for the generated .cpp')
    parser.add_argument('--objects', required=True, nargs='+',
                        help='object files to collect catalog symbols from')
    parser.add_argument('--flags', default=[], nargs='+',
                        help='extra compiler flags')
    parser.add_argument('--nm', required=True, help='nm tool to use')
    parser.add_argument('--id-key-strip', default=None,
                        help='regex matched at the start of a string: its group 1 is not part of the id '
                             '(uc_log: the line number), default: the whole string is the key')

    args = parser.parse_args(argv)

    # Ensure output directory exists
    os.makedirs(args.out_dir, exist_ok=True)

    # Check if tools are available
    if not shutil.which(args.nm):
        print(f"Error: nm '{args.nm}' not found.", file=sys.stderr)
        sys.exit(1)

    if not shutil.which(args.compiler):
        print(f"Error: Compiler '{args.compiler}' not found.", file=sys.stderr)
        sys.exit(1)

    symbols = collect_symbols(args.nm, args.objects)
    texts = group_texts(symbols)
    bad = invalid_utf8(list(texts))
    for text in bad:
        print(
            f"Error: string constant is not UTF-8: {text!r}", file=sys.stderr)
    if bad:
        sys.exit(1)
    try:
        rule = id_key_rule(args.id_key_strip) if args.id_key_strip else None
        ids = assign_ids(texts, rule)
    except (ValueError, re.error) as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)

    try:
        cpp_file = write_outputs(ids, texts, args.out_dir, args.target_name)
    except Error as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)
    compile_catalog(args.compiler, cpp_file, args.out_dir, args.target_name,
                    args.source_dir, args.flags)


if __name__ == "__main__":
    main()
