/* AmiFritz - TR-064: SOAP ueber HTTP an Port 49000.
 *
 * Anmeldung nach AVMs "SOAP-Auth" im SOAP-Header, NICHT HTTP-Digest:
 *
 *   1. Aufruf mit <InitChallenge><UserID>  -> Box liefert Nonce + Realm
 *   2. Aufruf mit <ClientAuth> und Auth = MD5(MD5(uid:realm:pwd):nonce)
 *   3. Jede Antwort bringt in <NextChallenge> die Nonce fuer den naechsten
 *      Aufruf mit - danach kostet jeder Aufruf nur noch einen Request. */

#ifndef FB_TR064_H
#define FB_TR064_H

#include "net.h"

#define TR_NONCE_LEN  40
#define TR_REALM_LEN  40
#define TR_URL_LEN    96

struct Tr064 {
    char host[64];
    int  port;                  /* 49000 */
    char user[64];              /* UTF-8, wie es die Box vergleicht */
    char pass[128];             /* UTF-8 */
    char nonce[TR_NONCE_LEN];   /* leer = noch keine Challenge */
    char realm[TR_REALM_LEN];
    char *desc;                 /* tr64desc.xml, einmal geholt */
    int  last_fault;            /* errorCode des letzten SOAP-Faults, 0 = keiner */
};

/* user und pass kommen als ISO-8859-1 und werden nach UTF-8 gewandelt. */
void tr_init(struct Tr064 *t, const char *host, const char *user,
             const char *pass);
void tr_free(struct Tr064 *t);

/* controlURL zu einem serviceType aus tr64desc.xml, z. B.
 * "urn:dslforum-org:service:DeviceInfo:1". 1 = gefunden. */
int tr_control_url(struct Tr064 *t, const char *service, char *url,
                   int urlsize);

/* Ruft eine Aktion auf. args ist fertiges XML fuer die Argumente oder NULL
 * (Werte vorher mit xml_escape behandeln). *out bekommt den Antwort-Rumpf
 * (malloc). Bei FB_EAUTH/FB_EHTTP steht der SOAP-errorCode in last_fault. */
int tr_call(struct Tr064 *t, const char *service, const char *action,
            const char *args, char **out);

/* Die aktuelle Internet-Adresse. Probiert WANPPPConnection (DSL mit PPPoE)
 * und dann WANIPConnection (Kabel, Glasfaser, IP-Client). */
int tr_external_ip(struct Tr064 *t, char *ip, int ipsize);

/* Klartext zu den TR-064-Fehlercodes. */
const char *tr_fault_text(int code);

#endif
