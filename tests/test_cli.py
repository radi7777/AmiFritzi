#!/usr/bin/env python3
"""Faehrt das Host-Binary von AmiFritzCLI gegen tests/mockbox.py."""

import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(__file__))
import mockbox  # noqa: E402

cli = sys.argv[1]
srv = mockbox.serve()           # login_sid.lua + AHA, sonst Port 80
trsrv = mockbox.serve()         # TR-064, sonst Port 49000
port = str(srv.server_address[1])
trport = str(trsrv.server_address[1])
failures = 0


def run(*args, password="gurkensalat", user=None):
    cmd = [cli, "-h", "127.0.0.1", "-P", port, "-T", trport, "-p", password]
    if user:
        cmd += ["-u", user]
    p = subprocess.run(cmd + list(args), capture_output=True, timeout=30)
    # Das Amiga-Programm gibt ISO-8859-1 aus.
    return p.returncode, p.stdout.decode("latin-1")


def check(name, cond, out):
    global failures
    if cond:
        print(f"ok   {name}")
    else:
        failures += 1
        print(f"FAIL {name}\n{out}")


rc, out = run("get", "/")
check("get / status line", rc == 0 and out.startswith("HTTP/1.1 200"), out)
check("get / body length", "27 bytes body (Content-Length 27)" in out, out)

rc, out = run("login")
check("login, user from <Users last=1>",
      rc == 0 and "'amiga'" in out and mockbox.SID in out, out)

rc, out = run("login", user="amiga")
check("login with explicit user", rc == 0 and mockbox.SID in out, out)

rc, out = run("login", password="falsch")
check("wrong password rejected", rc == 10 and "abgelehnt" in out, out)

rc, out = run("list")
check("list", rc == 0 and "08761 0000434" in out and "42.370 W" in out
      and "Amiga 1200" in out, out)
check("UTF-8 name shown as Latin-1", "Küchenlicht" in out, out)

rc, out = run("on", "08761 0000435")
check("switch on", rc == 0 and out.strip() == "08761 0000435: on", out)
check("state stored", mockbox.BOX.switches["08761 0000435"]["state"] == 1, "")

rc, out = run("toggle", "08761 0000434")
check("toggle", rc == 0 and out.strip() == "08761 0000434: off", out)

check("logged out afterwards", not mockbox.BOX.sids, mockbox.BOX.sids)

# ---- TR-064 ----

mockbox.BOX.soap_posts = 0
rc, out = run("info")
check("tr064 info", rc == 0 and "FRITZ!Box 7590 AX & Mock" in out
      and "154.08.00" in out and "3d 04:05" in out, out)
check("tr064 info: challenge + one auth call", mockbox.BOX.soap_posts == 2,
      mockbox.BOX.soap_posts)

mockbox.BOX.soap_posts = 0
rc, out = run("wanip")
check("tr064 wanip falls back to WANIPConnection",
      rc == 0 and out.strip() == "203.0.113.7", out)
check("tr064 wanip reuses NextChallenge nonce",
      mockbox.BOX.soap_posts == 3, mockbox.BOX.soap_posts)

rc, out = run("soap", "DeviceInfo:1", "GetInfo", user="amiga")
check("tr064 raw soap", rc == 0 and "<NewSoftwareVersion>" in out, out)

rc, out = run("info", password="falsch")
check("tr064 wrong password", rc == 10 and "401" in out, out)

rc, out = run("soap", "Nope:1", "X")
check("tr064 unknown service", rc == 10 and "nicht vorhanden" in out, out)

srv.shutdown()
trsrv.shutdown()
print("all passed" if not failures else f"{failures} FAILED")
sys.exit(1 if failures else 0)
