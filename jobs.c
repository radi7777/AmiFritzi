/* AmiFritzi - Auftraege an den Netzwerkprozess, siehe jobs.h. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jobs.h"
#include "mem.h"
#include "debug.h"

void job_state_init(struct JobState *st)
{
    memset(st, 0, sizeof(*st));
    fb_session_init(&st->fs, "");
}

void job_state_free(struct JobState *st)
{
    if (st->connected) {
        fb_logout(&st->fs);
    }
    tr_free(&st->tr);
    memset(st->pass, 0, sizeof(st->pass));
    st->connected = 0;
}

void job_result_free(struct JobData *j)
{
    fb_free(j->sw);
    j->sw = NULL;
    j->swcount = 0;
    j->host_count = 0;
    j->hosts_active = -1;
}

static void take_error(char *dst, int size)
{
    strncpy(dst, net_last_error(), size - 1);
    dst[size - 1] = '\0';
}

/* Die SID verfaellt nach 20 Minuten ohne Zugriff, und die Box startet auch
 * mal neu. Beides zeigt sich als 403 (FB_EAUTH) - dann einmal still neu
 * anmelden und den Aufruf wiederholen, statt den Anwender zu fragen. */
static int relogin(struct JobState *st)
{
    return fb_login(&st->fs, st->fs.user, st->pass);
}

static int list_switches(struct JobState *st, struct JobData *j)
{
    int rc = fb_switch_list(&st->fs, &j->sw, &j->swcount);

    if (rc == FB_EAUTH && relogin(st) == FB_OK) {
        rc = fb_switch_list(&st->fs, &j->sw, &j->swcount);
    }
    return rc;
}

/* "1.2.3.4, 5.6.7.8" -> dns1, dns2 (ohne Leerzeichen) */
static void split_dns(char *dns, struct JobData *j)
{
    char *second = strchr(dns, ',');

    if (second) {
        *second++ = '\0';
        while (*second == ' ') {
            second++;
        }
        strncpy(j->dns2, second, sizeof(j->dns2) - 1);
        j->dns2[sizeof(j->dns2) - 1] = '\0';
    }
    strncpy(j->dns1, dns, sizeof(j->dns1) - 1);
    j->dns1[sizeof(j->dns1) - 1] = '\0';
}

/* Zaehlt <Active>1</Active> in der Host-Liste. -1, wenn die Box sie nicht
 * liefert (aeltere Firmware) - dann zeigt die Oberflaeche nur die Anzahl. */
static int count_active_hosts(struct JobState *st)
{
    struct HttpResponse r;
    char path[TR_URL_LEN];
    char *resp;
    const char *p;
    int n = 0;

    if (tr_call(&st->tr, "urn:dslforum-org:service:Hosts:1",
                "X_AVM-DE_GetHostListPath", NULL, &resp) != FB_OK) {
        return -1;
    }
    if (!xml_tag(resp, "NewX_AVM-DE_HostListPath", path, sizeof(path))) {
        fb_free(resp);
        return -1;
    }
    fb_free(resp);
    if (http_get(st->tr.host, st->tr.port, path, &r) != FB_OK) {
        return -1;
    }
    if (r.status != 200) {
        http_free(&r);
        return -1;
    }
    for (p = r.body; (p = strstr(p, "<Active>1</Active>")) != NULL; p++) {
        n++;
    }
    http_free(&r);
    return n;
}

