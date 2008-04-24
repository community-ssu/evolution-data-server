#include <check.h>
#include <glib-object.h>

#include "test-prototypes.h"

int
main (void)
{
  int number_failed;
  SRunner *sr;
  
  g_type_init ();

  sr = srunner_create (NULL);
  srunner_add_suite (sr, contact_suite ());
  srunner_add_suite (sr, commit_suite ());

  srunner_run_all (sr, CK_NORMAL);
  number_failed = srunner_ntests_failed (sr);
  srunner_free (sr);

  return (number_failed == 0) ? 0 : 1;
}
