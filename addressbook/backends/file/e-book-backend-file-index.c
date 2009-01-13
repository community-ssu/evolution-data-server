/*
 * Copyright (C) 2008 Nokia Corporation
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of version 2 of the GNU Lesser General Public License as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Author: Rob Bradford <rob@openedhand.com>
 */

#include "e-book-backend-file-index.h"
#include "libedataserver/e-sexp.h"
#include "libebook/e-contact.h"

#include "db.h"

#include <string.h>
#include <errno.h>

G_DEFINE_TYPE (EBookBackendFileIndex, e_book_backend_file_index, G_TYPE_OBJECT)

#define GET_PRIVATE(o) \
  (G_TYPE_INSTANCE_GET_PRIVATE ((o), E_TYPE_BOOK_BACKEND_FILE_INDEX, EBookBackendFileIndexPrivate))

/* structures used for the parsing side of things */

typedef struct 
{
  gchar *name;            /* function name */
  ESExpFunc *test_function;   /* for just testing to see if we can use the index */
  ESExpFunc *result_function; /* for actually producing a result */
} EBookBackendFileIndexSExpSymbol;

static ESExpResult *test_always_fails (ESExp *sexp, gint argc, ESExpResult **argv, gpointer userdata);
static ESExpResult *test_generic_field_is_indexed (ESExp *sexp, gint argc, ESExpResult **argv, gpointer userdata);

static ESExpResult *is_query (ESExp *sexp, gint argc, ESExpResult **argv, gpointer userdata);
static ESExpResult *beginswith_query (ESExp *sexp, gint argc, ESExpResult **argv, gpointer userdata);

static const EBookBackendFileIndexSExpSymbol sexp_symbols[] = {
  {"contains", test_always_fails, NULL},
  {"is", test_generic_field_is_indexed, is_query},
  {"beginswith", test_generic_field_is_indexed, beginswith_query},
  {"endswith", test_always_fails, NULL},
  {"exists", test_always_fails, NULL},
  {"exists_vcard", test_always_fails, NULL},
  {"is_vcard", test_always_fails, NULL},
};

/* structures used for maintaining the indexes */
typedef enum
{
  INDEX_FIELD_TYPE_STRING = 0,     /* single string */
  INDEX_FIELD_TYPE_STRING_LIST /* list of strings */
} EBookBackendFileIndexFieldType;

typedef struct 
{
  gchar *query_term;                    /* what the query uses to get this */
  gchar *index_name;                    /* name for index */
  gchar *vfield;                        /* the (vcard) field this an index of */
} EBookBackendFileIndexData;

static const EBookBackendFileIndexData indexes[] = {
  {"full-name", "full_name", EVC_FN},
  {"im-jabber", "im_jabber", EVC_X_JABBER},
  {"tel", "tel", EVC_TEL},
  {"email", "email", EVC_EMAIL},
};

typedef struct _EBookBackendFileIndexPrivate EBookBackendFileIndexPrivate;

struct _EBookBackendFileIndexPrivate {
  DB *db;  /* primary database */
  GHashTable *sdbs; /* mapping for query->term to database used for it */
};

static gboolean index_populate (EBookBackendFileIndex *index, GList *fields);
static void index_add_contact (EBookBackendFileIndex *index, EContact *contact, 
    EBookBackendFileIndexData *data);
static void index_remove_contact (EBookBackendFileIndex *index, EContact *contact, 
    EBookBackendFileIndexData *data);
static void index_sync (EBookBackendFileIndex *index, EBookBackendFileIndexData *data);
static gboolean index_close_db_func (gpointer key, gpointer value, gpointer userdata);
static void dbt_fill_with_string (DBT *dbt, gchar *str);

#define E_BOOK_BACKEND_FILE_VERSION_NAME "PAS-DB-VERSION"

static void
e_book_backend_file_index_dispose (GObject *object)
{
  EBookBackendFileIndex *index = (EBookBackendFileIndex *)object;
  EBookBackendFileIndexPrivate *priv = GET_PRIVATE (index);

  if (priv->sdbs)
  {
    /* close all the open index databases */
    g_hash_table_foreach_remove (priv->sdbs, index_close_db_func, 
        E_BOOK_BACKEND_FILE_INDEX (object));

    g_hash_table_unref (priv->sdbs);
    priv->sdbs = NULL;
  }

  if (G_OBJECT_CLASS (e_book_backend_file_index_parent_class)->dispose)
    G_OBJECT_CLASS (e_book_backend_file_index_parent_class)->dispose (object);
}

