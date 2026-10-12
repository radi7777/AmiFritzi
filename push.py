#!/usr/bin/env python3
"""Deploy-Skript fuer AmiFritzi -> Amiga ueber amiagent / amimcp."""

import os
import socket
import sys

# Rechner, Token und Pfad zu amimcp stehen NICHT im Repository, sondern in
# push_local.py (per .gitignore ausgenommen) oder in Umgebungsvariablen:
#   AMIGA_HOSTS="192.168.x.y,192.168.x.z"  AMIGA_TOKEN=...  AMIMCP_PATH=...
try:
    import push_local as _local  # noqa: E402
except ImportError:
    _local = None


def _cfg(name, env, default=None):
    if os.environ.get(env):
        return os.environ[env]
    return getattr(_local, name, default)


CANDIDATES = [h for h in str(_cfg("HOSTS", "AMIGA_HOSTS", "")).split(",") if h]
PORT = int(_cfg("PORT", "AMIGA_PORT", 7846))
TOKEN = _cfg("TOKEN", "AMIGA_TOKEN")
DEFAULT_DEST = "RAM:AmiFritzi"

sys.path.insert(0, _cfg("AMIMCP_PATH", "AMIMCP_PATH", ""))
from amiga import Amiga  # noqa: E402


def detect_host():
    env_host = os.environ.get("AMIGA_HOST")
    if env_host:
        return env_host
    for host in CANDIDATES:
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(0.6)
            s.connect((host, PORT))
            s.close()
            return host
        except Exception:
            continue
    if not CANDIDATES:
        sys.exit("push.py: kein Amiga - AMIGA_HOSTS oder push_local.py setzen")
    return CANDIDATES[0]


def main(argv):
    files = []
    run = None
    dest_dir = DEFAULT_DEST
    host = detect_host()

    i = 0
    while i < len(argv):
        if argv[i] == "--run":
            i += 1
            run = argv[i]
        elif argv[i] == "--dest":
            i += 1
            dest_dir = argv[i]
        elif argv[i] == "--host":
            i += 1
            host = argv[i]
        else:
            files.append(argv[i])
        i += 1

    if not TOKEN:
        sys.exit("push.py: kein Token - AMIGA_TOKEN oder push_local.py setzen")
    a = Amiga(host, PORT, TOKEN)
    print("Verbinde mit Amiga %s:%d..." % (host, PORT))

    if files:
        a.exec_command("MakeDir >NIL: %s" % dest_dir)
        for f in files:
            dest = "%s/%s" % (dest_dir, os.path.basename(f))
            with open(f, "rb") as fh:
                data = fh.read()
            a.write_file(dest, data)
            print("%6d  ->  %s" % (len(data), dest))

    if run:
        print("Fuehre aus: %s" % run)
        rc, out = a.exec_command(run, timeout=300)
        print("rc=%d" % rc)
        if out:
            print(out)


if __name__ == "__main__":
    main(sys.argv[1:])
