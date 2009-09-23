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
#include "e-book-backend-file-log.h"
#include "e-book-backend-file-utils.h"

#include "libedataserver/e-sexp.h"
#include "libebook/e-contact.h"
#include "libebook/e-book-util.h"

#include "db.h"

#include <glib/gstdio.h>
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
static ESExpResult *test_generic_field_is_indexed_vcard (ESExp *sexp, gint argc, ESExpResult **argv, gpointer userdata);
static ESExpResult *test_generic_field_is_suffix_indexed (ESExp *sexp, gint argc, ESExpResult **argv, gpointer userdata);
static ESExpResult *test_generic_field_is_suffix_indexed_vcard (ESExp *sexp, gint argc, ESExpResult **argv, gpointer userdata);

static ESExpResult *is_query (ESExp *sexp, gint argc, ESExpResult **argv, gpointer userdata);
static ESExpResult *is_vcard_query (ESExp *sexp, gint argc, ESExpResult **argv, gpointer userdata);
static ESExpResult *beginswith_query (ESExp *sexp, gint argc, ESExpResult **argv, gpointer userdata);
static ESExpResult *beginswith_vcard_query (ESExp *sexp, gint argc, ESExpResult **argv, gpointer userdata);
static ESExpResult *endswith_query (ESExp *sexp, gint argc, ESExpResult **argv, gpointer userdata);
static ESExpResult *endswith_vcard_query (ESExp *sexp, gint argc, ESExpResult **argv, gpointer userdata);

static const EBookBackendFileIndexSExpSymbol sexp_symbols[] = {
  {"contains", test_always_fails, NULL},
  {"contains_vcard", test_always_fails, NULL},
  {"is", test_generic_field_is_indexed, is_query},
  {"is_vcard", test_generic_field_is_indexed_vcard, is_vcard_query},
  {"beginswith", test_generic_field_is_indexed, beginswith_query},
  {"beginswith_vcard", test_generic_field_is_indexed_vcard, beginswith_vcard_query},
  {"endswith", test_generic_field_is_suffix_indexed, endswith_query},
  {"endswith_vcard", test_generic_field_is_suffix_indexed_vcard, endswith_vcard_query},
  {"exists", test_always_fails, NULL},
  {"exists_vcard", test_always_fails, NULL},
};

/* structures used for maintaining the indexes */
typedef enum
{
  INDEX_FIELD_TYPE_STRING = 0,     /* single string */
  INDEX_FIELD_TYPE_STRING_LIST /* list of strings */
} EBookBackendFileIndexFieldType;

typedef struct _EBookBackendFileIndexData EBookBackendFileIndexData;

typedef int (*EBookBackendFileIndexAddFunc) (EBookBackendFileIndex *index, EContact *contact,
    EBookBackendFileIndexData *data, DB *db, const gchar *id, GPtrArray *ops);
typedef int (*EBookBackendFileIndexRemoveFunc) (EBookBackendFileIndex *index, EContact *contact,
    EBookBackendFileIndexData *data, DB *db, const gchar *id, GPtrArray *ops);

typedef int (*EBookBackendFileIndexOrderFunc) (DB *secondary, const DBT *akey, const DBT *bkey);

struct _EBookBackendFileIndexData
{
  gchar *query_term;                    /* what the query uses to get this */
  gchar *index_name;                    /* name for index */
  gboolean suffix;                      /* Index supports fast suffix querries */
  EBookBackendFileIndexAddFunc contact_add_func; /* function to use to derive index data */
  EBookBackendFileIndexRemoveFunc contact_remove_func; /* function to use to derive index data */
  gchar *vfield;                        /* the (vcard) field this an index of */
  EBookBackendFileIndexOrderFunc order_func; /* ordering func */
};

static int first_last_add_cb (EBookBackendFileIndex *index, EContact *contact,
    EBookBackendFileIndexData *data, DB *db, const gchar *uid, GPtrArray *ops);
static int last_first_add_cb (EBookBackendFileIndex *index, EContact *contact,
    EBookBackendFileIndexData *data, DB *db, const gchar *uid, GPtrArray *ops);

static int first_last_remove_cb (EBookBackendFileIndex *index, EContact *contact,
    EBookBackendFileIndexData *data, DB *db, const gchar *uid, GPtrArray *ops);
static int last_first_remove_cb (EBookBackendFileIndex *index, EContact *contact,
    EBookBackendFileIndexData *data, DB *db, const gchar *uid, GPtrArray *ops);

static int lexical_ordering_cb (DB *secondary, const DBT *akey, const DBT *bkey);

