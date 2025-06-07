CC=gcc
CFLAGS=-Wall -Wextra -std=c99 -O2
LIBS=-lxcb -lxcb-keysyms -lxcb-ewmh -lxcb-icccm -lX11
TARGET=cwm
PREFIX=/usr/local
BINDIR=$(PREFIX)/bin
DATADIR=$(PREFIX)/share
DESKTOPDIR=$(DATADIR)/xsessions

.PHONY: all clean install uninstall

all: $(TARGET)

$(TARGET): cwm.c
	$(CC) $(CFLAGS) -o $(TARGET) cwm.c $(LIBS)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	mkdir -p $(BINDIR)
	mkdir -p $(DESKTOPDIR)
	cp $(TARGET) $(BINDIR)/cwm
	cp cwm.desktop $(DESKTOPDIR)/
	chmod +x $(BINDIR)/cwm

uninstall:
	rm -f $(BINDIR)/cwm
	rm -f $(DESKTOPDIR)/cwm.desktop
