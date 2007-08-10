INCLUDES =						\
	-DG_LOG_DOMAIN=\"libebookbackend\"		\
	-I$(top_srcdir)					\
	-I$(top_builddir)				\
	-I$(top_srcdir)/addressbook			\
	-I$(top_builddir)/addressbook			\
	$(DB_CFLAGS)					\
	$(EVOLUTION_ADDRESSBOOK_CFLAGS)

extension_LTLIBRARIES = libebookbackendfile.la

libebookbackendfile_la_SOURCES =			\
	e-db3-utils.c e-db3-utils.h \
	e-dbhash.c e-dbhash.h \
	e-book-backend-file.c				\
	e-book-backend-file.h				\
	e-book-backend-file-factory.c

libebookbackendfile_la_LIBADD =						\
	$(top_builddir)/addressbook/libebook/libebook-$(API_VERSION).la		\
	$(top_builddir)/addressbook/libedata-book/libedata-book-$(API_VERSION).la	\
	$(top_builddir)/libedataserver/libedataserver-$(API_VERSION).la		\
	$(DB_LIBS)				\
	$(EVOLUTION_ADDRESSBOOK_LIBS)

libebookbackendfile_la_LDFLAGS =	\
	-module -avoid-version $(NO_UNDEFINED)
