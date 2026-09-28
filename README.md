# AmiFritz

Control an AVM FRITZ!Box from a classic Amiga: AmigaOS 3.x, 68020+, no FPU,
any `bsdsocket.library` stack (Roadshow, AmiTCP, Miami). Talks to the box's
official interfaces instead of scraping the web UI: the AHA HTTP interface
for smart home, and later TR-064 (SOAP) for WLAN, DSL, and telephony.

The plan and the protocol details are in [PROJECT.md](PROJECT.md).

## Status

| Phase | What                                       | State |
|-------|--------------------------------------------|-------|
| 1     | TCP + HTTP over bsdsocket (`net.c`)        | done, tested against mock |
| 2     | MD5, `login_sid.lua` login, smart plugs    | done, tested against mock |
| 3     | TR-064 SOAP auth                           | hash function done + verified, SOAP still open |
| 4     | MUI GUI with a separate network process    | open |

Not tested yet on real Amiga hardware or against a real box.

## AmiFritzCLI

```
AmiFritzCLI [-h host] [-P port] [-u user] [-p pass] <command> [arg]
  get <path>          raw HTTP GET, print status line and headers (phase 1)
  login               log in, print SID, log out
  list                list smart plugs: AIN, state, power, name
  on|off|toggle <AIN> switch a smart plug
```

The host defaults to `fritz.box`. Without `-u`, the CLI uses the user the box
reports as last logged in (FRITZ!OS 7.24+ always needs a username). The
password may also come from the environment variable `FRITZPASS`. Put the AIN
in quotes, because it contains a space: `AmiFritzCLI on "08761 0000434"`.

## Building

For the Amiga, cross-compile with
[m68k-amigaos-gcc](https://github.com/AmigaPorts/m68k-amigaos-gcc). This uses
the same profile as AmiHomeassist:

```sh
make                              # AmiFritzCLI
make TOOLCHAIN=/path/to/m68k-amigaos-gcc
```

## Tests without hardware

The network code builds unchanged on Linux and macOS with BSD sockets.
`make test` builds a host binary and checks:

- MD5 against the RFC 1321 vectors
- the TR-064 response against the worked example in PROJECT.md
- the `login_sid.lua` response against AVM's own example (`äbc`)
- the complete CLI against `tests/mockbox.py`, a mocked FRITZ!Box that
  implements `login_sid.lua` and the AHA plug commands

To try the mock by hand, run `python3 tests/mockbox.py 8080`, then
`build/host/AmiFritzCLI -h 127.0.0.1 -P 8080 -p gurkensalat list`.

## Notes

- `login_sid.lua` is called without `version=2`, so the box sends the MD5
  challenge rather than PBKDF2. PBKDF2 would need SHA-256 and thousands of
  iterations, which is too slow on a 68020. AVM still accepts MD5.
- Passwords are ISO-8859-1, the Amiga charset. For the UTF-16LE hash, each
  character becomes `byte, 0x00`.
- HTTP/1.0 with `Connection: close` means the box never answers with chunked
  encoding. The reader stops at `Content-Length` or when the connection
  closes, whichever comes first.
- `bsdsocket.library` is opened per request, because the library base is
  per task. This matters for phase 4, where the network code moves to its own
  process.
