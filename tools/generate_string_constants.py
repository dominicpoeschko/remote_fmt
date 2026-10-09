import argparse
import subprocess
import json
import sys
import os
import re
import shutil


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


SINGLE_PREFIX = "unsigned short remote_fmt::catalog<"
BLOCK_PREFIX = "unsigned short remote_fmt::catalog_block<"


def is_block(symbol):
    """A catalog_block<SC>: SC is several names joined with '\\0', one id each, consecutive."""
    return symbol.startswith(BLOCK_PREFIX)


def parse_symbol(symbol):
    """Extract the string constant's bytes from a symbol name."""
    symbol = symbol.removeprefix(BLOCK_PREFIX) if is_block(
        symbol) else symbol.removeprefix(SINGLE_PREFIX)
    symbol = symbol.removesuffix(">")

    result = parse_StringConstant(symbol)
    if result is None:
        print(f"Failed to parse symbol: {symbol}", file=sys.stderr)
        return None
    return result


def find_cxxfilt(nm):
    """llvm-cxxfilt of nm's own toolchain (llvm-nm-19 -> llvm-cxxfilt-19), else the one on PATH.
    Preferred over nm -C: binutils' demangler gives up on very long symbols."""
    path = shutil.which(nm) or nm
    for d, base in {os.path.split(path), os.path.split(os.path.realpath(path))}:
        names = [base.replace("llvm-nm", "llvm-cxxfilt")
                 ] if "llvm-nm" in base else []
        for name in names + ["llvm-cxxfilt"]:
            candidate = os.path.join(d, name)
            if os.path.isfile(candidate) and os.access(candidate, os.X_OK):
                return candidate
    return shutil.which("llvm-cxxfilt")


def collect_symbols(nm, objects):
    """The demangled remote_fmt::catalog<sc::StringConstant<...>> symbols the objects leave undefined."""
    symbols = []
    cxxfilt = find_cxxfilt(nm)

    for f in objects:
        if not os.path.exists(f):
            print(f"Error: Object file '{f}' not found.", file=sys.stderr)
            sys.exit(1)

        try:
            if cxxfilt:
                raw = subprocess.run([nm, "-u", f],
                                     check=True, capture_output=True, text=True)
                names = [l.split()[-1]
                         for l in raw.stdout.splitlines() if l.strip()]
                dem = subprocess.run([cxxfilt], input="\n".join(names) + "\n",
                                     check=True, capture_output=True, text=True)
                x = subprocess.CompletedProcess(args=[], returncode=0,
                                                stdout="\n".join("U " + d for d in dem.stdout.splitlines()))
            else:
                x = subprocess.run([nm, "-uC", f],
                                   check=True, capture_output=True, text=True)
        except subprocess.CalledProcessError as e:
            print(f"Error running nm on '{f}': {e}", file=sys.stderr)
            sys.exit(1)
        except FileNotFoundError:
            print("Error: nm command not found.", file=sys.stderr)
            sys.exit(1)
        for line in iter(x.stdout.splitlines()):
            line = line.strip()
            if line.startswith("U " + SINGLE_PREFIX + "sc::StringConstant<") \
                    or line.startswith("U " + BLOCK_PREFIX + "sc::StringConstant<"):
                symbols.append(line.removeprefix("U "))

    return symbols


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


def group_texts(symbols, blocks=False):
    """Map each string's bytes to its symbols (catalog<SC> and catalog<SC const&> share an id).
    An unparsable symbol is left out, so the link fails instead of decoding the wrong string.
    With blocks, only the catalog_block<> symbols, else only the single strings'."""
    texts = {}
    for s in sorted(set(symbols)):
        if is_block(s) != blocks:
            continue
        text = parse_symbol(s)
        if text is None:
            print(f"Skipping invalid symbol: {s}", file=sys.stderr)
            continue
        texts.setdefault(text, []).append(s)
    return texts


def block_names(joined):
    """A block's names: the string split at its '\\0' separators."""
    return joined.split(b"\0")


# Ids are hashes of the string (without a log line's line number), so a new string leaves the
# others alone - unless it collides and pushes a probed neighbour one slot on. Same strings,
# same ids.
LOCATION_LINE = re.compile(rb'^\("([^"]*)", (\d+), ')
ID_COUNT = 1 << 15   # 16 bits on the wire, the upper half is the call sites'


def id_key(text):
    return LOCATION_LINE.sub(rb'("\1", ', text, count=1)


def line_number(text):
    match = LOCATION_LINE.match(text)
    return int(match.group(2)) if match else -1


def preferred_id(key):
    h = 0x811C9DC5   # FNV-1a 32, folded to 16 bits
    for b in key:
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return (h ^ (h >> 16)) & (ID_COUNT - 1)


