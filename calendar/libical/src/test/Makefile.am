if WITH_CXX_BINDINGS
cxx_inc=-DWITH_CXX_BINDINGS
cxx_libs=../libical/libical-evolution_cxx.la
regression_cxx = regression-cxx.cpp
else
cxx_inc=
cxx_libs=
regression_cxx = 
AM_CXX=@CC@
endif

if WITH_BDB4
bdb4_inc=-DWITH_BDB -I@BDB_DIR@/include
bdb4_libs=@BDB_DIR_LIB@/@BDB_LIB@
else
bdb4_inc=
bdb4_libs=
endif

if OS_WIN32
else
STOW = stow
endif

noinst_PROGRAMS = copycluster regression parser $(STOW) recur testmime testvcal process

LDADD =  ../libicalss/libicalss-evolution.la ../libicalvcal/libicalvcal-evolution.la $(cxx_libs) ../libical/libical-evolution.la $(bdb4_libs)

LIBS = @PTHREAD_LIBS@

INCLUDES = 						\
	-I$(top_srcdir)					\
	-I$(top_srcdir)/src				\
	-I$(top_builddir)/src				\
	-I$(top_srcdir)/src/libical			\
	-I$(top_builddir)/src/libical			\
	-DTEST_DATADIR=\"$(top_srcdir)/test-data\"	\
	$(cxx_inc) $(bdb4_inc)

TESTS=regression
copycluster_SOURCES = copycluster.c
regression_SOURCES = 		\
	regression.c		\
	regression.h		\
	regression-component.c	\
	regression-classify.c	\
	regression-utils.c	\
	regression-recur.c	\
	regression-storage.c	\
	$(regression_cxx)

parser_SOURCES = icaltestparser.c
stow_SOURCES = stow.c
testvcal_SOURCES = testvcal.c
recur_SOURCES = recur.c 
testmime_SOURCES = testmime.c
process_SOURCES = process.c

# clusterin.vcd should be a real file with data but it doesn't seem to be in cvs
CLEANFILES = 				\
	test_fileset.ics 		\
	test_fileset_locktest.ics	\
	filesetout.ics 			\
	clusterin.vcd

clean-local:
	rm -rf calendar