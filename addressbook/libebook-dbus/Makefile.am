INCLUDES =						\
	-DEVOLUTION_LOCALEDIR=\""$(localedir)"\"	\
	-DG_LOG_DOMAIN=\"libebook\"			\
	-I$(top_srcdir)					\
	-I$(top_builddir)				\
	-I$(top_srcdir)/addressbook			\
	-I$(top_builddir)/addressbook			\
        $(EVOLUTION_ADDRESSBOOK_CFLAGS)

# The marshallers
MARSHAL_GENERATED = e-book-marshal.c e-book-marshal.h
@EVO_MARSHAL_RULE@

# The library
lib_LTLIBRARIES = libebook-1.2.la

libebook_1_2_la_SOURCES =					\
	$(MARSHAL_GENERATED)				\
	$(DBUS_GENERATED_H)				\
	e-address-western.c				\
	e-book-query.c					\
	e-book-view.c					\
	e-book-view-private.h				\
	e-book.c					\
	e-contact.c					\
	e-destination.c					\
	e-name-western.c				\
	e-name-western-tables.h				\
	e-vcard.c					\
	e-error.h

libebook_1_2_la_LIBADD =					\
	$(EVOLUTION_ADDRESSBOOK_LIBS)				\
	$(top_builddir)/camel/libcamel-$(API_VERSION).la \
	$(top_builddir)/libedataserver/libedataserver-$(API_VERSION).la

libebook_1_2_la_LDFLAGS = 							\
	-version-info $(LIBEBOOK_CURRENT):$(LIBEBOOK_REVISION):$(LIBEBOOK_AGE)

libebookincludedir = $(privincludedir)/libebook

libebookinclude_HEADERS =				\
	e-address-western.h				\
	e-book-query.h					\
	e-book-view.h					\
	e-book-types.h					\
	e-book.h					\
	e-contact.h					\
	e-destination.h					\
	e-name-western.h				\
	e-vcard.h

%-$(API_VERSION).pc: %.pc
	 cp $< $@

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = libebook-$(API_VERSION).pc

DBUS_GENERATED_H = e-data-book-factory-bindings.h e-data-book-bindings.h e-data-book-view-bindings.h
%-glue.h: $(top_srcdir)/addressbook/libedata-book-dbus/%.xml
	dbus-binding-tool --mode=glib-server --output=$@ --prefix=$(subst -,_,$*) $^

%-bindings.h: $(top_srcdir)/addressbook/libedata-book-dbus/%.xml
	dbus-binding-tool --mode=glib-client --output=$@ --prefix=$(subst -,_,$*) $^

BUILT_SOURCES = $(DBUS_GENERATED_H) $(MARSHAL_GENERATED)
CLEANFILES    = $(BUILT_SOURCES)
DISTCLEANFILES = $(pkgconfig_DATA)

EXTRA_DIST = 						\
	e-book-marshal.list				\
	$(pkgconfig_DATA:-$(API_VERSION).pc=.pc.in)

dist-hook:
	cd $(distdir); rm -f $(BUILT_SOURCES)


check_PROGRAMS = test-econtact
test_econtact_SOURCES = test-econtact.c
test_econtact_LDADD = ./libebook-1.2.la -lcheck

TESTS = test-econtact
