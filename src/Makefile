include ../config.mk

OBJ = $(patsubst %.c, %.o, $(wildcard *.c))
ARCHIVE_LIB = archive/libvimbarchive.a
JSFILES  = $(wildcard scripts/*.js)
CSSFILES = $(wildcard scripts/*.css)

all: vimb webextension.subdir-all

clean: webextension.subdir-clean archive.subdir-clean
	$(RM) vimb vimb.so $(OBJ)
	$(RM) scripts/scripts.h

vimb: $(OBJ) $(ARCHIVE_LIB)
	@echo "${CC} $@"
	$(Q)$(CC) $(OBJ) $(ARCHIVE_LIB) $(LDFLAGS) -o $@

vimb.so: $(OBJ) $(ARCHIVE_LIB)
	$(Q)$(CC) -shared $(OBJ) $(ARCHIVE_LIB) $(LDFLAGS) -o $@

# The archiving module builds with its own flags (see archive/Makefile).
$(ARCHIVE_LIB): archive-lib
archive-lib:
	$(Q)$(MAKE) -C archive libvimbarchive.a

$(OBJ): config.h ../config.mk

main.o: ../version.h

archiving.o: ../version.h $(wildcard archive/*.h)

input.o: scripts/scripts.h

normal.o: scripts/scripts.h

setting.o: scripts/scripts.h

scripts/scripts.h: $(JSFILES) $(CSSFILES)
	$(Q)$(RM) $@
	@echo "create $@ from *.{css,js}"
	$(Q)for file in $(JSFILES) $(CSSFILES); do \
		./scripts/js2h.sh $$file >> $@; \
	done

config.h:
	@echo create $@ from config.def.h
	$(Q)cp config.def.h $@

%.o: %.c
	@echo "${CC} $@"
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

%.subdir-all: config.h
	$(Q)$(MAKE) -C $*

%.subdir-clean:
	$(Q)$(MAKE) -C $* clean

-include $(OBJ:.o=.d)

.PHONY: all clean archive-lib
