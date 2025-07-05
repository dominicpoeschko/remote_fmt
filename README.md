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
    GIT_REPOSITORY git@github.com:dominicpoeschko/remote_fmt.git
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
In this example the usage of the catalog generator is covered [examples/basic_usage.cpp](examples/basic_usage.cpp).

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
The script generates another file named `${target_name}_string_constants.json`. This file contains the contents of the catalog in a `json` notation. This file can be parsed with the json file parser by [nlohmann](git@github.com:nlohmann/json.git) using the `parseStringConstantsFromJsonFile(...)` function in the `catalog_helpers.hpp`:

```c++
#include "remote_fmt/catalog_helpers.hpp"
auto const catalog = remote_fmt::parseStringConstantsFromJsonFile("path/to/catalog.json");
```