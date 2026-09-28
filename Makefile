# AmiFritz - Cross-Build mit m68k-amigaos-gcc (AmigaPorts-Toolchain),
# gleiches Zielprofil wie AmiHomeassist: 68020 aufwaerts, ohne FPU.
#
#   make            -> AmiFritzCLI (Amiga-Binary)
#   make test       -> baut fuer den Linux-Host und prueft MD5, Login-
#                      Rechnungen und das CLI gegen tests/mockbox.py
#
# Bewusst KEIN -lamiga: dessen sprintf ist die RawDoFmt-Fassung mit
# 16-Bit-%d und gewinnt sonst gegen die von libnix (siehe AmiHomeassist).

TOOLCHAIN ?= $(HOME)/opt/m68k-amigaos-gcc-16.2
CC         = $(TOOLCHAIN)/bin/m68k-amigaos-gcc
CPU        = 68020

CFLAGS  = -mcpu=$(CPU) -Os -fomit-frame-pointer -noixemul -MMD -MP \
          -Wall -Wextra -Wno-unused-parameter -Wno-pointer-sign -I.
LDFLAGS = -noixemul -s -lgcc

CORE_SRCS = net.c md5.c fritz.c
CLI_OBJS  = $(CORE_SRCS:.c=.o) cli.o

all: AmiFritzCLI

AmiFritzCLI: $(CLI_OBJS)
	$(CC) $(CFLAGS) -o $@ $(CLI_OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# ---- Host-Build fuer Tests (normales gcc/cc) ----

HOSTCC     ?= cc
HOSTCFLAGS  = -O2 -Wall -Wextra -Wno-unused-parameter -I.

build/host/AmiFritzCLI: $(CORE_SRCS) cli.c *.h
	@mkdir -p build/host
	$(HOSTCC) $(HOSTCFLAGS) -o $@ $(CORE_SRCS) cli.c

build/host/test_crypto: tests/test_crypto.c md5.c fritz.c net.c *.h
	@mkdir -p build/host
	$(HOSTCC) $(HOSTCFLAGS) -o $@ tests/test_crypto.c md5.c fritz.c net.c

test: build/host/test_crypto build/host/AmiFritzCLI
	build/host/test_crypto
	python3 tests/test_cli.py build/host/AmiFritzCLI

-include $(CLI_OBJS:.o=.d)

clean:
	rm -rf *.o *.d build AmiFritzCLI

.PHONY: all test clean
