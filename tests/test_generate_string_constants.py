"""Tests for tools/generate_string_constants.py: `python3 tests/test_generate_string_constants.py -v`.
The end-to-end case needs RF_TEST_CXX, RF_TEST_NM and RF_TEST_INCLUDES (remote_fmt/src first), as ctest sets them.
"""

import contextlib
import io
import json
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.dont_write_bytecode = True  # no __pycache__ next to the imported tools
sys.path.insert(0, os.path.join(os.path.dirname(
    os.path.abspath(__file__)), "..", "tools"))
import generate_string_constants as gen  # noqa: E402


def symbol(text, const_ref=False):
    """The demangled symbol remote_fmt::catalog<> gets for a string, as llvm-cxxfilt prints it."""
    chars = ", ".join(
        f"(char){b if b < 128 else b - 256}" for b in text.encode("utf-8"))
    ref = " const&" if const_ref else ""
    return f"unsigned short remote_fmt::catalog<sc::StringConstant<{chars}>{ref}>()"


def log_line(file, line, text, level=2, func="f", module=None):
    """A log format string as UC_LOG_DETAIL_FMT builds it (in a module scope with module)."""
    field = f'"{module}", ' if module else ""
    return f'("{file}", {line}, {level}, {{}}, {field}"""{func}"""){text}'.encode()


def quiet():
    return contextlib.redirect_stderr(io.StringIO())


class ParseTests(unittest.TestCase):
    def test_ascii(self):
        self.assertEqual(gen.parse_symbol(symbol("Test {}")), b"Test {}")

    def test_negative_chars_are_utf8_bytes(self):
        self.assertEqual(gen.parse_symbol(symbol("℃")), "℃".encode("utf-8"))

    def test_const_ref_symbol_parses_to_the_same_text(self):
        self.assertEqual(gen.parse_symbol(
            symbol("abc", const_ref=True)), b"abc")

    def test_malformed_character_code(self):
        with quiet():
            self.assertIsNone(gen.parse_symbol(
                "unsigned short remote_fmt::catalog<sc::StringConstant<(char)65, (char)x>>()"))


class KeyTests(unittest.TestCase):
    def test_line_number_is_left_out(self):
        self.assertEqual(gen.id_key(log_line("main.cpp", 79, "hello")),
                         b'("main.cpp", 2, {}, """f""")hello')

    def test_moved_line_keeps_its_key(self):
        self.assertEqual(gen.id_key(log_line("main.cpp", 10, "x {}")),
                         gen.id_key(log_line("main.cpp", 4711, "x {}")))

    def test_file_level_and_function_stay_in_the_key(self):
        base = gen.id_key(log_line("a.cpp", 1, "x"))
        self.assertNotEqual(base, gen.id_key(log_line("b.cpp", 1, "x")))
        self.assertNotEqual(base, gen.id_key(
            log_line("a.cpp", 1, "x", level=3)))
        self.assertNotEqual(base, gen.id_key(
            log_line("a.cpp", 1, "x", func="g")))

    def test_module_line_loses_only_its_line(self):
        self.assertEqual(gen.id_key(log_line("usb.cpp", 7, "stall", module="usb")),
                         b'("usb.cpp", 2, {}, "usb", """f""")stall')
        self.assertEqual(gen.id_key(log_line("usb.cpp", 7, "x", module="usb")),
                         gen.id_key(log_line("usb.cpp", 900, "x", module="usb")))

    def test_module_is_part_of_the_key(self):
        self.assertNotEqual(gen.id_key(log_line("a.cpp", 1, "x", module="usb")),
                            gen.id_key(log_line("a.cpp", 1, "x")))
        self.assertNotEqual(gen.id_key(log_line("a.cpp", 1, "x", module="usb")),
                            gen.id_key(log_line("a.cpp", 1, "x", module="i2c")))

    def test_other_strings_are_their_own_key(self):
        for text in (b"", b"on", b"(not a location)", b'("f", x, 1'):
            self.assertEqual(gen.id_key(text), text)

    def test_only_the_leading_location_is_touched(self):
        text = log_line("a.cpp", 5, '("b.cpp", 6, inside')
        self.assertEqual(gen.id_key(
            text), b'("a.cpp", 2, {}, """f""")("b.cpp", 6, inside')


class HashTests(unittest.TestCase):
    # FNV-1a 32 of "" is its offset basis, of "a" 0xE40C292C (the published test vectors)
    def test_known_vectors(self):
        self.assertEqual(gen.preferred_id(b""), (0x811C ^ 0x9DC5) & 0x7FFF)
        self.assertEqual(gen.preferred_id(b"a"), (0xE40C ^ 0x292C) & 0x7FFF)

    def test_range(self):
        for i in range(1000):
            self.assertLess(gen.preferred_id(str(i).encode()), gen.ID_COUNT)


