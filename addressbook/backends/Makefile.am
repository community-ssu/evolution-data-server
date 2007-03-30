if ENABLE_LDAP
LDAP_SUBDIR = ldap
else
LDAP_SUBDIR =
endif

if WITH_SOUP
GROUPWISE_SUBDIR = groupwise
else
GROUPWISE_SUBDIR =
endif

SUBDIRS = file $(LDAP_SUBDIR) $(GROUPWISE_SUBDIR)
DIST_SUBDIRS = file ldap groupwise vcf
