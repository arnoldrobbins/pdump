# makefile for "pdump"

CFLAGS= -O

DESTDIR=/usr/local/bin

pdump: pdump.c
	$(CC) pdump.c $(CFLAGS) $(LDFLAGS) -lrmt -o pdump

install: pdump
	cp pdump $(DESTDIR)
	/etc/chown admin $(DESTDIR)/pdump
	chgrp admin $(DESTDIR)/pdump
	chmod 711 $(DESTDIR)/pdump
	cp pdump.1 /usr/man/manl/pdump.l

print:
	pr pdump.c makefile pdump.1 | lpr -b pdump
