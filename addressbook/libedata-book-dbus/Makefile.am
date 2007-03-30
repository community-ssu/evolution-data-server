INCLUDES =						\
	-DG_LOG_DOMAIN=\"libedata-book\"		\
	-DLIBEXECDIR=\"$(libexecdir)\"			\
	-I$(top_srcdir)					\
	-I$(top_builddir)				\
	-I$(top_srcdir)/addressbook			\
	-I$(top_builddir)/addressbook			\
	$(EVOLUTION_ADDRESSBOOK_CFLAGS)			\
	-DDBUS_API_SUBJECT_TO_CHANGE

# The library
lib_LTLIBRARIES = libedata-book-1.2.la

DBUS_GENERATED_H = e-data-book-glue.h e-data-book-factory-glue.h e-data-book-view-glue.h

include glib-gen.mak
glib_enum_headers=e-data-book-types.h
glib_enum_define=E_DATA_BOOK
glib_enum_prefix=e_data_book

ENUM_GENERATED = e-data-book-enumtypes.h e-data-book-enumtypes.c

libedata_book_1_2_la_SOURCES =				\
	$(DBUS_GENERATED_H)				\
	e-book-backend-factory.c			\
	e-book-backend-sexp.c				\
	e-book-backend-summary.c			\
	e-book-backend-cache.c                          \
	e-book-backend.c				\
	e-data-book-view.c				\
	e-data-book.c					\
	ximian-vcard.h					\
	e-book-backend-sync.c \
	$(ENUM_GENERATED)

libedata_book_1_2_la_LIBADD =					\
	$(EVOLUTION_ADDRESSBOOK_LIBS)				\
	$(top_builddir)/addressbook/libebook-dbus/libebook-$(API_VERSION).la	\
	$(top_builddir)/libedataserver/libedataserver-$(API_VERSION).la

libedata_book_1_2_la_LDFLAGS = 									\
	-version-info $(LIBEDATABOOK_CURRENT):$(LIBEDATABOOK_REVISION):$(LIBEDATABOOK_AGE)


libedata_bookincludedir = $(privincludedir)/libedata-book

libedata_bookinclude_HEADERS =				\
	e-book-backend-factory.h			\
	e-book-backend-sexp.h				\
	e-book-backend-summary.h			\
	e-book-backend-sync.h				\
	e-book-backend.h				\
	e-data-book-factory.h				\
	e-data-book-types.h				\
	e-data-book-view.h				\
	e-data-book.h                                   \
	e-book-backend-cache.h 								

%-$(API_VERSION).pc: %.pc
	 cp $< $@

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = libedata-book-$(API_VERSION).pc

service_in_files = org.gnome.evolution.dataserver.AddressBook.service.in
servicedir = $(datadir)/dbus-1/services
service_DATA = $(service_in_files:.service.in=.service)
@EVO_SUBST_SERVICE_RULE@

BUILT_SOURCES = $(DBUS_GENERATED_H)
CLEANFILES = $(BUILT_SOURCES) $(service_DATA)
DISTCLEANFILES = $(pkgconfig_DATA)

EXTRA_DIST = 						\
	e-data-book-factory.xml				\
	e-data-book-view.xml				\
	e-data-book.xml					\
	$(pkgconfig_DATA:-$(API_VERSION).pc=.pc.in)	\
	$(service_in_files)

dist-hook:
	cd $(distdir); rm -f $(BUILT_SOURCES)

%-glue.h: %.xml
	dbus-binding-tool --mode=glib-server --output=$@ --prefix=$(subst -,_,$*) $^

%-bindings.h: %.xml
	dbus-binding-tool --mode=glib-client --output=$@ --prefix=$(subst -,_,$*) $^


DBUS_BINDINGS = e-data-book-factory-bindings.h e-data-book-bindings.h e-data-book-view-bindings.h
BUILT_SOURCES += $(DBUS_BINDINGS) $(ENUM_GENERATED)

factorydir = $(libexecdir)
factory_PROGRAMS = e-addressbook-factory

e_addressbook_factory_SOURCES = e-data-book-factory.c e-data-book-factory.h
e_addressbook_factory_LDADD = libedata-book-1.2.la
