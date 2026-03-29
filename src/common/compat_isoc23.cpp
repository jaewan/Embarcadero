/* compat_isoc23.cpp — Provide GLIBC_2.38 __isoc23_* symbols backed by the
 * classic strtoul/strtol/etc. at GLIBC_2.2.5.
 *
 * GCC 13 + GLIBC 2.39 headers redirect strtoul/strtol/sscanf/fscanf to their
 * ISO C23 variants (__isoc23_strtoul etc.), which carry a GLIBC_2.38 version
 * tag.  Binaries compiled on Ubuntu 24.04 (GLIBC 2.39) then fail to start on
 * Ubuntu 22.04 (GLIBC 2.35) with "version 'GLIBC_2.38' not found".
 *
 * Linking this translation unit BEFORE any .a archives satisfies all
 * __isoc23_* undefined-symbol references locally, so the linker emits them as
 * LOCAL definitions (no dynamic GLIBC_2.38 version tag in the output binary).
 * The implementations call strtoul@GLIBC_2.2.5 etc., available on every
 * supported Ubuntu release.
 */
#include <cstdarg>
#include <cstdio>

extern "C" {

/* Use private names + .symver to reach classic GLIBC_2.2.5 symbols without
 * triggering the C23 macro redirection (strtoul → __isoc23_strtoul on GCC 13
 * GLIBC 2.39 headers), which would cause infinite recursion. */
extern unsigned long      __compat_strtoul  (const char *, char **, int);
extern long               __compat_strtol   (const char *, char **, int);
extern long long          __compat_strtoll  (const char *, char **, int);
extern unsigned long long __compat_strtoull (const char *, char **, int);
extern int                __compat_vsscanf  (const char *, const char *, std::va_list);
extern int                __compat_vfscanf  (FILE *, const char *, std::va_list);

__asm__(".symver __compat_strtoul,  strtoul@GLIBC_2.2.5");
__asm__(".symver __compat_strtol,   strtol@GLIBC_2.2.5");
__asm__(".symver __compat_strtoll,  strtoll@GLIBC_2.2.5");
__asm__(".symver __compat_strtoull, strtoull@GLIBC_2.2.5");
__asm__(".symver __compat_vsscanf,  vsscanf@GLIBC_2.2.5");
__asm__(".symver __compat_vfscanf,  vfscanf@GLIBC_2.2.5");

unsigned long      __isoc23_strtoul (const char *s, char **e, int b) { return __compat_strtoul (s,e,b); }
long               __isoc23_strtol  (const char *s, char **e, int b) { return __compat_strtol  (s,e,b); }
long long          __isoc23_strtoll (const char *s, char **e, int b) { return __compat_strtoll (s,e,b); }
unsigned long long __isoc23_strtoull(const char *s, char **e, int b) { return __compat_strtoull(s,e,b); }

int __isoc23_vsscanf(const char *s, const char *f, std::va_list a) { return __compat_vsscanf(s, f, a); }
int __isoc23_vfscanf(FILE *s, const char *f, std::va_list a)       { return __compat_vfscanf(s, f, a); }

int __isoc23_sscanf(const char *s, const char *f, ...) {
    std::va_list a; va_start(a, f); int r = __compat_vsscanf(s, f, a); va_end(a); return r;
}
int __isoc23_fscanf(FILE *s, const char *f, ...) {
    std::va_list a; va_start(a, f); int r = __compat_vfscanf(s, f, a); va_end(a); return r;
}

/* _ZSt21ios_base_library_initv was introduced in GCC 13 as an alternative
 * constructor for std::ios_base / C++ streams.  On systems with GCC 11
 * libstdc++ (GLIBCXX up to 3.4.29), the equivalent initialization happens via
 * the old __attribute__((init_priority)) static objects.  A no-op here lets
 * the GCC 11 runtime handle stream setup through its own path; these binaries
 * use glog (not std::cout) for all output so the no-op is safe. */
void __compat_ios_base_library_initv(void) {}
__asm__(".symver __compat_ios_base_library_initv, _ZSt21ios_base_library_initv@@@GLIBCXX_3.4.32");

} // extern "C"
