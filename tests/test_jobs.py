#!/usr/bin/env python3
"""Startet die Mock-Box und laesst tests/test_jobs.c dagegen laufen."""

import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(__file__))
import mockbox  # noqa: E402

aha = mockbox.serve()
tr = mockbox.serve()
p = subprocess.run([sys.argv[1], str(aha.server_address[1]),
                    str(tr.server_address[1])], timeout=60)
aha.shutdown()
tr.shutdown()
sys.exit(p.returncode)
