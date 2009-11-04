AM_CFLAGS = -Wall
AM_CXXFLAGS = -Wall -fno-rtti -fno-exceptions -DMAEMO_CHANGES=1

noinst_PROGRAMS = test_libuchardet test_charsets

test_libuchardet_SOURCES = test_libuchardet.cpp
test_libuchardet_CPPFLAGS = $(AM_CPPFLAGS) -I$(top_srcdir)/libuchardet/src $(UCHARDET_CFLAGS)
test_libuchardet_LDADD = $(top_builddir)/libuchardet/src/libuchardet.a

test_charsets_SOURCES = test_charsets.c
test_charsets_CPPFLAGS = $(AM_CPPFLAGS) $(GLIB_CFLAGS)
test_charsets_LDADD = $(GLIB_LIBS)

TESTS = test_charsets
