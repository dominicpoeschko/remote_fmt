"""descriptor.hpp's grammar for the build tools, and the catalog entry it shapes.

A cataloged string is `fmt 0x1e descriptors` (remote_fmt::described<Args...>(fmt)); the catalog json lists it as
`{"id": <id>, "fmt": <fmt>, "args": [<descriptor>, ...]}` - one complete descriptor per argument, in argument order
(a call site's time stamp first; `"args": []` without arguments). A string without 0x1e is no cataloged string: an
Error, as a descriptor that does not split. The walk mirrors the host's `remote_fmt::detail::grammar::walk`
(descriptor_grammar.hpp; tests/test_descriptor_split.py holds the two to the same split): a descriptor this cannot
split is an Error, which fails the link. generate_string_constants.py (before the link) and extract_sites.py (after
it) both write entries through `catalog_entry`; both list this file in LINK_DEPENDS.
"""

from rf_symbols import Error

SEPARATOR = b"\x1e"   # remote_fmt::desc::separator
MAX_DEPTH = 64        # remote_fmt::detail::grammar::MaxDepth
UINT64_MAX = (1 << 64) - 1  # grammar::number refuses more


class Descriptor:
    """A walk over descriptor bytes: `one()` skips one descriptor, `refs` collects the type keys met."""

    def __init__(self, text):
        self.d = text
        self.i = 0
        self.refs = []

    def fail(self, what):
        raise Error(f"descriptor {self.d[:120]!r}: {what} at {self.i}")

    def peek(self):
        return self.d[self.i:self.i + 1]

    def take(self, c):
        if self.peek() != c:
            self.fail(f"{c!r} expected")
        self.i += 1

    def number(self):
        start = self.i
        while self.peek().isdigit():
            self.i += 1
        if self.i == start:
            self.fail("a number expected")
        value = int(self.d[start:self.i])
        if value > UINT64_MAX:
            self.fail("a number past 64 bits")
        return value

    def enumerator_value(self):
        """grammar::enumeratorValue: unsigned up to 2^64 - 1, or '-' and down to -2^63."""
        negative = self.peek() == b"-"
        if negative:
            self.i += 1
        magnitude = self.number()
        if negative and magnitude > 1 << 63:
            self.fail("an enumerator below -2^63")
        return -magnitude if negative else magnitude

    def number_colon(self):
        n = self.number()
        self.take(b":")
        return n

    def counted(self):
        n = self.number_colon()
        text = self.d[self.i:self.i + n]
        if len(text) != n:
            self.fail("a counted text runs past the end")
        self.i += n
        return text

    def names(self):
        """`<k>:` then k entries `<value>,<n>:<name>`"""
        for _ in range(self.number_colon()):
            self.enumerator_value()
            self.take(b",")
            self.counted()

    def one(self, depth=0):
        if depth > MAX_DEPTH:
            self.fail("nested too deep")
        tag = self.peek()
        if not tag:
            self.fail("a descriptor expected")
        self.i += 1
        if tag in (b"b", b"c", b"s", b"v", b"*"):
            return
        if tag in (b"u", b"i", b"f", b"p"):
            if self.peek() not in (b"1", b"2", b"4", b"8"):
                self.fail("a size expected")
            self.i += 1
        elif tag == b"k":
            self.counted()
        elif tag == b"d":
            self.number()
            self.take(b"/")
            self.number()
            self.one(depth + 1)
        elif tag in (b"O", b"L", b"S", b"M", b"A", b"Y"):
            self.one(depth + 1)
        elif tag == b"X":
            self.one(depth + 1)
            self.one(depth + 1)
        elif tag == b"V":
            for _ in range(self.number_colon()):
                self.one(depth + 1)
        elif tag == b"(":
            while self.peek() != b")":
                self.one(depth + 1)
            self.i += 1
        elif tag == b"Q":
            self.counted()
            self.one(depth + 1)
        elif tag == b"F":
            self.counted()
            for _ in range(self.number_colon()):
                self.one(depth + 1)
        elif tag in (b"E", b"G"):
            self.one(depth + 1)
            self.names()
        elif tag == b"R":
            self.one(depth + 1)
            self.counted()
            for _ in range(self.number_colon()):
                self.number()
                self.take(b",")
            for _ in range(self.number_colon()):
                self.number()
                self.take(b",")
                self.names()
        elif tag == b"r":
            self.one(depth + 1)
            for _ in range(self.number_colon()):
                self.number()
                self.take(b",")
                self.counted()
        elif tag == b"#":
            key = self.d[self.i:self.i + 16]
            if len(key) != 16:
                self.fail("a type key expected")
            # any 16 bytes, as grammar::walk takes them; one that is no key matches no "Types" entry (an Error there)
            self.refs.append(key.decode("latin-1"))
            self.i += 16
        else:
            self.i -= 1
            self.fail(f"unknown descriptor {tag!r}")

    def split(self):
        """The descriptors one after the other, each as its bytes."""
        parts = []
        while self.i < len(self.d):
            start = self.i
            self.one()
            parts.append(self.d[start:self.i])
        return parts


def split_arguments(descriptors):
    """The bytes after 0x1e cut into one complete descriptor per argument; an Error if they do not parse."""
    return Descriptor(descriptors).split()


def type_refs(descriptor):
    """The type keys one descriptor (or a run of them) refers to."""
    walk = Descriptor(descriptor)
    walk.split()
    return walk.refs


def split_described(text):
    """(fmt bytes, [argument descriptor bytes]) of a cataloged string; an Error for a string without 0x1e."""
    at = text.find(SEPARATOR)
    if at < 0:
        raise Error(f"string {text[:120]!r} has no descriptors: a cataloged string is remote_fmt::described"
                    f"<Args...>(fmt), the format string, 0x1e and one descriptor per argument")
    return text[:at], split_arguments(text[at + 1:])


def catalog_entry(string_id, text):
    """The catalog json's entry for a cataloged string (bytes): {"id", "fmt", "args"}."""
    fmt, args = split_described(text)
    return {"id": string_id, "fmt": fmt.decode("utf-8"), "args": [arg.decode("utf-8") for arg in args]}


def entry_text(entry):
    """The cataloged string of an entry as bytes, `fmt 0x1e descriptors` (the inverse of catalog_entry)."""
    return entry["fmt"].encode("utf-8") + SEPARATOR + b"".join(arg.encode("utf-8") for arg in entry["args"])
