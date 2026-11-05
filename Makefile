CC      ?= cc
CFLAGS  ?= -O2
CFLAGS  += -std=c99 -Wall -Wextra -pedantic
PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin

all: yeshup

yeshup: yeshup.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) -o $@ yeshup.c

# Test build: random hangup between 1 and 2 seconds instead of 2-10 minutes.
yeshup-test: yeshup.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -DYESHUP_MIN_MS=1000u -DYESHUP_MAX_MS=2000u \
		$(LDFLAGS) -o $@ yeshup.c

test: yeshup yeshup-test
	sh ./test.sh

install: yeshup
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 yeshup $(DESTDIR)$(BINDIR)/yeshup

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/yeshup

clean:
	rm -f yeshup yeshup-test

.PHONY: all test install uninstall clean
