#!/usr/bin/env python3
"""Nachgebaute FRITZ!Box fuer Tests ohne Hardware.

Kann login_sid.lua (MD5-Challenge, UTF-16LE), die AHA-Befehle fuer
Steckdosen und TR-064 mit AVMs SOAP-Auth (DeviceInfo, WANPPPConnection,
WANIPConnection, Hosts samt Host-Liste). Benutzer 'amiga', Passwort 'gurkensalat'. Absichtlich
streng: falsche SID -> 403, jede TR-064-Nonce gilt genau einmal, und die
Aufforderung zur Anmeldung kommt wie bei manchen Firmwares als Fault 503.

    python3 tests/mockbox.py [ahaport [tr064port]]
"""

import hashlib
import re
import secrets
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

USER = "amiga"
PASSWORD = "gurkensalat"
CHALLENGE = "1a2b3c4d"
SID = "a1b2c3d4e5f60718"
NOSID = "0000000000000000"
REALM = "F!Box SOAP-Auth"
AUTH_NS = "http://soap-authentication.org/digest/2001/10/"

SERVICES = {
    "urn:dslforum-org:service:DeviceInfo:1": "/upnp/control/deviceinfo",
    "urn:dslforum-org:service:WANPPPConnection:1":
        "/upnp/control/wanpppconn1",
    "urn:dslforum-org:service:WANIPConnection:1":
        "/upnp/control/wanipconnection1",
    "urn:dslforum-org:service:Hosts:1": "/upnp/control/hosts",
}

HOSTLIST_PATH = "/devicehostlist.lua?sid=b0b0b0b0b0b0b0b0"
# Drei Geraete, zwei davon online.
HOSTS = [("amiga1200", 1), ("drucker", 0), ("tf536", 1)]


class Box:
    def __init__(self):
        self.lock = threading.Lock()
        self.sids = set()
        self.switches = {
            "08761 0000434": {"name": "Stehlampe", "state": 1, "power": 42370},
            "08761 0000435": {"name": "Amiga 1200", "state": 0, "power": 0},
            "08761 0000436": {"name": "Küchenlicht", "state": 0, "power": 0},
        }
        self.nonce = None
        self.soap_posts = 0


BOX = Box()


def session_xml(sid):
    return (
        '<?xml version="1.0" encoding="utf-8"?><SessionInfo>'
        f"<SID>{sid}</SID><Challenge>{CHALLENGE}</Challenge>"
        "<BlockTime>0</BlockTime><Rights></Rights>"
        '<Users><User>admin</User><User last="1">amiga</User></Users>'
        "</SessionInfo>"
    )


def expected_response():
    h = hashlib.md5(f"{CHALLENGE}-{PASSWORD}".encode("utf-16-le")).hexdigest()
    return f"{CHALLENGE}-{h}"


def md5(s):
    return hashlib.md5(s.encode("utf-8")).hexdigest()


def tr64desc():
    svc = "".join(
        f"<service><serviceType>{t}</serviceType>"
        f"<serviceId>urn:x:{i}</serviceId><controlURL>{u}</controlURL>"
        f"<eventSubURL>/x</eventSubURL><SCPDURL>/x.xml</SCPDURL></service>"
        for i, (t, u) in enumerate(SERVICES.items()))
    return ('<?xml version="1.0"?><root xmlns="urn:dslforum-org:device-1-0">'
            "<device><deviceType>urn:dslforum-org:device:"
            "InternetGatewayDevice:1</deviceType>"
            f"<serviceList>{svc}</serviceList></device></root>")


def envelope(header, body):
    return ('<?xml version="1.0"?><s:Envelope '
            'xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
            's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">'
            f"<s:Header>{header}</s:Header><s:Body>{body}</s:Body>"
            "</s:Envelope>")


def challenge(tag, status):
    BOX.nonce = secrets.token_hex(8).upper()
    return (f'<h:{tag} xmlns:h="{AUTH_NS}" s:mustUnderstand="1">'
            f"<Status>{status}</Status><Nonce>{BOX.nonce}</Nonce>"
            f"<Realm>{REALM}</Realm></h:{tag}>")


def fault(code, text):
    return ("<s:Fault><faultcode>s:Client</faultcode>"
            "<faultstring>UPnPError</faultstring><detail>"
            '<UPnPError xmlns="urn:dslforum-org:control-1-0">'
            f"<errorCode>{code}</errorCode>"
            f"<errorDescription>{text}</errorDescription>"
            "</UPnPError></detail></s:Fault>")


def tag(xml, name):
    m = re.search(f"<{name}>(.*?)</{name}>", xml, re.S)
    return m.group(1) if m else None


