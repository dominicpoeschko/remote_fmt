"""Tests for tools/extract_sites.py: `python3 tests/test_extract_sites.py -v`.
The image cases build a small Cortex-M0+ image with arm-none-eabi-g++ (GNU ld) and with clang++ /
ld.lld on arm-none-eabi-g++'s headers, each with and without LTO (skipped without the tools);
RF_TEST_INCLUDES (remote_fmt/src first) as ctest sets it.
"""

import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import unittest.mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.dont_write_bytecode = True  # no __pycache__ next to the imported tools
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import descriptor as ds  # noqa: E402
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

    def test_a_substituted_string_constant(self):
        # A function whose own template argument is a StringConstant: its site's string type
        # is `N S1_ IJ...E E` (sc::StringConstant substituted), as g++ 16 and clang++ 22 both
        # mangle it.
        self.assertEqual(ex.split_site(
            "_ZZZ1fIN2sc14StringConstantIJLc120EEEEEPcvENKUlTyT_E_clINS1_IJLc104ELc105EEEEEE"
            "DaS4_E19REMOTE_FMT_SITE_TAG"),
            ("_Z1fIN2sc14StringConstantIJLc120EEEEEPcv", b"hi"))
        self.assertEqual(ex.split_site(
            "_ZZZ1fIiEPcvENKUlTyT_E_clIN2sc14StringConstantIJLc104ELc105EEEEEEDaS1_E"
            "19REMOTE_FMT_SITE_TAG"),
            ("_Z1fIiEPcv", b"hi"), "and the same function without it")

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


class DescriptorTests(unittest.TestCase):
    def test_references(self):
        d = b"u4O#0123456789abcdef(k2:#x)" + b"F5:{:#x}1:#fedcba9876543210" + \
            b"Ei41:-1,2:#z" + b"Ru46:{}{:#}0:1:3,0:"
        self.assertEqual(ds.type_refs(
            d), ["0123456789abcdef", "fedcba9876543210"])

    # every kind, one argument each, in a row: the split cuts after each complete descriptor
    EVERY_KIND = [b"b", b"c", b"u1", b"i8", b"f4", b"p4", b"s", b"k2:ok", b"d1/1000i8", b"Ou2", b"Xvi4", b"V2:u4f4",
                  b"Lu1", b"Su2", b"M(su4)", b"Ai2", b"(u1i2)", b"Q2: Vf4", b"Ys", b"*", b"F3:a{}1:u4",
                  b"Ei12:0,1:a1,1:b", b"Gu12:1,1:x2,1:y", b"Ei11:-5,1:n", b"Ru42:{}1:4,1:3,0:", b"ru42:1,1:a2,1:b",
                  b"#0123456789abcdef", b"()", b"(V1:k3:a)bF2:{}0:)"]

    def test_every_kind_reads(self):
        self.assertEqual(ds.split_arguments(
            b"".join(self.EVERY_KIND)), self.EVERY_KIND)

    # a register and its Flags as Kvasir_SDK's RegisterFmt.hpp writes them (a type entry's text in an RP2350 image)
    REGISTERS = [b'Ru473:systick::csr(\n"countflag": {}\n"clksource": {}\n"tickint": {}\n"enable": {})0:4:65536,2:0,'
                 b'26:timer_has_not_counted_to_01,22:timer_has_counted_to_04,2:0,8:external1,9:processor2,2:0,'
                 b'18:interrupt_disabled1,17:interrupt_enabled1,2:0,19:counter_is_disabled1,20:counter_is_operating',
                 b"ru44:65536,9:countflag4,9:clksource2,7:tickint1,6:enable"]

    def test_registers_split(self):
        self.assertEqual(ds.split_arguments(
            b"i8" + b"".join(self.REGISTERS) + b"u4"), [b"i8", *self.REGISTERS, b"u4"])

    def test_a_malformed_descriptor_is_an_error(self):
        for bad in (b"u3", b"k9:ab", b"V2:u4", b"(u4", b"#12", b"Z", b"d-1/1u4", b"E-u11:0,1:a", b"u", b"F2:{}"):
            with self.subTest(descriptor=bad), self.assertRaises(ex.Error):
                ds.split_arguments(bad)

    def test_too_deep_is_an_error(self):
        ds.split_arguments(b"O" * 64 + b"u4")
        with self.assertRaisesRegex(ex.Error, "too deep"):
            ds.split_arguments(b"O" * 65 + b"u4")

    def test_catalog_entries(self):
        self.assertEqual(ds.catalog_entry(7, b"a {} {}\x1eu4F7:<{} {}>2:i4i4"),
                         {"id": 7, "fmt": "a {} {}", "args": ["u4", "F7:<{} {}>2:i4i4"]})
        self.assertEqual(ds.catalog_entry(8, b"none\x1e"), {
                         "id": 8, "fmt": "none", "args": []})
        for text in (b"a {} {}\x1eu4F7:<{} {}>2:i4i4", b"none\x1e", "\u2103 {}\x1ek3:\u2103".encode("utf-8")):
            with self.subTest(text=text):
                self.assertEqual(ds.entry_text(
                    ds.catalog_entry(1, text)), text)
        with self.assertRaises(ex.Error):
            ds.catalog_entry(1, b"x {}\x1eu3")

    def test_a_string_without_descriptors_is_refused(self):
        # every cataloged string is remote_fmt::described<Args...>(fmt): one without 0x1e has no place in a catalog
        with self.assertRaisesRegex(ex.Error, "has no descriptors"):
            ds.catalog_entry(9, b"plain")

    def test_a_string_without_references(self):
        self.assertEqual(ex.type_refs(b"plain {}\x1eu4"), [])

    def test_the_key_is_fnv1a_64(self):
        self.assertEqual(ex.fnv1a64(b""), 0xcbf29ce484222325)
        self.assertEqual(ex.type_key(b"a"), "af63dc4c8601ec8c")

    def test_two_texts_on_one_key_fail(self):
        elf = (False, {1: 0}, [("_ZZN10remote_fmt6detail7typeTagIN2sc14StringConstantIJLc97EEEEEvvE19REMOTE_FMT_TYPE_TAG", 0, 1),
                               ("_ZZN10remote_fmt6detail7typeTagIN2sc14StringConstantIJLc98EEEEEvvE19REMOTE_FMT_TYPE_TAG", 0, 1)])
        with unittest.mock.patch.object(ex, "type_key", lambda text: "0" * 16):
            with self.assertRaisesRegex(ex.Error, "share the key"):
                ex.types_of(elf)

    def test_missing_entries_inside_entries(self):
        self.assertEqual(ex.missing_types(
            [], {"a" * 16: b"O#" + b"b" * 16}), [(f"type {'a' * 16}", "b" * 16)])


