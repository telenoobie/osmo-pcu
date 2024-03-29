AM_CPPFLAGS = $(STD_DEFINES_AND_INCLUDES) $(LIBOSMOCORE_CFLAGS) $(LIBOSMOGB_CFLAGS) $(LIBOSMOGSM_CFLAGS) -I$(top_srcdir)/src/

check_PROGRAMS = rlcmac/RLCMACTest alloc/AllocTest tbf/TbfTest types/TypesTest
noinst_PROGRAMS = emu/pcu_emu

rlcmac_RLCMACTest_SOURCES = rlcmac/RLCMACTest.cpp
rlcmac_RLCMACTest_LDADD = \
	$(top_builddir)/src/libgprs.la \
	$(LIBOSMOCORE_LIBS) \
	$(COMMON_LA)

alloc_AllocTest_SOURCES = alloc/AllocTest.cpp
alloc_AllocTest_LDADD = \
	$(top_builddir)/src/libgprs.la \
	$(LIBOSMOGB_LIBS) \
	$(LIBOSMOGSM_LIBS) \
	$(LIBOSMOCORE_LIBS) \
	$(COMMON_LA)

tbf_TbfTest_SOURCES = tbf/TbfTest.cpp
tbf_TbfTest_LDADD = \
	$(LIBOSMOGB_LIBS) \
	$(LIBOSMOGSM_LIBS) \
	$(LIBOSMOCORE_LIBS) \
	$(COMMON_LA) \
	$(top_builddir)/src/libgprs.la

emu_pcu_emu_SOURCES = emu/pcu_emu.cpp emu/test_replay_gprs_attach.cpp \
	emu/openbsc_clone.c emu/openbsc_clone.h emu/gprs_tests.h \
	emu/test_pdp_activation.cpp
emu_pcu_emu_LDADD = \
	$(top_builddir)/src/libgprs.la \
	$(LIBOSMOGB_LIBS) \
	$(LIBOSMOGSM_LIBS) \
	$(LIBOSMOCORE_LIBS) \
	$(COMMON_LA)

types_TypesTest_SOURCES = types/TypesTest.cpp
types_TypesTest_LDADD = \
	$(LIBOSMOGB_LIBS) \
	$(LIBOSMOGSM_LIBS) \
	$(LIBOSMOCORE_LIBS) \
	$(COMMON_LA) \
	$(top_builddir)/src/libgprs.la


# The `:;' works around a Bash 3.2 bug when the output is not writeable.
$(srcdir)/package.m4: $(top_srcdir)/configure.ac
	:;{ \
               echo '# Signature of the current package.' && \
               echo 'm4_define([AT_PACKAGE_NAME],' && \
               echo '  [$(PACKAGE_NAME)])' && \
               echo 'm4_define([AT_PACKAGE_TARNAME],' && \
               echo '  [$(PACKAGE_TARNAME)])' && \
               echo 'm4_define([AT_PACKAGE_VERSION],' && \
               echo '  [$(PACKAGE_VERSION)])' && \
               echo 'm4_define([AT_PACKAGE_STRING],' && \
               echo '  [$(PACKAGE_STRING)])' && \
               echo 'm4_define([AT_PACKAGE_BUGREPORT],' && \
               echo '  [$(PACKAGE_BUGREPORT)])'; \
               echo 'm4_define([AT_PACKAGE_URL],' && \
               echo '  [$(PACKAGE_URL)])'; \
             } >'$(srcdir)/package.m4'

EXTRA_DIST = \
	testsuite.at $(srcdir)/package.m4 $(TESTSUITE)	\
	rlcmac/RLCMACTest.ok rlcmac/RLCMACTest.err \
	alloc/AllocTest.ok alloc/AllocTest.err \
	tbf/TbfTest.ok tbf/TbfTest.err \
	types/TypesTest.ok types/TypesTest.err

DISTCLEANFILES = atconfig

TESTSUITE = $(srcdir)/testsuite

# Python testing
if ENABLE_VTY_TESTS
python-tests: $(BUILT_SOURCES)
	osmotestvty.py -p $(abs_top_srcdir) -w $(abs_top_builddir) -v
	osmotestconfig.py -p $(abs_top_srcdir) -w $(abs_top_builddir) -v

else
python-tests: $(BUILT_SOURCES)
	@echo "Not running python-based tests (determined at configure-time)"
endif

check-local: $(BUILT_SOURCES) $(TESTSUITE)
	$(SHELL) '$(TESTSUITE)' $(TESTSUITEFLAGS)
	$(MAKE) $(AM_MAKEFLAGS) python-tests

installcheck-local: atconfig $(TESTSUITE)
	$(SHELL) '$(TESTSUITE)' AUTOTEST_PATH='$(bindir)' \
		$(TESTSUITEFLAGS)

clean-local:
	test ! -f '$(TESTSUITE)' || \
		$(SHELL) '$(TESTSUITE)' --clean

AUTOM4TE = $(SHELL) $(top_srcdir)/missing --run autom4te
AUTOTEST = $(AUTOM4TE) --language=autotest
$(TESTSUITE): $(srcdir)/testsuite.at $(srcdir)/package.m4
	$(AUTOTEST) -I '$(srcdir)' -o $@.tmp $@.at
	mv $@.tmp $@