static void
e_book_backend_file_index_finalize (GObject *object)
{
  if (G_OBJECT_CLASS (e_book_backend_file_index_parent_class)->finalize)
    G_OBJECT_CLASS (e_book_backend_file_index_parent_class)->finalize (object);
}

static void
e_book_backend_file_index_class_init (EBookBackendFileIndexClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (EBookBackendFileIndexPrivate));

  object_class->dispose = e_book_backend_file_index_dispose;
  object_class->finalize = e_book_backend_file_index_finalize;
}

static void
e_book_backend_file_index_init (EBookBackendFileIndex *self)
{
  EBookBackendFileIndexPrivate *priv = GET_PRIVATE (self);

  priv->sdbs = g_hash_table_new (g_str_hash, g_str_equal);
}

/* 'public' api */
EBookBackendFileIndex *
e_book_backend_file_index_new (void)
{
  return g_object_new (E_TYPE_BOOK_BACKEND_FILE_INDEX, NULL);
}

/* can we use the index on this query, yay or nay ? */
gboolean
e_book_backend_file_index_is_usable (EBookBackendFileIndex *index, const gchar *query)
{
  ESExp *sexp = NULL;
  ESExpResult *sexp_result = NULL;
  gint i = 0;
  gint sexp_error = 0;
  gboolean res = TRUE; /* whether we can actually use the index */

  g_return_val_if_fail (E_IS_BOOK_BACKEND_FILE_INDEX (index), FALSE);
  g_return_val_if_fail (query != NULL, FALSE);

  if (g_getenv ("_EDS_FILE_BACKEND_NO_INDEX"))
    return FALSE;

  sexp = e_sexp_new ();

  /* Now add the 'test' functions, we just use these to find out whether we
   * can do a query. They return booleans and then ESexp logic should figure
   * out the logic...
   */
  for (i = 0; i < G_N_ELEMENTS (sexp_symbols); i++)
  {
    e_sexp_add_function (sexp, 0, sexp_symbols[i].name, 
        sexp_symbols[i].test_function, &res);
  }

  /* Pull in the query as the text for the s-exp */
  e_sexp_input_text (sexp, query, strlen (query));
  sexp_error = e_sexp_parse (sexp);

  if (sexp_error == -1)
  {
    g_warning (G_STRLOC ": Failed to parse query %s, %s", query, e_sexp_error (sexp));
    e_sexp_unref (sexp);
    return FALSE;
  }

  /* evaluate ... */
  sexp_result = e_sexp_eval(sexp);

  /* and then ignore the result */
  e_sexp_result_free (sexp, sexp_result);
  e_sexp_unref (sexp);

  g_debug (G_STRLOC ": able to use index %s", res ? "yes": "no");

  return res;
}

GPtrArray *
e_book_backend_file_index_query (EBookBackendFileIndex *index, const gchar *query)
{
  ESExp *sexp = NULL;
  ESExpResult *sexp_result = NULL;
  gint i = 0;
  gint sexp_error = 0;
  GPtrArray *res = NULL;

  g_return_val_if_fail (E_IS_BOOK_BACKEND_FILE_INDEX (index), FALSE);
  g_return_val_if_fail (query != NULL, FALSE);

  sexp = e_sexp_new ();

  g_debug (G_STRLOC ": Using query %s", query);

  /* Now add the 'test' functions, we just use these to find out whether we
   * can do a query. They return booleans and then ESexp logic should figure
   * out the logic...
   */
  for (i = 0; i < G_N_ELEMENTS (sexp_symbols); i++)
  {
    e_sexp_add_function (sexp, 0, sexp_symbols[i].name, 
        sexp_symbols[i].result_function, index);
  }

  /* Pull in the query as the text for the s-exp */
  e_sexp_input_text (sexp, query, strlen (query));
  sexp_error = e_sexp_parse (sexp);

  if (sexp_error == -1)
  {
    g_warning (G_STRLOC ": Failed to parse query %s, %s", query, e_sexp_error (sexp));
    e_sexp_unref (sexp);
    return NULL;
  }

  /* evaluate ... */
  sexp_result = e_sexp_eval(sexp);

  if (sexp_result)
  {
    if (sexp_result->type == ESEXP_RES_ARRAY_PTR)
    {
      /* we have to copy everything into a new GPtrArray because the old one
       * gets freed with e_sexp_result_free. :-(
       */
      res = g_ptr_array_sized_new (sexp_result->value.ptrarray->len);

      for (i = 0; i < sexp_result->value.ptrarray->len; i ++)
      {
        g_ptr_array_add (res, g_ptr_array_index (sexp_result->value.ptrarray, i));
      }

      e_sexp_result_free (sexp, sexp_result);
    } else {
      g_warning (G_STRLOC ": Unexpected result type");
    }
  }

  e_sexp_unref (sexp);
  return res;
}