static void tr064_info(struct JobState *st, struct JobData *j)
{
    char up[16], tmp[32];
    char *resp = NULL;
    char url[TR_URL_LEN];
    int rc;

    dbg_log("Worker: tr064_info starting...\n");
    j->uptime = -1;
    tr_free(&st->tr);
    tr_init(&st->tr, st->fs.host, st->fs.user, st->pass);
    if (st->trport > 0) {
        st->tr.port = st->trport;
    }

    dbg_log("Worker: calling DeviceInfo:1 GetInfo...\n");
    rc = tr_call(&st->tr, "urn:dslforum-org:service:DeviceInfo:1", "GetInfo",
                 NULL, &resp);
    dbg_log("Worker: DeviceInfo GetInfo returned %d\n", rc);
    if (rc != FB_OK) {
        take_error(j->tr_error, sizeof(j->tr_error));
        return;
    }
    xml_tag(resp, "NewModelName", j->model, sizeof(j->model));
    xml_tag(resp, "NewSoftwareVersion", j->fwver, sizeof(j->fwver));
    xml_tag(resp, "NewSerialNumber", j->serial, sizeof(j->serial));
    if (xml_tag(resp, "NewUpTime", up, sizeof(up))) {
        j->uptime = atol(up);
    }
    fb_free(resp);
    utf8_to_latin1(j->model);
    j->tr_ok = 1;
    dbg_log("Worker: Model='%s' FW='%s' UpTime=%ld\n", j->model, j->fwver, j->uptime);

    /* Internet / WAN */
    dbg_log("Worker: calling tr_external_ip...\n");
    if (tr_external_ip(&st->tr, j->wanip, sizeof(j->wanip)) != FB_OK) {
        j->wanip[0] = '\0';
    }
    dbg_log("Worker: WAN IP='%s'\n", j->wanip);

    /* WANIPConnection GetInfo fuer Status & DNS */
    if (tr_control_url(&st->tr, "urn:dslforum-org:service:WANIPConnection:1", url, sizeof(url))) {
        if (tr_call(&st->tr, "urn:dslforum-org:service:WANIPConnection:1", "GetInfo", NULL, &resp) == FB_OK) {
            char dns[80];
            xml_tag(resp, "NewConnectionStatus", j->conn_status, sizeof(j->conn_status));
            if (!j->wanip[0]) {
                xml_tag(resp, "NewExternalIPAddress", j->wanip, sizeof(j->wanip));
                if (strcmp(j->wanip, "0.0.0.0") == 0) {
                    j->wanip[0] = '\0';
                }
            }
            if (xml_tag(resp, "NewDNSServers", dns, sizeof(dns))) {
                split_dns(dns, j);
            }
            fb_free(resp);
        }
    }

    /* WANCommonInterfaceConfig: Link Properties & Bytes */
    if (tr_control_url(&st->tr, "urn:dslforum-org:service:WANCommonInterfaceConfig:1", url, sizeof(url))) {
        if (tr_call(&st->tr, "urn:dslforum-org:service:WANCommonInterfaceConfig:1", "GetCommonLinkProperties", NULL, &resp) == FB_OK) {
            if (xml_tag(resp, "NewLayer1DownstreamMaxBitRate", tmp, sizeof(tmp))) {
                j->max_down_bps = strtoul(tmp, NULL, 10);
            }
            if (xml_tag(resp, "NewLayer1UpstreamMaxBitRate", tmp, sizeof(tmp))) {
                j->max_up_bps = strtoul(tmp, NULL, 10);
            }
            fb_free(resp);
        }
        if (tr_call(&st->tr, "urn:dslforum-org:service:WANCommonInterfaceConfig:1", "GetTotalBytesSent", NULL, &resp) == FB_OK) {
            if (xml_tag(resp, "NewTotalBytesSent", tmp, sizeof(tmp))) {
                j->bytes_sent = strtoul(tmp, NULL, 10);
            }
            fb_free(resp);
        }
        if (tr_call(&st->tr, "urn:dslforum-org:service:WANCommonInterfaceConfig:1", "GetTotalBytesReceived", NULL, &resp) == FB_OK) {
            if (xml_tag(resp, "NewTotalBytesReceived", tmp, sizeof(tmp))) {
                j->bytes_recv = strtoul(tmp, NULL, 10);
            }
            fb_free(resp);
        }
    }

    /* 5G / Mobilfunk: X_AVM-DE_WANMobileConnection:1 */
    if (tr_control_url(&st->tr, "urn:dslforum-org:service:X_AVM-DE_WANMobileConnection:1", url, sizeof(url))) {
        if (tr_call(&st->tr, "urn:dslforum-org:service:X_AVM-DE_WANMobileConnection:1", "GetInfoEx", NULL, &resp) == FB_OK) {
            j->has_mobile = 1;
            xml_tag(resp, "NewCurrentAccessTechnology", j->mobile_tech, sizeof(j->mobile_tech));
            xml_tag(resp, "NewSignalRSRP0", j->mobile_signal, sizeof(j->mobile_signal));
            xml_tag(resp, "NewAPN", j->mobile_apn, sizeof(j->mobile_apn));
            xml_tag(resp, "NewRoaming", j->mobile_roaming, sizeof(j->mobile_roaming));
            fb_free(resp);
        }
    }

    /* WLAN 2.4 GHz */
    if (tr_control_url(&st->tr, "urn:dslforum-org:service:WLANConfiguration:1", url, sizeof(url))) {
        if (tr_call(&st->tr, "urn:dslforum-org:service:WLANConfiguration:1", "GetInfo", NULL, &resp) == FB_OK) {
            if (xml_tag(resp, "NewEnable", tmp, sizeof(tmp))) {
                j->wlan24_active = atoi(tmp);
            }
            if (xml_tag(resp, "NewChannel", tmp, sizeof(tmp))) {
                j->wlan24_channel = atoi(tmp);
            }
            xml_tag(resp, "NewSSID", j->wlan24_ssid, sizeof(j->wlan24_ssid));
            utf8_to_latin1(j->wlan24_ssid);
            fb_free(resp);
        }
    }

    /* WLAN 5 GHz */
    if (tr_control_url(&st->tr, "urn:dslforum-org:service:WLANConfiguration:2", url, sizeof(url))) {
        if (tr_call(&st->tr, "urn:dslforum-org:service:WLANConfiguration:2", "GetInfo", NULL, &resp) == FB_OK) {
            if (xml_tag(resp, "NewEnable", tmp, sizeof(tmp))) {
                j->wlan50_active = atoi(tmp);
            }
            if (xml_tag(resp, "NewChannel", tmp, sizeof(tmp))) {
                j->wlan50_channel = atoi(tmp);
            }
            xml_tag(resp, "NewSSID", j->wlan50_ssid, sizeof(j->wlan50_ssid));
            utf8_to_latin1(j->wlan50_ssid);
            fb_free(resp);
        }
    }

    /* WLAN Gastzugang */
    if (tr_control_url(&st->tr, "urn:dslforum-org:service:WLANConfiguration:3", url, sizeof(url))) {
        if (tr_call(&st->tr, "urn:dslforum-org:service:WLANConfiguration:3", "GetInfo", NULL, &resp) == FB_OK) {
            if (xml_tag(resp, "NewEnable", tmp, sizeof(tmp))) {
                j->wlangast_active = atoi(tmp);
            }
            xml_tag(resp, "NewSSID", j->wlangast_ssid, sizeof(j->wlangast_ssid));
            utf8_to_latin1(j->wlangast_ssid);
            fb_free(resp);
        }
    }

    /* Heimnetz: Anzahl aus GetHostNumberOfEntries. Wie viele davon online
     * sind, steht in der Host-Liste, die die Box als eine Datei ausliefert -
     * ein Abruf statt einem SOAP-Aufruf pro Geraet. */
    if (tr_call(&st->tr, "urn:dslforum-org:service:Hosts:1",
                "GetHostNumberOfEntries", NULL, &resp) == FB_OK) {
        if (xml_tag(resp, "NewHostNumberOfEntries", tmp, sizeof(tmp))) {
            j->host_count = atoi(tmp);
        }
        fb_free(resp);
    }
    j->hosts_active = count_active_hosts(st);
    dbg_log("Worker: hosts %d, active %d\n", j->host_count, j->hosts_active);

    /* Telefonie vorhanden? */
    j->has_tel = tr_control_url(&st->tr, "urn:dslforum-org:service:X_AVM-DE_OnTel:1", url, sizeof(url)) != 0;

    dbg_log("Worker: tr064_info completed successfully.\n");
}

