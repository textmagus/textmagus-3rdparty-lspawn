# Copyright 2012-2014 Mitchell mitchell.att.foicica.com. See LICENSE.

CC = gcc
CFLAGS = -fpic -Wall -Wno-unused-variable
ifdef GLIB
  plat_flags = -DGTK $(shell pkg-config --cflags glib-2.0)
  plat_libs = $(shell pkg-config --libs glib-2.0)
else
  plat_flags = -D_XOPEN_SOURCE_EXTENDED
endif

all: spawn.so
lspawn.o: lspawn.c ; $(CC) $(CFLAGS) -c $(plat_flags) -o $@ $^
spawn.so: lspawn.o ; $(CC) $(CFLAGS) -shared -o $@ $^ $(plat_libs)
clean: ; rm -f lspawn.o spawn.so