/* setup indicies in index_filename for the primary database db */
gboolean
e_book_backend_file_index_setup_indicies (EBookBackendFileIndex *index, DB *db, 
    const gchar *index_filename)
{
  EBookBackendFileIndexPrivate *priv;

  g_return_val_if_fail (E_IS_BOOK_BACKEND_FILE_INDEX (index), FALSE);
  g_return_val_if_fail (db != NULL, FALSE);
  g_return_val_if_fail (index_filename != NULL, FALSE);

  priv = GET_PRIVATE (index);

  GList *dbs_to_populate = NULL; /* list of EBookBackendFileIndexData to populate */
  int db_error = 0;
  int i = 0;
  DB *sdb = NULL;
  DB_ENV *env = NULL;

  /* for each database we go through and try and create it exclusively, if
   * that fails then we know that we don't need to populate it so we go and
   * create it anyway and add it for the list of databases to populate */

  priv->db = db;

  db_error = db->get_env (db, &env);

  for (i = 0; i < G_N_ELEMENTS (indexes); i++)
  {
    db_error = db_create (&sdb, env, 0);

    if (db_error != 0) {
      g_warning (G_STRLOC ": db_create failed: %s", db_strerror (db_error));
      return FALSE;
    }

    db_error = sdb->set_flags(sdb, DB_DUP | DB_DUPSORT);
    if (db_error != 0) {
      g_warning (G_STRLOC ":set_flags failed: %s", db_strerror (db_error));
    }

    db_error = sdb->open (sdb, NULL, index_filename, 
        indexes[i].index_name, DB_BTREE, DB_CREATE | DB_EXCL | DB_THREAD, 0666);

    if (db_error == EEXIST) {

      /* try and open again, this time without exclusivity */
      db_error = sdb->open (sdb, NULL, index_filename,
          indexes[i].index_name, DB_BTREE, DB_CREATE | DB_THREAD, 0666);

      if (db_error != 0)
      {
        g_warning (G_STRLOC ": open failed: %s", db_strerror (db_error));
        sdb->close (sdb, 0);
        return FALSE;
      }
    } else if (db_error != 0) {
      if (db_error != 0)
      {
        g_warning (G_STRLOC ": open failed: %s", db_strerror (db_error));
        sdb->close (sdb, 0);
        return FALSE;
      }
    } else {
      /* we know need to populate this one */
      dbs_to_populate = g_list_prepend (dbs_to_populate, (gpointer)&(indexes[i]));
    }

    /* add it to the hash table of mappings of query term to database */
    g_hash_table_insert (priv->sdbs, indexes[i].query_term, sdb);
  }

  /* now go through and populate these */
  if (dbs_to_populate)
  {
    if (!index_populate (index, dbs_to_populate))
    {
      g_warning (G_STRLOC ": Error whilst trying to populate the index");
      g_list_free (dbs_to_populate);
      return FALSE;
    }
    g_list_free (dbs_to_populate);
  }
  e_book_backend_file_index_sync (index);

  return TRUE;
}

void
e_book_backend_file_index_add_contact (EBookBackendFileIndex *index, EContact *contact)
{
  gint i = 0;

  g_return_if_fail (E_IS_BOOK_BACKEND_FILE_INDEX (index));
  g_return_if_fail (E_IS_CONTACT (contact));

  /* for each index database try add this contact */
  for (i = 0; i < G_N_ELEMENTS (indexes); i++)
  {
    index_add_contact (index, contact, (EBookBackendFileIndexData *)&indexes[i]);
  }
}