class AssignTests(unittest.TestCase):
    def texts(self, n, seed=1):
        rng = random.Random(seed)
        return [log_line(f"file{rng.randrange(8)}.cpp", rng.randrange(1, 900),
                         f"message {i} {rng.random()}") for i in range(n)]

    def test_unique_ids(self):
        ids = gen.assign_ids(self.texts(300))
        self.assertEqual(len(set(ids.values())), 300)

    def test_input_order_does_not_matter(self):
        texts = self.texts(300)
        shuffled = list(texts)
        random.Random(7).shuffle(shuffled)
        self.assertEqual(gen.assign_ids(texts), gen.assign_ids(shuffled))

    def test_a_new_string_changes_no_other_id(self):
        texts = self.texts(300)
        before = gen.assign_ids(texts)
        after = gen.assign_ids(
            texts + [log_line("file3.cpp", 42, "a new line")])
        self.assertEqual({t: after[t] for t in before}, before)

    def test_a_colliding_new_string_moves_only_its_cluster(self):
        # the price of ids that depend on the strings alone: C lands on A's slot and pushes B,
        # which sat right behind it; D, one free slot further on, keeps its id
        slots = {b"A": 100, b"B": 101, b"C": 100, b"D": 103}
        with mock.patch.object(gen, "preferred_id", side_effect=slots.__getitem__):
            before = gen.assign_ids([b"A", b"B", b"D"])
            after = gen.assign_ids([b"A", b"B", b"C", b"D"])
        self.assertEqual(before, {b"A": 100, b"B": 101, b"D": 103})
        self.assertEqual(after, {b"A": 100, b"C": 101, b"B": 102, b"D": 103})

    def test_collisions_take_the_next_slots_in_key_order(self):
        texts = [b"c", b"a", b"b"]
        with mock.patch.object(gen, "preferred_id", return_value=100):
            ids = gen.assign_ids(texts)
        self.assertEqual(ids, {b"a": 100, b"b": 101, b"c": 102})

    def test_identical_lines_sit_in_neighbouring_slots_ordered_by_line(self):
        # 9 before 10: as numbers, not as the bytes b"9" > b"1"
        first = log_line("b.cpp", 9, "SKIP")
        second = log_line("b.cpp", 10, "SKIP")
        third = log_line("b.cpp", 100, "SKIP")
        ids = gen.assign_ids([third, second, first])
        self.assertEqual(ids[second], (ids[first] + 1) % gen.ID_COUNT)
        self.assertEqual(ids[third], (ids[first] + 2) % gen.ID_COUNT)

    def test_probing_wraps_to_zero(self):
        with mock.patch.object(gen, "preferred_id", return_value=gen.ID_COUNT - 1):
            ids = gen.assign_ids([b"x", b"y"])
        self.assertEqual(ids, {b"x": gen.ID_COUNT - 1, b"y": 0})

    def test_too_many_strings(self):
        with self.assertRaises(ValueError):
            gen.assign_ids([str(i).encode() for i in range(gen.ID_COUNT + 1)])

    def test_a_full_catalog_fits(self):
        # probing must find the last free slot (16 slots: a clustered 65536 is quadratic)
        with mock.patch.object(gen, "ID_COUNT", 16), \
                mock.patch.object(gen, "preferred_id", side_effect=lambda k: 14 + len(k) % 2):
            ids = gen.assign_ids([str(i).encode() for i in range(16)])
        self.assertEqual(sorted(ids.values()), list(range(16)))


def block_symbol(names):
    """The demangled symbol remote_fmt::catalog_block<> gets for an enum's names."""
    joined = "\0".join(names)
    chars = ", ".join(f"(char){b}" for b in joined.encode("utf-8"))
    return f"unsigned short remote_fmt::catalog_block<sc::StringConstant<{chars}>>()"


