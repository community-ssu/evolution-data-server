AM_CXXFLAGS = -Wall

noinst_PROGRAMS = test_libuchardet

test_libuchardet_SOURCES = test_libuchardet.cpp
test_libuchardet_CPPFLAGS = $(AM_CPPFLAGS) -I$(top_srcdir)/libuchardet/src $(UCHARDET_CFLAGS)
test_libuchardet_LDADD = $(top_builddir)/libuchardet/src/libuchardet.a