void
e_book_backend_file_index_remove_contact (EBookBackendFileIndex *index, EContact *contact)
{
  gint i = 0;

  g_return_if_fail (E_IS_BOOK_BACKEND_FILE_INDEX (index));
  g_return_if_fail (E_IS_CONTACT (contact));

  /* for each index database try add this contact */
  for (i = 0; i < G_N_ELEMENTS (indexes); i++)
  {
    index_remove_contact (index, contact, (EBookBackendFileIndexData *)&indexes[i]);
  }
}

void
e_book_backend_file_index_modify_contact (EBookBackendFileIndex *index, EContact *old_contact, EContact *new_contact)
{
  gint i = 0;

  g_return_if_fail (E_IS_BOOK_BACKEND_FILE_INDEX (index));
  g_return_if_fail (E_IS_CONTACT (old_contact));
  g_return_if_fail (E_IS_CONTACT (new_contact));

  /* for each index database try add this contact */
  for (i = 0; i < G_N_ELEMENTS (indexes); i++)
  {
    index_remove_contact (index, old_contact, (EBookBackendFileIndexData *)&indexes[i]);
    index_add_contact (index, new_contact, (EBookBackendFileIndexData *)&indexes[i]);
  }
}

void
e_book_backend_file_index_sync (EBookBackendFileIndex *index)
{
  gint i = 0;

  g_return_if_fail (E_IS_BOOK_BACKEND_FILE_INDEX (index));

  /* for each index database try add this contact */
  for (i = 0; i < G_N_ELEMENTS (indexes); i++)
  {
    index_sync (index, (EBookBackendFileIndexData *)&indexes[i]);
  }
}

/* functions used for testing whether we can use the index */

/* always return false since we can't handle that type of query */
static ESExpResult *
test_always_fails (ESExp *sexp, gint argc, ESExpResult **argv, gpointer userdata)
{
  ESExpResult *result;

  result = e_sexp_result_new (sexp, ESEXP_RES_BOOL);

  *(gboolean *)userdata = FALSE;

  g_debug (G_STRLOC ": failing the test");
  return result;
}

/* 
 * return true or false based on whether we recognise that query field as
 * something we have an index for or not
 */
static ESExpResult *
test_generic_field_is_indexed (ESExp *sexp, gint argc, ESExpResult **argv, gpointer userdata)
{
  ESExpResult *result = NULL;
  gint i = 0;
  gchar *query_term = NULL;

  result = e_sexp_result_new (sexp, ESEXP_RES_BOOL);

  /* set to false now, we may set it to true in the loop below */
  result->value.bool = FALSE;

  /* we must have two arguments and they must both be strings */
  if (argc == 2 &&
      argv[0]->type == ESEXP_RES_STRING &&
      argv[1]->type == ESEXP_RES_STRING)
  {
    query_term = argv[0]->value.string;

    for (i = 0; i < G_N_ELEMENTS (indexes); i++)
    {
      if (query_term && g_str_equal (query_term, indexes[i].query_term))
      {
        return result;
      }
    }
  } else {
    g_warning (G_STRLOC ": Unexpected query structure");
  }
  
  *(gboolean *)userdata = FALSE;
  g_debug (G_STRLOC ": failing the test");

  return result;
}

/* index operations themselves */

/* boring helper function */
static void
dbt_fill_with_string (DBT *dbt, gchar *str)
{
  memset (dbt, 0, sizeof (DBT));
  dbt->data = (void *)str;
  dbt->size = strlen (str) + 1;
  dbt->flags = DB_DBT_USERMEM;
}

/* when given a list of fields, walk through the primary database, walk
 * through all contacts, parse them, then call the function to add to each
 * index that needs updating
 */
