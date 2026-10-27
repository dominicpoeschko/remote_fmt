# Remote FMT
Remote FMT is a C++ library to provide remote functionality to FMT. It is suitable for remote and memory efficient logging. The FMT-strings can be stored in a string catalog and are not transmitted in the logging process. This saves communication bandwidth on the communication interface.

## Usage
To use this library, you can include it into your existing project by using `git submodules` and adding the following lines to your `CMakeLists.txt`:
```cmake
add_subdirectory(remote_fmt)
target_link_libraries(
    ${target_name} 
    remote_fmt::remote_fmt
    remote_fmt::parser
    )
```
If you want to use the FetchContent feature of CMake to include the library to your project use the following lines in your `CMakeLists.txt`:
```cmake
include(FetchContent)
FetchContent_Declare(
    remote_fmt
    GIT_REPOSITORY https://github.com/dominicpoeschko/remote_fmt.git
    GIT_TAG master
)
FetchContent_MakeAvailable(remote_fmt)
target_link_libraries(
    ${target_name} 
    remote_fmt::remote_fmt
    remote_fmt::parser
    )
```

### Layers

- `wire.hpp`: what both ends agree on - the frame's markers and CRC-8, the characters a string may hold, the bounds of
  a replacement field, `FrameCheck`. The standard library only.
- `catalog.hpp`, `descriptor.hpp`, `format_grammar.hpp`, `config.hpp`: ids and call sites, the descriptors, the one
  format-string grammar, the image-wide switches (std and string_constant).
- The encoder, `remote_fmt.hpp` (target and host): `Printer`, the formatters, the format-string check (fmt's checker,
  enchantum, mp-units as the switches say).
- The decoder (`remote_fmt::parser`, host): `descriptor_grammar.hpp` (reading a descriptor, std only), `parser.hpp`
  (`parseMessage`, fmt and enchantum) and `catalog_helpers.hpp` (the catalog json, glaze). None of them includes the
  encoder: a reader decodes every kind - a styled value, an enum, a quantity - whatever switches the image was built
  with (`tests/config_switch_tests.cpp` decodes them in every switch build).

### Examples
To build the examples go into the [examples](examples) folder and run the following commands:

```bash
mkdir build
cd build
cmake ..
make
```

#### Basic Example
The basic usage is covered in the [examples/basic_usage.cpp](examples/basic_usage.cpp) example.
This example does not explain the catalog feature set.

The example features a communication backend class which should provide at least the following functions:
```c++
struct CommunicationBackend{
    void write(std::span<std::byte const> s){}
};
```

After initialization the Remote FMT `printer` can be used to print messages through the communication backend.
```c++
printer.print("Test {}"_sc, 123);
```

The `buffer` on the remote device can be parsed with the `remote_fmt::parseMessage(...)` function to print the transmitted format string: it gives the text (`message`), the bytes after the frame (`remaining`), the bytes skipped before it (`discarded`) and the frame's catalog id. The required catalog of this function is empty in this example. Problems go to the error callback, `(text)` or `(text, remote_fmt::ParseError)`: `format` for a whole frame whose text the host could not render (consumed, the line lost to its format string), `stream` for bytes that make no frame.

#### Catalog Example

> Note: The catalog example only shows the behaviour of the catalog system. If you want to use the catalog feature in your project refer to the Catalog Generator Example below!

This example covers the catalog functionality in [examples/catalog.cpp](examples/catalog.cpp).
Most of the example is the same as the basic example except of the catalog on the top of the example.
This catalog code can be generated in the toolchain with a python script.

Another difference is the function call of the `remote_fmt::parseMessage(...)` function. In this example the function is called with a catalog.

#### Call sites

`catalog<String>()` is the id of a string (0x0000-0x7FFF), defined by the build before the link
([tools/generate_string_constants.py](tools/generate_string_constants.py)) - to LTO a constant the compiler folds into
immediates and tables.

`REMOTE_FMT_SITE()` gives a string an id of its call site instead (0x8000-0xFFFF), for a string of which the host wants
to know where it was printed:

```c++
auto const site = REMOTE_FMT_SITE();
printer.print(remote_fmt::SiteId{site(remote_fmt::described<Args...>(fmt))}, fmt, args...);
```

The site names the *described* string - the format string, `0x1e` and its arguments' descriptors (below) - because
that is what the host reads the values with.

