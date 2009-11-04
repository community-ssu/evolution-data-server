AUTOMAKE_OPTIONS = 1.6
DISTCHECK_CONFIGURE_FLAGS = --enable-gtk-doc

if ! WITH_LIBDB
LIBDB = libdb
endif

if ENABLE_UCHARDET
LIBUCHARDET = libuchardet
endif

if ENABLE_CALENDAR
CALENDAR_DIR = calendar
endif

if ENABLE_DBUS
SUBDIRS = $(LIBDB) $(LIBUCHARDET) libedataserver addressbook docs
else
SUBDIRS = $(LIBDB) $(LIBUCHARDET) libedataserver servers camel addressbook $(CALENDAR_DIR) libedataserverui src docs art po
endif

DIST_SUBDIRS = $(SUBDIRS)

changelogs =			\
	ChangeLog

%-$(API_VERSION).pc: %.pc
	 cp $< $@

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = evolution-data-server-$(API_VERSION).pc

EXTRA_DIST = 						\
	AUTHORS						\
	$(changelogs)					\
	README						\
	HACKING						\
	MAINTAINERS					\
	TODO						\
	NEWS						\
	marshal.mk					\
	iconv-detect.c					\
	$(pkgconfig_DATA:-$(API_VERSION).pc=.pc.in) 	\
	autogen.sh					

DISTCLEANFILES = intltool-extract intltool-merge intltool-update iconv-detect.h $(pkgconfig_DATA)
distclean-local:
	(cd libdb && $(MAKE) $(AM_MAKEFLAGS) distclean)

