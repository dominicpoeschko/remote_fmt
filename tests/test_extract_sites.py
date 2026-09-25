"""Tests for tools/extract_sites.py: `python3 tests/test_extract_sites.py -v`.
The image cases build a small Cortex-M0+ image with arm-none-eabi-g++ (GNU ld) and with clang++ /
ld.lld on arm-none-eabi-g++'s headers, each with and without LTO (skipped without the tools);
RF_TEST_INCLUDES (remote_fmt/src first) as ctest sets it.
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import extract_sites as ex  # noqa: E402


def literals(text):
    return "".join(f"Lc{'n' + str(256 - b) if b >= 128 else b}E" for b in text.encode("utf-8"))


def site(enclosing, text, lambda_name="UlTyRKT_E_"):
    """A site tag's symbol: the generic lambda in the function with encoding `enclosing` (no `_Z`)."""
    return (f"_ZZZ{enclosing}ENK{lambda_name}clIN2sc14StringConstantIJ{literals(text)}EEEEtS1_"
            f"E19REMOTE_FMT_SITE_TAG")


def clang_local(enclosing, text):
    """The same in a function of internal linkage, where clang names the lambda `$_0`."""
    return site(enclosing, text, "3$_0")


DEMANGLERS = [tool for tool in (
    "llvm-cxxfilt", "c++filt") if shutil.which(tool)]


class SplitTests(unittest.TestCase):
    def test_split(self):
        self.assertEqual(ex.split_site(site("N6Kvasir3I2C3BusIcE3runEv", "up {}")),
                         ("_ZN6Kvasir3I2C3BusIcE3runEv", b"up {}"))

    def test_negative_chars_are_utf8_bytes(self):
        self.assertEqual(ex.split_site(site("1fv", "℃"))
                         [1], "℃".encode("utf-8"))

    def test_empty_string(self):
        self.assertEqual(ex.split_site(site("1fv", "")), ("_Z1fv", b""))

    def test_a_later_lambda_of_the_function(self):
        self.assertEqual(ex.split_site(
            site("1fv", "x", "UlTyRKT_E0_")), ("_Z1fv", b"x"))

    def test_a_substituted_parameter_type(self):
        self.assertEqual(ex.split_site(
            site("1fv", "x", "UlTyRKS0_E_")), ("_Z1fv", b"x"))

    def test_a_site_in_a_lambda_keeps_the_outer_lambda(self):
        self.assertEqual(ex.split_site(site("Z1fvENKUlvE_clEv", "in"))[0],
                         "_ZZ1fvENKUlvE_clEv")

    def test_template_arguments_with_strings_of_their_own(self):
        enclosing = f"N4RingIN2sc14StringConstantIJ{literals('a')}EEEE6recordEv"
        self.assertEqual(ex.split_site(site(enclosing, "r")),
                         ("_Z" + enclosing, b"r"))

    def test_lto_suffix(self):
        self.assertEqual(ex.split_site(
            site("1fv", "x") + ".llvm.123"), ("_Z1fv", b"x"))

    def test_clang_local(self):
        self.assertEqual(ex.split_site(clang_local("N12_GLOBAL__N_11fEv", "℃ {}")),
                         ("_ZN12_GLOBAL__N_11fEv", "℃ {}".encode()))

    def test_a_clang_local_length_that_does_not_fit(self):
        self.assertIsNone(ex.split_site(site("1fv", "x", "4$_0")))

    def test_other_symbols_are_no_site(self):
        self.assertIsNone(ex.split_site("_ZN10remote_fmt6detail10siteAnchorE"))

    def test_an_enclosing_clang_lambda_is_named(self):
        enclosing = "ZN12_GLOBAL__N_11fEvENK3$_1clEv"   # f()'s own lambda, which logs
        self.assertEqual(ex.local_lambda_named(enclosing),
                         "ZN12_GLOBAL__N_11fEvENK8lambda_1clEv")

    def test_numbers_that_are_not_its_length_stay(self):
        self.assertEqual(ex.local_lambda_named("_Z12$_0x"), "_Z12$_0x")


@unittest.skipUnless(DEMANGLERS, "needs llvm-cxxfilt or c++filt")
class DemangleTests(unittest.TestCase):
    def test_every_demangler(self):
        names = [clang_local("N12_GLOBAL__N_11fEv", "y" * 400),
                 site("N6Kvasir3I2C3BusIcE3runEv", "up"), "_Z1gv"]
        for tool in DEMANGLERS:
            with self.subTest(tool=tool):
                self.assertEqual(ex.demangle_sites(names, shutil.which(tool)),
                                 [("(anonymous namespace)::f()", b"y" * 400),
                                  ("Kvasir::I2C::Bus<char>::run()", b"up"), None])

    def test_an_unreadable_function_keeps_its_mangled_name(self):
        self.assertEqual(ex.demangle_sites([site("Q9broken", "x")], shutil.which(DEMANGLERS[0])),
                         [("_ZQ9broken", b"x")])


ARM = ["-mcpu=cortex-m0plus", "-mthumb"]
LONG_LINE = "a log line longer than c++filt reads: " + "0123456789" * 40


def gcc_system_includes():
    """arm-none-eabi-g++'s own include directories, for clang to compile against."""
    out = subprocess.run(["arm-none-eabi-g++", *ARM, "-std=c++26", "-E", "-x", "c++", "-", "-v"],
                         input="", capture_output=True, text=True).stderr.splitlines()
    start = out.index("#include <...> search starts here:") + 1
    end = out.index("End of search list.")
    return [f"-isystem{line.strip()}" for line in out[start:end]]


