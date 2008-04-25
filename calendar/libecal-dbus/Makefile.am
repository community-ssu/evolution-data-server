INCLUDES =						\
	-DEVOLUTION_LOCALEDIR=\""$(localedir)"\"	\
	-DG_LOG_DOMAIN=\"libecal\"			\
	-I$(top_srcdir)/calendar			\
	-I$(srcdir) -I$(top_srcdir) 			\
	-I. 						\
	-I.. 						\
	-I$(top_builddir)				\
	-I$(top_srcdir)/calendar/libical/src		\
	-I$(top_builddir)/calendar/libical/src		\
	-I$(top_srcdir)/calendar/libical/src/libical	\
	-I$(top_builddir)/calendar/libical/src/libical	\
	$(EVOLUTION_CALENDAR_CFLAGS)

# The marshallers
MARSHAL_GENERATED = e-cal-marshal.c e-cal-marshal.h
@EVO_MARSHAL_RULE@

# The library
lib_LTLIBRARIES = libecal-1.2.la

libecal_1_2_la_SOURCES =		\
	$(MARSHAL_GENERATED)	\
	$(DBUS_GENERATED_H)	\
	e-cal.c			\
	e-cal-component.c	\
	e-cal-recur.c		\
	e-cal-time-util.c	\
	e-cal-util.c		\
	e-cal-view.c		\
	e-cal-view-listener.c

libecal_1_2_la_LIBADD =									\
	$(EVOLUTION_CALENDAR_LIBS)							\
	$(top_builddir)/calendar/libical/src/libical/libical-evolution.la		\
	$(top_builddir)/calendar/libical/src/libicalvcal/libicalvcal-evolution.la	\
	$(top_builddir)/calendar/libical/src/libicalss/libicalss-evolution.la		\
	$(top_builddir)/libedataserver/libedataserver-$(API_VERSION).la

libecal_1_2_la_LDFLAGS = 								\
	-version-info $(LIBECAL_CURRENT):$(LIBECAL_REVISION):$(LIBECAL_AGE) $(NO_UNDEFINED)

libecalincludedir = $(privincludedir)/libecal

libecalinclude_HEADERS =	\
	e-cal.h			\
	e-cal-component.h	\
	e-cal-recur.h		\
	e-cal-time-util.h	\
	e-cal-types.h		\
	e-cal-util.h		\
	e-cal-view.h		\
	e-cal-view-listener.h

%-$(API_VERSION).pc: %.pc
	 cp $< $@

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = libecal-$(API_VERSION).pc

DBUS_GENERATED_H = e-data-cal-factory-bindings.h e-data-cal-bindings.h e-data-cal-view-bindings.h e-data-cal-view-listener-glue.h
%-glue.h: $(top_srcdir)/calendar/libedata-cal-dbus/%.xml
	dbus-binding-tool --mode=glib-server --output=$@ --prefix=$(subst -,_,$*) $^

%-bindings.h: $(top_srcdir)/calendar/libedata-cal-dbus/%.xml
	dbus-binding-tool --mode=glib-client --output=$@ --prefix=$(subst -,_,$*) $^

BUILT_SOURCES = $(DBUS_GENERATED_H) $(MARSHAL_GENERATED)
CLEANFILES = $(BUILT_SOURCES)
DISTCLEANFILES = $(pkgconfig_DATA)

EXTRA_DIST = 						\
	e-cal-marshal.list				\
	$(pkgconfig_DATA:-$(API_VERSION).pc=.pc.in)

dist-hook:
	cd $(distdir); rm -f $(BUILT_SOURCES)