static const EBookBackendFileIndexData indexes[] = {
  {"full-name", "full_name", FALSE, NULL, NULL, EVC_FN, lexical_ordering_cb},
  {"im-jabber", "im_jabber", FALSE, NULL, NULL, EVC_X_JABBER, NULL},
  {"phone", "phone", TRUE, NULL, NULL, EVC_TEL, NULL},
  {"email", "email", FALSE, NULL, NULL, EVC_EMAIL, NULL},
  {"first-last", "first_last", FALSE, first_last_add_cb, first_last_remove_cb, NULL, lexical_ordering_cb},
  {"last-first", "last_first", FALSE, last_first_add_cb, last_first_remove_cb, NULL, lexical_ordering_cb},
};

typedef struct _EBookBackendFileIndexPrivate EBookBackendFileIndexPrivate;

struct _EBookBackendFileIndexPrivate {
  DB *db;  /* primary database */
  GHashTable *sdbs; /* mapping for query->term to database used for it */
  char *index_dirname; /* dirname where the index dbs are stored */
};

static gboolean index_populate (EBookBackendFileIndex *index, GList *fields, GPtrArray *ops);
static int index_add_contact (EBookBackendFileIndex *index, EContact *contact,
    EBookBackendFileIndexData *data, GPtrArray *ops);
static int index_remove_contact (EBookBackendFileIndex *index, EContact *contact,
    EBookBackendFileIndexData *data, GPtrArray *ops);
static void index_sync (EBookBackendFileIndex *index, const EBookBackendFileIndexData *data);
static gboolean index_close_db_func (gpointer key, gpointer value, gpointer userdata);

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
  EBookBackendFileIndexPrivate *priv = GET_PRIVATE (E_BOOK_BACKEND_FILE_INDEX (object));

  g_free (priv->index_dirname);
  priv->index_dirname = NULL;

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
    WARNING ("Failed to parse query %s, %s", query, e_sexp_error (sexp));
    e_sexp_unref (sexp);
    return FALSE;
  }

  /* evaluate ... */
  sexp_result = e_sexp_eval(sexp);

  /* and then ignore the result */
  e_sexp_result_free (sexp, sexp_result);
  e_sexp_unref (sexp);

  DEBUG ("able to use index: %s", res ? "yes": "no");

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

  DEBUG ("Using query %s", query);

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
    WARNING ("Failed to parse query %s, %s", query, e_sexp_error (sexp));
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
      WARNING ("Unexpected result type");
    }
  }

  e_sexp_unref (sexp);
  return res;
}

static char *
make_index_db_path (const char *index_dir, const char *index_name)
{
  return g_strdup_printf ("%s/index_%s.db", index_dir, index_name);
}

/* create and set up a DB object for an index db */
static int
create_index_db (DB_ENV *env, DB **dbp, EBookBackendFileIndexOrderFunc order_func)
{
  int db_error;
  DB *db = NULL;

  g_return_val_if_fail (env != NULL, EINVAL);

  db_error = db_create (&db, env, 0);
  if (db_error != 0) {
    WARNING ("db_create failed: %s", db_strerror (db_error));
    goto err;
  }

  db_error = db->set_flags (db, DB_DUP | DB_DUPSORT);
  if (db_error != 0) {
    WARNING ("set_flags failed: %s", db_strerror (db_error));
    goto err;
  }

  if (order_func) {
    db_error = db->set_bt_compare (db, order_func);
    if (db_error != 0) {
      WARNING ("set_bt_compare failed: %s", db_strerror (db_error));
      goto err;
    }
  }

  *dbp = db;
  return 0;

err:
  if (db)
    db->close (db, 0);
  return db_error;
}

