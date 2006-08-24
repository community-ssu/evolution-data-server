#include <check.h>
#include <string.h>
#include <libebook/e-contact.h>

static gboolean strcompare(const char *a, const char *b)
{
  if (a == b)
    return TRUE;
  if (a == NULL || b == NULL)
    return FALSE;
  return strcmp (a, b) == 0;
}

static gboolean compare_attr(EVCardAttribute *a, EVCardAttribute *b)
{
  GList *la, *lb;
  /* Identity check */
  if (a == b)
    return TRUE;
  /* Null check */
  if (a == NULL || b == NULL)
    return FALSE;
  /* Compare group */
  if (!strcompare (e_vcard_attribute_get_group (a), e_vcard_attribute_get_group (b)))
    return FALSE;
  /* Compare name */
  if (!strcompare (e_vcard_attribute_get_name (a), e_vcard_attribute_get_name (b)))
    return FALSE;
  /* Compare parameters */
  la = e_vcard_attribute_get_params (a);
  lb = e_vcard_attribute_get_params (b);
  if (g_list_length (la) != g_list_length (lb))
    return FALSE;
  while (la) {
    EVCardAttributeParam *pa, *pb;
    GList *va, *vb;
    pa = la->data; pb = lb->data;
    if (!strcompare (e_vcard_attribute_param_get_name (pa), e_vcard_attribute_param_get_name (pb)))
      return FALSE;
    va = e_vcard_attribute_param_get_values (pa);
    vb = e_vcard_attribute_param_get_values (pb);
    if (g_list_length (va) != g_list_length (vb))
      return FALSE;
    while (va) {
      if (!strcompare (va->data, vb->data))
        return FALSE;
      va = va->next; vb = vb->next;
    }
    la = la->next; lb = lb->next;
  }
  /* Compare values */
  la = e_vcard_attribute_get_values (a);
  lb = e_vcard_attribute_get_values (b);
  if (g_list_length (la) != g_list_length (lb))
    return FALSE;
  while (la) {
    if (!strcompare (la->data, lb->data))
      return FALSE;
    la = la->next; lb = lb->next;
  }
  /* Made it here, must be identical */
  return TRUE;
}

START_TEST(test_parse_frankdawson)
{
  char *str;
  EVCard *vcard;
  GList *attrs;
  EVCardAttribute *attr;

  g_file_get_contents ("vcards/frankdawson.vcf", &str, NULL, NULL);
  vcard = e_vcard_new_from_string (str);
  g_free (str);
  fail_unless (vcard != NULL, "Parse failed");

  attrs = e_vcard_get_attributes (vcard);
  fail_unless (g_list_length (attrs) == 9, "Wrong number of attributes");

  attr = e_vcard_attribute_new (NULL, EVC_VERSION);
  e_vcard_attribute_add_value (attr, "3.0");
  fail_unless (compare_attr (attr, attrs->data), "VERSION incorrect");
  e_vcard_attribute_free (attr);
  attrs = attrs->next;

  attr = e_vcard_attribute_new (NULL, EVC_FN);
  e_vcard_attribute_add_value (attr, "Frank Dawson");
  fail_unless (compare_attr (attr, attrs->data), "FN incorrect");
  e_vcard_attribute_free (attr);
  attrs = attrs->next;

  attr = e_vcard_attribute_new (NULL, EVC_ORG);
  e_vcard_attribute_add_value (attr, "Lotus Development Corporation");
  fail_unless (compare_attr (attr, attrs->data), "ORG incorrect");
  e_vcard_attribute_free (attr);
  attrs = attrs->next;

  attr = e_vcard_attribute_new (NULL, EVC_ADR);
  e_vcard_attribute_add_param_with_values (attr, e_vcard_attribute_param_new ("TYPE"), "WORK", "POSTAL", "PARCEL", NULL);
  e_vcard_attribute_add_value (attr, "");
  e_vcard_attribute_add_value (attr, "");
  e_vcard_attribute_add_value (attr, "6544 Battleford Drive");
  e_vcard_attribute_add_value (attr, "Raleigh");
  e_vcard_attribute_add_value (attr, "NC");
  e_vcard_attribute_add_value (attr, "27613-3502");
  e_vcard_attribute_add_value (attr, "U.S.A.");
  fail_unless (compare_attr (attr, attrs->data), "ADR incorrect");
  e_vcard_attribute_free (attr);
  attrs = attrs->next;

  attr = e_vcard_attribute_new (NULL, EVC_TEL);
  e_vcard_attribute_add_param_with_values (attr, e_vcard_attribute_param_new ("TYPE"), "VOICE", "MSG", "WORK", NULL);
  e_vcard_attribute_add_value (attr, "+1-919-676-9515");
  fail_unless (compare_attr (attr, attrs->data), "TEL incorrect");
  e_vcard_attribute_free (attr);
  attrs = attrs->next;

  attr = e_vcard_attribute_new (NULL, EVC_TEL);
  e_vcard_attribute_add_param_with_values (attr, e_vcard_attribute_param_new ("TYPE"), "FAX", "WORK", NULL);
  e_vcard_attribute_add_value (attr, "+1-919-676-9564");
  fail_unless (compare_attr (attr, attrs->data), "TEL incorrect");
  e_vcard_attribute_free (attr);
  attrs = attrs->next;

  attr = e_vcard_attribute_new (NULL, EVC_EMAIL);
  e_vcard_attribute_add_param_with_values (attr, e_vcard_attribute_param_new ("TYPE"), "INTERNET", "PREF", NULL);
  e_vcard_attribute_add_value (attr, "Frank_Dawson@Lotus.com");
  fail_unless (compare_attr (attr, attrs->data), "EMAIL incorrect");
  e_vcard_attribute_free (attr);
  attrs = attrs->next;

  attr = e_vcard_attribute_new (NULL, EVC_EMAIL);
  e_vcard_attribute_add_param_with_values (attr, e_vcard_attribute_param_new ("TYPE"), "INTERNET", NULL);
  e_vcard_attribute_add_value (attr, "fdawson@earthlink.net");
  fail_unless (compare_attr (attr, attrs->data), "EMAIL incorrect");
  e_vcard_attribute_free (attr);
  attrs = attrs->next;

  attr = e_vcard_attribute_new (NULL, EVC_URL);
  e_vcard_attribute_add_value (attr, "http://home.earthlink.net/~fdawson");
  fail_unless (compare_attr (attr, attrs->data), "URL incorrect");
  e_vcard_attribute_free (attr);
  attrs = attrs->next;
  
  fail_unless (attrs == NULL, "More attributes unexpected");

  g_object_unref (vcard);
} END_TEST;


Suite *make_vcard_parse_suite (void)
{
  Suite *s = suite_create("vCardParse");
  TCase *tc = tcase_create("Parse");
  suite_add_tcase (s, tc);
  tcase_add_test (tc, test_parse_frankdawson);
  return s;
}