def compilers():
    found = []
    if shutil.which("arm-none-eabi-g++"):
        found.append(("gcc", ["arm-none-eabi-g++", *ARM]))
        if shutil.which("clang++") and shutil.which("ld.lld"):
            found.append(("clang", ["clang++", "--target=arm-none-eabi", *ARM, "-nostdinc++",
                                    "-nostdinc", *gcc_system_includes(), "-fuse-ld=lld"]))
    return found


@unittest.skipUnless(compilers() and os.environ.get("RF_TEST_INCLUDES"),
                     "needs arm-none-eabi-g++ and RF_TEST_INCLUDES")
class ImageTests(unittest.TestCase):
    SOURCE = r'''
#include "remote_fmt/catalog.hpp"
#include "string_constant/string_constant.hpp"
using namespace sc::literals;
volatile unsigned sink;
namespace { void local() { auto s = REMOTE_FMT_SITE(); sink = s("in local {} ℃"_sc); }
void longLine() { auto s = REMOTE_FMT_SITE(); sink = s(LONG_LINE); } }
template<typename T> struct Box { static void get() { auto s = REMOTE_FMT_SITE(); sink = s("box"_sc); } };
extern "C" [[noreturn]] void Reset_Handler() {
    local(); longLine(); Box<int>::get(); Box<char>::get();
    while(true) {}
}
'''
    SCRIPT = """MEMORY { FLASH (rx) : ORIGIN = 0x0, LENGTH = 64K  RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 16K }
ENTRY(Reset_Handler)
SECTIONS {
  .text : { *(.text*) *(.rodata*) } > FLASH
  .data : { *(.data*) } > RAM AT > FLASH
  .bss (NOLOAD) : { *(.bss*) } > RAM
%s
}
"""
    CATALOG = "remote_fmt_sites 0 (INFO) : { KEEP(*(remote_fmt_sites remote_fmt_sites.*)) }"

    def build(self, directory, catalog_section, lto, compiler=None):
        compiler = compiler or compilers()[0][1]
        source = os.path.join(directory, "t.cpp")
        script = os.path.join(directory, "t.ld")
        elf = os.path.join(directory, "t.elf")
        with open(source, "w") as f:
            f.write(self.SOURCE)
        with open(script, "w") as f:
            f.write(self.SCRIPT % (self.CATALOG if catalog_section else ""))
        includes = [
            f"-I{p}" for p in os.environ["RF_TEST_INCLUDES"].split(":")]
        includes.append(f'-DLONG_LINE="{LONG_LINE}"_sc')
        subprocess.run([*compiler, "-std=c++26", "-O2", *lto, "-nostdlib", "-fno-exceptions",
                        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                        f"-Wl,-T,{script}", *includes, "-o", elf, source],
                       check=True, capture_output=True, text=True)
        with open(elf, "rb") as f:
            return f.read()

    def test_image(self):
        for (name, compiler), lto, tool in [(c, lto, tool) for c in compilers()
                                            for lto in ([], ["-flto"]) for tool in DEMANGLERS]:
            with self.subTest(compiler=name, lto=lto, demangler=tool), \
                    tempfile.TemporaryDirectory() as directory:
                image = self.build(directory, True, lto, compiler)
                strings, sites = ex.sites_of(image, shutil.which(tool))
                self.assertEqual(sorted(strings.values()),
                                 sorted([b"box", b"box", "in local {} ℃".encode(),
                                         LONG_LINE.encode()]))
                self.assertEqual(len(set(strings)), 4, "every site its own id")
                self.assertTrue(all(0x8000 <= i < 0x10000 for i in strings))
                self.assertEqual(sorted(sites.values()),
                                 ["(anonymous namespace)::local()",
                                  "(anonymous namespace)::longLine()", "Box<char>::get()",
                                  "Box<int>::get()"])

    def test_a_catalog_in_the_image_is_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            image = self.build(directory, False, [])
            with self.assertRaisesRegex(ex.Error, "part of the image"):
                ex.sites_of(image, ex.find_demangler())

    def test_main_writes_the_json(self):
        with tempfile.TemporaryDirectory() as directory:
            self.build(directory, True, [])
            out = os.path.join(directory, "c.json")
            with open(out, "w") as f:   # as the generator writes it
                json.dump({"StringConstants": [[5, "a string"]]}, f)
            self.assertEqual(
                ex.main(["--elf", os.path.join(directory, "t.elf"), "--json", out]), 0)
            with open(out) as f:
                data = json.load(f)
            texts = {text for _, text in data["StringConstants"]}
            self.assertEqual(
                texts, {"a string", "box", "in local {} ℃", LONG_LINE})
            self.assertEqual(len(data["Sites"]), 4)

    def test_a_site_on_a_string_id_is_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            image = self.build(directory, True, [])
            strings, sites = ex.sites_of(image, ex.find_demangler())
            out = os.path.join(directory, "c.json")
            with open(out, "w") as f:
                json.dump({"StringConstants": [[min(strings), "taken"]]}, f)
            with self.assertRaisesRegex(ex.Error, "a string's id too"):
                ex.merge(out, strings, sites)


if __name__ == "__main__":
    unittest.main()