def assign_all(texts, blocks):
    """Ids for the single strings and a base id for each block (a run of consecutive ids, one
    per name: the firmware sends base + the enumerator's index). Blocks go first, each at the
    first free run at or after its hash, so they move only when another block takes its place;
    the single strings then fill around them as before."""
    needed = len(texts) + sum(len(block_names(b)) for b in blocks)
    if needed > ID_COUNT:
        raise ValueError(
            f"{needed} string constants, the catalog has room for {ID_COUNT}")
    taken = set()
    bases = {}
    for start, joined in sorted((preferred_id(b), b) for b in blocks):
        n = len(block_names(joined))
        last = ID_COUNT - n   # the highest base that still fits
        base = min(start, last)
        for _ in range(last + 1):
            if not any(i in taken for i in range(base, base + n)):
                break
            base = base + 1 if base < last else 0
        else:
            raise ValueError(
                f"no run of {n} free ids for a block of {n} names")
        taken.update(range(base, base + n))
        bases[joined] = base
    # identical lines in one function share a key: they go by line number, as numbers
    order = sorted((preferred_id(id_key(t)), id_key(t),
                   line_number(t), t) for t in texts)
    ids = {}
    for slot, _, _, text in order:
        while slot in taken:
            slot = (slot + 1) % ID_COUNT
        taken.add(slot)
        ids[text] = slot
    return ids, bases


def assign_ids(texts):
    return assign_all(texts, {})[0]


def write_outputs(ids, texts, out_dir, target_name, bases=None, blocks=None):
    """Write the catalog .cpp and .json; returns the .cpp's path."""
    bases = bases or {}
    blocks = blocks or {}
    outfilename = os.path.join(
        out_dir, f"{target_name}_string_constants.cpp")
    jsonfilename = os.path.join(
        out_dir, f"{target_name}_string_constants.json")

    indexmap = []

    try:
        with open(outfilename, 'w') as outfile:
            outfile.write("#include <remote_fmt/catalog.hpp>\n")
            outfile.write("#include <string_constant/string_constant.hpp>\n")
            for text, id in sorted(ids.items(), key=lambda item: item[1]):
                for s in texts[text]:
                    outfile.write("template<>")
                    outfile.write(s)
                    outfile.write("{return ")
                    outfile.write(str(id))
                    outfile.write(";}\n")
                indexmap.append([id, text.decode("utf-8")])
            for joined, base in sorted(bases.items(), key=lambda item: item[1]):
                for s in blocks[joined]:
                    outfile.write("template<>")
                    outfile.write(s)
                    outfile.write("{return ")
                    outfile.write(str(base))
                    outfile.write(";}\n")
                for i, name in enumerate(block_names(joined)):
                    indexmap.append([base + i, name.decode("utf-8")])
            indexmap.sort(key=lambda entry: entry[0])
            outfile.write("\n")
    except IOError as e:
        print(f"Error writing C++ file '{outfilename}': {e}", file=sys.stderr)
        sys.exit(1)

    try:
        with open(jsonfilename, 'w') as outfile:
            json.dump({"StringConstants": indexmap}, outfile, indent=2)
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
        command.append("-std=c++20")

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
        description='create object files with implemented remote_fmt:catalog functions and corresponding json')

    parser.add_argument('--target_name', help='prefix of the generated files')
    parser.add_argument(
        '--out_dir', help='directory for the .cpp, .json and .obj')
    parser.add_argument(
        '--source_dir', help='remote_fmt root, for the includes')
    parser.add_argument('--compiler', help='compiler for the generated .cpp')
    parser.add_argument('--objects', nargs='+',
                        help='object files to collect catalog symbols from')
    parser.add_argument('--flags', default=[],
                        nargs='+', help='extra compiler flags')
    parser.add_argument('--nm', help='nm tool to use')

    args = parser.parse_args(argv)

    # Validate required arguments
    if not args.target_name:
        print("Error: --target_name is required", file=sys.stderr)
        sys.exit(1)
    if not args.out_dir:
        print("Error: --out_dir is required", file=sys.stderr)
        sys.exit(1)
    if not args.source_dir:
        print("Error: --source_dir is required", file=sys.stderr)
        sys.exit(1)
    if not args.compiler:
        print("Error: --compiler is required", file=sys.stderr)
        sys.exit(1)
    if not args.objects:
        print("Error: --objects is required", file=sys.stderr)
        sys.exit(1)
    if not args.nm:
        print("Error: --nm is required", file=sys.stderr)
        sys.exit(1)

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
    blocks = group_texts(symbols, blocks=True)
    bad = invalid_utf8(list(texts) + list(blocks))
    for text in bad:
        print(
            f"Error: string constant is not UTF-8: {text!r}", file=sys.stderr)
    if bad:
        sys.exit(1)
    try:
        ids, bases = assign_all(texts, blocks)
    except ValueError as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)

    cpp_file = write_outputs(ids, texts, args.out_dir,
                             args.target_name, bases, blocks)
    compile_catalog(args.compiler, cpp_file, args.out_dir, args.target_name,
                    args.source_dir, args.flags)


if __name__ == "__main__":
    main()