class BlockTests(unittest.TestCase):
    NAMES = [b"IDLE", b"RUN", b"FAULT", b"OFF"]
    JOINED = b"\0".join(NAMES)

    def test_a_block_symbol_parses_to_the_joined_names(self):
        sym = block_symbol([n.decode() for n in self.NAMES])
        self.assertTrue(gen.is_block(sym))
        self.assertEqual(gen.parse_symbol(sym), self.JOINED)
        self.assertEqual(gen.block_names(self.JOINED), self.NAMES)

    def test_blocks_and_single_strings_are_grouped_apart(self):
        syms = [symbol("hello"), block_symbol(["A", "B"])]
        self.assertEqual(list(gen.group_texts(syms)), [b"hello"])
        self.assertEqual(list(gen.group_texts(syms, blocks=True)), [b"A\0B"])

    def test_a_block_gets_consecutive_ids(self):
        ids, bases = gen.assign_all({}, {self.JOINED: []})
        base = bases[self.JOINED]
        self.assertEqual(
            base, min(gen.preferred_id(self.JOINED), gen.ID_COUNT - 4))
        self.assertEqual(ids, {})

    def test_single_strings_stay_out_of_a_block(self):
        base = gen.assign_all({}, {self.JOINED: []})[1][self.JOINED]
        # strings whose preferred slot lies inside the block's run
        inside = []
        for i in range(200000):
            t = f"s{i}".encode()
            if base <= gen.preferred_id(t) < base + len(self.NAMES):
                inside.append(t)
            if len(inside) == 3:
                break
        ids, bases = gen.assign_all({t: [] for t in inside}, {self.JOINED: []})
        self.assertEqual(bases[self.JOINED], base, "the block keeps its place")
        run = set(range(base, base + len(self.NAMES)))
        self.assertFalse(run & set(ids.values()))
        self.assertEqual(len(set(ids.values())), len(inside))

    def test_an_unrelated_new_string_moves_no_block(self):
        blocks = {self.JOINED: [], b"X\0Y\0Z": []}
        texts = {b"one": [], b"two": []}
        before = gen.assign_all(texts, blocks)
        after = gen.assign_all({**texts, b"three": []}, blocks)
        self.assertEqual(before[1], after[1])
        self.assertEqual(before[0][b"one"], after[0][b"one"])
        self.assertEqual(before[0][b"two"], after[0][b"two"])

    def test_two_blocks_do_not_overlap(self):
        a, b = self.JOINED, b"\0".join(f"N{i}".encode() for i in range(40))
        bases = gen.assign_all({}, {a: [], b: []})[1]
        ra = set(range(bases[a], bases[a] + 4))
        rb = set(range(bases[b], bases[b] + 40))
        self.assertFalse(ra & rb)
        self.assertLessEqual(bases[b] + 40, gen.ID_COUNT)

    def test_a_block_counts_against_the_capacity(self):
        big = b"\0".join(f"N{i}".encode() for i in range(gen.ID_COUNT))
        with self.assertRaises(ValueError):
            gen.assign_all({b"one more": []}, {big: []})

    def test_the_catalog_has_one_entry_per_name(self):
        sym = block_symbol([n.decode() for n in self.NAMES])
        blocks = {self.JOINED: [sym]}
        ids, bases = gen.assign_all({b"hi": [symbol("hi")]}, blocks)
        with tempfile.TemporaryDirectory() as d:
            cpp = gen.write_outputs(
                ids, {b"hi": [symbol("hi")]}, d, "t", bases, blocks)
            entries = json.load(open(os.path.join(d, "t_string_constants.json")))[
                "StringConstants"]
            source = open(cpp).read()
        base = bases[self.JOINED]
        for i, n in enumerate(self.NAMES):
            self.assertIn([base + i, n.decode()], entries)
        self.assertIn(f"template<>{sym}{{return {base};}}", source)
        self.assertEqual([e[0] for e in entries], sorted(e[0]
                         for e in entries))


class GroupTests(unittest.TestCase):
    def test_format_string_and_argument_share_one_text(self):
        texts = gen.group_texts(
            [symbol("on"), symbol("on", const_ref=True), symbol("off")])
        self.assertEqual(set(texts), {b"on", b"off"})
        self.assertEqual(len(texts[b"on"]), 2)

    def test_duplicate_symbols_are_merged(self):
        texts = gen.group_texts([symbol("x"), symbol("x")])
        self.assertEqual(texts, {b"x": [symbol("x")]})

    def test_a_bad_symbol_gets_no_id(self):
        bad = "unsigned short remote_fmt::catalog<sc::StringConstant<(char)65, (char)?>>()"
        with quiet():
            texts = gen.group_texts([bad, symbol("good")])
        self.assertEqual(list(texts), [b"good"])


class Utf8Tests(unittest.TestCase):
    def test_utf8_passes(self):
        self.assertEqual(gen.invalid_utf8(["m℃".encode(), b"ascii"]), [])

    def test_latin1_is_named(self):
        self.assertEqual(gen.invalid_utf8(
            [b"ok", "°C".encode("latin-1")]), [b"\xb0C"])

    def test_main_stops_before_writing(self):
        with tempfile.TemporaryDirectory() as d, quiet() as err, \
                mock.patch.object(gen, "collect_symbols", return_value=[symbol("ok")]), \
                mock.patch.object(gen, "parse_symbol", return_value=b"\xb0C"), \
                mock.patch.object(gen.shutil, "which", return_value="/bin/true"):
            with self.assertRaises(SystemExit) as exit:
                gen.main(["--target_name", "t", "--out_dir", d, "--source_dir", d,
                          "--compiler", "c", "--nm", "n", "--objects", "o"])
            self.assertEqual(exit.exception.code, 1)
            self.assertIn("not UTF-8: b'\\xb0C'", err.getvalue())
            self.assertEqual(os.listdir(d), [])


