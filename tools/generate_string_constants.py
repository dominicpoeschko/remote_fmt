import argparse
import subprocess

parser = argparse.ArgumentParser(
                    prog = 'generate_string_constatns',
                    description = 'create object files with implementet remote_fmt:catalog functions and coresponding json')

parser.add_argument('--target_name', help='target name')
parser.add_argument('--out_dir', help='dir name')
parser.add_argument('--source_dir', help='dir name')
parser.add_argument('--compiler', help='compiler name')
parser.add_argument('--objects',nargs='+', help='compiler name')
parser.add_argument('--flags',default=[],  nargs='+', help='compiler name')

args = parser.parse_args()

def parse_StringConstant(s):
    s=s.removeprefix("sc::StringConstant<")
    s=s.partition(">")[0]
    l=""

    while s:
        s=s.removeprefix("(char)")
        part=s.partition(", ")[0]
        s=s.removeprefix(part)
        s=s.removeprefix(", ")
        l +=chr(int(part))

    return l

def parse_symbol(symbol):
    symbol = symbol.removeprefix("unsigned short remote_fmt::catalog<")
    symbol = symbol.removesuffix(">")

    return parse_StringConstant(symbol)

def generate_entry(symbol, id):
    ret="["
    ret+=str(id)
    ret+=","

    ret+="\""
    ret+=parse_symbol(symbol).replace('"','\\"')

    ret+="\""
    ret+="]"
    return ret

symboles = [];

for f in args.objects:
    x=subprocess.run(["llvm-nm","-uC", f],check=True, capture_output=True,text=True)
    for l in iter(x.stdout.splitlines()):
        if l.strip().startswith("U unsigned short remote_fmt::catalog<sc::StringConstant<"):
            symboles.append(l.strip().removeprefix("U "))

symboles = list(set(symboles))
symboles.sort()
outfilename=args.out_dir+"/"+args.target_name+"_string_constants.cpp"

indexmap=[]

with open(outfilename, 'w') as outfile:
    outfile.write("#include <remote_fmt/catalog.hpp>\n")
    outfile.write("#include <string_constant/string_constant.hpp>\n")
    id=0
    for s in symboles:
        outfile.write("template<>")
        outfile.write(s)
        outfile.write("{return ")
        outfile.write(str(id));
        outfile.write(";}\n")
        indexmap.append(generate_entry(s,id))
        id=id+1
    outfile.write("\n")

with open(args.out_dir+"/"+args.target_name+"_string_constants.json", 'w') as outfile:
    outfile.write('{"StringConstants":[')
    first = True
    for e in indexmap:
        if first:
            first = False
        else:
            outfile.write(',')
        outfile.write(e)
    outfile.write(']}')

command = []
command.append(args.compiler)
command.append("-o")
command.append(args.out_dir+"/"+args.target_name+"_string_constants.obj")
command.append("-c")
command.append(outfilename)
command.append("-Wno-old-style-cast")
command.append("-isystem"+ args.source_dir+"/src/")
command.append("-isystem"+ args.source_dir+"/string_constant/src/")

for f in args.flags:
    for a in f.split():
        command.append(a)

std_defined=False
for arg in command:
    if(arg.startswith("-std=")):
        std_defined=True

if(not std_defined):
    command.append("-std=c++20")

subprocess.run(command, check=True)