static gboolean 
index_populate (EBookBackendFileIndex *index, GList *fields)
{
  EBookBackendFileIndexPrivate *priv = GET_PRIVATE (index);

  GList *l = NULL;
  DBC *dbc = NULL; /* primary cursor */
  DBT id_dbt, vcard_dbt;
  EContact *contact = NULL;
  gint db_error = 0;
  DB *db = NULL;

  db = priv->db;

  db_error = db->cursor (db, NULL, &dbc, 0);

  if (db_error != 0)
  {
    g_warning (G_STRLOC ": db->cursor failed: %s", db_strerror (db_error)); 
    return FALSE;
  }

  memset (&id_dbt, 0, sizeof (id_dbt));
  memset (&vcard_dbt, 0, sizeof (vcard_dbt));

  db_error = dbc->c_get (dbc, &id_dbt, &vcard_dbt, DB_FIRST);

  while (db_error == 0)
  {
    if (!g_str_equal (id_dbt.data, E_BOOK_BACKEND_FILE_VERSION_NAME))
    {
      /* parse the vcard */
      contact = e_contact_new_from_vcard (vcard_dbt.data);

      if (contact)
      {
        /* now we interate through all the indexes that need doing */
        for (l = fields; l != NULL; l = l->next)
        {
          index_add_contact (index, contact, (EBookBackendFileIndexData *)l->data);
        }
      }
    }

    db_error = dbc->c_get (dbc, &id_dbt, &vcard_dbt, DB_NEXT);
  }

  if (db_error != DB_NOTFOUND)
  {
    g_warning (G_STRLOC ":dbc->c_get failed: %s", db_strerror (db_error));
    return FALSE;
  }

  db_error = dbc->c_close (dbc);

  if (db_error != 0)
  {
    g_warning (G_STRLOC ":dbc->c_close failed: %s", db_strerror (db_error));
    return FALSE;
  }

  return TRUE;
}

/* 
 * for a given index add this contact, for the single string field case we
 * just add a single entry to the index database. for the multiple case we add
 * all the values for the field
 */
static void
index_add_contact (EBookBackendFileIndex *index, EContact *contact, 
    EBookBackendFileIndexData *data)
{
  EBookBackendFileIndexPrivate *priv = GET_PRIVATE (index);
  gchar *uid = NULL;
  DBT index_dbt, id_dbt;
  DB *db = NULL;
  GList *attrs; 
  GList *values;
  gint db_error = 0;
  gchar *tmp = NULL;

  /* we need the uid, this becomes the data for our index databases */
  uid = (gchar *)e_contact_get (contact, E_CONTACT_UID);
  dbt_fill_with_string (&id_dbt, uid);

  /* we need the index database too */
  db = g_hash_table_lookup (priv->sdbs, data->query_term);

  for (attrs = e_vcard_get_attributes (E_VCARD (contact)); 
      attrs != NULL; 
      attrs = attrs->next)
  {
    EVCardAttribute *attr = (EVCardAttribute *)attrs->data;

    if (g_str_equal (e_vcard_attribute_get_name (attr), data->vfield))
    {
      for (values = e_vcard_attribute_get_values (attr);
          values != NULL; 
          values = values->next)
      {
        tmp = g_utf8_casefold (values->data, -1);
        dbt_fill_with_string (&index_dbt, tmp);

        g_debug (G_STRLOC ": adding to index with key %s and data %s", 
            (gchar *)index_dbt.data, (gchar *)id_dbt.data);

        db_error = db->put (db, NULL, &index_dbt, &id_dbt, 0);
        g_free (tmp);

        if (db_error != 0)
        {
          g_warning (G_STRLOC ": db->put failed: %s", db_strerror (db_error));
        }
      }
    }
  }
  g_free (uid);
}

/* 
 * for a given index remove this contact
 */