class AbbreviateTests(unittest.TestCase):
    def check(self, signature, expected, keep=8, keep_names=None):
        self.assertEqual(ex.abbreviated(signature, keep, keep if keep_names is None else keep_names),
                         expected)

    def test_short_lists_stay(self):
        self.check("ns::A<int, B<char>>::f(C<int>)",
                   "ns::A<int, B<char>>::f(C<int>)", keep=12)

    def test_a_long_list_is_cut_after_an_argument(self):
        self.check("A<int, long, char>::f()", "A<int, …>::f()")
        self.check("A<unsigned long, char>::f()", "A<…>::f()", keep=8)

    def test_a_long_nested_list_collapses(self):
        self.check("A<B<int, long, char>>::f()",
                   "A<B<…>>::f()", keep_names=100)

    def test_innermost_first(self):
        # uc_log's qualifiedFunction still reads `App<Bus<>, Cfg>::run`
        self.check("App<Bus<Arg, Arg, Arg>, Cfg>::run()",
                   "App<Bus<…>, Cfg>::run()", keep=12)
        self.check("App<Bus<Arg, Arg, Arg>, Config, Other>::run()", "App<Bus<…>, Config, …>::run()",
                   keep=12, keep_names=15)

    def test_counted_as_uc_log_shows_it(self):
        # no qualifiers, and a nested list's content not at all
        self.check("App<ns::inner::Bus<ns::Pin<1>, ns::Pin<2>>, ns::Config>::run()",
                   "App<ns::inner::Bus<ns::Pin<1>, ns::Pin<2>>, ns::Config>::run()",
                   keep=100, keep_names=12)

    def test_only_the_class_list_keeps_its_arguments(self):
        # uc_log shows the arguments of the class a function belongs to, and nothing else
        self.check("A<int, long, char>::f()",
                   "A<int, long, …>::f()", keep_names=12)
        self.check("A<int, long, char> g()", "A<…> g()", keep_names=100)
        self.check("f(A<int, long, char> const&, B<int>)", "f(A<…> const&, B<int>)",
                   keep_names=100)
        self.check("ns::f<int, long, char>(int)",
                   "ns::f<…>(int)", keep_names=100)
        self.check("App<Bus<int>>::run<int, long, char>(Bus<int, long, char>)",
                   "App<Bus<int>>::run<…>(Bus<…>)", keep_names=100)

    def test_gnu_spacing(self):
        self.check("A<B<int, long, char> >::f()",
                   "A<B<…> >::f()", keep_names=100)

    def test_operators_are_names(self):
        for signature in ["X::operator<(X)", "X::operator>>(int)", "X::operator->()",
                          "X::operator<=>(X const&)", "X::operator()(int)", "X::operator[](int)",
                          "operator<<(std::ostream&, X)", "X::operator bool() const"]:
            self.check(signature, signature)

    def test_an_operator_template(self):
        # llvm-cxxfilt writes the arguments right after the symbol, GNU c++filt after a space;
        # uc_log shows them as they are, so they stay whole
        for signature in ["bool operator<<<int, long, char>(int, X)",
                          "bool operator< <int, long, char>(int, X)",
                          "bool B<int>::operator><int, long, char>(int)",
                          "X::operator()<A<B<int, long, char>>>(int)::'lambda'()"]:
            self.check(signature, signature)
        longer = "A<" + "B<int, long>, " * 30 + "C>"
        self.check(f"X::operator()<{longer}>(int)", "X::operator()<A<…>>(int)")

    def test_a_template_operator_less(self):
        # llvm-cxxfilt writes operator< with its arguments as `operator<<int>`; read as
        # `operator<<` it left a `>` over, which crashed the step and the build
        for signature in ["bool Conv::operator<<int>(int) const", "bool Conv::operator<<<int>(int) const",
                          "bool Conv::operator< <int>(int) const",
                          "bool Conv::operator><int>(int) const", "bool Conv::operator>><int>(int) const"]:
            self.check(signature, signature)
        self.check("void f<&X::operator<>(int)",
                   "void f<&X::operator<>(int)", keep=100)
        self.check("bool Conv::operator<<A<int, long, char>>(int) const",
                   "bool Conv::operator<<A<int, long, char>>(int) const")

    def test_a_stray_closing_bracket_stays(self):
        self.check("a > b", "a > b")
        self.check("f(A<int, long, char>) >", "f(A<…>) >")

    def test_expressions_in_arguments(self):
        # as llvm-cxxfilt and c++filt print a dependent `N > 2`, `N < 2`, `(N >> 1) > 1`
        for signature in ["void f<3>(A<(3 > 2)>)", "void f<3>(A<((3)>(2))>)",
                          "void g<3>(A<(3)<(2)>)", "void h<5>(A<(((5)>>(1))>(1))>)"]:
            self.check(signature, signature, keep=100)
        self.check("void h<5>(A<(5 >> 1 > 1), int, long>)",
                   "void h<5>(A<…>)", keep=16)
        self.check("B<A<(5 >> 1 > 1), int, long>>::f()",
                   "B<A<…>>::f()", keep=16)

    def test_a_function_type_argument_is_abbreviated_inside(self):
        self.check("F<void (int)>::f()", "F<void (int)>::f()", keep=10)
        self.check("F<void (B<int, long, char>)>::f()",
                   "F<void (B<…>)>::f()", keep_names=100)

    def test_lambdas_and_anonymous_namespaces(self):
        signature = "(anonymous namespace)::f()::'lambda'(auto)::operator()<int>(int) const"
        self.check(signature, signature)
        self.check("{lambda(auto:1)#1}::operator()<A<int, long, char>>(A<int, long, char>) const",
                   "{lambda(auto:1)#1}::operator()<A<int, long, char>>(A<…>) const")
        # a lambda's parameters inside a template argument list are abbreviated too
        self.check("f<T<'lambda'(Bus<int, long, char, short, bool>&)>>(int)",
                   "f<T<'lambda'(Bus<…>&)>>(int)", keep=20, keep_names=100)

    def test_unbalanced_stays(self):
        # llvm-cxxfilt's `N < 2` in an argument has no parentheses
        self.check("void g<3>(A<3 < 2>)", "void g<3>(A<3 < 2>)")
        self.check("f(A<B<int, long, char>)", "f(A<B<…>)")
        self.check("f(A<B<int, long, char>) x", "f(A<B<…>) x")
        self.check("f(a))", "f(a))")

    def test_a_signature_it_cannot_read_is_cut(self):
        with unittest.mock.patch.object(ex, "abbreviated", side_effect=IndexError("bug")), \
                unittest.mock.patch("sys.stderr", new_callable=io.StringIO) as err:
            self.assertEqual(ex.abbreviated_or_cut("f()", {}), "f()")
            self.assertEqual(ex.abbreviated_or_cut("g" * 300, {}),
                             "g" * ex.MAX_MANGLED + "…")
        self.assertIn("IndexError: bug", err.getvalue())

    def test_memo_gives_the_same(self):
        inner = "B<" + ", ".join(f"T{i}<int, long>" for i in range(40)) + ">"
        signature = f"f<A<{inner}, {inner}>>(A<{inner}, {inner}>, C<{inner}>)"
        memo = {}
        first = ex.abbreviated(signature, memo=memo)
        self.assertTrue(memo, "the long nested lists are remembered")
        self.assertEqual(ex.abbreviated(signature, memo=memo), first)
        self.assertEqual(first, "f<A<B<…>, B<…>>>(A<B<…>, B<…>>, C<B<…>>)")
        # a remembered nested list met as a name's own list is not taken from the memo
        self.assertEqual(ex.abbreviated(f"g({inner})", memo=memo),
                         ex.abbreviated(f"g({inner})"))

    def test_default_keeps_the_ordinary(self):
        signature = ("Kvasir::I2C::Device<Kvasir::I2C::Bus<Hw::Sda, Hw::Scl>, Kvasir::Clock, "
                     "Chip::Tca9548a>::run()")
        self.assertEqual(ex.abbreviated(signature), signature)


