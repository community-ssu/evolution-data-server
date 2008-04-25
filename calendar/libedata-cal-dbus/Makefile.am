INCLUDES =						\
	-DG_LOG_DOMAIN=\"libedata-cal\"			\
	-I$(top_srcdir)					\
	-I$(top_builddir)				\
	-I$(top_srcdir)/calendar			\
	-I$(top_builddir)/calendar			\
	-I$(top_srcdir)/calendar/libical/src		\
	-I$(top_builddir)/calendar/libical/src		\
	-I$(top_srcdir)/calendar/libical/src/libical	\
	-I$(top_builddir)/calendar/libical/src/libical	\
	$(EVOLUTION_CALENDAR_CFLAGS) \
	-DDBUS_API_SUBJECT_TO_CHANGE

MARSHAL_GENERATED = e-data-cal-marshal.c e-data-cal-marshal.h
@EVO_MARSHAL_RULE@

DBUS_GENERATED_H = e-data-cal-glue.h e-data-cal-factory-glue.h e-data-cal-view-glue.h e-data-cal-view-listener-bindings.h

include glib-gen.mak
glib_enum_headers=e-data-cal-types.h
glib_enum_define=E_DATA_CAL
glib_enum_prefix=e_data_cal

ENUM_GENERATED = e-data-cal-enumtypes.h e-data-cal-enumtypes.c

# The libraray
lib_LTLIBRARIES = libedata-cal-1.2.la

libedata_cal_1_2_la_SOURCES =		\
	$(MARSHAL_GENERATED)				\
	$(DBUS_GENERATED_H)				\
	e-cal-backend.c			\
	e-cal-backend-cache.c		\
	e-cal-backend-factory.c		\
	e-cal-backend-sexp.c		\
	e-cal-backend-sync.c		\
	e-cal-backend-util.c		\
	e-data-cal.c			\
	e-data-cal-view.c		\
	$(ENUM_GENERATED)

libedata_cal_1_2_la_LIBADD =					\
	$(EVOLUTION_CALENDAR_LIBS)				\
	$(top_builddir)/calendar/libecal-dbus/libecal-$(API_VERSION).la		\
	$(top_builddir)/libedataserver/libedataserver-$(API_VERSION).la

libedata_cal_1_2_la_LDFLAGS = 								\
	-version-info $(LIBEDATACAL_CURRENT):$(LIBEDATACAL_REVISION):$(LIBEDATACAL_AGE) $(NO_UNDEFINED) \
	`pkg-config --libs dbus-glib-1`

libedata_calincludedir = $(privincludedir)/libedata-cal

libedata_calinclude_HEADERS = 		\
	e-cal-backend.h			\
	e-cal-backend-cache.h		\
	e-cal-backend-factory.h		\
	e-cal-backend-sync.h		\
	e-cal-backend-util.h		\
	e-cal-backend-sexp.h		\
	e-data-cal.h			\
	e-data-cal-common.h		\
	e-data-cal-factory.h		\
	e-data-cal-view.h		\
	e-data-cal-types.h

%-$(API_VERSION).pc: %.pc
	 cp $< $@

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = libedata-cal-$(API_VERSION).pc

service_in_files = org.gnome.evolution.dataserver.Calendar.service.in
servicedir = $(datadir)/dbus-1/services
service_DATA = $(service_in_files:.service.in=.service)
@EVO_SUBST_SERVICE_RULE@

BUILT_SOURCES = $(DBUS_GENERATED_H) $(MARSHAL_GENERATED)
CLEANFILES = $(BUILT_SOURCES)
DISTCLEANFILES = $(pkgconfig_DATA) $(service_DATA)

EXTRA_DIST = 						\
	e-data-cal-marshal.list				\
	e-data-cal-factory.xml				\
	e-data-cal-view-listener.xml			\
	e-data-cal-view.xml				\
	e-data-cal.xml					\
	$(pkgconfig_DATA:-$(API_VERSION).pc=.pc.in)	\
	$(service_in_files)

dist-hook:
	cd $(distdir); rm -f $(BUILT_SOURCES)

%-glue.h: %.xml
	dbus-binding-tool --mode=glib-server --output=$@ --prefix=$(subst -,_,$*) $^

%-bindings.h: %.xml
	dbus-binding-tool --mode=glib-client --output=$@ --prefix=$(subst -,_,$*) $^


DBUS_BINDINGS = e-data-cal-factory-bindings.h e-data-cal-bindings.h e-data-cal-view-bindings.h
BUILT_SOURCES += $(DBUS_BINDINGS) $(ENUM_GENERATED)

factorydir = $(libexecdir)
factory_PROGRAMS = e-calendar-factory

e_calendar_factory_SOURCES = e-data-cal-factory.c e-data-cal-factory.h
e_calendar_factory_LDADD = libedata-cal-1.2.la

