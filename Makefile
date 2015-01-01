# Copyright 2012-2015 Mitchell mitchell.att.foicica.com. See LICENSE.

CC = gcc
lspawn_flags = -std=c99 -pedantic -fpic -D_XOPEN_SOURCE -Wall
ifdef GLIB
  plat_flags = -DGTK $(shell pkg-config --cflags glib-2.0)
  plat_libs = $(shell pkg-config --libs glib-2.0)
endif

all: spawn.so
lspawn.o: lspawn.c ; $(CC) -c $(CFLAGS) $(lspawn_flags) $(plat_flags) -o $@ $^
spawn.so: lspawn.o ; $(CC) -shared $(CFLAGS) -o $@ $^ $(plat_libs)
clean: ; rm -f lspawn.o spawn.so