/* setup indicies in index_filename for the primary database db */
gboolean
e_book_backend_file_index_setup_indicies (EBookBackendFileIndex *index, DB *db, const char *index_dirname)
{
  EBookBackendFileIndexPrivate *priv;

  g_return_val_if_fail (E_IS_BOOK_BACKEND_FILE_INDEX (index), FALSE);
  g_return_val_if_fail (db != NULL, FALSE);
  g_return_val_if_fail (index_dirname != NULL, FALSE);

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
  priv->index_dirname = g_strdup (index_dirname);

  db_error = db->get_env (db, &env);
  if (db_error != 0) {
    WARNING ("get_env failed: %s", db_strerror (db_error));
    return FALSE;
  }

  for (i = 0; i < G_N_ELEMENTS (indexes); i++)
  {
    char *index_db_path;

    db_error = create_index_db (env, &sdb, indexes[i].order_func);
    if (db_error != 0) {
      return FALSE;
    }

    index_db_path = make_index_db_path (index_dirname, indexes[i].index_name);

    /* we cannot pass DB_CREATE in the first time, because if index dbs aren't
     * present we need to populate them */
    db_error = sdb->open (sdb, NULL, index_db_path, NULL, DB_BTREE,
        DB_THREAD | DB_AUTO_COMMIT, 0666);
    if (db_error != 0)
    {
      DEBUG ("%s doesn't exist, try to create it", index_db_path);

      /* close and create the DB object again to avoid memleak */
      sdb->close (sdb, 0);
      db_error = create_index_db (env, &sdb, indexes[i].order_func);
      if (db_error != 0)
      {
        g_free (index_db_path);
        return FALSE;
      }

      db_error = sdb->open (sdb, NULL, index_db_path, NULL, DB_BTREE,
          DB_CREATE | DB_THREAD | DB_AUTO_COMMIT, 0666);
      if (db_error != 0)
      {
        WARNING ("creating %s failed: %s", index_db_path, db_strerror (db_error));
        sdb->close (sdb, 0);
        g_free (index_db_path);
        return FALSE;
      }

      /* db didn't exist before, so we now need to populate this one */
      dbs_to_populate = g_list_prepend (dbs_to_populate, (gpointer)&(indexes[i]));
    }

    g_free (index_db_path);

    /* add it to the hash table of mappings of query term to database */
    g_hash_table_insert (priv->sdbs, indexes[i].query_term, sdb);
  }

  /* now go through and populate these */
  if (dbs_to_populate)
  {
    GPtrArray *ops = g_ptr_array_new ();

    if (!index_populate (index, dbs_to_populate, ops))
    {
      WARNING ("Error whilst trying to populate the index");
      txn_ops_free (ops);
      g_list_free (dbs_to_populate);
      return FALSE;
    }
    g_list_free (dbs_to_populate);

    /* create and commit the transaction */
    db_error = txn_execute_ops (env, NULL, ops);
    txn_ops_free (ops);

    if (db_error != 0) {
      WARNING ("cannot commit index populate transaction: %s", db_strerror (db_error));
      return FALSE;
    }

    /* sync the db file, since we created dbs here */
    e_book_backend_file_index_sync (index);
  }

  return TRUE;
}

int
e_book_backend_file_index_add_contact (EBookBackendFileIndex *index, EContact *contact, GPtrArray *ops)
{
  gint i = 0;
  int retval = 0;

  g_return_val_if_fail (E_IS_BOOK_BACKEND_FILE_INDEX (index), EINVAL);
  g_return_val_if_fail (E_IS_CONTACT (contact), EINVAL);
  g_return_val_if_fail (ops, EINVAL);

  /* for each index database try add this contact */
  for (i = 0; i < G_N_ELEMENTS (indexes); i++)
  {
    retval = index_add_contact (index, contact, (EBookBackendFileIndexData *)&indexes[i], ops);
    if (retval != 0) {
      break;
    }
  }

  return retval;
}

int
e_book_backend_file_index_remove_contact (EBookBackendFileIndex *index, EContact *contact, GPtrArray *ops)
{
  gint i = 0;
  int retval = 0;

  g_return_val_if_fail (E_IS_BOOK_BACKEND_FILE_INDEX (index), EINVAL);
  g_return_val_if_fail (E_IS_CONTACT (contact), EINVAL);
  g_return_val_if_fail (ops, EINVAL);

  /* for each index database try add this contact */
  for (i = 0; i < G_N_ELEMENTS (indexes); i++)
  {
    retval = index_remove_contact (index, contact, (EBookBackendFileIndexData *)&indexes[i], ops);
    if (retval != 0) {
      break;
    }
  }

  return retval;
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
    index_remove_contact (index, old_contact, (EBookBackendFileIndexData *)&indexes[i], NULL); // TODO: ops
    index_add_contact (index, new_contact, (EBookBackendFileIndexData *)&indexes[i], NULL); // TODO: ops
  }
}

void
e_book_backend_file_index_sync (EBookBackendFileIndex *index)
{
  gint i = 0;

  g_return_if_fail (E_IS_BOOK_BACKEND_FILE_INDEX (index));

  /* sync each index database */
  for (i = 0; i < G_N_ELEMENTS (indexes); i++)
  {
    index_sync (index, &indexes[i]);
  }
}