The id is 0x8000 plus the address of a one-byte tag, one per call site and per instantiation of the function it is in,
whose symbol names the string and that function. After the link [tools/extract_sites.py](tools/extract_sites.py) adds
each site's string and the demangled signature of its function to the json (`"Sites": {"<id>": "<signature>"}`), and
`remote_fmt::parseMessage(...)` hands the id of a message back with it. Nothing is read by the constant evaluator for it.
The tags live in sections `remote_fmt_sites.<n>`; a target's linker script keeps them out of the image at address 0:

```
remote_fmt_sites 0 (INFO) : { KEEP(*(remote_fmt_sites remote_fmt_sites.*)) }
```

A host executable gets [cmake/remote_fmt_sites_host.ld](cmake/remote_fmt_sites_host.ld) from
`target_generate_string_constants`. A site id is an address, no constant to the compiler: a plain string keeps
`catalog<>`. uc_log gives every log line its function this way.

#### Catalog Generator Example
In this example the usage of the catalog generator is covered in [examples/catalog_generator.cpp](examples/catalog_generator.cpp).

To use the catalog generator you must have installed python on your computer. The generator can be called in the `CMakeLists.txt` of your project with the following function:

```cmake
target_generate_string_constants(${target_name})
```

A string's id is a hash of the whole string. `ID_KEY_STRIP <regex>` leaves a part of each string out of it: the regex
is matched at the start of a string, its group 1 is not hashed, and the first number in group 1 orders strings that then
share a key. uc_log leaves the line number of its log lines out that way (its `UC_LOG_ID_KEY_STRIP`), so a line that
moves keeps its id:

```cmake
target_generate_string_constants(${target_name} ID_KEY_STRIP [[^\("[^"]*", ([0-9]+, )]])
```

All static strings used in `print` functions are automatically stored in the catalog:

```c++
printer.print("Test {}"_sc, 123);
```

##### Generated Files
Before the link, [tools/generate_string_constants.py](tools/generate_string_constants.py) writes
`${target_name}_string_constants.cpp`, the generated catalog (it builds it into the `.obj` the link takes), and
`${target_name}_string_constants.prelink.json`, the strings it found. After the link,
[tools/extract_sites.py](tools/extract_sites.py) adds the image's call sites, its `FrameCheck` and its type entries and
writes `${target_name}_string_constants.json` - once, whole, by renaming a finished file over the old one: a reader
sees the previous catalog or the new one, never half of one, and a failed step leaves the previous one. It holds the
catalog in a `json` notation, one entry per string, the described string split up
([tools/descriptor.py](tools/descriptor.py), the C++ grammar in Python; a string without descriptors or a descriptor it
cannot split fails the build):

```json
{
  "StringConstants": [
    {"id": 32818, "fmt": "(\"main.cpp\", 113, 2, {}[1/150000000]s, \"\"\"\"\"\")tick {} at {}, mood {} ({})",
     "args": ["i8", "u4", "d1/1i8", "#7c01d350804748af", "s"]},
    {"id": 5895, "fmt": " (+{} not logged)", "args": ["u4"]}
  ],
  "Sites": {"32818": "void app::tick()"},
  "FrameCheck": true,
  "Types": {"7c01d350804748af": "Eu13:0,4:calm1,7:curious2,7:excited"}
}
```

`args` holds one complete descriptor per argument, in argument order (a call site's time stamp first; `"args": []`
without arguments). The image still carries `fmt` 0x1e `descriptors` - the json only spells it out.
`catalog_helpers.hpp` reads it, with [glaze](https://github.com/stephenberry/glaze) (a dependency of
`remote_fmt::parser`): `readCatalogFile` gives the strings (with their type references resolved, see "Descriptors"),
the call sites, the image's `FrameCheck` and its type entries. Every key above is required, and so is each entry's
`args`; a missing one, or a type reference without its entry, is an error that names it:

```c++
#include "remote_fmt/catalog_helpers.hpp"
auto const catalog = remote_fmt::readCatalogFile("path/to/catalog.json");   // std::expected<CatalogFile, std::string>
// catalog->strings: the id -> string map parseMessage takes
```

## The wire

```
start | id (2 bytes, little endian) | the values | [CRC-8] | 0xAA
```

* No type bytes. The string behind the id is `fmt` 0x1e `descriptors`: the descriptor of every value, built at compile
  time from the formatters (`src/remote_fmt/descriptor.hpp`), so only data goes out. The id is a call site's
  (0x8000-0xFFFE), a cataloged string's (0x0000-0x7FFF) or `0xFFFF`: the described string follows inline (a LEB128
  length and its bytes; `REMOTE_FMT_USE_CATALOG=false`).
