#include "e-book-util.h"

static void
list_free (gpointer data)
{
        GList *l = data;

        while (l) {
                g_object_unref (l->data);
                l = g_list_delete_link (l, l);
        }
}

static GHashTable*
ht_new (void)
{
        return g_hash_table_new_full (g_str_hash, g_str_equal, g_free, list_free);
}

static void
ht_add (GHashTable *ht, char *key, EContact *contact)
{
        GList *list = NULL;
        char *old_key = NULL;

        g_hash_table_lookup_extended (ht, key, (gpointer)&old_key, (gpointer)&list);
        g_hash_table_steal (ht, key);
        g_free (old_key);
        list = g_list_prepend (list, g_object_ref (contact));
        g_hash_table_insert (ht, key, list);
}

static char*
create_hash (EContact *contact)
{
        const char *first, *last;

        first = e_contact_get_const (contact, E_CONTACT_GIVEN_NAME);
        last = e_contact_get_const (contact, E_CONTACT_FAMILY_NAME);

        return g_strdup_printf ("%s\n%s\n", first, last);
}

static char*
serialize_list (GList *l)
{
        GString *s;

        s = g_string_new ("");
        for (;l;l = l->next) {
                char *escaped = g_strescape (l->data, "");
                g_string_append (s, escaped);
                g_free (escaped);
                if (l->next) {
                        g_string_append (s, "\\");
                }
        }
        return g_string_free (s, FALSE);
}

static char*
serialize_attribute (EVCardAttribute *attr)
{
        const char *name, **field;
        const char *vcard_fields[] = { EVC_N, EVC_TEL, EVC_EMAIL, EVC_ADR, EVC_BDAY, NULL };
        char *serialized, *tmp;

        name = e_vcard_attribute_get_name (attr);
        for (field = vcard_fields; *field; field++) {
                if (0 == g_strcmp0 (*field, name))
                goto found;
        }
        return NULL;

found:
        serialized = serialize_list (e_vcard_attribute_get_values (attr));

        tmp = serialized;
        serialized = g_strdup_printf ("%s\\%s", name, serialized);
        g_free (tmp);
        g_debug ("-> %s", serialized);

        return serialized;
}

static gboolean
find_single (gpointer key, gpointer value, gpointer user_data)
{
        GList *l = value;

        g_debug ("%s -> %d", (char*)key, value ? g_list_length (value) : 0);
        if (l == NULL || l->next == NULL)
                return TRUE;
        return FALSE;
}

static gboolean
contact_compare (EContact *a, EContact *b)
{
        GHashTable *ht;
        GList *attribs;
        gboolean rv;

        ht = ht_new ();

        g_debug ("Comparing: %s", (char*)e_contact_get_const (a, E_CONTACT_NAME_OR_ORG));
        //e_vcard_dump_structure (E_VCARD (a));
        g_debug ("With: %s", (char*)e_contact_get_const (a, E_CONTACT_NAME_OR_ORG));
        //e_vcard_dump_structure (E_VCARD (b));

        for (attribs = e_vcard_get_attributes (E_VCARD (a));
             attribs;
             attribs = attribs->next) {
                char *serialized;

                serialized = serialize_attribute (attribs->data);
                if (serialized) {
                        ht_add (ht, serialized, a);
                }
        }
    
        for (attribs = e_vcard_get_attributes (E_VCARD (b));
             attribs;
             attribs = attribs->next) {
                char *serialized;

                serialized = serialize_attribute (attribs->data);
                if (serialized) {
                        ht_add (ht, serialized, b);
                }
        }
        rv = (NULL == g_hash_table_find (ht, find_single, NULL));

        g_hash_table_destroy (ht);

        g_debug ("=> %s", rv ? "Identical" : "Different");

        return rv;
}

/**
 * e_book_util_remove_duplicates:
 * @haystack: #GList of #EContacts
 * @needles: #GList of #EContacts
 * @duplicate_ids: #GList of ids which are removed from @needles
 *
 * Searches duplicates of contacts from needles in haystack. Duplicates are
 * removed from needles, they uids are put to duplicate_ids.
 */
void
e_book_util_remove_duplicates (GList  *haystack,
                               GList **needles,
                               GList **duplicate_ids)
{
        GHashTable *local_names;
        GList *contacts;

        g_return_if_fail (needles != NULL);
        g_return_if_fail (duplicate_ids != NULL && *duplicate_ids == NULL);

        local_names = ht_new ();

        while (haystack) {
                EContact *contact = haystack->data;
                char *key;

                key = create_hash (contact);
                ht_add (local_names, key, contact);
                haystack = haystack->next;
        }

        contacts = *needles;
        while (contacts) {
                EContact *contact = (contacts)->data;
                char *key;
                GList *local_matches;
                gboolean remove = FALSE;

                key = create_hash (contact);
                local_matches = g_hash_table_lookup (local_names, key);
                g_free (key);
                for (;local_matches; local_matches = local_matches->next) {
                        EContact *match = local_matches->data;
                        if (contact != match && contact_compare (contact, match)) {
                                /* get uid of duplicate contact from haystack */
                                const char *uid = e_contact_get_const (match,
                                                E_CONTACT_UID);
                                *duplicate_ids = g_list_prepend (*duplicate_ids,
                                                g_strdup (uid));
                                remove = TRUE;
                                break;
                        }
                }

                if (remove) {
                        /* remove duplicate from needles */
                        g_object_unref (contact);
                        contacts = *needles = g_list_delete_link (*needles, contacts);
                } else {
                        contacts = contacts->next;
                }
        }

        g_hash_table_destroy (local_names);
}

/**
 * e_book_util_remove_duplicates_using_book:
 *
 * @book: an #EBook
 * @contacts: a #GList of #EContacts
 * @duplicate_ids: a #GList of ids which are removed from @contacts
 * @error: #GError to set on failure
 * 
 * Removes duplicated contacts from @contacts list, so the resulting @contacts
 * list contains contacts that are not present in @book. @book should be opened
 * before this operation.
 *
 * Return value: TRUE on success, FALSE otherwise (if there was an error during
 * EBook operations)
 */
gboolean
e_book_util_remove_duplicates_using_book (EBook   *book,
                                          GList  **contacts,
                                          GList  **duplicate_ids,
                                          GError **error)
{
        EBookQuery *query;
        GList *local_contacts;

        g_return_val_if_fail (book != NULL, FALSE);
        g_return_val_if_fail (e_book_is_opened (book), FALSE);
        g_return_val_if_fail ((duplicate_ids != NULL && *duplicate_ids == NULL), FALSE);
        g_return_val_if_fail (error || !*error, FALSE);

        query = e_book_query_any_field_contains ("");
        e_book_get_contacts (book, query, &local_contacts, error);
        e_book_query_unref (query);
        if (error) {
                return FALSE;
        }

        e_book_util_remove_duplicates (local_contacts, contacts, duplicate_ids);
        while (local_contacts) {
                EContact *contact = local_contacts->data;
                g_object_unref (contact);
                local_contacts = g_list_delete_link (local_contacts, local_contacts);
        }
        return TRUE;
}

