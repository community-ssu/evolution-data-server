
INCLUDES =						\
	-I$(includedir)					\
	-I$(top_srcdir)					\
	-I$(top_srcdir)/intl			 	\
	-I$(top_srcdir)/camel				\
	-I$(top_srcdir)/camel/tests/lib 		\
	-DG_LOG_DOMAIN=\"evolution-tests\"		\
	-DSOURCEDIR=\"$(srcdir)\"				\
	$(CAMEL_CFLAGS)

LDADD = \
	$(top_builddir)/camel/libcamel-${API_VERSION}.la 		\
	$(top_builddir)/libedataserver/libedataserver-${API_VERSION}.la \
	$(top_builddir)/camel/tests/lib/libcameltest.a	\
	$(INTLLIBS)

EXTRA_DIST =				\
	crlf-1.in			\
	crlf-1.out			\
	charset-iso-2022-jp.0.in	\
	charset-iso-2022-jp.0.out	\
	charset-gb2312.0.in		\
	charset-gb2312.0.out

check_PROGRAMS =  		\
	test1			\
	test-crlf		\
	test-charset		\
	test-tohtml

TESTS = test1 \
	test-crlf test-charset test-tohtml



