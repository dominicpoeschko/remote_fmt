// A failed FMT_ASSERT under remote_fmt's cross-build definition calls fmt::assert_fail with the condition's text, and
// a firmware's runtime defines that function with assert()'s end (fmt's library object is not linked). This program
// defines it the same way and makes fmt's to_unsigned fail its check at run time: the test passes on the line printed
// here, before the exit.
#include <cstdio>
#include <cstdlib>
#include <fmt/base.h>

[[gnu::noinline]] void fmt::assert_fail(char const* file,
                                        int         line,
                                        char const* message) {
    std::printf("assert path: %s %s:%d end\n", message, file, line);
    std::fflush(stdout);
    std::_Exit(0);
}

int main(int argc,
         char**) {
    int volatile negative = -argc;
    std::printf("to_unsigned returned %u: no assert path\n",
                fmt::detail::to_unsigned(static_cast<int>(negative)));
    return 1;
}