static void
index_remove_contact (EBookBackendFileIndex *index, EContact *contact, 
    EBookBackendFileIndexData *data)
{
  EBookBackendFileIndexPrivate *priv = GET_PRIVATE (index);
  gchar *uid = NULL;
  DBT index_dbt, id_dbt;
  DB *db = NULL;
  GList *attrs; 
  GList *values;
  gint db_error = 0;
  gchar *tmp = NULL;
  DBC *dbc = NULL; /* cursor */

  /* we need the uid, so that we can match both key and value of index-entries */
  uid = (gchar *)e_contact_get (contact, E_CONTACT_UID);
  dbt_fill_with_string (&id_dbt, uid);

  /* we need the index database too */
  db = g_hash_table_lookup (priv->sdbs, data->query_term);

  /* create the cursor for the interation */
  db_error = db->cursor (db, NULL, &dbc, 0);

  if (db_error != 0)
  {
    g_warning (G_STRLOC ": db->cursor failed: %s", db_strerror (db_error)); 
    return;
  }

  for (attrs = e_vcard_get_attributes (E_VCARD (contact)); 
      attrs != NULL; 
      attrs = attrs->next)
  {
    EVCardAttribute *attr = (EVCardAttribute *)attrs->data;

    if (g_str_equal (e_vcard_attribute_get_name (attr), data->vfield))
    {
      for (values = e_vcard_attribute_get_values (attr);
          values != NULL; 
          values = values->next)
      {
        tmp = g_utf8_casefold (values->data, -1);
        dbt_fill_with_string (&index_dbt, tmp);

        g_debug (G_STRLOC ": removing from index with key %s and data %s", 
            (gchar *)index_dbt.data, (gchar *)id_dbt.data);

        id_dbt.flags = 0;
        index_dbt.flags = 0;
        db_error = dbc->c_get (dbc, &index_dbt, &id_dbt, DB_GET_BOTH);
        if (db_error != 0)
        {
          g_warning (G_STRLOC ": dbc->c_get failed: %s", db_strerror (db_error));
        } else {
          db_error = dbc->c_del (dbc, 0);
          if (db_error != 0)
          {
            g_warning (G_STRLOC ": dbc->c_del failed: %s", db_strerror (db_error));
          }
        }
        g_free (tmp);
      }
    }
  }
  db_error = dbc->c_close (dbc);

  if (db_error != 0)
  {
    g_warning (G_STRLOC ": db->c_closed failed: %s", db_strerror (db_error));
  }
  g_free (uid);
}

static void
index_sync (EBookBackendFileIndex *index, EBookBackendFileIndexData *data)
{
  EBookBackendFileIndexPrivate *priv = GET_PRIVATE (index);
  DB *db = NULL;
  gint db_error = 0;

  db = g_hash_table_lookup (priv->sdbs, data->query_term);

  db_error = db->sync (db, 0);

  if (db_error != 0)
  {
    g_warning (G_STRLOC ": db->sync failed: %s", db_strerror (db_error));
  }
}

static ESExpResult *
is_query (ESExp *sexp, gint argc, ESExpResult **argv, gpointer userdata)
{
  EBookBackendFileIndex *index = (EBookBackendFileIndex *)userdata;
  EBookBackendFileIndexPrivate *priv = GET_PRIVATE (index);
  
  ESExpResult *result = NULL;
  DBT index_dbt, id_dbt;
  DB *db = NULL;
  gint db_error = 0;
  gchar *query_term = NULL, *query_key = NULL;
  GPtrArray *ids = NULL;
  DBC *dbc = NULL;

  ids = g_ptr_array_new ();

  /* we must have two arguments and they must both be strings */
  if (argc == 2 &&
      argv[0]->type == ESEXP_RES_STRING &&
      argv[1]->type == ESEXP_RES_STRING)
  {
    /* we need the index database too */
    query_term = argv[0]->value.string;
    db = g_hash_table_lookup (priv->sdbs, query_term);

    /* populate a dbt for looking up in the index */
    query_key = g_utf8_casefold (argv[1]->value.string, -1);
    dbt_fill_with_string (&index_dbt, query_key);

    /* we want bdb to use g_malloc this memory for us */
    memset (&id_dbt, 0, sizeof (id_dbt));
    id_dbt.flags = DB_DBT_MALLOC;

    /* cursor */
    db_error = db->cursor (db, NULL, &dbc, 0);

    if (db_error != 0)
    {
      g_warning (G_STRLOC ": db->cursor failed: %s", db_strerror (db_error)); 
      goto out;
    } else {
      db_error = dbc->c_get (dbc, &index_dbt, &id_dbt, DB_SET);

      while (db_error == 0)
      {
        g_debug (G_STRLOC ": index query found: %s", (gchar *)id_dbt.data);
        g_ptr_array_add (ids, g_strdup (id_dbt.data));
        g_free (id_dbt.data);

        /* clear the index dbt since this time bdb will push memory into it */
        memset (&index_dbt, 0, sizeof (index_dbt));
        db_error = dbc->c_get (dbc, &index_dbt, &id_dbt, DB_NEXT_DUP);
      }

      if (db_error != DB_NOTFOUND)
      {
        g_warning (G_STRLOC ": dbc->c_get failed: %s", db_strerror (db_error));
        goto out;
      }

      db_error = dbc->c_close (dbc);

      if (db_error != 0)
      {
        g_warning (G_STRLOC ": dbc->c_close failed: %s", db_strerror (db_error));
        goto out;
      }
    }
  } else {
    g_warning (G_STRLOC ": Unexpected query structure");
  }

out:

  g_free (query_key);
  result = e_sexp_result_new (sexp, ESEXP_RES_ARRAY_PTR);
  result->value.ptrarray = ids;

  return result;
}