GPtrArray *
e_book_backend_file_index_get_ordered_ids (EBookBackendFileIndex *index, const gchar *query_term)
{
  EBookBackendFileIndexPrivate *priv;

  DB *db = NULL;
  DBC *dbc = NULL;
  GPtrArray *ids = NULL;
  int db_error = 0;
  DB_BTREE_STAT *stat = NULL;
  DBT index_dbt, id_dbt;

  g_return_val_if_fail (E_IS_BOOK_BACKEND_FILE_INDEX (index), NULL);
  g_return_val_if_fail (query_term != NULL, NULL);

  priv = GET_PRIVATE (index);

  db = g_hash_table_lookup (priv->sdbs, query_term);

  if (db == NULL)
  {
    WARNING ("invalid query term: %s", query_term);
    return NULL;
  }

  db_error = db->stat (db, &stat, DB_FAST_STAT);

  if (db_error != 0)
  {
    WARNING ("db->stat failed: %s", db_strerror (db_error));
    stat = NULL;
  }

  if (stat && stat->bt_ndata > 0)
    ids = g_ptr_array_sized_new (stat->bt_ndata);
  else
    ids = g_ptr_array_sized_new (128);

  /* free the stat struct, since it's allocated with g_try_malloc
   * as we set up the allocation routine by env->set_alloc */
  g_free (stat);

  db_error = db->cursor (db, NULL, &dbc, 0);

  if (db_error != 0)
  {
    WARNING ("db->cursor failed: %s", db_strerror (db_error));
    return NULL;
  }

  memset (&index_dbt, 0, sizeof (id_dbt));
  memset (&id_dbt, 0, sizeof (id_dbt));

  id_dbt.flags = DB_DBT_MALLOC;

  db_error = dbc->c_get (dbc, &index_dbt, &id_dbt, DB_FIRST);

  while (db_error == 0)
  {
    g_ptr_array_add (ids, id_dbt.data);
    db_error = dbc->c_get (dbc, &index_dbt, &id_dbt, DB_NEXT);
  }

  if (db_error != DB_NOTFOUND)
  {
    WARNING ("dbc->c_get failed: %s", db_strerror (db_error));
  }

  db_error = dbc->c_close (dbc);

  if (db_error != 0)
  {
    WARNING ("dbc->c_close failed: %s", db_strerror (db_error));
  }

  return ids;
}

void
e_book_backend_file_index_remove_dbs (EBookBackendFileIndex *index)
{
  EBookBackendFileIndexPrivate *priv;
  int i;

  g_return_if_fail (E_IS_BOOK_BACKEND_FILE_INDEX (index));

  priv = GET_PRIVATE (index);

  /* close dbs first */
  if (priv->sdbs) {
    g_hash_table_foreach_remove (priv->sdbs, index_close_db_func, index);

    g_hash_table_unref (priv->sdbs);
    priv->sdbs = NULL;
  }

  /* remove files */
  for (i = 0; i < G_N_ELEMENTS (indexes); i++) {
    char *index_db_path = make_index_db_path (priv->index_dirname, indexes[i].index_name);
    if (-1 == g_unlink (index_db_path))
      WARNING ("failed to remove %s: %s", index_db_path, strerror (errno));
    g_free (index_db_path);
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

  DEBUG ("failing the test");
  return result;
}

/*
 * return true or false based on whether we recognise that query field as
 * something we have an index for or not
 */
static ESExpResult *
real_test_generic_field_is_indexed (gboolean vcard_query, gboolean endswith_query,
                                    ESExp *sexp, gint argc, ESExpResult **argv, gpointer userdata)
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

    g_return_val_if_fail (query_term != NULL, NULL);
    if (vcard_query)
    {
      for (i = 0; i < G_N_ELEMENTS (indexes); i++)
      {
        if (endswith_query == indexes[i].suffix &&
            0 == g_strcmp0 (query_term, indexes[i].vfield))
        {
          return result;
        }
      }
    } else {
      for (i = 0; i < G_N_ELEMENTS (indexes); i++)
      {
        if (endswith_query == indexes[i].suffix &&
            0 == g_strcmp0 (query_term, indexes[i].query_term))
        {
          return result;
        }
      }
    }

  } else {
    WARNING ("Unexpected query structure");
  }

  *(gboolean *)userdata = FALSE;
  DEBUG ("failing the test");

  return result;
}
static ESExpResult *
test_generic_field_is_indexed (ESExp *sexp, gint argc, ESExpResult **argv, gpointer userdata)
{
  return real_test_generic_field_is_indexed (FALSE, FALSE, sexp, argc, argv, userdata);
}

static ESExpResult *
test_generic_field_is_indexed_vcard (ESExp *sexp, gint argc, ESExpResult **argv, gpointer userdata)
{
  return real_test_generic_field_is_indexed (TRUE, FALSE, sexp, argc, argv, userdata);
}

