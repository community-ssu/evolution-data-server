#include <check.h>
#include <glib-object.h>

#include "craphash.h"
#include "test-vcard-parse.h"
#include "test-vcard-photo.h"


/**
 * Test that the CrapHash algorithm is working correctly.
 */
START_TEST(test_craphash)
{
  fail_unless (craphash ("Test", 4) == g_str_hash ("Test"), "CrapHash check failed");
} END_TEST;

Suite *make_hash_suite (void)
{
  Suite *s = suite_create("Hash");
  TCase *tc = tcase_create("Core");
  suite_add_tcase (s, tc);
  tcase_add_test (tc, test_craphash);
  return s;
}

int main (void)
{
  int nf;
  Suite *hash_suite, *parse_suite, *photo_suite;
  SRunner *sr;

  g_type_init ();

  hash_suite = make_hash_suite();
  parse_suite = make_vcard_parse_suite();
  photo_suite = make_vcard_photo_suite();

  sr = srunner_create(hash_suite);
  srunner_add_suite (sr, parse_suite);
  srunner_add_suite (sr, photo_suite);

  srunner_run_all (sr, CK_NORMAL);
  nf = srunner_ntests_failed(sr);

  srunner_free(sr);
  suite_free(hash_suite);
  suite_free(parse_suite);
  suite_free(photo_suite);

  return nf;
}
