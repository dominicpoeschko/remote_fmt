// Compiled alone against the patched fmt, with the definitions a cross build of remote_fmt puts on every TU (see
// CMakeLists.txt). Nothing may come before this include: the header must compile with no other header in front.
#include <string_constant/string_constant.hpp>