static ESExpResult *
test_generic_field_is_suffix_indexed (ESExp *sexp, gint argc, ESExpResult **argv, gpointer userdata)
{
  return real_test_generic_field_is_indexed (FALSE, TRUE, sexp, argc, argv, userdata);
}

static ESExpResult *
test_generic_field_is_suffix_indexed_vcard (ESExp *sexp, gint argc, ESExpResult **argv, gpointer userdata)
{
  return real_test_generic_field_is_indexed (TRUE, TRUE, sexp, argc, argv, userdata);
}

/* index operations themselves */

/* when given a list of fields, walk through the primary database, walk
 * through all contacts, parse them, then call the function to add to each
 * index that needs updating
 */
static gboolean
index_populate (EBookBackendFileIndex *index, GList *fields, GPtrArray *ops)
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
    WARNING ("db->cursor failed: %s", db_strerror (db_error));
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
          if (0 != index_add_contact (index, contact, (EBookBackendFileIndexData *)l->data, ops)) {
            WARNING ("cannot populate index");
            g_object_unref (contact);
            return FALSE;
          }
        }
        g_object_unref (contact);
      }
    }

    db_error = dbc->c_get (dbc, &id_dbt, &vcard_dbt, DB_NEXT);
  }

  if (db_error != DB_NOTFOUND)
  {
    WARNING ("dbc->c_get failed: %s", db_strerror (db_error));
    return FALSE;
  }

  db_error = dbc->c_close (dbc);

  if (db_error != 0)
  {
    WARNING ("dbc->c_close failed: %s", db_strerror (db_error));
    return FALSE;
  }

  return TRUE;
}

static char *key_priority[] = {
  EVC_N,
  EVC_NICKNAME,
  EVC_X_COMPANY,
  EVC_TEL,
  EVC_EMAIL,
  NULL
};

static EVCardAttribute *
get_key_attribute (EContact *contact)
{
  GList *attrs = NULL;
  GHashTable *ht = NULL;
  EVCardAttribute *key_attr = NULL;
  int i;

  g_return_val_if_fail (E_IS_CONTACT (contact), NULL);

  ht = g_hash_table_new (g_str_hash, g_str_equal);

  /* seems that EVC_N is always the last element, speeding up */
  attrs = e_vcard_get_attributes (E_VCARD (contact));
  for (attrs = g_list_last (attrs);
       attrs != NULL;
       attrs = attrs->prev)
  {
    EVCardAttribute *attr = (EVCardAttribute *)attrs->data;
    const char *attr_name = e_vcard_attribute_get_name (attr);

    DEBUG ("looking attrib '%s'", attr_name);

    if (g_str_equal (attr_name, key_priority[0])) {
      if (!e_vcard_attribute_get_values (attr))
        continue;

      g_hash_table_destroy (ht);
      return attr;
    }

    g_hash_table_insert (ht, (gpointer)attr_name, (gpointer)attr);
  }

  /* iterate from 2nd element since 1st was not found */
  for (i = 1; key_priority[i]; i++) {
    key_attr = (EVCardAttribute *) g_hash_table_lookup (ht, key_priority[i]);
    if (key_attr && e_vcard_attribute_get_values (key_attr)) {
      break;
    }
  }

  /* get the "first" item from hash table as key_attr if it is still NULL */
  if (G_UNLIKELY (!key_attr)) {
    GHashTableIter iter;
    gpointer value;

    g_hash_table_iter_init (&iter, ht);
    if (g_hash_table_iter_next (&iter, NULL, &value))
      key_attr = (EVCardAttribute *) value;
    else {
      WARNING ("cannot obtain key_attr");
      key_attr = NULL;
    }
  }

  g_hash_table_destroy (ht);
  return key_attr;
}

enum
{
  ORDER_FIRSTLAST = 0,
  ORDER_LASTFIRST
};

static char *
get_key (EContact *contact, gint ordering)
{
  EVCardAttribute *key_attr;
  GList *values;
  const char *first, *last;
  char *combined, *key;

  g_return_val_if_fail (E_IS_CONTACT (contact), NULL);

  key_attr = get_key_attribute (contact);
  if (!key_attr) {
    WARNING ("contact is empty?");
    return NULL;
  }

  values = e_vcard_attribute_get_values (key_attr);
  if (!values) {
    WARNING ("attrib's value is empty, this shouldn't happen");
    return NULL;
  }

  last = values->data ? values->data : "";
  values = values->next;
  first = values && values->data ? values->data : "";

  if (ordering == ORDER_FIRSTLAST) {
    combined = g_strdup_printf ("%s %s", first, last);
  } else {
    combined = g_strdup_printf ("%s %s", last, first);
  }

  key = g_utf8_casefold (combined, -1);
  g_free (combined);

  return key;
}

