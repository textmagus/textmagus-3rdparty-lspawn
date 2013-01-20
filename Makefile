# Copyright 2012-2013 Mitchell mitchell.att.foicica.com. See LICENSE.

ta_src = /home/mitchell/code/textadept/src

kernel = $(shell uname -s)
ifneq (, $(or $(findstring Linux, $(kernel)), $(findstring BSD, $(kernel))))
  ifeq (win, $(findstring win, $(MAKECMDGOALS)))
    ifeq (win32, $(MAKECMDGOALS))
      CROSS = i686-w64-mingw32-
    else ifeq (win64, $(MAKECMDGOALS))
      CROSS = x86_64-w64-mingw32-
    endif
    CC = gcc
    CFLAGS = -mms-bitfields

    prefix = $(ta_src)/$(MAKECMDGOALS)
    gtk_flags = $(shell PKG_CONFIG_PATH=$(prefix)gtk/lib/pkgconfig \
                        pkg-config --define-variable=prefix=$(prefix)gtk \
                        --cflags gtk+-2.0)
    gtk_libs = $(shell PKG_CONFIG_PATH=$(prefix)gtk/lib/pkgconfig \
                        pkg-config --define-variable=prefix=$(prefix)gtk \
                        --libs gtk+-2.0)
    lua_libs = -L. -llua52

    target = spawn.dll
  else ifeq (osx, $(findstring osx, $(MAKECMDGOALS)))
    CROSS = i686-apple-darwin10-
    CC = gcc
    CFLAGS = -m32 -arch i386 -mmacosx-version-min=10.5 \
             -isysroot /usr/lib/apple/SDKs/MacOSX10.5.sdk \
             -undefined dynamic_lookup

    gtk_flags = $(shell PKG_CONFIG_PATH=$(ta_src)/gtkosx/lib/pkgconfig \
                        pkg-config --define-variable=prefix=$(ta_src)/gtkosx \
                        --cflags gtk+-2.0)

    target = spawn.so
  else
    CC = gcc
    CFLAGS = -fpic

    ifndef GTK3
      gtk_version = 2.0
    else
      gtk_version = 3.0
    endif
    gtk_flags = $(shell pkg-config --cflags gtk+-$(gtk_version))

    target = spawn.so.$(shell uname -i)
  endif
endif

# Build.

all: $(target)
win32: $(target)
osx: $(target)
	mv $^ $(target).osx
lspawn.o: lspawn.c
	$(CROSS)$(CC) $(CFLAGS) -c $(gtk_flags) -I$(ta_src)/lua/src -o $@ $^
$(target): lspawn.o
	$(CROSS)$(CC) $(CFLAGS) -shared -o $@ $^ $(gtk_libs) $(lua_libs)
mostlyclean:
	rm -f lspawn.o
clean:
	rm -f lspawn.o spawn.so

# Documentation.

adeptsense: spawn.luadoc
	luadoc -d . --doclet adeptsensedoc $^

# Package.

pkg_dir = lspawn_$(VERSION)

release: | spawn.so.x86_64 spawn.so.i386 spawn.so.osx spawn.dll
	mkdir $(pkg_dir)
	cp $| Makefile spawn.luadoc tags api $(pkg_dir)
	zip -r releases/$(pkg_dir).zip $| Makefile spawn.luadoc tags api
	rm -r $(pkg_dir)
