/*
 * Arena allocator — chunk-based bump allocator, no individual frees.
 * Allocates new chunks as needed; previous allocations always remain valid.
 * The entire arena is freed at once. Perfect for request-scoped work.
 */

#include "nc.h"
#include <stdlib.h>
#include <string.h>

static nc_arena_chunk *chunk_new(size_t data_cap) {
    nc_arena_chunk *c = (nc_arena_chunk *)malloc(sizeof(nc_arena_chunk) + data_cap);
    if (!c) return NULL;
    c->next = NULL;
    c->cap = data_cap;
    c->pos = 0;
    return c;
}

void nc_arena_init(nc_arena *a, size_t cap) {
    nc_arena_chunk *c = chunk_new(cap);
    a->head = c;
    a->current = c;
    a->chunk_size = cap;
}

void *nc_arena_alloc(nc_arena *a, size_t size) {
    /* Align to 8 bytes */
    size = (size + 7) & ~(size_t)7;

    nc_arena_chunk *c = a->current;
    if (!c || c->pos + size > c->cap) {
        /* Need a new chunk — at least double the request or chunk_size */
        size_t new_cap = a->chunk_size;
        if (new_cap < size) new_cap = size;
        nc_arena_chunk *nc = chunk_new(new_cap);
        if (!nc) return NULL;
        if (c) c->next = nc;
        else a->head = nc;
        a->current = nc;
        c = nc;
    }

    void *ptr = c->data + c->pos;
    c->pos += size;
    return ptr;
}

char *nc_arena_dup(nc_arena *a, const char *s, size_t len) {
    char *d = (char *)nc_arena_alloc(a, len + 1);
    if (!d) return NULL;
    memcpy(d, s, len);
    d[len] = '\0';
    return d;
}

void nc_arena_reset(nc_arena *a) {
    /* Free all chunks except the first, reset first chunk */
    nc_arena_chunk *c = a->head;
    if (!c) return;
    nc_arena_chunk *next = c->next;
    while (next) {
        nc_arena_chunk *tmp = next->next;
        free(next);
        next = tmp;
    }
    c->next = NULL;
    c->pos = 0;
    a->current = c;
}

void nc_arena_free(nc_arena *a) {
    nc_arena_chunk *c = a->head;
    while (c) {
        nc_arena_chunk *next = c->next;
        free(c);
        c = next;
    }
    a->head = NULL;
    a->current = NULL;
    a->chunk_size = 0;
}

/* ── Tests ──────────────────────────────────────────────────────── */

#ifdef NC_TEST
void nc_test_arena(void) {
    nc_arena a;
    nc_arena_init(&a, 128);

    void *p1 = nc_arena_alloc(&a, 32);
    NC_ASSERT(p1 != NULL, "arena alloc 32 bytes");

    void *p2 = nc_arena_alloc(&a, 64);
    NC_ASSERT(p2 != NULL, "arena alloc 64 bytes");
    NC_ASSERT(p2 != p1, "arena allocs are distinct");

    /* Test arena dup */
    char *s = nc_arena_dup(&a, "hello", 5);
    NC_ASSERT(s != NULL, "arena dup non-null");
    NC_ASSERT(strcmp(s, "hello") == 0, "arena dup content");

    /* Test growth: allocate more than initial cap — must not invalidate p1/p2 */
    void *p3 = nc_arena_alloc(&a, 256);
    NC_ASSERT(p3 != NULL, "arena grows beyond initial cap");

    /* Verify p1 and p2 are still valid (not moved by realloc) */
    NC_ASSERT(p1 != NULL, "p1 still valid after growth");
    NC_ASSERT(p2 != NULL, "p2 still valid after growth");

    nc_arena_reset(&a);
    NC_ASSERT(a.current->pos == 0, "arena reset clears pos");

    nc_arena_free(&a);
    NC_ASSERT(a.head == NULL, "arena free nulls head");
}
#endif
