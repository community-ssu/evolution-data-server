INCLUDES =						\
	-DG_LOG_DOMAIN=\"libebookbackend\"		\
	-I$(top_srcdir)					\
	-I$(top_builddir)				\
	-I$(top_srcdir)/addressbook			\
	-I$(top_builddir)/addressbook			\
	-I$(top_srcdir)/servers/groupwise               \
	$(EVOLUTION_ADDRESSBOOK_CFLAGS)                 \
	$(SOUP_CFLAGS)	
extension_LTLIBRARIES = libebookbackendgroupwise.la

libebookbackendgroupwise_la_SOURCES =			\
	e-book-backend-groupwise.c			\
	e-book-backend-groupwise.h			\
	e-book-backend-groupwise-factory.c

libebookbackendgroupwise_la_LIBADD =					\
	$(top_builddir)/servers/groupwise/libegroupwise-$(API_VERSION).la		\
	$(top_builddir)/addressbook/libebook/libebook-$(API_VERSION).la		\
	$(top_builddir)/addressbook/libedata-book/libedata-book-$(API_VERSION).la	\
	$(top_builddir)/libedataserver/libedataserver-$(API_VERSION).la		\
	$(EVOLUTION_ADDRESSBOOK_LIBS)					\
	$(SOUP_LIBS)

libebookbackendgroupwise_la_LDFLAGS =		\
	-module -avoid-version $(NO_UNDEFINED)

noinst_PROGRAMS = create-account
create_account_SOURCES = create-account.c

create_account_LDADD =  $(top_builddir)/addressbook/libedata-book/libedata-book-$(API_VERSION).la      \
        $(top_builddir)/libedataserver/libedataserver-$(API_VERSION).la              \
        $(top_builddir)/servers/groupwise/libegroupwise-$(API_VERSION).la \
        $(E_DATA_SERVER_LIBS)