@unittest.skipUnless(DEMANGLERS, "needs llvm-cxxfilt or c++filt")
class DemangleTests(unittest.TestCase):
    def test_long_template_arguments_are_abbreviated(self):
        # Box<Pack<T0, ..., T39>>::get(): each demangler, the inner list gives way first
        args = "".join(f"N2ns2T{i}E" if i <
                       10 else f"N2ns3T{i}E" for i in range(40))
        name = site(f"N3BoxIJ4PackIJ{args}EEEE3getEv", "x")
        for tool in DEMANGLERS:
            with self.subTest(tool=tool):
                (signature, text), = ex.demangle_sites(
                    [name], shutil.which(tool))
                self.assertEqual((signature, text),
                                 ("Box<Pack<…>>::get()", b"x"))

    def test_the_same_spelling_from_each_demangler(self):
        # GNU c++filt's `> >` is written `>>`: the catalog does not depend on the machine
        name = site("N1AIN1BIiEEE1fEv", "x")    # A<B<int>>::f()
        for tool in DEMANGLERS:
            with self.subTest(tool=tool):
                self.assertEqual(ex.demangle_sites([name], shutil.which(tool)),
                                 [("A<B<int>>::f()", b"x")])

    def test_an_unreadable_long_name_is_cut(self):
        (signature, _), = ex.demangle_sites([site("Q" + "9broken" * 60, "x")],
                                            shutil.which(DEMANGLERS[0]))
        self.assertEqual(len(signature), ex.MAX_MANGLED + 1)
        self.assertTrue(signature.startswith("_ZQ9broken")
                        and signature.endswith("…"))

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

    @unittest.skipUnless(len(DEMANGLERS) == 2, "needs llvm-cxxfilt and c++filt")
    def test_the_other_demangler_reads_what_the_first_cannot(self):
        # a generic lambda in main (`$_7`): llvm-cxxfilt reads no `T_` in a member
        # template of a local class; f<1>() with clang's `TnDa`: GNU c++filt reads no `Tn`
        names = [site("Z4mainENK3$_7clIN9Telemetry11SetMetaDataEEEDaRKT_", "x"),
                 site("1fITnDaLi1EEvv", "y")]
        for tool in DEMANGLERS:
            with self.subTest(tool=tool):
                self.assertEqual(ex.demangle_sites(names, shutil.which(tool)), [
                    ("auto main::lambda_7::operator()<Telemetry::SetMetaData>"
                     "(Telemetry::SetMetaData const&) const", b"x"),
                    ("void f<1>()", b"y")])

    @unittest.skipUnless(len(DEMANGLERS) == 2, "needs llvm-cxxfilt and c++filt")
    def test_a_name_with_both_is_read_without_the_builtin_tn(self):
        # F::handler<Id::a>(e, [] {}) in a generic lambda in t2(): the closure's `RT_` stops
        # llvm-cxxfilt, the `TnDa` GNU c++filt -- which reads it with the mark dropped
        name = site(
            "N1F7handlerITnDaL2Id0EiZZ2t2vENK3$_0clIiEEDaRT_EUlvE_EEvRT0_T1_", "x")
        for tool in DEMANGLERS:
            with self.subTest(tool=tool):
                self.assertEqual(ex.demangle_sites([name], shutil.which(tool)),
                                 [("void F::handler<…>(int&, t2()::lambda_0::operator()<int>(int&) "
                                   "const::{lambda()#1})", b"x")])

    def test_only_a_tn_before_a_builtin_type_is_dropped(self):
        self.assertEqual(ex.without_builtin_tn(
            "_Z1fITnDaLi1EEvv"), "_Z1fILi1EEvv")
        self.assertEqual(ex.without_builtin_tn(
            "_Z1fITniLi1EEvv"), "_Z1fILi1EEvv")
        # a class type is a substitution candidate: dropping it would move every `S<n>_`
        self.assertEqual(ex.without_builtin_tn(
            "_Z1fITn1ALi1EEvv"), "_Z1fITn1ALi1EEvv")

    def test_an_unreadable_name_is_reported_once_with_the_tools_tried(self):
        with unittest.mock.patch("sys.stderr", new_callable=io.StringIO) as err:
            ex.demangle_sites([site("Q9broken", "x"), site("Q9broken", "y")],
                              shutil.which(DEMANGLERS[0]))
        self.assertEqual(err.getvalue().count("_ZQ9broken"), 1)
        for tool in DEMANGLERS:
            self.assertIn(tool, err.getvalue())


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
    # site strings as remote_fmt::described makes them (the format string, 0x1e, descriptors); one site names a type
    # entry by its key, and the image holds that entry's tag (descriptor.hpp)
    SOURCE = r'''
#include "remote_fmt/catalog.hpp"
#include "remote_fmt/descriptor.hpp"
#include "string_constant/string_constant.hpp"
using namespace sc::literals;
volatile unsigned sink;
using Entry = decltype("Eu12:1,1:a2,4:b{}c"_sc);
namespace { void local() { auto s = REMOTE_FMT_SITE(); sink = s("in local {} ℃\x1eu4"_sc); }
void longLine() { auto s = REMOTE_FMT_SITE(); sink = s(LONG_LINE + "\x1e"_sc); } }
template<typename T> struct Box { static void get() { auto s = REMOTE_FMT_SITE(); sink = s("box\x1e"_sc); } };
template<typename S> struct Named { static void get() { auto s = REMOTE_FMT_SITE(); sink = s("named\x1e"_sc); } };
void typed() {
    auto s = REMOTE_FMT_SITE();
    sink = s("t {:#x}\x1e"_sc + remote_fmt::desc::typeKey<Entry>());
    remote_fmt::detail::typeTag<Entry>();
}
extern "C" [[noreturn]] void Reset_Handler() {
    local(); longLine(); Box<int>::get(); Box<char>::get(); Named<decltype("x"_sc)>::get(); typed();
    while(true) {}
}
'''
    ENTRY = b"Eu12:1,1:a2,4:b{}c"
    STRINGS = sorted([b"box\x1e", b"box\x1e", "in local {} ℃\x1eu4".encode(), LONG_LINE.encode() + b"\x1e",
                      b"named\x1e", b"t {:#x}\x1e#" + ex.type_key(ENTRY).encode()])
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
                strings, sites = ex.sites_of(
                    ex.read_symbols(image), shutil.which(tool))
                self.assertEqual(sorted(strings.values()), self.STRINGS)
                self.assertEqual(len(set(strings)), 6, "every site its own id")
                self.assertTrue(all(0x8000 <= i < 0x10000 for i in strings))
                self.assertEqual(sorted(sites.values()),
                                 ["(anonymous namespace)::local()",
                                  "(anonymous namespace)::longLine()", "Box<char>::get()",
                                  "Box<int>::get()",
                                  "Named<sc::StringConstant<(char)120>>::get()", "typed()"])
                self.assertEqual(ex.types_of(ex.read_symbols(image)), {
                                 ex.type_key(self.ENTRY): self.ENTRY}, "the type entry")

    @unittest.skipUnless(shutil.which("clang++") and shutil.which("ld.lld"), "needs clang++ and ld.lld")
    def test_a_32_bit_image_of_another_machine(self):
        # catalog.hpp counts from address 0 wherever pointers are 32 bits, not only on ARM
        riscv = ["clang++", "--target=riscv32-unknown-elf", "-march=rv32imac", "-mabi=ilp32",
                 "-nostdinc++", "-nostdinc", *gcc_system_includes(), "-fuse-ld=lld"]
        with tempfile.TemporaryDirectory() as directory:
            strings, sites = ex.sites_of(ex.read_symbols(
                self.build(directory, True, [], riscv)), ex.find_demangler())
        self.assertEqual(sorted(strings.values()), self.STRINGS)
        self.assertTrue(all(0x8000 <= i < 0x10000 for i in strings))

    def test_a_catalog_in_the_image_is_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            image = self.build(directory, False, [])
            with self.assertRaisesRegex(ex.Error, "part of the image"):
                ex.sites_of(ex.read_symbols(image), ex.find_demangler())

    # as the generator writes it
    PRELINK = {"StringConstants": [{"id": 5, "fmt": "a string", "args": []}]}

    def run_main(self, directory, prelink):
        """main() on the image built in `directory` with `prelink` as --strings: (exit code, the catalog's path)."""
        strings = os.path.join(directory, "c.prelink.json")
        with open(strings, "w") as f:
            json.dump(prelink, f, indent=2)
        out = os.path.join(directory, "c.json")
        return ex.main(["--elf", os.path.join(directory, "t.elf"), "--strings", strings, "--json", out]), out

    def test_main_writes_the_json(self):
        with tempfile.TemporaryDirectory() as directory:
            self.build(directory, True, [])
            code, out = self.run_main(directory, self.PRELINK)
            self.assertEqual(code, 0)
            with open(out) as f:
                data = json.load(f)
            self.assertEqual(list(data), [
                             "StringConstants", "Sites", "FrameCheck", "Types"], "the catalog's order")
            texts = {ds.entry_text(entry).decode()
                     for entry in data["StringConstants"]}
            self.assertTrue(all(isinstance(arg, str) for entry in data["StringConstants"]
                                for arg in entry["args"]))
            with open(out, encoding="utf-8") as f:
                self.assertIn(
                    "℃", f.read(), "written as UTF-8, not \\u escapes")
            self.assertEqual(texts, {"a string\x1e"} | {
                             t.decode() for t in self.STRINGS})
            self.assertEqual(len(data["Sites"]), 6)
            self.assertEqual(data["Types"], {ex.type_key(
                self.ENTRY): self.ENTRY.decode()})
            with open(os.path.join(directory, "c.prelink.json")) as f:
                self.assertEqual(json.load(f), self.PRELINK,
                                 "the generator's file is only read")

    def test_a_failing_step_leaves_the_previous_catalog(self):
        # a reader keeps the last complete catalog: nothing is written unless everything is known
        with tempfile.TemporaryDirectory() as directory:
            self.build(directory, True, [])
            out = os.path.join(directory, "c.json")
            with open(out, "w") as f:
                f.write("the previous catalog")
            unresolved = {"StringConstants": [
                {"id": 5, "fmt": "x {}", "args": ["#00000000000000aa"]}]}
            with unittest.mock.patch("sys.stderr", new_callable=io.StringIO) as err:
                code, _ = self.run_main(directory, unresolved)
            self.assertEqual(code, 1)
            self.assertIn("without an entry", err.getvalue())
            with open(out) as f:
                self.assertEqual(f.read(), "the previous catalog")
            self.assertEqual(sorted(os.listdir(directory)), [
                             "c.json", "c.prelink.json", "t.cpp", "t.elf", "t.ld"])

    def test_a_reference_without_its_entry_fails_the_link_step(self):
        with tempfile.TemporaryDirectory() as directory:
            image = self.build(directory, True, [])
            strings, sites = ex.sites_of(
                ex.read_symbols(image), ex.find_demangler())
            with self.assertRaisesRegex(ex.Error, "without an entry"):
                ex.catalog({"StringConstants": []}, strings, sites, types={})
            # the generator's strings are checked too
            prelink = {"StringConstants": [
                {"id": 5, "fmt": "x {}", "args": ["#00000000000000aa"]}]}
            with self.assertRaisesRegex(ex.Error, "first #00000000000000aa in string 5"):
                ex.catalog(prelink, strings, sites,
                           types=ex.types_of(ex.read_symbols(image)))

    def test_a_site_on_a_string_id_is_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            image = self.build(directory, True, [])
            strings, sites = ex.sites_of(
                ex.read_symbols(image), ex.find_demangler())
            with self.assertRaisesRegex(ex.Error, "a string's id too"):
                ex.catalog({"StringConstants": [
                           {"id": min(strings), "fmt": "taken", "args": []}]}, strings, sites)

    def build_config(self, directory, values, switch="REMOTE_FMT_USE_ENCHANTUM"):
        """An image of one TU per value of a config.hpp switch, each including config.hpp."""
        compiler = compilers()[0][1]
        script = os.path.join(directory, "t.ld")
        elf = os.path.join(directory, "t.elf")
        with open(script, "w") as f:
            f.write(self.SCRIPT % self.CATALOG)
        sources = []
        for n, value in enumerate(values):
            source = os.path.join(directory, f"c{n}.cpp")
            with open(source, "w") as f:
                f.write(f'#define {switch} {value}\n#include "remote_fmt/config.hpp"\n'
                        f'int tu{n}() {{ return {n}; }}\n')
                if n == 0:
                    f.write(
                        'extern "C" [[noreturn]] void Reset_Handler() { while(true) {} }\n')
            sources.append(source)
        includes = [
            f"-I{p}" for p in os.environ["RF_TEST_INCLUDES"].split(":")]
        subprocess.run([*compiler, "-std=c++26", "-O2", "-nostdlib", "-fno-exceptions", "-ffunction-sections",
                        "-fdata-sections", "-Wl,--gc-sections", f"-Wl,-T,{script}", *
                        includes, "-o", elf,
                        *sources], check=True, capture_output=True, text=True)
        with open(elf, "rb") as f:
            return f.read()

    def test_one_value_per_switch_passes(self):
        with tempfile.TemporaryDirectory() as directory:
            self.assertEqual(ex.config_conflicts(
                ex.read_symbols(self.build_config(directory, [1, 1]))), {})

    def test_tus_that_disagree_fail_the_link_step(self):
        with tempfile.TemporaryDirectory() as directory:
            self.assertEqual(ex.config_conflicts(ex.read_symbols(self.build_config(directory, [0, 1]))),
                             {"USE_ENCHANTUM": [0, 1]})
            with unittest.mock.patch("sys.stderr", new_callable=io.StringIO) as err:
                self.assertEqual(self.run_main(
                    directory, {"StringConstants": []})[0], 1)
            self.assertIn("disagree on REMOTE_FMT_USE_ENCHANTUM",
                          err.getvalue())
            self.assertFalse(os.path.exists(os.path.join(
                directory, "c.json")), "no catalog written")

    def test_the_encoder_knobs_have_markers(self):
        """A mixed FRAME_CHECK fails the link step (before, the catalog said "FrameCheck": true and the unchecked TU's
        frames were refused as another image's); one value is the catalog's FrameCheck."""
        for switch, values, seen in (("FRAME_CHECK", [0, 1], [0, 1]), ("STAGING_BYTES", [0, 32], [0, 32]),
                                     ("USE_CATALOG", ["false", "true"], [0, 1])):
            with self.subTest(switch=switch), tempfile.TemporaryDirectory() as directory:
                elf = ex.read_symbols(self.build_config(
                    directory, values, f"REMOTE_FMT_{switch}"))
                self.assertEqual(ex.config_conflicts(elf), {switch: seen})
        for value, checked in ((0, False), (1, True)):
            with tempfile.TemporaryDirectory() as directory:
                elf = ex.read_symbols(self.build_config(
                    directory, [value, value], "REMOTE_FMT_FRAME_CHECK"))
                self.assertEqual(ex.config_conflicts(elf), {})
                self.assertEqual(ex.frame_check_of(elf), checked)


if __name__ == "__main__":
    unittest.main()
