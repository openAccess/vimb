# The archiving module: WARC capture through a recording proxy. It depends
# only on GLib/GIO, GnuTLS, json-glib and zlib -- not on GTK or WebKit -- so that it can
# be tested on its own and some day serve another front end.

CC         ?= cc
AR         ?= ar
PKG_CONFIG ?= pkg-config

ifneq ($(V),1)
Q := @
endif

ARCHIVE_LIBS = glib-2.0 gio-2.0 gnutls json-glib-1.0 zlib

CFLAGS ?= -O2 -g
override CFLAGS   += -std=c99 -Wall -Wextra -Wno-unused-parameter -fPIC \
                     $(shell $(PKG_CONFIG) --cflags $(ARCHIVE_LIBS))
override CPPFLAGS += -D_POSIX_C_SOURCE=200809L -DG_LOG_DOMAIN=\"archive\"
LDLIBS             = $(shell $(PKG_CONFIG) --libs $(ARCHIVE_LIBS))

SRC = spool.c http1.c ca.c warc.c recorder.c netcheck.c proxy.c \
      json.c surt.c request_key.c indexer.c zip.c wacz.c replay.c
OBJ = $(SRC:.c=.o)

all: libvimbarchive.a vimb-archive-proxy vimb-archive-pack

libvimbarchive.a: $(OBJ)
	@echo "$(AR) $@"
	$(Q)$(RM) $@
	$(Q)$(AR) rcs $@ $^

vimb-archive-proxy: proxy-main.o libvimbarchive.a
	@echo "$(CC) $@"
	$(Q)$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

vimb-archive-pack: pack-main.o libvimbarchive.a
	@echo "$(CC) $@"
	$(Q)$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c $(wildcard *.h)
	@echo "$(CC) $@"
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

clean:
	$(RM) $(OBJ) proxy-main.o pack-main.o libvimbarchive.a vimb-archive-proxy \
		vimb-archive-pack

.PHONY: all clean
