# AmiFritz - Cross-Build mit m68k-amigaos-gcc (AmigaPorts-Toolchain),
# gleiches Zielprofil wie AmiHomeassist: 68020 aufwaerts, ohne FPU.
#
#   make            -> AmiFritz (MUI) + AmiFritzCLI
#   make test       -> baut fuer den Linux-Host und prueft MD5, Login-
#                      Rechnungen, das CLI und die Auftragslogik des
#                      Netzwerkprozesses gegen tests/mockbox.py
#
# Die MUI-Entwicklerheader liegen NICHT im Repository (MUI-Lizenz), sie
# gehoeren nach vendor/mui/include/ - wie bei AmiHomeassist.
#
# Bewusst KEIN -lamiga: dessen sprintf ist die RawDoFmt-Fassung mit
# 16-Bit-%d und gewinnt sonst gegen die von libnix (siehe AmiHomeassist).
# Aus amiga.lib kommt nur DoMethod() als einzelnes Objekt.

TOOLCHAIN ?= $(HOME)/opt/m68k-amigaos-gcc-16.2
CC         = $(TOOLCHAIN)/bin/m68k-amigaos-gcc
AR         = $(TOOLCHAIN)/bin/m68k-amigaos-ar
AMIGALIB   = $(TOOLCHAIN)/m68k-amigaos/lib/libamiga.a
CPU        = 68020

CFLAGS  = -mcpu=$(CPU) -Os -fomit-frame-pointer -noixemul -MMD -MP \
          -Wall -Wextra -Wno-unused-parameter -Wno-pointer-sign \
          -Ivendor/mui/include -I.
LDFLAGS = -noixemul -s -lgcc

CORE_SRCS = net.c md5.c fritz.c tr064.c mem.c
CORE_OBJS = $(CORE_SRCS:.c=.o)
CLI_OBJS  = $(CORE_OBJS) cli.o
# muistubs.c MUSS eine eigene Uebersetzungseinheit bleiben - siehe dort.
GUI_OBJS  = $(CORE_OBJS) jobs.o worker.o gui.o muistubs.o build/DoMethod.o

all: AmiFritz AmiFritzCLI

AmiFritz: $(GUI_OBJS)
	$(CC) $(CFLAGS) -o $@ $(GUI_OBJS) $(LDFLAGS)

AmiFritzCLI: $(CLI_OBJS)
	$(CC) $(CFLAGS) -o $@ $(CLI_OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

build/DoMethod.o: $(AMIGALIB)
	@mkdir -p build
	cd build && $(AR) x $(AMIGALIB) DoMethod.o

# ---- Host-Build fuer Tests (normales gcc/cc) ----

HOSTCC     ?= cc
HOSTCFLAGS  = -O2 -Wall -Wextra -Wno-unused-parameter -I.

build/host/AmiFritzCLI: $(CORE_SRCS) cli.c *.h
	@mkdir -p build/host
	$(HOSTCC) $(HOSTCFLAGS) -o $@ $(CORE_SRCS) cli.c

build/host/test_crypto: tests/test_crypto.c $(CORE_SRCS) *.h
	@mkdir -p build/host
	$(HOSTCC) $(HOSTCFLAGS) -o $@ tests/test_crypto.c $(CORE_SRCS)

build/host/test_jobs: tests/test_jobs.c jobs.c $(CORE_SRCS) *.h
	@mkdir -p build/host
	$(HOSTCC) $(HOSTCFLAGS) -o $@ tests/test_jobs.c jobs.c $(CORE_SRCS)

test: build/host/test_crypto build/host/AmiFritzCLI build/host/test_jobs
	build/host/test_crypto
	python3 tests/test_cli.py build/host/AmiFritzCLI
	python3 tests/test_jobs.py build/host/test_jobs

-include $(GUI_OBJS:.o=.d) cli.d

clean:
	rm -rf *.o *.d build AmiFritz AmiFritzCLI

.PHONY: all test clean
