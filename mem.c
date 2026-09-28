/* AmiFritz - prozessfester Speicher, siehe mem.h. */

#include <string.h>
#include <stdlib.h>

#include "mem.h"

#if defined(__amigaos__) || defined(AMIGA) || defined(__AMIGA__)

#include <exec/memory.h>
#include <proto/exec.h>

/* Vor jedem Block steht seine Nutzgroesse - realloc() muss wissen, wie
 * viel es umkopieren darf. 8 Byte, damit der Nutzteil so ausgerichtet
 * bleibt wie der von AllocVec. */
#define HDR 8

void *fb_malloc(size_t size)
{
    unsigned char *p = AllocVec(size + HDR, MEMF_ANY);

    if (!p) {
        return NULL;
    }
    *(size_t *)p = size;
    return p + HDR;
}

void fb_free(void *p)
{
    if (p) {
        FreeVec((unsigned char *)p - HDR);
    }
}

void *fb_realloc(void *p, size_t size)
{
    size_t old;
    void *n;

    if (!p) {
        return fb_malloc(size);
    }
    old = *(size_t *)((unsigned char *)p - HDR);
    n = fb_malloc(size);
    if (!n) {
        return NULL;            /* der alte Block bleibt gueltig */
    }
    memcpy(n, p, old < size ? old : size);
    fb_free(p);
    return n;
}

#else

void *fb_malloc(size_t size)            { return malloc(size); }
void *fb_realloc(void *p, size_t size)  { return realloc(p, size); }
void  fb_free(void *p)                  { free(p); }

#endif

void *fb_calloc(size_t n, size_t size)
{
    void *p = fb_malloc(n * size);

    if (p) {
        memset(p, 0, n * size);
    }
    return p;
}