* 32-bit integers go out as LEB128 (zigzag for signed ones), integers of 1, 2 and 8 bytes (the time stamp too), floats
  and pointers raw, string and range lengths as LEB128, a contiguous range of
  numbers as one copy of its memory. An enum, a register and a `Register::Flags` go out as their raw value: the host
  names them from the descriptor.
* A frame starts with `0x57` and carries a CRC-8/AUTOSAR (one table load a byte, every byte once) before its end marker
  (`REMOTE_FMT_FRAME_CHECK`, default `1`): a damaged frame is dropped with a message. `0` writes unchecked frames that
  start with `0x58`. The image says which kind it writes (`"FrameCheck"` in the catalog).
* The parser takes a frame only at a start marker followed by an id it knows, walks the descriptor to the frame's exact
  end and checks the CRC and the end marker there; a frame whose text fmt refuses is consumed and reported.
* A message is gathered in `REMOTE_FMT_STAGING_BYTES` (default 32) bytes on the stack and handed to the backend in
  one `write`; a longer one goes out in pieces. `0` writes every field on its own.

### Descriptors

| text | type | on the wire |
|---|---|---|
| `b` `c` | bool, char | 1 byte |
| `u1` `u2` `u4` `u8`, `i1`..`i8` | integers | `u4`/`i4` LEB128 (zigzag for `i4`), the other sizes raw, little endian |
| `f4` `f8` / `p4` `p8` | float, double / `void*` | raw |
| `s` | string_view, char[N] | LEB128 length + bytes |
| `k<n>:<text>` | a string constant argument | nothing |
| `d<num>/<den><D>` | duration, time_point | the count as D |
| `O<D>` / `X<D><D>` / `v` | optional / expected / void | engaged byte + value |
| `V<n>:<D>...` | variant | index byte + the alternative |
| `L<D>` `S<D>` `M<D>` / `A<D>` | list, set, map / contiguous numbers | LEB128 size + elements / the raw elements |
| `(<D>...)` | tuple, pair | the elements |
| `Q<n>:<symbol><D>` | mp-units quantity | the number |
| `Y<D>` | fmt::styled | flag byte, colours, the value |
| `F<n>:<fmt><k>:<D>...` | a declared sub format string | its arguments |
| `E<D><k>:<v>,<n>:<name>...`, `G...` | enum, bitflag enum (enchantum) | the raw value |
| `R<D><n>:<fmt><d>:<dim>,...<f>:<mask>,<k>:<v>,<n>:<name>...` | register (svd_converter) | the raw word as D (`u<size>`) |
| `r<D><f>:<mask>,<n>:<name>...` | `Kvasir::Register::Flags<R>` | the raw word as D (`u<size>`) |
| `#<16 hex>` | a type entry of the catalog | as its text |
| `*` | self-described | the id of a described sub format string, then its values |

Free text is always counted (`<n>:<text>`). An enumerator's `<v>` is decimal, `-` before a negative one (an unsigned
one up to 2^64 - 1, a signed one down to -2^63). Enums, registers, Flags and reflected structs are **type entries**: with
a catalog a site's descriptor holds `#<FNV-1a 64 of the text>`, the text travels once per image in the name of a tag
(`remote_fmt::detail::REMOTE_FMT_TYPE_TAG<Text>`, section `remote_fmt_sites.types`), `tools/extract_sites.py` writes them to
`"Types"` (failing the link on a key two texts share or a reference without an entry) and the catalog reader puts the
text back in place. Without a catalog the text is inline.

The host reads a descriptor with one walker (`src/remote_fmt/descriptor_grammar.hpp`, `grammar::walk`): skipping one,
cutting a run into its descriptors (`take`) and resolving the type references are that walk with different children.
`tools/descriptor.py` mirrors it for the build tools, and ctest `test_descriptor_split` holds the two to the same split
of every case `tests/descriptor_cases.cpp` prints and of the edge strings (enumerator values, the deepest nesting).

The test suite builds the round-trip and catalog tests once per knob at its other value
(`test_roundtrip_<KNOB>_0`, `test_catalog_<KNOB>_0`), and each build checks that its knob took effect.

## Format string checking

Format strings are checked against their arguments at compile time, using FMT's own checker, so a
spec that does not match its argument is an error at the `print` call instead of a message that gets
dropped on the host:

```c++
printer.print("{:s}"_sc, 123);      // error: 's' is not a presentation type for an integer
printer.print("{:%Y}"_sc, 5ms);     // error: a duration has no date
printer.print("{:.1f}"_sc, 2.5);    // fine
```

The check costs nothing at run time - it emits no code - and adds a few percent to compile time.