static int
generic_name_add_cb (EBookBackendFileIndex *index, EContact *contact,
    EBookBackendFileIndexData *data, DB *db, const gchar *uid, gint ordering, GPtrArray *ops)
{
  int db_error;
  char *key;

  key = get_key (contact, ordering);
  if (!key) {
    WARNING ("cannot create a sufficient key value");
    return EINVAL;
  }

  DEBUG ("adding to index with key %s and data %s", key, uid);

  db_error = txn_ops_add_new (ops, TXN_PUT, db, key, g_strdup (uid));
  if (db_error != 0) {
    WARNING ("cannot create new txn item: %s", db_strerror (db_error));
  }

  return db_error;
}

static int
generic_name_remove_cb (EBookBackendFileIndex *index, EContact *contact,
    EBookBackendFileIndexData *data, DB *db, const gchar *uid, gint ordering, GPtrArray *ops)
{
  int db_error;
  char *key;

  key = get_key (contact, ordering);
  if (!key) {
    /* TODO: remove index only by uid */
    WARNING ("cannot determine the key value");
    return EINVAL;
  }

  DEBUG ("removing from index with key %s and data %s", key, uid);

  db_error = txn_ops_add_new (ops, TXN_CDEL, db, key, g_strdup (uid));
  if (db_error != 0) {
    WARNING ("cannot create new txn item: %s", db_strerror (db_error));
  }

  return db_error;
}

static int
first_last_add_cb (EBookBackendFileIndex *index, EContact *contact,
    EBookBackendFileIndexData *data, DB *db, const gchar *uid, GPtrArray *ops)
{
  return generic_name_add_cb (index, contact, data, db, uid, ORDER_FIRSTLAST, ops);
}

static int
first_last_remove_cb (EBookBackendFileIndex *index, EContact *contact,
    EBookBackendFileIndexData *data, DB *db, const gchar *uid, GPtrArray *ops)
{
  return generic_name_remove_cb (index, contact, data, db, uid, ORDER_FIRSTLAST, ops);
}

static int
last_first_add_cb (EBookBackendFileIndex *index, EContact *contact,
    EBookBackendFileIndexData *data, DB *db, const gchar *uid, GPtrArray *ops)
{
  return generic_name_add_cb (index, contact, data, db, uid, ORDER_LASTFIRST, ops);
}

static int
last_first_remove_cb (EBookBackendFileIndex *index, EContact *contact,
    EBookBackendFileIndexData *data, DB *db, const gchar *uid, GPtrArray *ops)
{
  return generic_name_remove_cb (index, contact, data, db, uid, ORDER_LASTFIRST, ops);
}

/*
 * generic version of function to get the index key from a contact based on
 * the VCard field
 */
static int
generic_field_add (EBookBackendFileIndex *index, EContact *contact,
    EBookBackendFileIndexData *data, DB *db, const gchar *uid, GPtrArray *ops)
{
  gchar *tmp = NULL;
  int db_error = 0;
  GList *attrs = NULL;
  GList *values = NULL;

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
        /* normalize the phone number before we add it to db */
        if (g_str_equal (data->vfield, EVC_TEL))
        {
          char *normalized;
          normalized = e_normalize_phone_number (values->data);
          tmp = g_utf8_casefold (normalized, -1);
          g_free (normalized);
        }
        else {
          tmp = g_utf8_casefold (values->data, -1);
        }
        if (data->suffix)
        {
          gchar *reversed;
          reversed = g_utf8_strreverse (tmp, -1);
          g_free (tmp);
          tmp = reversed;
        }

        g_strstrip (tmp);
        DEBUG ("adding to index '%s' with key %s and data %s",
            data->index_name, tmp, uid);

        db_error = txn_ops_add_new (ops, TXN_PUT, db, tmp, g_strdup (uid));
        if (db_error != 0)
        {
          WARNING ("cannot create new txn item: %s", db_strerror (db_error));
          return db_error;
        }
      }
    }
  }

  return 0;
}

