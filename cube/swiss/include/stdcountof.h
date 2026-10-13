/* Compatibility shim for <stdcountof.h> (C23): devkitPPC r47's GCC does not
 * ship it. If the toolchain provides the real header (searched after this
 * directory), defer to it; otherwise provide countof() for this codebase's
 * fixed-size array uses. */
#if defined(__has_include_next) && __has_include_next(<stdcountof.h>)
#include_next <stdcountof.h>
#else
#ifndef _COMPAT_STDCOUNTOF_H
#define _COMPAT_STDCOUNTOF_H
#ifndef countof
#define countof(a) (sizeof(a) / sizeof((a)[0]))
#endif
#endif
#endif
