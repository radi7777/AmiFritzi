/* Prueft MD5 und die beiden Login-Rechnungen gegen feste Werte. Laeuft auf
 * dem Host; die Rechnungen sind rein und haengen nicht vom Netz ab. */

#include <stdio.h>
#include <string.h>

#include "md5.h"
#include "fritz.h"
#include "net.h"

static int failures = 0;

static void check(const char *what, const char *got, const char *want)
{
    if (strcmp(got, want) != 0) {
        printf("FAIL %s\n  got  %s\n  want %s\n", what, got, want);
        failures++;
    } else {
        printf("ok   %s\n", what);
    }
}

int main(void)
{
    /* RFC 1321, Anhang A.5 */
    static const char *rfc[][2] = {
        { "", "d41d8cd98f00b204e9800998ecf8427e" },
        { "a", "0cc175b9c0f1b6a831c399e269772661" },
        { "abc", "900150983cd24fb0d6963f7d28e17f72" },
        { "message digest", "f96b697d7cb7938d525a2f31aaf161d0" },
        { "abcdefghijklmnopqrstuvwxyz", "c3fcd3d76192e4007dfb496cca67e13b" },
        { "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
          "d174ab98d277d9f5a5611c2c9f419d9f" },
        { "1234567890123456789012345678901234567890"
          "1234567890123456789012345678901234567890",
          "57edf4a22be3c955ac49da2e2107b67a" },
    };
    char hex[33], buf[128];
    struct MD5Ctx c;
    unsigned char d[16];
    unsigned i;

    for (i = 0; i < sizeof(rfc) / sizeof(rfc[0]); i++) {
        md5_hex(rfc[i][0], strlen(rfc[i][0]), hex);
        sprintf(buf, "md5(\"%.20s\")", rfc[i][0]);
        check(buf, hex, rfc[i][1]);
    }

    /* Stueckweise gefuettert muss dasselbe herauskommen. */
    md5_init(&c);
    for (i = 0; i < 8; i++) {
        md5_update(&c, "1234567890", 10);
    }
    md5_final(&c, d);
    md5_digest_hex(d, hex);
    check("md5 incremental", hex, rfc[6][1]);

    /* TR-064, Testrechnung aus PROJECT.md */
    fb_tr064_response("admin", "F!Box SOAP-Auth", "gurkensalat",
                      "F758BE72FB999CEA", hex);
    check("tr064 response", hex, "b4f67585f22b0af7c4615db5a18faa14");
    md5_hex("admin:F!Box SOAP-Auth:gurkensalat", 33, hex);
    check("tr064 secret", hex, "09b67a406f01ca26439fc9c2104aa5da");

    /* AHA, Beispiel aus AVMs "Session-IDs im FRITZ!Box Webinterface":
     * Passwort "äbc" - hier in ISO-8859-1, wie es der Amiga liefert. */
    fb_aha_response("1234567z", "\xe4" "bc", buf, sizeof(buf));
    check("aha response", buf, "1234567z-9e224a41eeefa284df7bb0f26c2913e2");

    /* Kleinkram */
    xml_tag("<SessionInfo><SID>0123456789abcdef</SID></SessionInfo>", "SID",
            buf, sizeof(buf));
    check("xml_tag", buf, "0123456789abcdef");
    url_encode("08761 0000434", buf, sizeof(buf));
    check("url_encode ain", buf, "08761%200000434");
    url_encode("f\xfcr&=", buf, sizeof(buf));
    check("url_encode latin1", buf, "f%FCr%26%3D");

    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