static int
generic_field_remove (EBookBackendFileIndex *index, EContact *contact,
    EBookBackendFileIndexData *data, DB *db, const gchar *uid, GPtrArray *ops)
{
  GList *attrs;
  GList *values;
  gint db_error = 0;
  gchar *tmp = NULL;

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
        /* normalize phone number before we remove it, otherwise we won't find it */
        if (g_str_equal (data->vfield, EVC_TEL))
        {
          char *normalized = e_normalize_phone_number (values->data);
          tmp = g_utf8_casefold (normalized, -1);
          g_free (normalized);
        }
        else {
          tmp = g_utf8_casefold (values->data, -1);
        }

        if (data->suffix)
        {
          gchar *reversed;
          reversed = g_utf8_strreverse (tmp, -1);
          g_free (tmp);
          tmp = reversed;
        }

        g_strstrip (tmp);
        DEBUG ("removing from index '%s' with key %s and data %s",
            data->index_name, tmp, uid);

        db_error = txn_ops_add_new (ops, TXN_CDEL, db, tmp, g_strdup (uid));
        if (db_error != 0) {
          WARNING ("cannot create new txn item: %s", db_strerror (db_error));
          return db_error;
        }
      }
    }
  }

  return 0;
}

/*
 * for a given index remove this contact
 */
static int
index_remove_contact (EBookBackendFileIndex *index, EContact *contact,
    EBookBackendFileIndexData *data, GPtrArray *ops)
{
  EBookBackendFileIndexPrivate *priv = GET_PRIVATE (index);
  gchar *uid = NULL;
  DB *db = NULL;
  int retval;

  /* we need the uid, so that we can match both key and value of index-entries */
  uid = (gchar *)e_contact_get (contact, E_CONTACT_UID);

  /* we need the index database too */
  db = g_hash_table_lookup (priv->sdbs, data->query_term);

  if (data->contact_remove_func)
  {
    retval = data->contact_remove_func (index, contact, data, db, uid, ops);
  } else {
    retval = generic_field_remove (index, contact, data, db, uid, ops);
  }

  g_free (uid);

  return retval;
}

/*
 * for a given index add this contact, for the single string field case we
 * just add a single entry to the index database. for the multiple case we add
 * all the values for the field
 */
static int
index_add_contact (EBookBackendFileIndex *index, EContact *contact,
    EBookBackendFileIndexData *data, GPtrArray *ops)
{
  EBookBackendFileIndexPrivate *priv = GET_PRIVATE (index);
  gchar *uid = NULL;
  DB *db = NULL;
  int retval = 0;

  /* we need the uid, this becomes the data for our index databases */
  uid = (gchar *)e_contact_get (contact, E_CONTACT_UID);

  /* we need the index database too */
  db = g_hash_table_lookup (priv->sdbs, data->query_term);

  if (data->contact_add_func)
  {
    retval = data->contact_add_func (index, contact, data, db, uid, ops);
  } else {
    retval = generic_field_add (index, contact, data, db, uid, ops);
  }
  g_free (uid);

  return retval;
}

static void
index_sync (EBookBackendFileIndex *index, const EBookBackendFileIndexData *data)
{
  EBookBackendFileIndexPrivate *priv = GET_PRIVATE (index);
  DB *db = NULL;
  gint db_error = 0;

  db = g_hash_table_lookup (priv->sdbs, data->query_term);

  db_error = db->sync (db, 0);

  if (db_error != 0)
  {
    WARNING ("db->sync failed: %s", db_strerror (db_error));
  }
}

static const char*
find_query_term_for_vfield (const char *vfield)
{
  int i;
  for (i = 0; i < G_N_ELEMENTS (indexes); i++)
  {
    if (0 == g_ascii_strcasecmp (vfield, indexes[i].vfield))
    {
      return indexes[i].query_term;
    }
  }
  CRITICAL ("vcard-attribute %s is not indexed", vfield);

  return NULL;
}

static ESExpResult *
real_is_query (gboolean vcard_query, ESExp *sexp, gint argc, ESExpResult **argv, gpointer userdata)
{
  EBookBackendFileIndex *index = (EBookBackendFileIndex *)userdata;
  EBookBackendFileIndexPrivate *priv = GET_PRIVATE (index);

  ESExpResult *result = NULL;
  DBT index_dbt, id_dbt;
  DB *db = NULL;
  gint db_error = 0;
  const gchar *query_term = NULL;
  gchar *query_key = NULL;
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
    if (vcard_query)
    {
      query_term = find_query_term_for_vfield (query_term);
    }
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
      WARNING ("db->cursor failed: %s", db_strerror (db_error));
      goto out;
    } else {
      db_error = dbc->c_get (dbc, &index_dbt, &id_dbt, DB_SET);

      while (db_error == 0)
      {
        DEBUG ("index query found: %s", (gchar *)id_dbt.data);
        g_ptr_array_add (ids, g_strdup (id_dbt.data));
        g_free (id_dbt.data);

        /* clear the index dbt since this time bdb will push memory into it */
        memset (&index_dbt, 0, sizeof (index_dbt));
        db_error = dbc->c_get (dbc, &index_dbt, &id_dbt, DB_NEXT_DUP);
      }

      if (db_error != DB_NOTFOUND)
      {
        WARNING ("dbc->c_get failed: %s", db_strerror (db_error));
        goto out;
      }

      db_error = dbc->c_close (dbc);

      if (db_error != 0)
      {
        WARNING ("dbc->c_close failed: %s", db_strerror (db_error));
        goto out;
      }
    }
  } else {
    WARNING ("Unexpected query structure");
  }

