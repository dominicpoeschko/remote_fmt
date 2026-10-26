#pragma once

// Tells clang that a returned reference or a constructed object refers to this argument (or to *this), so its lifetime
// analysis can see a dangling use. Nothing on other compilers.
#if __has_cpp_attribute(clang::lifetimebound)
    #define REMOTE_FMT_LIFETIMEBOUND [[clang::lifetimebound]]
#else
    #define REMOTE_FMT_LIFETIMEBOUND
#endif
