#ifdef __cplusplus
extern "C" {
#pragma }
#endif /* __cplusplus */

#include <libical/icaltime.h>
#include <libical/icalduration.h>
#include <libical/icalperiod.h>
#include <libical/icalenums.h>
#include <libical/icaltypes.h>
#include <libical/icalrecur.h>
#include <libical/icalattach.h>
#include <libical/icalvalue.h>
#include <libical/icalparameter.h>
#include <libical/icalderivedproperty.h>
#include <libical/icalproperty.h>
#include <libical/pvl.h>
#include <libical/icalarray.h>
#include <libical/icalcomponent.h>
#include <libical/icaltimezone.h>
#include <libical/icalparser.h>
#include <libical/icalmemory.h>
#include <libical/icalerror.h>
#include <libical/icalrestriction.h>
#include <libical/sspm.h>
#include <libical/icalmime.h>
#include <libical/icallangbind.h>

#ifndef HANDLE_LIBICAL_MEMORY
#warning "Please ensure that the memory returned by the functions mentioned at http://bugzilla.gnome.org/show_bug.cgi?id=516408#c1 are free'ed"
#endif

#define LIBICAL_MEMFIXES 1

#ifdef __cplusplus
}
#endif /* __cplusplus */
