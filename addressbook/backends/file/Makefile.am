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
	e-book-backend-file.c				\
	e-book-backend-file.h				\
	e-book-backend-file-index.c			\
	e-book-backend-file-index.h			\
	e-book-backend-file-factory.c			\
	e-book-backend-file-log.c			\
	e-book-backend-file-log.h			\
	e-book-backend-file-utils.c			\
	e-book-backend-file-utils.h			\
	$(top_srcdir)/libedataserver/e-db3-utils.c	\
	$(top_srcdir)/libedataserver/e-dbhash.c		\
	$(top_srcdir)/libedataserver/md5-utils.c

libebookbackendfile_la_LIBADD =						\
	$(top_builddir)/addressbook/libebook/libebook-$(API_VERSION).la		\
	$(top_builddir)/addressbook/libedata-book/libedata-book-$(API_VERSION).la	\
	$(top_builddir)/libedataserver/libedataserver-$(API_VERSION).la		\
	$(DB_LIBS)				\
	$(EVOLUTION_ADDRESSBOOK_LIBS)

libebookbackendfile_la_LDFLAGS =	\
	-module -avoid-version $(NO_UNDEFINED)

-include $(top_srcdir)/Makefile.local
