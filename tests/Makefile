CPPFLAGS = -I ../

include ../config.mk

TEST_PROGS = test-util \
			 test-shortcut \
			 test-handler \
			 test-file-storage \
			 test-util-file \
			 test-util-completion \
			 test-shortcut-completion \
			 test-map

# The archiving module is built and tested on its own: it doesn't depend on
# GTK or WebKit. webkit-check (not run here: it needs a display and the
# network) loads pages through the proxy in a WebKit view.
ARCHIVE_DIR   = ../$(SRCDIR)/archive
ARCHIVE_LIB   = $(ARCHIVE_DIR)/libvimbarchive.a
ARCHIVE_PKGS  = glib-2.0 gio-2.0 gnutls json-glib-1.0 zlib
ARCHIVE_TESTS = archive/test-archive

all: $(TEST_PROGS) $(ARCHIVE_TESTS)
	$(Q)LD_LIBRARY_PATH="$(LD_LIBRARY_PATH):." gtester --verbose $(TEST_PROGS)
	$(Q)gtester --verbose $(ARCHIVE_TESTS)

archive-lib:
	$(Q)$(MAKE) -C $(ARCHIVE_DIR) libvimbarchive.a

archive/test-archive: archive/test-archive.c archive-lib
	@echo "${CC} $@"
	$(Q)$(CC) -std=c99 -Wall -g -D_POSIX_C_SOURCE=200809L -o $@ $< $(ARCHIVE_LIB) \
		$(shell $(PKG_CONFIG) --cflags --libs $(ARCHIVE_PKGS))

archive/webkit-check: archive/webkit-check.c archive-lib
	@echo "${CC} $@"
	$(Q)$(CC) -std=gnu11 -Wall -g -o $@ $< $(ARCHIVE_LIB) \
		$(shell $(PKG_CONFIG) --cflags --libs webkitgtk-6.0 gtk4 $(ARCHIVE_PKGS))

${TEST_PROGS}: ../$(SRCDIR)/vimb.so

test-%: test-%.c
	@echo "${CC} $@"
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< ../$(SRCDIR)/vimb.so $(LDFLAGS)

clean:
	$(RM) $(TEST_PROGS) $(ARCHIVE_TESTS) archive/webkit-check

.PHONY: all clean archive-lib