Because the device only serializes and the host does the formatting, the spec is checked against the
type the *parser* reconstructs, not the type that was passed in. An enum travels as its enumerator
name, a pointer comes back as `void const*`, `std::byte` as `std::uint8_t`. That mapping lives in
`src/remote_fmt/fmt_check.hpp` as `remote_fmt::detail::host_type`. A type that only has a
`remote_fmt::formatter` and no FMT formatter - a wrapper of your own - is skipped rather than
rejected; specialize `host_type` for it to get its spec checked too:

```c++
namespace remote_fmt { namespace detail {
template<typename T>
struct HostType<MyWrapper<T>> {
    using type = host_type_t<T>;   // MyWrapper is transparent on the wire
};
}}
```

A formatter written with `format_to` is the case to watch. It sends a *sub format string*, which
the host renders to text before the outer field is applied, so only a spec FMT accepts on a string
can ever reach it - `{:>20}` works, `{:.2f}` cannot. Give such a type
`using type = std::string_view;` and the difference becomes a compile error at the `print` call
instead of a diagnostic on the host:

```c++
namespace remote_fmt { namespace detail {
template<>
struct HostType<Suppressed> {
    using type = std::string_view;   // Suppressed's formatter uses format_to
};
}}
```

Two rules come from the protocol rather than from FMT, which accepts both: an argument index
(`"{1} {0}"`) and a dynamic width or precision (`"{:{}}"`) are rejected, because each replacement
field is paired with exactly one argument, in order, and no argument index travels on the wire.

The target's check and the host's parser read a format string with one scanner
(`src/remote_fmt/format_grammar.hpp`): braces escape only outside a field, a field ends where its braces balance,
and a field with a brace inside is a dynamic spec, refused on both sides. `tests/scanner_equivalence_tests.cpp` walks
every string of up to 9 characters over `{ } : 0 x` through it.

## Writing a formatter

A `remote_fmt::formatter<T>` writes through the printer's public vocabulary, never wire bytes, in one of two ways:

