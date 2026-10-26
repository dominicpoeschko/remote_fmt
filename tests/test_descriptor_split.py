"""tools/descriptor.py against the C++ descriptors: `python3 tests/test_descriptor_split.py <descriptor_cases>...`.
Each program prints `<argument count>\t<described string as hex>\t<the C++ walker's split>` per line
(descriptor_cases.cpp, built with the catalog on and off); every string must split into exactly that many complete
descriptors that join back to it, and into the same ones the C++ walker (descriptor_grammar.hpp grammar::take) finds.
"""

import os
import subprocess
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.dont_write_bytecode = True  # no __pycache__ next to the imported tools
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import descriptor as ds  # noqa: E402

PROGRAMS = []


class SplitMatchesCxx(unittest.TestCase):
    def test_every_case_splits_into_its_arguments(self):
        self.assertTrue(PROGRAMS, "no descriptor_cases program given")
        for program in PROGRAMS:
            out = subprocess.run([program], check=True,
                                 capture_output=True, text=True).stdout
            lines = out.splitlines()
            self.assertGreater(
                len(lines), 10, f"{program} printed {len(lines)} cases")
            kinds = set()
            for line in lines:
                count, text, cxx = line.split("\t")
                text = bytes.fromhex(text)
                with self.subTest(program=os.path.basename(program), text=text):
                    entry = ds.catalog_entry(1, text)
                    self.assertEqual(entry["fmt"], "f")
                    self.assertEqual(
                        len(entry["args"]), int(count), entry["args"])
                    self.assertEqual([arg.encode() for arg in entry["args"]], cxx_pieces(
                        cxx), "C++ split differs")
                    self.assertEqual(ds.entry_text(entry), text)
                    for arg in entry["args"]:
                        self.assertEqual(ds.split_arguments(
                            arg.encode()), [arg.encode()])
                        kinds.add(arg[0])
            # the grammar's kinds the cases reach (Q and Y only where mp-units / styled are built in; R, r: SDK)
            self.assertEqual(set("bcuifpskdOXVLSMA(F*") - kinds,
                             set(), f"kinds not reached, reached {kinds}")
            self.assertTrue("#" in kinds or {
                            "E", "G"} <= kinds, f"no enum (type key or inline), reached {kinds}")


def cxx_pieces(column):
    """The C++ walker's split as printed by descriptor_cases: hex pieces joined by ','; None for "error"."""
    if column == "error":
        return None
    return [bytes.fromhex(piece) for piece in column.split(",")] if column else []


def cxx_split(texts):
    """`descriptor_cases split`: the C++ walker's split of each run of descriptors (hex lines on stdin)."""
    out = subprocess.run([PROGRAMS[0], "split"], input="".join(text.hex() + "\n" for text in texts), check=True,
                         capture_output=True, text=True).stdout
    return [cxx_pieces(line) for line in out.split("\n")[:len(texts)]]


class EnumeratorValues(unittest.TestCase):
    """An enumerator's value as parser.hpp's grammar takes it: unsigned up to 2^64-1, or '-' and down to -2^63 (the
    same strings as roundtrip_tests.cpp's enumeratorValues). Past that the parser cannot read the descriptor, so the
    link must fail here."""

    EDGES = (b"Gu82:1,2:lo9223372036854775808,2:hi", b"Eu81:18446744073709551615,3:max",
             b"Ei81:-9223372036854775808,3:min", b"Ei22:-3,3:low3,4:high")
    PAST = (b"Eu81:18446744073709551616,3:max",
            b"Ei81:-9223372036854775809,3:min")

    def test_the_edges_split(self):
        for text, cxx in zip(self.EDGES, cxx_split(self.EDGES)):
            with self.subTest(text=text):
                self.assertEqual(ds.split_arguments(text), [text])
                self.assertEqual(cxx, [text], "the C++ walker")

    def test_past_the_edges_fails(self):
        for text, cxx in zip(self.PAST, cxx_split(self.PAST)):
            with self.subTest(text=text):
                with self.assertRaises(ds.Error):
                    ds.split_arguments(text)
                self.assertIsNone(cxx, "the C++ walker")


class Depth(unittest.TestCase):
    """Nesting up to MaxDepth (64) levels below the argument splits, one more does not - in both walkers."""

    def test_the_deepest_splits_and_one_more_fails(self):
        deepest, deeper = b"O" * 64 + b"u1", b"O" * 65 + b"u1"
        cxx_deepest, cxx_deeper = cxx_split((deepest, deeper))
        self.assertEqual(ds.split_arguments(deepest), [deepest])
        self.assertEqual(cxx_deepest, [deepest], "the C++ walker")
        with self.assertRaises(ds.Error):
            ds.split_arguments(deeper)
        self.assertIsNone(cxx_deeper, "the C++ walker")


if __name__ == "__main__":
    PROGRAMS[:] = sys.argv[1:]
    unittest.main(argv=sys.argv[:1])
