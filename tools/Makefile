# SPDX-License-Identifier: GPL-2.0
#
# Build and install the camera tools on the board:
#
#   make              build cam2h264, h264-rtsp and mjpeg-http
#   make install      copy them and the j1-*.sh and j2-*.sh scripts to
#                     $(PREFIX)/bin
#   make uninstall
#
# Run it on the board, not on a PC: a PC's riscv64 compiler can target a
# newer CPU than the SG2000's C906. As root, install needs no sudo. As
# another user, run "sudo make install" or set PREFIX=$HOME/.local.
#
# The scripts find the binaries and each other in their own directory, so
# keep them together. The diagnostic scripts and test programs stay here.
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

CC ?= cc
# The C906's T-Head extensions, when this compiler has them (gcc 13 or later)
THEAD := $(shell $(CC) -mcpu=thead-c906 -x c -c -o /dev/null /dev/null 2>/dev/null && echo -mcpu=thead-c906)
CFLAGS ?= -O3 $(THEAD)

PROGS   = cam2h264 h264-rtsp mjpeg-http
SCRIPTS = j1-stream.sh j1-h264.sh j1-rtsp.sh j1-http.sh \
          j2-stream.sh j2-h264.sh j2-rtsp.sh j2-http.sh

all: $(PROGS)

cam2h264: cam2h264.c
	$(CC) $(CFLAGS) -o $@ $< -lm

h264-rtsp: h264-rtsp.c
	$(CC) $(CFLAGS) -o $@ $<

mjpeg-http: mjpeg-http.c
	$(CC) $(CFLAGS) -o $@ $<

install: all
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(PROGS) $(SCRIPTS) $(DESTDIR)$(BINDIR)

uninstall:
	cd $(DESTDIR)$(BINDIR) && rm -f $(PROGS) $(SCRIPTS)

clean:
	rm -f $(PROGS)

.PHONY: all install uninstall clean
