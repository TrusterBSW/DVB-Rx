/*
 * The SDR libraries (libuhd, libbladeRF, libhackrf) are loaded at run time
 * with dlopen: one dvbrx binary runs on any distribution, whatever the
 * library's version (libuhd.so.4.1.0 on Ubuntu 22.04, 4.8.0 on Debian 13),
 * and a library that is not installed only disables its driver.
 *
 * A driver lists the functions it calls, X(name) per function, includes its
 * library's header, then:
 *   FOO_SYMS(DL_PTR)                     pointers typed from the header
 *   static int foo_load(void) { ... DL_LOAD(names, "what", FOO_SYMS) }
 *   #define foo_function (*dl_foo_function)   for each function
 */
#ifndef DVBRX_SDR_DL_H
#define DVBRX_SDR_DL_H

#include <dlfcn.h>
#include <glob.h>
#include <stdio.h>
#include <string.h>

#define DL_PTR(n) static __typeof__(n)* dl_##n;
#define DL_SYM(n)                                                                                  \
    if (!(*(void**)&dl_##n = dlsym(h, #n))) {                                                      \
        fprintf(stderr, "%s: %s has no %s (too old?)\n", what, lib, #n);                           \
        return -1;                                                                                 \
    }

/* Opens the first library found: each name is a dlopen name or, with a
 * '*', a glob pattern searched in the usual library directories (the last
 * match, the highest version). */
static inline void* dl_open_first(const char* const* names, char* found, size_t n)
{
    static const char* const dirs[] = { "/usr/lib/x86_64-linux-gnu", "/usr/lib/aarch64-linux-gnu",
                                        "/usr/lib64", "/usr/lib", "/usr/local/lib",
                                        "/usr/local/lib/x86_64-linux-gnu", NULL };
    for (const char* const* p = names; *p; p++) {
        if (!strchr(*p, '*')) {
            void* h = dlopen(*p, RTLD_NOW | RTLD_LOCAL);
            if (h) {
                snprintf(found, n, "%s", *p);
                return h;
            }
            continue;
        }
        for (const char* const* d = dirs; *d; d++) {
            char pat[256];
            glob_t g;
            snprintf(pat, sizeof(pat), "%s/%s", *d, *p);
            if (glob(pat, 0, NULL, &g) || !g.gl_pathc) {
                globfree(&g);
                continue;
            }
            for (size_t i = g.gl_pathc; i-- > 0;) {
                void* h = dlopen(g.gl_pathv[i], RTLD_NOW | RTLD_LOCAL);
                if (h) {
                    snprintf(found, n, "%s", g.gl_pathv[i]);
                    globfree(&g);
                    return h;
                }
            }
            globfree(&g);
        }
    }
    return NULL;
}

/* Body of a driver's loader: once per process, 0 when every function is
 * there. */
#define DL_LOAD(names, what_, SYMS, hint)                                                          \
    static int state; /* 0 not tried, 1 loaded, -1 failed */                                       \
    if (state)                                                                                     \
        return state > 0 ? 0 : -1;                                                                 \
    state = -1;                                                                                    \
    const char* what = what_;                                                                      \
    char lib[256];                                                                                 \
    void* h = dl_open_first(names, lib, sizeof(lib));                                              \
    if (!h) {                                                                                      \
        fprintf(stderr, "%s: library not found (%s)\n", what, hint);                               \
        return -1;                                                                                 \
    }                                                                                              \
    SYMS(DL_SYM)                                                                                   \
    state = 1;                                                                                     \
    return 0;

#endif
