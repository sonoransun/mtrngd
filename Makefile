.PHONY: clean all default install remove
default all: mtrngd

ifeq (,$(BINPREFIX))
  BINPREFIX=/bin
endif
ifeq (,$(ETCPREFIX))
  ETCPREFIX=/etc
endif

ifeq (,$(CFLAGS))
  CFLAGS=-O3 -g -D_GNU_SOURCE -D_CONCURRENT -D_REENTRANT 
endif

BINARY=$(BINPREFIX)/mtrngd
RCFILE=$(ETCPREFIX)/init.d/mtrngd

mtrngd: mtrngd.cpp fips.c fips.h Makefile
	g++ $(CFLAGS) $< -lpthread -o $@

clean:
	@rm -f mtrngd 1>/dev/null 2>/dev/null;true

install:
	@echo "Installing $(BINARY) and $(RCFILE)"
	@cp -f mtrngd $(BINARY) && cp -f rc.mtrngd $(RCFILE)
	@echo "Remember to add $(RCFILE) to your init scripts if necessary"

