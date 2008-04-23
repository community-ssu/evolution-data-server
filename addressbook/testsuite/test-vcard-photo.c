#include <check.h>
#include <string.h>
#include <libebook/e-contact.h>

#include "craphash.h"

/**
 * Test that the vCard parser is handling PHOTO fields containing a URI.
 */
START_TEST(test_photo_read_uri)
{
  char *str;
  EContact *contact;
  EContactPhoto *photo;
  
  g_file_get_contents ("vcards/photo-uri.vcf", &str, NULL, NULL);
  contact = e_contact_new_from_vcard (str);
  g_free (str);

  photo = e_contact_get (contact, E_CONTACT_PHOTO);
  fail_unless (photo->type == E_CONTACT_PHOTO_TYPE_URI, "Photo type is not URI");
  fail_unless (strcmp (photo->data.uri, "http://www.burtonini.com/photo.jpg") == 0, "Photo URI incorrect");
  e_contact_photo_free (photo);
  g_object_unref (contact);
} END_TEST;


/**
 * Test that the vCard parser is handling PHOTO fields containing inlined binary
 * data.
 */
START_TEST(test_photo_read_inlined)
{
  char *str;
  EContact *contact;
  EContactPhoto *photo;
  
  g_file_get_contents ("vcards/photo-inlined.vcf", &str, NULL, NULL);
  contact = e_contact_new_from_vcard (str);
  g_free (str);

  photo = e_contact_get (contact, E_CONTACT_PHOTO);
  fail_unless (photo->type == E_CONTACT_PHOTO_TYPE_INLINED, "Photo type is not inline");
  fail_unless (photo->data.inlined.length == 9847, "Photo data size incorrect");
  fail_unless (craphash (photo->data.inlined.data, photo->data.inlined.length) == 3225337096U, "Photo data hash incorrect");
  e_contact_photo_free (photo);
  g_object_unref (contact);
} END_TEST;


START_TEST(test_photo_write_uri)
{
  EContact *contact;
  EContactPhoto *photo;
  char *vcard;

  contact = e_contact_new ();
  e_contact_set (contact, E_CONTACT_FULL_NAME, "Fred Smith");
  photo = g_new0 (EContactPhoto, 1);
  photo->type = E_CONTACT_PHOTO_TYPE_URI;
  photo->data.uri = g_strdup ("http://www.example.com/photo.jpg");
  e_contact_set (contact, E_CONTACT_PHOTO, photo);
  e_contact_photo_free (photo);

  vcard = e_vcard_to_string (E_VCARD (contact), EVC_FORMAT_VCARD_30);
  g_object_unref (contact);

  contact = e_contact_new_from_vcard (vcard);
  g_free (vcard);

  fail_unless (strcmp (e_contact_get_const (contact, E_CONTACT_FULL_NAME), "Fred Smith") == 0, "Name is wrong");
  photo = e_contact_get (contact, E_CONTACT_PHOTO);
  fail_unless (photo->type == E_CONTACT_PHOTO_TYPE_URI, "Photo type is not URI");
  fail_unless (strcmp (photo->data.uri, "http://www.example.com/photo.jpg") == 0, "Photo URI incorrect");
  e_contact_photo_free (photo);
  g_object_unref (contact);
} END_TEST;


Suite *make_vcard_photo_suite (void)
{
  Suite *s = suite_create("vCardPhoto");
  TCase *tc_photo = tcase_create("Photo");
  suite_add_tcase (s, tc_photo);
  tcase_add_test (tc_photo, test_photo_read_uri);
  tcase_add_test (tc_photo, test_photo_read_inlined);
  tcase_add_test (tc_photo, test_photo_write_uri);
  return s;
}