void job_run(struct JobState *st, struct JobData *j)
{
    j->rc = FB_OK;
    j->error[0] = '\0';
    j->sw = NULL;
    j->swcount = 0;

    switch (j->type) {
    case JOB_CONNECT:
        job_state_free(st);
        fb_session_init(&st->fs, j->host);
        if (j->port > 0) {
            st->fs.port = j->port;
        }
        st->trport = j->trport;
        memcpy(st->pass, j->pass, sizeof(st->pass));
        st->pass[sizeof(st->pass) - 1] = '\0';
        memset(j->pass, 0, sizeof(j->pass));   /* nicht zurueckschicken */

        j->tr_ok = 0;
        j->tr_error[0] = j->model[0] = j->fwver[0] = j->wanip[0] = '\0';
        j->uptime = -1;

        j->rc = fb_login(&st->fs, j->user, st->pass);
        if (j->rc != FB_OK) {
            take_error(j->error, sizeof(j->error));
            memset(st->pass, 0, sizeof(st->pass));
            return;
        }
        st->connected = 1;
        strcpy(j->user_used, st->fs.user);

        /* Fehlen dem Benutzer die Smart-Home-Rechte, ist die Anmeldung
         * trotzdem gelungen - die Meldung kommt mit, rc bleibt FB_OK. */
        if (list_switches(st, j) != FB_OK) {
            take_error(j->error, sizeof(j->error));
        }
        tr064_info(st, j);
        dbg_log("=== FRITZ!Box Daten ===\n");
        dbg_log("Modell: '%s', FW: '%s', Uptime: %ld s\n", j->model, j->fwver, j->uptime);
        dbg_log("WAN IP: '%s', DNS: '%s' / '%s'\n", j->wanip, j->dns1, j->dns2);
        dbg_log("Status: '%s', Speed: Down %lu, Up %lu bps\n", j->conn_status, j->max_down_bps, j->max_up_bps);
        dbg_log("Bytes: Sent %lu, Recv %lu\n", j->bytes_sent, j->bytes_recv);
        dbg_log("Mobilfunk: Vorhanden=%d, Tech='%s', Signal='%s', APN='%s', Roaming='%s'\n",
                j->has_mobile, j->mobile_tech, j->mobile_signal, j->mobile_apn, j->mobile_roaming);
        dbg_log("WLAN 2.4 GHz: Aktiv=%d, SSID='%s', Kanal=%d\n",
                j->wlan24_active, j->wlan24_ssid, j->wlan24_channel);
        dbg_log("WLAN 5 GHz:   Aktiv=%d, SSID='%s', Kanal=%d\n",
                j->wlan50_active, j->wlan50_ssid, j->wlan50_channel);
        dbg_log("WLAN Gast:    Aktiv=%d, SSID='%s'\n",
                j->wlangast_active, j->wlangast_ssid);
        dbg_log("Telefonie:    Vorhanden=%d\n", j->has_tel);
        dbg_log("Heimnetz Geraete: Gesamt=%d, Online=%d\n", j->host_count, j->hosts_active);
        dbg_log("Smart Home Steckdosen: %d gefunden\n", j->swcount);
        if (j->swcount > 0 && j->sw) {
            int k;
            for (k = 0; k < j->swcount; k++) {
                dbg_log("  Steckdose[%d]: AIN='%s', Name='%s', Zustand=%d, Leistung=%ld mW\n",
                        k, j->sw[k].ain, j->sw[k].name, j->sw[k].state, j->sw[k].power_mw);
            }
        }
        dbg_log("=======================\n");
        return;

    case JOB_REFRESH:
        if (!st->connected) {
            j->rc = FB_EAUTH;
            strcpy(j->error, "Nicht angemeldet");
            return;
        }
        j->rc = list_switches(st, j);
        if (j->rc != FB_OK) {
            take_error(j->error, sizeof(j->error));
        }
        return;

    case JOB_SWITCH: {
        const char *cmd = j->on ? "setswitchon" : "setswitchoff";
        int state = -1;

        if (!st->connected) {
            j->rc = FB_EAUTH;
            strcpy(j->error, "Nicht angemeldet");
            return;
        }
        j->rc = fb_switch_set(&st->fs, j->ain, cmd, &state);
        if (j->rc == FB_EAUTH && relogin(st) == FB_OK) {
            j->rc = fb_switch_set(&st->fs, j->ain, cmd, &state);
        }
        if (j->rc != FB_OK) {
            take_error(j->error, sizeof(j->error));
        }
        j->on = state;          /* was die Box danach meldet, -1 unbekannt */
        return;
    }

    case JOB_LOGOUT:
    case JOB_QUIT:
        job_state_free(st);
        return;
    }
}
