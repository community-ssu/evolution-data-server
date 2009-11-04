AM_CXXFLAGS = -Wall $(UCHARDET_CFLAGS) -fno-rtti -fno-exceptions -DMAEMO_CHANGES=1

lib_LIBRARIES = libuchardet.a

libuchardet_a_SOURCES = \
	LangBulgarianModel.cpp \
	LangCyrillicModel.cpp \
	LangGreekModel.cpp \
	LangHungarianModel.cpp \
	nsCharSetProber.cpp \
	nsCharSetProber.h \
	nsCodingStateMachine.h \
	nsLatin1Prober.cpp \
	nsLatin1Prober.h \
	nsMBCSGroupProber.cpp \
	nsMBCSGroupProber.h \
	nsMBCSSM.cpp \
	nsPkgInt.h \
	nsSBCSGroupProber.cpp \
	nsSBCSGroupProber.h \
	nsSBCharSetProber.cpp \
	nsSBCharSetProber.h \
	nsUTF8Prober.cpp \
	nsUTF8Prober.h \
	nsUniversalDetector.cpp \
	nsUniversalDetector.h \
	$(NULL)

# This languages require charset not supported in Maemo 5.
UNSUPPORTED_CHARSETS = \
	LangHebrewModel.cpp \
	LangThaiModel.cpp \
	nsHebrewProber.cpp \
	nsHebrewProber.h \
	$(NULL)

# This charsets aren't proper ASCII supersets and therefore cannot really be
# used to encode version 2.1 vCards. Also this probers need much more data
# for their decision than typical vCards provide.
NON_ASCII_CHARSETS = \
	CharDistribution.cpp \
	CharDistribution.h \
	JpCntx.cpp \
	JpCntx.h \
	nsBig5Prober.cpp \
	nsBig5Prober.h \
	nsEUCJPProber.cpp \
	nsEUCJPProber.h \
	nsEUCKRProber.cpp \
	nsEUCKRProber.h \
	nsEUCTWProber.cpp \
	nsEUCTWProber.h \
	nsEscCharsetProber.cpp \
	nsEscCharsetProber.h \
	nsEscSM.cpp \
	nsGB2312Prober.cpp \
	nsGB2312Prober.h \
	nsSJISProber.cpp \
	nsSJISProber.h \
	$(NULL)

EXTRA_DIST = $(UNSUPPORTED_CHARSETS) $(NON_ASCII_CHARSETS)