class CxxfiltTests(unittest.TestCase):
    def tool(self, d, name):
        path = os.path.join(d, name)
        with open(path, "w") as f:
            f.write("#!/bin/sh\n")
        os.chmod(path, 0o755)
        return path

    def test_versioned_sibling(self):
        with tempfile.TemporaryDirectory() as d:
            nm = self.tool(d, "llvm-nm-19")
            self.tool(d, "llvm-cxxfilt")
            want = self.tool(d, "llvm-cxxfilt-19")
            self.assertEqual(gen.find_cxxfilt(nm), want)

    def test_plain_sibling_of_another_nm(self):
        with tempfile.TemporaryDirectory() as d:
            nm = self.tool(d, "arm-none-eabi-nm")
            want = self.tool(d, "llvm-cxxfilt")
            self.assertEqual(gen.find_cxxfilt(nm), want)

    def test_through_a_symlink(self):
        with tempfile.TemporaryDirectory() as d:
            real = os.path.join(d, "toolchain")
            os.mkdir(real)
            want = self.tool(real, "llvm-cxxfilt")
            link = os.path.join(d, "llvm-nm")
            os.symlink(self.tool(real, "llvm-nm"), link)
            self.assertEqual(gen.find_cxxfilt(link), want)

    def test_path_is_the_fallback(self):
        with tempfile.TemporaryDirectory() as d:
            nm = self.tool(d, "llvm-nm")
            self.assertEqual(gen.find_cxxfilt(
                nm), shutil.which("llvm-cxxfilt"))


CXX = os.environ.get("RF_TEST_CXX", "")
NM = os.environ.get("RF_TEST_NM", "")
INCLUDES = [p for p in os.environ.get(
    "RF_TEST_INCLUDES", "").split(os.pathsep) if p]


@unittest.skipUnless(CXX and NM and INCLUDES and shutil.which(CXX) and shutil.which(NM),
                     "RF_TEST_CXX, RF_TEST_NM and RF_TEST_INCLUDES not set")
class EndToEndTests(unittest.TestCase):
    SOURCE = """
#include <remote_fmt/catalog.hpp>
#include <string_constant/string_constant.hpp>
#include <cstdint>
#include <type_traits>
using namespace sc::literals;
static constexpr auto fmtString{"value {} \\u2103"_sc};
static constexpr auto argString{"on"_sc};
std::uint16_t use();
std::uint16_t use() {
    return static_cast<std::uint16_t>(
        remote_fmt::catalog<std::remove_cvref_t<decltype(fmtString)>>()
        + remote_fmt::catalog<std::remove_cvref_t<decltype(argString)>>()
        + remote_fmt::catalog<std::remove_cvref_t<decltype(argString)> const&>());
}
"""

    def test_object_to_catalog(self):
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "use.cpp")
            obj = os.path.join(d, "use.o")
            with open(src, "w") as f:
                f.write(self.SOURCE)
            subprocess.run([CXX, "-std=c++20", "-c", src, "-o", obj]
                           + [f"-I{p}" for p in INCLUDES], check=True)

            # --source_dir is remote_fmt's root, the parent of INCLUDES[0]
            source_dir = os.path.dirname(os.path.normpath(INCLUDES[0]))
            out = os.path.join(d, "out")
            gen.main(["--target_name", "t", "--out_dir", out, "--source_dir", source_dir,
                      "--compiler", CXX, "--nm", NM, "--objects", obj])

            self.assertTrue(os.path.exists(
                os.path.join(out, "t_string_constants.obj")))
            with open(os.path.join(out, "t_string_constants.json")) as f:
                catalog = {text: id for id, text in json.load(f)[
                    "StringConstants"]}
            self.assertEqual(set(catalog), {"value {} ℃", "on"})

            with open(os.path.join(out, "t_string_constants.cpp")) as f:
                cpp = f.read()
            returns = [int(n) for n in re.findall(r"\{return (\d+);\}", cpp)]
            # three definitions: the shared text once per symbol, with the same id
            self.assertEqual(sorted(returns),
                             sorted([catalog["value {} ℃"], catalog["on"], catalog["on"]]))
            self.assertIn("const&>(){return " + str(catalog["on"]), cpp)

            # the object defines every symbol the user's object left undefined
            undefined = subprocess.run([NM, "-u", obj], check=True, capture_output=True,
                                       text=True).stdout.split()
            defined = subprocess.run([NM, "--defined-only",
                                      os.path.join(out, "t_string_constants.obj")],
                                     check=True, capture_output=True, text=True).stdout
            wanted = [s for s in undefined if "catalog" in s]
            self.assertEqual(len(wanted), 3)
            for s in wanted:
                self.assertIn(s, defined)


if __name__ == "__main__":
    unittest.main()
