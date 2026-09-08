/*
 * Compiles the vendored engine with its allocation calls redirected to the
 * SDRAM bump allocator.
 *
 * Doing it here rather than as a global -D keeps the redirect scoped to this
 * translation unit -- libDaisy and everything else keep the real calloc/free.
 * The libc headers come FIRST so the macros rewrite call sites only, never
 * the library's own declarations.
 *
 * Build this file; do NOT add vendor/mark_core.c to the source list too, or
 * you get duplicate symbols.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "patch_alloc.h"

#define calloc patch_calloc
#define free   patch_free

#include "vendor/mark_core.c"
