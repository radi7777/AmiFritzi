#!/usr/bin/env python3
"""Faehrt das Host-Binary von AmiFritzCLI gegen tests/mockbox.py."""

import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(__file__))
import mockbox  # noqa: E402

cli = sys.argv[1]
srv = mockbox.serve()
port = str(srv.server_address[1])
failures = 0


def run(*args, password="gurkensalat", user=None):
    cmd = [cli, "-h", "127.0.0.1", "-P", port, "-p", password]
    if user:
        cmd += ["-u", user]
    p = subprocess.run(cmd + list(args), capture_output=True, text=True,
                       timeout=30)
    return p.returncode, p.stdout


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

rc, out = run("on", "08761 0000435")
check("switch on", rc == 0 and out.strip() == "08761 0000435: on", out)
check("state stored", mockbox.BOX.switches["08761 0000435"]["state"] == 1, "")

rc, out = run("toggle", "08761 0000434")
check("toggle", rc == 0 and out.strip() == "08761 0000434: off", out)

check("logged out afterwards", not mockbox.BOX.sids, mockbox.BOX.sids)

srv.shutdown()
print("all passed" if not failures else f"{failures} FAILED")
sys.exit(1 if failures else 0)
