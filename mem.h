/* AmiFritzi - Speicher, der aus zwei Prozessen zugleich benutzt werden darf.
 *
 * Die Netzwerkaufrufe laufen in einem eigenen Prozess (worker.c). libnix'
 * malloc() haelt seine Verwaltung in globalen Variablen ohne Sperre - rufen
 * Oberflaeche und Arbeitsprozess es gleichzeitig auf, zerbricht die Liste.
 * Alles, was net.c, fritz.c, tr064.c und jobs.c anfordern, geht deshalb
 * ueber AllocVec(), das Exec selbst absichert. Auf dem Unix-Host fuer die
 * Tests ist es schlicht malloc(). */

#ifndef FB_MEM_H
#define FB_MEM_H

#include <stddef.h>

void *fb_malloc(size_t size);
void *fb_calloc(size_t n, size_t size);
void *fb_realloc(void *p, size_t size);
void  fb_free(void *p);

#endif
