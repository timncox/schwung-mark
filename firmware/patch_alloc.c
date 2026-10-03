/*
 * Bump allocator backing patch_alloc.h. See that header for why this exists.
 *
 * Deliberately trivial: a pointer that only moves forward. Every allocation
 * Belt makes happens inside one belt_create() call at boot, and the module
 * never destroys the engine, so reclaiming memory would be dead code.
 */
#include "patch_alloc.h"

#include <stdint.h>
#include <string.h>

static uint8_t *g_pool;
static size_t   g_cap;
static size_t   g_used;
static int      g_failed;

/* Every type the engine allocates is <= 8 bytes wide (double is the worst
 * case; belt_core uses float and int16_t). Align to 8 so no member of a
 * bumped block is ever misaligned on Cortex-M7. */
#define ALIGN 8u

void patch_alloc_init(void *pool, size_t bytes)
{
    g_pool   = (uint8_t *)pool;
    g_cap    = bytes;
    g_used   = 0;
    g_failed = 0;
}

void *patch_calloc(size_t nmemb, size_t size)
{
    if (!g_pool) { g_failed = 1; return NULL; }

    /* Overflow guard: nmemb * size is attacker-free here (all sizes are
     * compile-time constants in belt_core) but a silent wrap would corrupt
     * the pool, so check rather than assume. */
    if (nmemb != 0 && size > (SIZE_MAX / nmemb)) { g_failed = 1; return NULL; }

    size_t want    = nmemb * size;
    size_t aligned = (want + (ALIGN - 1u)) & ~(size_t)(ALIGN - 1u);

    if (aligned > g_cap - g_used) { g_failed = 1; return NULL; }

    uint8_t *p = g_pool + g_used;
    g_used += aligned;

    /* calloc contract: zeroed. The engine relies on this -- belt_create()
     * does not explicitly clear the rings it allocates. */
    memset(p, 0, want);
    return p;
}

void patch_free(void *ptr)
{
    (void)ptr; /* no-op by design; see the header */
}

size_t patch_alloc_used(void)     { return g_used; }
size_t patch_alloc_capacity(void) { return g_cap;  }
int    patch_alloc_failed(void)   { return g_failed; }
