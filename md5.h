/* AmiFritz - MD5 nach RFC 1321. Keine Abhaengigkeiten, endian-neutral. */

#ifndef FB_MD5_H
#define FB_MD5_H

struct MD5Ctx {
    unsigned long state[4];
    unsigned long count[2];     /* Laenge in Bit, niederwertiges Wort zuerst */
    unsigned char buffer[64];
};

void md5_init(struct MD5Ctx *c);
void md5_update(struct MD5Ctx *c, const void *data, unsigned long len);
void md5_final(struct MD5Ctx *c, unsigned char digest[16]);

/* Bequemlichkeit: MD5 ueber einen Puffer, Ergebnis als 32 Zeichen
 * Kleinbuchstaben-Hex plus NUL - so, wie die FRITZ!Box es erwartet. */
void md5_hex(const void *data, unsigned long len, char out[33]);
void md5_digest_hex(const unsigned char digest[16], char out[33]);

#endif
