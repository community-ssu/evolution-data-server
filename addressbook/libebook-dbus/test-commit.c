#include <check.h>
#include <string.h>
#include <glib/gstdio.h>
#include <libebook/e-book.h>

#include "test-prototypes.h"

static char *book_path = NULL;
static EBook *book = NULL;

static void
create_book (void)
{
  char *uri;
  GError *error = NULL;

  fail_unless (book_path == NULL, "book_path not reset");

  book_path = tempnam (g_get_tmp_dir (), "ebook");

  uri = g_filename_to_uri (book_path, NULL, &error);
  fail_unless (uri != NULL, "Cannot get URI");

  book = e_book_new_from_uri (uri, &error);
  fail_unless (book != NULL, "Cannot get book");

  fail_unless (e_book_open (book, FALSE, &error), "Cannot open book");

  g_free (uri);
}

static void
destroy_book (void)
{
  char *path;

  g_object_unref (book);
  book = NULL;

  path = g_build_filename (book_path, "addressbook.db", NULL);
  g_unlink (path);
  g_free (path);

  path = g_build_filename (book_path, "addressbook.db.summary", NULL);
  g_unlink (path);
  g_free (path);

  g_rmdir (book_path);

  free (book_path);
  book_path = NULL;
}

static EContact *
make_contact (void)
{
  char *vcard;
  EContact *contact;

  vcard = g_strdup_printf ("BEGIN:VCARD\n"
                           "VERSION:3.0\n"
                           "FN:Name %c%c%c%c\n"
                           "END:VCARD",
                           g_random_int_range ('a', 'z'+1),
                           g_random_int_range ('a', 'z'+1),
                           g_random_int_range ('a', 'z'+1),
                           g_random_int_range ('a', 'z'+1));
  
  contact = e_contact_new_from_vcard (vcard);
  
  g_free (vcard);

  return contact;
}

#define ARBITRARY "Some Arbitrary String"

START_TEST (test_commit)
{
  EContact *contact;
  char *uid;

  contact = make_contact ();
  fail_unless (e_contact_get_const (contact, E_CONTACT_NOTE) == NULL, "Note exists somehow");

  fail_unless (e_book_add_contact (book, contact, NULL), "Cannot add contact");
  uid = e_contact_get (contact, E_CONTACT_UID);
 
  e_contact_set (contact, E_CONTACT_NOTE, ARBITRARY);
  fail_unless (e_book_commit_contact (book, contact, NULL), "Cannot commit contact");
  
  g_object_unref (contact);
  contact = NULL;

  fail_unless (e_book_get_contact (book, uid, &contact, NULL), "Cannot get contact");
  
  fail_unless (strcmp (ARBITRARY, e_contact_get_const (contact, E_CONTACT_NOTE)) == 0,
               "Contact not committed");

  g_object_unref (contact);
  g_free (uid);

} END_TEST

START_TEST (test_commit_bulk)
{
  EContact *contact1, *contact2;
  char *uid1, *uid2;
  GList *l = NULL;

  contact1 = make_contact ();
  contact2 = make_contact ();
  
  fail_unless (e_contact_get_const (contact1, E_CONTACT_NOTE) == NULL, "Note exists somehow");
  fail_unless (e_contact_get_const (contact2, E_CONTACT_NOTE) == NULL, "Note exists somehow");

  fail_unless (e_book_add_contact (book, contact1, NULL), "Cannot add contact1");
  uid1 = e_contact_get (contact1, E_CONTACT_UID);

  fail_unless (e_book_add_contact (book, contact2, NULL), "Cannot add contact2");
  uid2 = e_contact_get (contact2, E_CONTACT_UID);

  e_contact_set (contact1, E_CONTACT_NOTE, ARBITRARY);
  e_contact_set (contact2, E_CONTACT_NOTE, ARBITRARY);

  l = g_list_prepend (l, contact1);
  l = g_list_prepend (l, contact2);

  fail_unless (e_book_commit_contacts (book, l, NULL), "Cannot commit contacts");

  g_object_unref (contact1);
  g_object_unref (contact2);
  contact1 = NULL;
  contact2 = NULL;

  fail_unless (e_book_get_contact (book, uid1, &contact1, NULL), "Cannot get contact1");
  fail_unless (e_book_get_contact (book, uid1, &contact2, NULL), "Cannot get contact2");
  
  fail_unless (strcmp (ARBITRARY, e_contact_get_const (contact1, E_CONTACT_NOTE)) == 0,
               "Contact1 not committed");
  fail_unless (strcmp (ARBITRARY, e_contact_get_const (contact2, E_CONTACT_NOTE)) == 0,
               "Contact2 not committed");

  g_list_free (l);
  g_object_unref (contact1);
  g_object_unref (contact2);
  g_free (uid1);
  g_free (uid2);

} END_TEST


Suite *
commit_suite (void)
{
  Suite *s = suite_create ("EBook");
  
  TCase *tc = tcase_create ("Commit");
  tcase_add_checked_fixture (tc, create_book, destroy_book);
  tcase_add_test (tc, test_commit);
  tcase_add_test (tc, test_commit_bulk);
  suite_add_tcase (s, tc);
  
  return s;
}
