INCLUDES =						\
	-DG_LOG_DOMAIN=\"libebookbackend\"		\
	-I$(top_srcdir)					\
	-I$(top_builddir)				\
	-I$(top_srcdir)/addressbook			\
	-I$(top_builddir)/addressbook			\
        $(EVOLUTION_ADDRESSBOOK_CFLAGS)

extension_LTLIBRARIES = libebookbackendvcf.la

libebookbackendvcf_la_SOURCES =			\
	e-book-backend-vcf.c			\
	e-book-backend-vcf.h			\
	e-book-backend-vcf-factory.c

libebookbackendvcf_la_LIBADD =						\
	$(top_builddir)/addressbook/libebook/libebook-$(API_VERSION).la		\
	$(top_builddir)/addressbook/libedata-book/libedata-book-$(API_VERSION).la	\
	$(top_builddir)/libedataserver/libedataserver-$(API_VERSION).la		\
	$(EVOLUTION_ADDRESSBOOK_LIBS)

libebookbackendvcf_la_LDFLAGS =		\
	-module -avoid-version $(NO_UNDEFINED)