static ESExpResult *
beginswith_query (ESExp *sexp, gint argc, ESExpResult **argv, gpointer userdata)
{
  EBookBackendFileIndex *index = (EBookBackendFileIndex *)userdata;
  EBookBackendFileIndexPrivate *priv = GET_PRIVATE (index);
  
  ESExpResult *result = NULL;
  DBT index_dbt, id_dbt;
  DB *db = NULL;
  gint db_error = 0;
  gchar *query_term = NULL, *query_key = NULL;
  GPtrArray *ids = NULL;
  DBC *dbc = NULL;

  ids = g_ptr_array_new ();

  /* we must have two arguments and they must both be strings */
  if (argc == 2 &&
      argv[0]->type == ESEXP_RES_STRING &&
      argv[1]->type == ESEXP_RES_STRING)
  {
    /* we need the index database too */
    query_term = argv[0]->value.string;
    db = g_hash_table_lookup (priv->sdbs, query_term);

    /* populate a dbt for looking up in the index */
    query_key = g_utf8_casefold (argv[1]->value.string, -1);
    dbt_fill_with_string (&index_dbt, query_key);

    /* we want bdb to use g_malloc this memory for us */
    memset (&id_dbt, 0, sizeof (id_dbt));
    id_dbt.flags = DB_DBT_MALLOC;

    /* we want bdb to actually manage chunk of memory for returning the
     * actual key to us
     */
    index_dbt.flags = 0;

    /* cursor */
    db_error = db->cursor (db, NULL, &dbc, 0);

    if (db_error != 0)
    {
      g_warning (G_STRLOC ": db->cursor failed: %s", db_strerror (db_error)); 
      goto out;
    } else {
      db_error = dbc->c_get (dbc, &index_dbt, &id_dbt, DB_SET_RANGE);

      while (db_error == 0)
      {
        if (g_str_has_prefix (index_dbt.data, query_key))
        {
          g_debug (G_STRLOC ": index query found: %s for %s for %s", 
              (gchar *)id_dbt.data, query_key, (gchar *)index_dbt.data);
          g_ptr_array_add (ids, g_strdup (id_dbt.data));
          g_free (id_dbt.data);

          memset (&index_dbt, 0, sizeof (index_dbt));
          db_error = dbc->c_get (dbc, &index_dbt, &id_dbt, DB_NEXT);
        } else {
          break;
        }
      }

      if (db_error != 0 && db_error != DB_NOTFOUND)
      {
        g_warning (G_STRLOC ": dbc->c_get failed: %s", db_strerror (db_error));
        goto out;
      }

      db_error = dbc->c_close (dbc);

      if (db_error != 0)
      {
        g_warning (G_STRLOC ": dbc->c_close failed: %s", db_strerror (db_error));
        goto out;
      }
    }
  } else {
    g_warning (G_STRLOC ": Unexpected query structure");
  }

out:
  g_free (query_key);

  result = e_sexp_result_new (sexp, ESEXP_RES_ARRAY_PTR);
  result->value.ptrarray = ids;

  return result;
}

static gboolean
index_close_db_func (gpointer key, gpointer value, gpointer userdata)
{
  DB *db = (DB *)value;
  gint db_error = 0;

  db_error = db->close (db, 0);

  if (db_error != 0)
  {
    g_warning (G_STRLOC ": close failed: %s", db_strerror (db_error));
  }

  return TRUE;
}