noinst_LTLIBRARIES = libcamel-1.2.la

INCLUDES = -I.. -I$(srcdir)/..                          \
        -I$(top_srcdir)/intl                            \
        -DG_LOG_DOMAIN=\"camel\"                        \
        $(CAMEL_CFLAGS)

libcamel_1_2_la_SOURCES =                               \
        camel-address.c \
        camel-address.h \
        camel-charset-map.c \
        camel-charset-map.h \
        camel-charset-map-private.h \
        camel-internet-address.c \
        camel-internet-address.h \
        camel-mime-tables.c \
        camel-mime-utils.c \
        camel-mime-utils.h \
        camel-object.h \
        camel-utf8.c \
        camel-utf8.h

libcamel_1_2_la_LDFLAGS = -version-info 0:0:0

libcamel_1_2_la_LIBADD =                                \
        $(top_builddir)/libedataserver/libedataserver-${API_VERSION}.la         \
        $(CAMEL_LIBS)
