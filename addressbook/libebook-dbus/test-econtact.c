#include <check.h>
#include <string.h>
#include <libebook/e-contact.h>

START_TEST (test_get_names_full)
{
  EContact *contact;
  EContactName *names;

  contact = e_contact_new_from_vcard ("BEGIN:VCARD\nVERSION:3.0\nUID:1\nFN:Full Name\nN:Family;Given;Additional;Prefixes;Suffixes\nEND:VCARD");
  fail_unless (contact != NULL, "Cannot parse vCard");

  fail_unless (strcmp (e_contact_get_const (contact, E_CONTACT_FULL_NAME), "Full Name") == 0,
               "Full name parsed incorrectly");

  fail_unless (strcmp (e_contact_get_const (contact, E_CONTACT_FAMILY_NAME), "Family") == 0,
               "Family name parsed incorrectly");

  fail_unless (strcmp (e_contact_get_const (contact, E_CONTACT_GIVEN_NAME), "Given") == 0,
               "Give name parsed incorrectly");

  names = e_contact_get (contact, E_CONTACT_NAME);
  fail_unless (names != NULL, "Cannot get EContactName struct");

  fail_unless (strcmp (names->family, "Family") == 0,
               "Family name (struct) parsed incorrectly");

  fail_unless (strcmp (names->given, "Given") == 0,
               "Given name (struct) parsed incorrectly");

  fail_unless (strcmp (names->additional, "Additional") == 0,
               "Additional (struct) parsed incorrectly");

  fail_unless (strcmp (names->prefixes, "Prefixes") == 0,
               "Prefixes (struct) parsed incorrectly");

  fail_unless (strcmp (names->suffixes, "Suffixes") == 0,
               "Suffixes (struct) parsed incorrectly");

  e_contact_name_free (names);
  g_object_unref (contact);
}
END_TEST

START_TEST (test_get_names_empty)
{
  EContact *contact;
  EContactName *names;

  contact = e_contact_new_from_vcard ("BEGIN:VCARD\nVERSION:3.0\nUID:1\nEND:VCARD");
  fail_unless (contact != NULL, "Cannot parse vCard");

  fail_unless (e_contact_get_const (contact, E_CONTACT_FULL_NAME) == NULL,
               "Full name parsed incorrectly");

  fail_unless (e_contact_get_const (contact, E_CONTACT_FAMILY_NAME) == NULL,
               "Family name parsed incorrectly");

  fail_unless (e_contact_get_const (contact, E_CONTACT_GIVEN_NAME) == NULL,
               "Given name parsed incorrectly");

  names = e_contact_get (contact, E_CONTACT_NAME);
  fail_unless (names == NULL, "Got invalid EContactName struct");

  g_object_unref (contact);
}
END_TEST

START_TEST (test_get_contact_state)
{
  EContact *contact;
  char *state;
  const char *const_state;

  contact = e_contact_new_from_vcard ("BEGIN:VCARD\nVERSION:3.0\nUID:1\nX-OSSO-CONTACT-STATE:FOO,BAR\nEND:VCARD");
  fail_unless (contact != NULL, "Cannot parse vCard");

  state = e_contact_get (contact, E_CONTACT_OSSO_CONTACT_STATE);
  fail_unless (state != NULL, "Cannot get contact state");
  g_free (state);

  const_state = e_contact_get_const (contact, E_CONTACT_OSSO_CONTACT_STATE);
  fail_unless (const_state != NULL, "Cannot get constant contact state");

  g_object_unref (contact);  
}
END_TEST

static Suite *
contact_suite (void)
{
  Suite *s = suite_create ("EContact");
  
  TCase *tc_get = tcase_create ("Getters");
  tcase_add_test (tc_get, test_get_names_empty);
  tcase_add_test (tc_get, test_get_names_full);
  tcase_add_test (tc_get, test_get_contact_state);
  suite_add_tcase (s, tc_get);
  
  return s;
}

int
main (void)
{
  int number_failed;
  Suite *s;
  SRunner *sr;
  g_type_init ();

  s = contact_suite ();

  sr = srunner_create (s);
  srunner_run_all (sr, CK_NORMAL);
  number_failed = srunner_ntests_failed (sr);
  srunner_free (sr);

  return (number_failed == 0) ? 0 : 1;
}
