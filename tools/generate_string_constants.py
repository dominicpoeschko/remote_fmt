import argparse
import subprocess
import json
import sys
import os
import shutil

parser = argparse.ArgumentParser(
    prog='generate_string_constants',
    description='create object files with implemented remote_fmt:catalog functions and corresponding json')

parser.add_argument('--target_name', help='target name')
parser.add_argument('--out_dir', help='dir name')
parser.add_argument('--source_dir', help='dir name')
parser.add_argument('--compiler', help='compiler name')
parser.add_argument('--objects', nargs='+', help='compiler name')
parser.add_argument('--flags', default=[],  nargs='+', help='compiler name')
parser.add_argument('--nm', help='nm tool to use')

args = parser.parse_args()

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


def parse_StringConstant(s):
    """Parse a StringConstant template parameter into a readable string."""
    s = s.removeprefix("sc::StringConstant<")
    s = s.partition(">")[0]
    l = ""

    while s:
        s = s.removeprefix("(char)")
        part = s.partition(", ")[0]
        s = s.removeprefix(part)
        s = s.removeprefix(", ")
        try:
            l += chr(int(part))
        except (ValueError, OverflowError) as e:
            print(
                f"Error parsing character code '{part}': {e}", file=sys.stderr)
            return None

    return l


def parse_symbol(symbol):
    """Extract the string constant from a symbol name."""
    symbol = symbol.removeprefix("unsigned short remote_fmt::catalog<")
    symbol = symbol.removesuffix(">")

    result = parse_StringConstant(symbol)
    if result is None:
        print(f"Failed to parse symbol: {symbol}", file=sys.stderr)
        return None
    return result


# Check if tools are available
if not shutil.which(args.nm):
    print(f"Error: nm '{args.nm}' not found.", file=sys.stderr)
    sys.exit(1)

if not shutil.which(args.compiler):
    print(f"Error: Compiler '{args.compiler}' not found.", file=sys.stderr)
    sys.exit(1)

symbols = []

for f in args.objects:
    if not os.path.exists(f):
        print(f"Error: Object file '{f}' not found.", file=sys.stderr)
        sys.exit(1)

    try:
        x = subprocess.run([args.nm, "-uC", f],
                           check=True, capture_output=True, text=True)
    except subprocess.CalledProcessError as e:
        print(f"Error running nm on '{f}': {e}", file=sys.stderr)
        sys.exit(1)
    except FileNotFoundError:
        print("Error: nm command not found.", file=sys.stderr)
        sys.exit(1)
    for l in iter(x.stdout.splitlines()):
        if l.strip().startswith("U unsigned short remote_fmt::catalog<sc::StringConstant<"):
            symbols.append(l.strip().removeprefix("U "))

symbols = list(set(symbols))
symbols.sort()
outfilename = os.path.join(
    args.out_dir, f"{args.target_name}_string_constants.cpp")
jsonfilename = os.path.join(
    args.out_dir, f"{args.target_name}_string_constants.json")

indexmap = []

try:
    with open(outfilename, 'w') as outfile:
        outfile.write("#include <remote_fmt/catalog.hpp>\n")
        outfile.write("#include <string_constant/string_constant.hpp>\n")
        id = 0
        for s in symbols:
            outfile.write("template<>")
            outfile.write(s)
            outfile.write("{return ")
            outfile.write(str(id))
            outfile.write(";}\n")
            entry = []
            entry.append(id)
            parsed_symbol = parse_symbol(s)
            if parsed_symbol is None:
                print(f"Skipping invalid symbol: {s}", file=sys.stderr)
                continue
            entry.append(parsed_symbol)
            indexmap.append(entry)
            id = id+1
        outfile.write("\n")
except IOError as e:
    print(f"Error writing C++ file '{outfilename}': {e}", file=sys.stderr)
    sys.exit(1)

try:
    with open(jsonfilename, 'w') as outfile:
        json.dump({"StringConstants": indexmap}, outfile, indent=2)
except IOError as e:
    print(f"Error writing JSON file '{jsonfilename}': {e}", file=sys.stderr)
    sys.exit(1)

command = []
command.append(args.compiler)
command.append("-o")
command.append(os.path.join(
    args.out_dir, f"{args.target_name}_string_constants.obj"))
command.append("-c")
command.append(outfilename)
command.append("-Wno-old-style-cast")
command.append(f"-isystem{os.path.join(args.source_dir, 'src')}")
command.append(
    f"-isystem{os.path.join(args.source_dir, 'string_constant', 'src')}")

for f in args.flags:
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
    print(f"Error: Compiler '{args.compiler}' not found.", file=sys.stderr)
    sys.exit(1)
