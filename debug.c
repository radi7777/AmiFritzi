/* AmiFritzi - Diagnose-Log, siehe debug.h. */

#include "debug.h"

#if defined(DEBUG) && (defined(__amigaos__) || defined(AMIGA) || defined(__AMIGA__))

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <dos/dos.h>
#include <proto/dos.h>

void dbg_log(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    BPTR fh;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    fh = Open((STRPTR)"RAM:AmiFritzi.log", MODE_READWRITE);
    if (fh) {
        Seek(fh, 0, OFFSET_END);
        Write(fh, buf, strlen(buf));
        Close(fh);
    }
}

#endif
