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

The `buffer` on the remote device can be parsed with the `remote_fmt::parse(...)` function to print the transmitted format string. The required catalog of this function is empty in this example.

#### Catalog Example

> Note: The catalog example only shows the behaviour of the catalog system. If you want to use the catalog feature in your project refer to the Catalog Generator Example below!

This example covers the catalog functionality in [examples/catalog.cpp](examples/catalog.cpp).
Most of the example is the same as the basic example except of the catalog on the top of the example.
This catalog code can be generated in the toolchain with a python script.

Another difference is the function call of the `remote_fmt::parse(...)` function. In this example the function is called with a catalog.

#### Catalog Generator Example
In this example the usage of the catalog generator is covered in [examples/catalog_generator.cpp](examples/catalog_generator.cpp).

To use the catalog generator you must have installed python on your computer. The generator can be called in the `CMakeLists.txt` of your project with the following function:

```cmake
target_generate_string_constants(${target_name})
```

All static strings used in `print` functions are automatically stored in the catalog:

```c++
printer.print("Test {}"_sc, 123);
```

##### Generated Files
The python script called by CMake generates different output files. The `${target_name}_string_constants.cpp` file contains the generated catalog. The file is automatically built by the python script.
The script generates another file named `${target_name}_string_constants.json`. This file contains the contents of the catalog in a `json` notation. This file can be parsed with the json file parser by [nlohmann](https://github.com/nlohmann/json) using the `parseStringConstantsFromJsonFile(...)` function in the `catalog_helpers.hpp`:

```c++
#include "remote_fmt/catalog_helpers.hpp"
auto const catalog = remote_fmt::parseStringConstantsFromJsonFile("path/to/catalog.json");
```

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
struct host_type<MyWrapper<T>> {
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
struct host_type<Suppressed> {
    using type = std::string_view;   // Suppressed's formatter uses format_to
};
}}
```

Two rules come from the protocol rather than from FMT, which accepts both: an argument index
(`"{1} {0}"`) and a dynamic width or precision (`"{:{}}"`) are rejected, because each replacement
field is paired with exactly one argument, in order, and no argument index travels on the wire.

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

A quantity costs the unit symbol *once*: the symbol is computed at compile time and travels as a
cataloged string, so every value after the first pays two bytes for it. The number keeps its own
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

The host side needs no mp-units at all: a quantity arrives as a string and a number.