def action_result(service, action):
    u = f'<u:{action}Response xmlns:u="{service}">'
    if action == "GetInfo" and service.endswith("DeviceInfo:1"):
        return (u + "<NewManufacturerName>AVM</NewManufacturerName>"
                "<NewModelName>FRITZ!Box 7590 AX &amp; Mock</NewModelName>"
                "<NewSoftwareVersion>154.08.00</NewSoftwareVersion>"
                f"<NewUpTime>{3 * 86400 + 4 * 3600 + 5 * 60}</NewUpTime>"
                f"</u:{action}Response>")
    if action == "GetExternalIPAddress":
        # Kabelbox: PPP ist unbenutzt, die Adresse steht bei WANIP.
        ip = "0.0.0.0" if "PPP" in service else "203.0.113.7"
        return (u + f"<NewExternalIPAddress>{ip}</NewExternalIPAddress>"
                f"</u:{action}Response>")
    if action == "GetHostNumberOfEntries":
        return (u + f"<NewHostNumberOfEntries>{len(HOSTS)}"
                f"</NewHostNumberOfEntries></u:{action}Response>")
    if action == "X_AVM-DE_GetHostListPath":
        return (u + "<NewX_AVM-DE_HostListPath>"
                + HOSTLIST_PATH.replace("&", "&amp;")
                + f"</NewX_AVM-DE_HostListPath></u:{action}Response>")
    return None


def hostlist():
    items = "".join(
        f"<Item><Index>{i + 1}</Index><HostName>{n}</HostName>"
        f"<Active>{a}</Active></Item>"
        for i, (n, a) in enumerate(HOSTS))
    return f'<?xml version="1.0"?><List>{items}</List>'


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"   # Content-Length wird gesetzt

    def log_message(self, *a):
        pass

    def reply(self, status, body, ctype="text/plain"):
        data = body.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        if u.path == "/":
            return self.reply(200, "<html>FRITZ!Box Mock</html>", "text/html")
        if u.path == "/login_sid.lua":
            return self.login(q)
        if u.path == "/webservices/homeautoswitch.lua":
            return self.aha(q)
        if u.path == "/tr64desc.xml":
            return self.reply(200, tr64desc(), "text/xml")
        if self.path == HOSTLIST_PATH:
            return self.reply(200, hostlist(), "text/xml")
        if u.path == "/__test/expire":      # SIDs verfallen lassen
            with BOX.lock:
                BOX.sids.clear()
            return self.reply(200, "ok")
        return self.reply(404, "not found")

    def do_POST(self):
        body = self.rfile.read(int(self.headers["Content-Length"])).decode()
        service, _, action = self.headers.get("SOAPACTION", "").strip(
            '"').partition("#")
        if SERVICES.get(service) != self.path:
            return self.reply(404, "not found")
        with BOX.lock:
            BOX.soap_posts += 1
            auth = tag(body, "Auth")
            if auth is None:            # InitChallenge
                return self.reply(500, envelope(
                    challenge("Challenge", "Unauthenticated"),
                    fault(503, "Auth. failed")), "text/xml")
            ok = (tag(body, "UserID") == USER
                  and BOX.nonce is not None
                  and tag(body, "Nonce") == BOX.nonce
                  and auth == md5(md5(f"{USER}:{REALM}:{PASSWORD}")
                                  + ":" + BOX.nonce))
            if not ok:
                return self.reply(500, envelope(
                    challenge("Challenge", "Unauthenticated"),
                    fault(401, "Invalid Action")), "text/xml")
            result = action_result(service, action)
            hdr = challenge("NextChallenge", "Authenticated")
            if result is None:
                return self.reply(500, envelope(
                    hdr, fault(401, "Invalid Action")), "text/xml")
            return self.reply(200, envelope(hdr, result), "text/xml")

    def login(self, q):
        with BOX.lock:
            if "logout" in q:
                BOX.sids.discard(q.get("sid"))
                return self.reply(200, session_xml(NOSID), "text/xml")
            if "response" in q:
                ok = (q.get("username") == USER
                      and q["response"] == expected_response())
                if ok:
                    BOX.sids.add(SID)
                return self.reply(200, session_xml(SID if ok else NOSID),
                                  "text/xml")
            return self.reply(200, session_xml(NOSID), "text/xml")

    def aha(self, q):
        with BOX.lock:
            if q.get("sid") not in BOX.sids:
                return self.reply(403, "Forbidden")
            cmd = q.get("switchcmd")
            if cmd == "getswitchlist":
                return self.reply(200, ",".join(BOX.switches) + "\n")
            sw = BOX.switches.get(q.get("ain", ""))
            if sw is None:
                return self.reply(400, "Bad Request")
            if cmd == "getswitchname":
                return self.reply(200, sw["name"] + "\n")
            if cmd == "getswitchstate":
                return self.reply(200, f"{sw['state']}\n")
            if cmd == "getswitchpower":
                return self.reply(200, f"{sw['power'] if sw['state'] else 0}\n")
            if cmd in ("setswitchon", "setswitchoff", "setswitchtoggle"):
                sw["state"] = {"setswitchon": 1, "setswitchoff": 0}.get(
                    cmd, 1 - sw["state"])
                return self.reply(200, f"{sw['state']}\n")
            return self.reply(400, "Bad Request")


def serve(port=0):
    srv = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


if __name__ == "__main__":
    a = serve(int(sys.argv[1]) if len(sys.argv) > 1 else 8080)
    t = serve(int(sys.argv[2]) if len(sys.argv) > 2 else 49000)
    print(f"Mock FRITZ!Box: AHA on 127.0.0.1:{a.server_address[1]}, "
          f"TR-064 on 127.0.0.1:{t.server_address[1]}")
    threading.Event().wait()