out:

  g_free (query_key);
  result = e_sexp_result_new (sexp, ESEXP_RES_ARRAY_PTR);
  result->value.ptrarray = ids;

  return result;
}

static ESExpResult *
is_query (ESExp *sexp, gint argc, ESExpResult **argv, gpointer userdata)
{
  return real_is_query (FALSE, sexp, argc, argv, userdata);
}

static ESExpResult *
is_vcard_query (ESExp *sexp, gint argc, ESExpResult **argv, gpointer userdata)
{
  return real_is_query (TRUE, sexp, argc, argv, userdata);
}

static ESExpResult *
real_query (gboolean vcard_query, gboolean endswith_query, ESExp *sexp, gint argc, ESExpResult **argv, gpointer userdata)
{
  EBookBackendFileIndex *index = (EBookBackendFileIndex *)userdata;
  EBookBackendFileIndexPrivate *priv = GET_PRIVATE (index);

  ESExpResult *result = NULL;
  DBT index_dbt, id_dbt;
  DB *db = NULL;
  gint db_error = 0;
  const gchar *query_term = NULL;
  gchar *query_key = NULL;
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
    if (vcard_query)
    {
      query_term = find_query_term_for_vfield (query_term);
    }
    db = g_hash_table_lookup (priv->sdbs, query_term);

    /* if the query contains a phone number then normalize it */
    if (g_str_equal (query_term, "phone")) {
      char *normalized = e_normalize_phone_number (argv[1]->value.string);
      query_key = g_utf8_casefold (normalized, -1);
      g_free (normalized);
    }
    else
    {
      query_key = g_utf8_casefold (argv[1]->value.string, -1);
    }
    if (endswith_query)
    {
      gchar *reversed;
      reversed = g_utf8_strreverse (query_key, -1);
      g_free (query_key);
      query_key = reversed;
    }

    /* populate a dbt for looking up in the index */
    dbt_fill_with_string (&index_dbt, g_strstrip (query_key));

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
      WARNING ("db->cursor failed: %s", db_strerror (db_error));
      goto out;
    } else {
      db_error = dbc->c_get (dbc, &index_dbt, &id_dbt, DB_SET_RANGE);

      while (db_error == 0)
      {
        if (g_str_has_prefix (index_dbt.data, query_key))
        {
          DEBUG ("index query found: %s for %s for %s",
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
        WARNING ("dbc->c_get failed: %s", db_strerror (db_error));
        goto out;
      }

      db_error = dbc->c_close (dbc);

      if (db_error != 0)
      {
        WARNING ("dbc->c_close failed: %s", db_strerror (db_error));
        goto out;
      }
    }
  } else {
    WARNING ("Unexpected query structure");
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
  return real_query (FALSE, FALSE, sexp, argc, argv, userdata);
}

static ESExpResult *
beginswith_vcard_query (ESExp *sexp, gint argc, ESExpResult **argv, gpointer userdata)
{
  return real_query (TRUE, FALSE, sexp, argc, argv, userdata);
}

static ESExpResult *
endswith_query (ESExp *sexp, gint argc, ESExpResult **argv, gpointer userdata)
{
  return real_query (FALSE, TRUE, sexp, argc, argv, userdata);
}

static ESExpResult *
endswith_vcard_query (ESExp *sexp, gint argc, ESExpResult **argv, gpointer userdata)
{
  return real_query (TRUE, TRUE, sexp, argc, argv, userdata);
}


static gboolean
index_close_db_func (gpointer key, gpointer value, gpointer userdata)
{
  DB *db = (DB *)value;
  gint db_error = 0;

  db_error = db->close (db, 0);

  if (db_error != 0)
  {
    WARNING ("close failed: %s", db_strerror (db_error));
  }

  return TRUE;
}

/* functions for the ordering */
static int
lexical_ordering_cb (DB *secondary, const DBT *akey, const DBT *bkey)
{
  return g_utf8_collate (akey->data, bkey->data);
}

