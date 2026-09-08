/*
 * patch_alloc — a bump allocator so the vendored engine needs no edits.
 *
 * mark_create() allocates the track buffers, the undo buffer and a delay line per track. Daisy has
 * no meaningful heap, and rewriting mark_create() would fork the engine.
 *
 * Instead the ARM build compiles belt_core.c with
 *     -Dcalloc=patch_calloc -Dfree=patch_free
 * pointing those calls at a static pool. Allocation happens once at boot and
 * is never returned, so free() is a no-op -- correct here, because the module
 * creates exactly one engine and keeps it until power-off.
 *
 * Same pattern as smack-versio's versio_alloc. Sizes for this build:
 *
 *     4 track buffers + 1 undo, mono int16, 60 s at 48 kHz  = 28.8 MB
 *     4 delay lines, MARK_DLY_LEN * 2 floats each           =  1.0 MB
 *     mark_t itself                                          ~ tens of KB
 *
 * The pool is 32 MB, declared in mark_patch.cpp because it needs
 * DSY_SDRAM_BSS. If it ever does not fit, mark_create() steps down its own
 * alloc_seconds ladder rather than failing outright.
 *
 * This file stays plain C so it can be unit-tested natively.
 */
#ifndef PATCH_ALLOC_H
#define PATCH_ALLOC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void   patch_alloc_init(void *pool, size_t bytes);
void  *patch_calloc(size_t nmemb, size_t size);
void   patch_free(void *ptr);

/* For the boot-time log: how much of the pool the engine actually took. */
size_t patch_alloc_used(void);
size_t patch_alloc_capacity(void);

/* Set when an allocation did not fit. If this is true after belt_create(),
 * the module is broken and must say so rather than run half-initialised. */
int    patch_alloc_failed(void);

#ifdef __cplusplus
}
#endif

#endif /* PATCH_ALLOC_H */