* **declare nothing, write through `format_to(printer.out(), "{} of {}"_sc, a, b)`** (`out()` is the printer, as
  FMT's `ctx.out()`): the descriptor is `*`, and the sub format string goes out as the id of its described string -
  the shape may depend on the value (one `format_to` or another);
* **declare a descriptor** - `static consteval auto descriptor()` - and write exactly the values it names with
  `printer.value(x)` / `printer.formatArguments(xs...)`: `remote_fmt::descriptorOf<U>()` for "the same as U",
  `remote_fmt::subFormatDescriptor<Args...>(fmt)` for a fixed sub format string whose arguments alone travel,
  `remote_fmt::typeDescriptor<Text>()` for a type entry (it brings the entry's tag along).

The descriptor and the bytes `format` writes are two descriptions of one truth that the CRC does not cross-check (it
covers the bytes that were sent): keep them next to each other, and give a new formatter a round trip in the tests. The
raw fields behind the vocabulary are private (remote_fmt's own formatters reach them through `detail::Access`).

Knobs, both rarely needed:

* `REMOTE_FMT_USE_FMT_CHECK` - set to `0` to disable the check. Defaults to on when the FMT headers
  are reachable.
* `REMOTE_FMT_FMT_CHECK_FULL` - whether `fmt/chrono.h` and `fmt/std.h` can be included. At `0`,
  `std::chrono::duration`, `std::optional`, `std::variant` and `std::expected` arguments are skipped
  while every other field in the same format string is still checked. Cross builds set this to `1`
  because they apply [fmt.patch](fmt.patch), which makes those two headers respect `FMT_USE_LOCALE`
  and so work on a standard library without localization.

## mp-units

When [mp-units](https://github.com/mpusz/mp-units) is reachable, `mp_units::quantity` and
`mp_units::quantity_point` are formattable like any other argument - no include or opt-in beyond
having the library:

```c++
printer.print("supply {:.2f}"_sc, 3.28F * si::volt);       // supply 3.28 V
printer.print("ambient {}"_sc, quantity_point{delta<si::degree_Celsius>(21)});  // ambient 21 ℃
```

A quantity costs nothing for its unit symbol: the symbol is computed at compile time and lives in the descriptor
(`Q`), only the number travels. The number keeps its own
representation - an integral quantity stays integral, and nothing on the device converts to
floating point merely to be logged.

The replacement field means what it always means, because it is applied to the number on the host:
`{:.2f}` on a float quantity, `{:#x}` on an integral one. Fill, align and width are the exception -
those position the number and the unit symbol together, so `{:>10}` right-aligns `1.5 V` as a unit
rather than padding the number and leaving the symbol past the column. The zero flag stays with the
number, since `{:08.2f}` asks for sign-aware zero padding and `00001.50 V` is what that means.

Whether a space separates the number from the symbol is mp-units' `space_before_unit_symbol`, so a
dimensionless ratio prints as `5` and an angle as `45°`. Symbols use mp-units' default character
set, which is UTF-8: `m/s²`, `Ω`, `℃`.

`mp_units::quantity_point` is logged as its `quantity_from_zero()`. mp-units deliberately gives a
point no text output of its own - it is meaningless without its origin - so this is a convenience
this library adds; if the origin matters to the reader, log it too. The unit comes from the
quantity that call returns rather than from the point's own reference, because `quantity_from_zero`
restores the point's unit only when doing so is non-truncating.

* `REMOTE_FMT_USE_MP_UNITS` - set to `0` to keep the dependency out even where the headers are
  reachable. Defaults to on when `mp-units/framework/quantity.h`, `quantity_point.h` and `unit.h`
  are all reachable.

The optional formatter sets - `REMOTE_FMT_USE_MP_UNITS`, `REMOTE_FMT_USE_ENCHANTUM` (enums by name),
`REMOTE_FMT_USE_STYLED` (`fmt::styled`) - live in `src/remote_fmt/config.hpp`. Each changes which
formatters exist and which strings the catalog holds, so all TUs of one image must agree: a build
sets them for the whole image (this CMakeLists.txt's cross branch defines ENCHANTUM and STYLED as 1;
the library that brings mp-units defines MP_UNITS), never per TU. Without such a definition the
`__has_include` default applies. Every TU leaves one marker per switch and value in the
`remote_fmt_sites` section, and `tools/extract_sites.py` fails the post-link step when an image
holds both values of a switch ("TUs of this image disagree on REMOTE_FMT_USE_<X>").

The host side needs no mp-units at all: a quantity arrives as a symbol in the descriptor and a number.

## CI and fuzzing

`.github/workflows/ci.yml` runs on push and pull request to master, on dispatch and weekly (Sunday 03:00 UTC):

* `build-and-test` - the test suite (`tests/`) with gcc and clang, Release and Debug, ASan/UBSan and libc++ debug
  entries. In the clang entries ctest also runs the three fuzzers as short smokes, the same on every run: a fixed
  seed and run count (`-runs=20000 -seed=1 -use_cmp=0`, built with `-fno-sanitize-coverage=stack-depth`), each
  writable corpus emptied first.
* `examples` - builds and runs `examples/` with gcc and clang.
* `fuzz` - blocking: the corpora replayed once each (`-runs=0`), built like the clang ASan entries above (libc++
  debug mode, `-DUSE_SANITIZER=address`; the fuzz targets add `-fsanitize=fuzzer,address,undefined
  -fno-sanitize-recover=all`), in Release and Debug: the parser's generated seeds, `tests/corpus.tar.xz` (unpacked by
  the configure step) and `tests/corpus_regressions/`, the descriptor fuzzer's generated seeds, and a fixed
  round-trip sequence (`-seed=1`, 100000 runs). Any crash, leak, sanitizer report, timeout or OOM on them fails it.
* `fuzz-explore` - real libFuzzer runs of all three from those corpora with a random seed that the job prints, 300 s
  per fuzzer, 1800 s on the weekly schedule (`FUZZ_SECONDS` in the environment overrides both); the parser and the
  descriptor fuzzer use `tests/fuzz_parser.dict`. `continue-on-error`: a finding marks the job red without failing
  the workflow, and its inputs are uploaded as the artifact `fuzz-crashes-<build type>` (`fuzz-artifacts/<fuzzer>/`).
  Reproduce with the printed seed and add the input to `tests/corpus_regressions/`, so `fuzz` replays it from then
  on.

The fuzz job locally (needs clang with libFuzzer):

```sh
cmake -S tests -B build -DCMAKE_BUILD_TYPE=Release -DUSE_SANITIZER=address -DCMAKE_CXX_COMPILER=clang++
cmake --build build --target fuzz_parser fuzz_descriptor fuzz_roundtrip fuzz_corpus_generator
./build/fuzz_corpus_generator build/corpus build/corpus_descriptor_seeds
./build/fuzz_descriptor -max_total_time=300 -dict=tests/fuzz_parser.dict build/corpus_descriptor build/corpus_descriptor_seeds
```

or the jobs under [act](https://github.com/nektos/act): `act workflow_dispatch -j fuzz` (the replay) and
`act workflow_dispatch -j fuzz-explore --env FUZZ_SECONDS=20`.
