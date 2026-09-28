#!/usr/bin/env python3
"""Nachgebaute FRITZ!Box fuer Tests ohne Hardware.

Kann login_sid.lua (MD5-Challenge, UTF-16LE) und die AHA-Befehle fuer
Steckdosen. Benutzer 'amiga', Passwort 'gurkensalat'. Absichtlich streng:
falsche SID -> 403, wie die echte Box.

    python3 tests/mockbox.py [port]
"""

import hashlib
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

USER = "amiga"
PASSWORD = "gurkensalat"
CHALLENGE = "1a2b3c4d"
SID = "a1b2c3d4e5f60718"
NOSID = "0000000000000000"


class Box:
    def __init__(self):
        self.lock = threading.Lock()
        self.sids = set()
        self.switches = {
            "08761 0000434": {"name": "Stehlampe", "state": 1, "power": 42370},
            "08761 0000435": {"name": "Amiga 1200", "state": 0, "power": 0},
        }


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
        return self.reply(404, "not found")

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
    s = serve(int(sys.argv[1]) if len(sys.argv) > 1 else 8080)
    print(f"Mock FRITZ!Box on 127.0.0.1:{s.server_address[1]}")
    threading.Event().wait()
