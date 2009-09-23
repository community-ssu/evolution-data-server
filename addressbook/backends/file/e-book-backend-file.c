/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/* e-book-backend-file.c - File contact backend.
 *
 * Copyright (C) 2005 Novell, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 * Authors: Nat Friedman <nat@novell.com>
 *          Chris Toshok <toshok@ximian.com>
 *          Hans Petter Jansson <hpj@novell.com>
 */

#include <config.h>

/* Needed for strcasestr() which is used for reading pre-installed contacts. */
/* TODO: create some maemo specific macro, so it's only enabled when
 * this is compiled in sbox. This way we wouldn't break cross-compiling. */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include "db.h"
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <glib/gi18n-lib.h>

#include "libedataserver/e-dbhash.h"
#include "libedataserver/e-db3-utils.h"
#include "libedataserver/md5-utils.h"
#include "libedataserver/e-data-server-util.h"
#include "libedataserver/e-flag.h"

#include "libebook/e-contact.h"

#include "libedata-book/e-book-backend-sexp.h"
#include "libedata-book/e-data-book.h"
#include "libedata-book/e-data-book-view.h"

#include "e-book-backend-file.h"
#include "e-book-backend-file-index.h"
#include "e-book-backend-file-log.h"
#include "e-book-backend-file-utils.h"

#define CHANGES_DB_SUFFIX ".changes.db"
#define RUNNING_ID_DB_NAME "running_id.db"
#define RUNNING_ID_MAX 99999999

#define E_BOOK_BACKEND_FILE_VERSION_NAME "PAS-DB-VERSION"
#define E_BOOK_BACKEND_FILE_VERSION "0.2"

#define PAS_ID_PREFIX "pas-id-"

#define RUNREC_FILE "runrec"

G_DEFINE_TYPE (EBookBackendFile, e_book_backend_file, E_TYPE_BOOK_BACKEND_SYNC);

struct _EBookBackendFilePrivate {
	char                    *dirname;
	char                    *filename;
	DB                      *file_db;
	DB                      *id_db;
	DB_ENV                  *env;
	EBookBackendFileIndex   *index;
	char                    *sort_order;
	int                      running_id;
};

G_LOCK_DEFINE_STATIC (running_id);

static EBookBackendSyncStatus
db_error_to_status (const int db_error)
{
	switch (db_error) {
	case 0:
		return GNOME_Evolution_Addressbook_Success;
	case DB_NOTFOUND:
		return GNOME_Evolution_Addressbook_ContactNotFound;
	case EACCES:
		return GNOME_Evolution_Addressbook_PermissionDenied;
	case ENOMEM:
	case ENOSPC:
		return GNOME_Evolution_Addressbook_NoSpace;
	default:
		return GNOME_Evolution_Addressbook_OtherError;
	}
}

static EContact*
create_contact (char *uid, const char *vcard)
{
	EContact *contact = e_contact_new_from_vcard (vcard);
	if (!e_contact_get_const (contact, E_CONTACT_UID))
		e_contact_set (contact, E_CONTACT_UID, uid);

	return contact;
}

static void
set_revision (EContact *contact)
{
	char time_string[25] = {0};
	const struct tm *tm = NULL;
	GTimeVal tv;

	g_get_current_time (&tv);
	tm = gmtime (&tv.tv_sec);
	if (tm)
		strftime (time_string, sizeof (time_string), "%Y-%m-%dT%H:%M:%SZ", tm);
	e_contact_set (contact, E_CONTACT_REV, time_string);
}

/* sync main, running_id and index DBs */
static int
sync_dbs (EBookBackendFile *bf)
{
	int db_error;
	int retval = 0;

	/* sync the index file if available */
	if (bf->priv->index)
		e_book_backend_file_index_sync (bf->priv->index);

	/* sync the id db */
	db_error = bf->priv->id_db->sync (bf->priv->id_db, 0);
	if (db_error != 0) {
		WARNING ("id_db->sync failed with %s", db_strerror (db_error));
		retval = db_error;
		/* try to sync the main db even if id_db->sync has been failed */
	}

	/* sync the main db */
	db_error = bf->priv->file_db->sync (bf->priv->file_db, 0);
	if (db_error != 0) {
		WARNING ("file_db->sync failed with %s", db_strerror (db_error));
		retval = db_error;
	}

	return retval;
}

static void
load_last_running_id (EBookBackendFile *bf)
{
	DB *db = NULL;
	DBC *dbc = NULL;
	DBT rid_dbt, id_dbt;
	int db_error;

	g_return_if_fail (bf->priv->id_db);

	db = bf->priv->id_db;

	db_error = db->cursor (db, NULL, &dbc, 0);
	if (db_error != 0) {
		WARNING ("db->cursor failed: %s", db_strerror (db_error));
		g_assert_not_reached ();
	}

	memset (&rid_dbt, 0, sizeof (rid_dbt));
	memset (&id_dbt, 0, sizeof (id_dbt));

	id_dbt.flags = DB_DBT_MALLOC;

	db_error = dbc->c_get (dbc, &rid_dbt, &id_dbt, DB_FIRST);
	if (db_error == DB_NOTFOUND) {
		bf->priv->running_id = 0;
	} else if (db_error != 0) {
		WARNING ("c_get failed: %s", db_strerror (db_error));
		g_assert_not_reached ();
	} else {
		/* id from string to uint */
		long int ret = strtol ((const char *)id_dbt.data, (char **)NULL, 10);
		if (ret == LONG_MIN) {
			WARNING ("String to long integer conversion failed");
			g_assert_not_reached ();
		}
		g_free (id_dbt.data);

		bf->priv->running_id = ret;
	}

	db_error = dbc->c_close (dbc);
	if (db_error != 0) {
		WARNING ("dbc->c_close failed: %s", db_strerror (db_error));
	}
}

static char*
unique_id_to_string (int id)
{
	return g_strdup_printf ("%d", id);
}

/* create and add a new TxnItem to ops */
static int
store_unique_id (EBookBackendFile *bf, GPtrArray *ops)
{
	char *uid;

	g_assert (bf->priv->running_id > 0);

	uid = unique_id_to_string (bf->priv->running_id);
	DEBUG ("storing runnind_id = %s", uid);

	return txn_ops_add_new (ops, TXN_PUT, bf->priv->id_db, NULL, uid);
}

static char *
e_book_backend_file_create_unique_id (EBookBackendFile *bf)
{
	G_LOCK (running_id);
	if (0 == bf->priv->running_id) {
		load_last_running_id (bf);
	}
	if (bf->priv->running_id >= RUNNING_ID_MAX) {
		WARNING ("running id overrun");
		bf->priv->running_id = 1;
	}
	++bf->priv->running_id;
	G_UNLOCK (running_id);

	g_assert (bf->priv->running_id > 0);
	return unique_id_to_string (bf->priv->running_id);
}

static gboolean
is_contact_preserved (EContact *contact)
{
	EVCardAttribute *attr;
	GList *params, *l;

	attr = e_vcard_get_attribute (E_VCARD (contact), EVC_UID);
	if (NULL == attr)
		return FALSE;

	params = e_vcard_attribute_get_params (attr);
	for (l = params; l; l = g_list_next (l)) {
		const char *name;

		name = e_vcard_attribute_param_get_name (l->data);

		if (g_ascii_strcasecmp (name, "X-OSSO-PRESERVE") == 0) {
			e_vcard_attribute_remove_param (attr, "X-OSSO-PRESERVE");
			return TRUE;
		}
	}

	return FALSE;
}

/* put the vcard to main db and index db, but don't sync the DBs *
 * returns 0 on success */
static int
insert_contact (EBookBackendFile *bf,
		const char       *vcard_req,
		EContact        **contact,
		GPtrArray        *ops)
{
	DB *db = bf->priv->file_db;
	char *id;
	char *vcard;
	const char *rev;
	int db_error;
	int id_new = 0;

	g_assert (bf);
	g_assert (vcard_req);
	g_assert (contact);
	g_assert (ops);

	if (0 == bf->priv->running_id) {
		G_LOCK (running_id);
		load_last_running_id (bf);
		G_UNLOCK (running_id);
	}

	*contact = e_contact_new_from_vcard (vcard_req);
	if (is_contact_preserved (*contact)) {
		id = (char *) e_contact_get (*contact, E_CONTACT_UID);
		g_warn_if_fail (id);
		/* convert uid to running_id */
		errno = 0;
		if (id) {
			id_new = strtol ((const char *)id, (char **)NULL, 10);
		}

		if ((errno == ERANGE && (id_new == LONG_MAX || id_new == LONG_MIN))
				|| (errno != 0 && id_new == 0)) {
			WARNING ("unable to preserve id, ceating new one");
			g_free (id);
			id_new = 0;
		}
		/* update running id */
		if (id_new > bf->priv->running_id)
			bf->priv->running_id = id_new;
	}
	if (!id_new) {
		id = e_book_backend_file_create_unique_id (bf);
		e_contact_set (*contact, E_CONTACT_UID, id);
	}

	rev = e_contact_get_const (*contact,  E_CONTACT_REV);
	if (!(rev && *rev))
		set_revision (*contact);

	/* extract the attached image(s) (if there is any) to default photo dir */
	/* NOTE: no need to check the return value, since it denotes only that
	 * original contact was changed or not */
	e_contact_persist_data (*contact, NULL);

	/* recreate the vcard string with the new values */
	vcard = e_vcard_to_string (E_VCARD (*contact), EVC_FORMAT_VCARD_30);

	/* put the vcard to db */
	db_error = txn_ops_add_new (ops, TXN_PUT, db, id, vcard);
	if (db_error != 0) {
		g_free (vcard);
		g_free (id);
		return db_error;
	}

	return e_book_backend_file_index_add_contact (bf->priv->index, *contact, ops);
}

/* put contact to main and index DBs, put running id, sync DBs */
static int
insert_contact_sync (EBookBackendFile *bf,
		     const char       *vcard_req,
		     EContact        **contact)
{
	GPtrArray *ops;
	int db_error;

	ops = g_ptr_array_new ();

	db_error = insert_contact (bf, vcard_req, contact, ops);
	if (db_error != 0) {
		goto out;
	}

	db_error = store_unique_id (bf, ops);
	if (db_error != 0) {
		goto out;
	}

	/* create and commit the transaction */
	db_error = txn_execute_ops (bf->priv->env, NULL, ops);
	if (db_error != 0) {
		goto out;
	}

	db_error = sync_dbs (bf);

out:
	txn_ops_free (ops);
	return db_error;
}

static EBookBackendSyncStatus
e_book_backend_file_create_contact (EBookBackendSync *backend,
				    EDataBook *book,
				    guint32 opid,
				    const char *vcard,
				    EContact **contact)
{
	EBookBackendFile *bf = E_BOOK_BACKEND_FILE (backend);
	int db_error;

	db_error = insert_contact_sync (bf, vcard, contact);

	return db_error_to_status (db_error);
}

static EBookBackendSyncStatus
e_book_backend_file_create_contacts (EBookBackendSync *backend,
				     EDataBook *book,
				     guint32 opid,
				     const char **vcards,
				     GList **contacts)
{
	EBookBackendFile *bf;
	EContact *contact;
	DB *db;
	int db_error = 0;
	GPtrArray *ops;

	bf = E_BOOK_BACKEND_FILE (backend);
	db = bf->priv->file_db;

	ops = g_ptr_array_new ();

	/* Commit each of the new contacts, aborting if there was an error. */
	/* XXX: is this a good policy for the batch operation? */
	for (; *vcards; vcards++) {
		contact = NULL;

		/* insert the contact */
		db_error = insert_contact (bf, *vcards, &contact, ops);

		if (db_error != 0) {
			/* TODO: proper error handling */
			/* try to sync the already added contacts now */
			WARNING ("cannot add contact");
			break;
		}

		/* Pass the contact back to the server for view updates */
		*contacts = g_list_prepend (*contacts, contact);
	}
	/* reverse contacts list, to keep order */
	*contacts = g_list_reverse (*contacts);

	/* store the running id */
	db_error = store_unique_id (bf, ops);
	if (db_error != 0) {
		goto out;
	}

	db_error = txn_execute_ops (bf->priv->env, NULL, ops);
	if (db_error == 0) {
		/* sync databases */
		db_error = sync_dbs (bf);
	}

out:
	txn_ops_free (ops);

	return db_error_to_status (db_error);
}

static EBookBackendSyncStatus
e_book_backend_file_remove_contacts (EBookBackendSync *backend,
				     EDataBook *book,
				     guint32 opid,
				     GList *id_list,
				     GList **ids)
{
	EBookBackendFile *bf = E_BOOK_BACKEND_FILE (backend);
	DB *db = bf->priv->file_db;
	DBT id_dbt;
	DBT vcard_dbt;
	int db_error;
	int db_error_final = 0;
	char *id;
	GList *l;
	GList *removed_cards = NULL;
	GList *removed_contacts = NULL;
	GPtrArray *ops;

	ops = g_ptr_array_new ();

	for (l = id_list; l; l = l->next) {
		id = l->data;

		dbt_fill_with_string (&id_dbt, id);

		memset (&vcard_dbt, 0, sizeof (vcard_dbt));
		vcard_dbt.flags = DB_DBT_MALLOC;

		db_error = db->get (db, NULL, &id_dbt, &vcard_dbt, 0);
		if (0 != db_error) {
			WARNING ("db->get failed: %s", db_strerror (db_error));
			db_error_final = db_error;
			continue;
		}

		db_error = txn_ops_add_new (ops, TXN_DEL, db, g_strdup (id), NULL);
		if (0 != db_error) {
			WARNING ("cannot create new txn item");
			db_error_final = db_error;
			continue;
		}

		removed_contacts = g_list_prepend (removed_contacts,
						e_contact_new_from_vcard (vcard_dbt.data));
		removed_cards = g_list_prepend (removed_cards, id);
		g_free (vcard_dbt.data);
	}

	while (removed_contacts) {
		EContact *contact = removed_contacts->data;
		db_error = e_book_backend_file_index_remove_contact (bf->priv->index,
				contact, ops);
		if (db_error != 0) {
			WARNING ("cannot create txn items for remove index");
			/* this case we cannot commit the transaction */
			goto out;
		}
		g_object_unref (contact);
		removed_contacts = g_list_delete_link (removed_contacts, removed_contacts);
		/* TODO: check if conversion to EContact is realy needed for
		 * removing from index.  At 1st glance it seems it uses only
		 * the id, which is already given */
	}

	*ids = removed_cards;

	/* create and commit the transaction */
	db_error = txn_execute_ops (bf->priv->env, NULL, ops);
	if (db_error != 0) {
		WARNING ("cannot commit transaction: %s", db_strerror (db_error));
		goto out;
	}

	db_error = sync_dbs (bf);
	if (db_error != 0) {
		WARNING ("cannot sync DBs: %s", db_strerror (db_error));
	}

out:
	txn_ops_free (ops);

	/* if index_remove_contacts failed */
	while (removed_contacts) {
		g_object_unref (removed_contacts->data);
		removed_contacts = g_list_delete_link (removed_contacts, removed_contacts);
	}

	return db_error_to_status (db_error_final ? db_error_final : db_error);
}

static EBookBackendSyncStatus
modify_contact (EBookBackendFile *bf,
		const char       *vcard,
		EContact        **contact)
{
	DB *db;
	int db_error;
	char *vcard_with_rev;
	const char *id, *dbt_id;
	DBT id_dbt, vcard_dbt;
	EContact *old_contact;

	g_assert (bf);
	g_assert (vcard);
	g_assert (contact);
	g_assert (*contact == NULL);

	db = bf->priv->file_db;

	*contact = e_contact_new_from_vcard (vcard);
	id = e_contact_get_const (*contact, E_CONTACT_UID);

	if (id == NULL)
		return GNOME_Evolution_Addressbook_OtherError;

	/* update the revision (modified time of contact) */
	set_revision (*contact);
	vcard_with_rev = e_vcard_to_string (E_VCARD (*contact), EVC_FORMAT_VCARD_30);

	/*
	 * This is disgusting, but for a time cards were added with IDs that are
	 * no longer used (they contained both the URI and the ID). If we
	 * recognize it as a URI (file:///...) trim off everything before the
	 * last '/', and use that as the ID.
	 */
	if (!strncmp (id, "file:///", strlen ("file:///")))
		dbt_id = strrchr (id, '/') + 1;
	else
		dbt_id = id;

	dbt_fill_with_string (&id_dbt, dbt_id);
	memset (&vcard_dbt, 0, sizeof (vcard_dbt));
	vcard_dbt.flags = DB_DBT_MALLOC;

	db_error = db->get (db, NULL, &id_dbt, &vcard_dbt, 0);
	if (0 != db_error) {
		WARNING ("db->get failed with %s", db_strerror (db_error));
		return db_error_to_status (db_error);
	}
	old_contact = e_contact_new_from_vcard (vcard_dbt.data);
	g_free (vcard_dbt.data);
	dbt_fill_with_string (&vcard_dbt, vcard_with_rev);

	db_error = db->put (db, NULL, &id_dbt, &vcard_dbt, 0);

	g_free (vcard_with_rev);

	if (db_error == 0) {
		e_book_backend_file_index_modify_contact (bf->priv->index, old_contact, *contact);
		g_object_unref (old_contact);
		return GNOME_Evolution_Addressbook_Success;
	} else {
		WARNING ("db->put failed with %s", db_strerror (db_error));
		g_object_unref (old_contact);
		return db_error_to_status (db_error);
	}
}

static EBookBackendSyncStatus
e_book_backend_file_modify_contact (EBookBackendSync *backend,
				    EDataBook        *book,
				    guint32           opid,
				    const char       *vcard,
				    EContact        **contact)
{
	EBookBackendSyncStatus status;
	EBookBackendFile *bf = E_BOOK_BACKEND_FILE (backend);
	int db_error = 0;

	status = modify_contact (bf, vcard, contact);

	if (status == GNOME_Evolution_Addressbook_Success) {
		/* sync databases */
		db_error = sync_dbs (bf);
	}

	return db_error_to_status (db_error);
}

static EBookBackendSyncStatus
e_book_backend_file_modify_contacts (EBookBackendSync *backend,
				     EDataBook        *book,
				     guint32           opid,
				     const char      **vcards,
				     GList           **contacts)
{
	EBookBackendFile *bf;
	EBookBackendSyncStatus status = GNOME_Evolution_Addressbook_OtherError;
	EContact *contact;
	DB *db;
	int db_error;

	bf = E_BOOK_BACKEND_FILE (backend);
	db = bf->priv->file_db;

	/* Commit each of the new contacts, aborting if there was an error.
	 * Really this should be in a transaction... */
	for (; *vcards; vcards++) {
		contact = NULL;

		/* Commit the contact */
		status = modify_contact (bf, *vcards, &contact);
		if (status != GNOME_Evolution_Addressbook_Success)
			return status;

		/* Pass the contact back to the server for view updates */
		*contacts = g_list_prepend (*contacts, contact);
	}

	/* sync databases */
	db_error = sync_dbs (bf);

	return db_error_to_status (db_error);
}

static EBookBackendSyncStatus
e_book_backend_file_get_contact (EBookBackendSync *backend,
				 EDataBook        *book,
				 guint32           opid,
				 const char       *id,
				 char            **vcard)
{
	EBookBackendFile *bf;
	DB *db;
	DBT id_dbt, vcard_dbt;
	int db_error = 0;

	bf = E_BOOK_BACKEND_FILE (backend);
	db = bf->priv->file_db;

	dbt_fill_with_string (&id_dbt, id);
	memset (&vcard_dbt, 0, sizeof (vcard_dbt));
	vcard_dbt.flags = DB_DBT_MALLOC;

	db_error = db->get (db, NULL, &id_dbt, &vcard_dbt, 0);

	if (db_error == 0) {
		*vcard = vcard_dbt.data;
		return GNOME_Evolution_Addressbook_Success;
	} else {
		WARNING ("db->get failed with %s", db_strerror (db_error));
		*vcard = g_strdup ("");
		return GNOME_Evolution_Addressbook_ContactNotFound;
	}
}

static EBookBackendSyncStatus
e_book_backend_file_get_contact_list (EBookBackendSync *backend,
				      EDataBook        *book,
				      guint32           opid,
				      const char       *query,
				      GList           **contacts)
{
	EBookBackendFile *bf = E_BOOK_BACKEND_FILE (backend);
	DB *db = bf->priv->file_db;
	DBC *dbc;
	int db_error;
	DBT id_dbt, vcard_dbt;
	EBookBackendSExp *card_sexp = NULL;
	gboolean search_needed;
	const char *search = query;
	GList *contact_list = NULL;
	EBookBackendSyncStatus status;

	MESSAGE ("e_book_backend_file_get_contact_list (%s)", search);
	status = GNOME_Evolution_Addressbook_Success;

	search_needed = TRUE;
	if (!strcmp (search, "(contains \"x-evolution-any-field\" \"\")"))
		search_needed = FALSE;

	card_sexp = e_book_backend_sexp_new (search);
	if (!card_sexp) {
		/* XXX this needs to be an invalid query error of some sort*/
		return GNOME_Evolution_Addressbook_OtherError;
	}

	db_error = db->cursor (db, NULL, &dbc, 0);

	if (db_error != 0) {
		WARNING ("db->cursor failed with %s", db_strerror (db_error));
		/* XXX this needs to be some CouldNotOpen error */
		return db_error_to_status (db_error);
	}

	memset (&vcard_dbt, 0, sizeof (vcard_dbt));
	vcard_dbt.flags = DB_DBT_MALLOC;
	memset (&id_dbt, 0, sizeof (id_dbt));
	db_error = dbc->c_get(dbc, &id_dbt, &vcard_dbt, DB_FIRST);

	while (db_error == 0) {
		/* don't include the version in the list of cards */
		if (id_dbt.size != strlen(E_BOOK_BACKEND_FILE_VERSION_NAME) + 1
		    || strcmp (id_dbt.data, E_BOOK_BACKEND_FILE_VERSION_NAME)) {

			if ((!search_needed) || (card_sexp != NULL && e_book_backend_sexp_match_vcard  (card_sexp, vcard_dbt.data))) {
				contact_list = g_list_prepend (contact_list, vcard_dbt.data);
			}
			else {
				g_free (vcard_dbt.data);
			}
		}
		else {
			g_free (vcard_dbt.data);
		}

		db_error = dbc->c_get(dbc, &id_dbt, &vcard_dbt, DB_NEXT);
	}
	g_object_unref (card_sexp);

	if (db_error == DB_NOTFOUND) {
		status = GNOME_Evolution_Addressbook_Success;
	} else {
		WARNING ("dbc->c_get failed with %s", db_strerror (db_error));
		status = db_error_to_status (db_error);
	}

	db_error = dbc->c_close(dbc);
	if (db_error != 0) {
		WARNING ("dbc->c_close failed with %s", db_strerror (db_error));
	}

	*contacts = contact_list;
	return status;
}

typedef struct {
	EBookBackendFile *bf;
	GThread *thread;
	EFlag *running;
} FileBackendSearchClosure;

static void
closure_destroy (FileBackendSearchClosure *closure)
{
	MESSAGE ("destroying search closure");
	e_flag_free (closure->running);
	g_free (closure);
}

static FileBackendSearchClosure*
init_closure (EDataBookView *book_view, EBookBackendFile *bf)
{
	FileBackendSearchClosure *closure = g_new (FileBackendSearchClosure, 1);

	closure->bf = bf;
	closure->thread = NULL;
	closure->running = e_flag_new ();

	g_object_set_data_full (G_OBJECT (book_view), "EBookBackendFile.BookView::closure",
				closure, (GDestroyNotify)closure_destroy);

	return closure;
}

static FileBackendSearchClosure*
get_closure (EDataBookView *book_view)
{
	return g_object_get_data (G_OBJECT (book_view), "EBookBackendFile.BookView::closure");
}

static gpointer
book_view_thread (gpointer data)
{
	EDataBookView *book_view;
	FileBackendSearchClosure *closure;
	EBookBackendFile *bf;
	EBookBackendFilePrivate *priv;
	const char *query;
	DB *db;
	DBT id_dbt, vcard_dbt;
	int db_error;
	gboolean allcontacts;
	GPtrArray *ids = NULL;

	g_return_val_if_fail (E_IS_DATA_BOOK_VIEW (data), NULL);

	book_view = data;

	/* ref the book view because it'll be removed and unrefed
	 * when/if it's stopped */
	g_object_ref (book_view);

	closure = get_closure (book_view);
	if (!closure) {
		WARNING ("NULL closure in book view thread");
		g_object_unref (book_view);
		return NULL;
	}
	bf = closure->bf;
	priv = bf->priv;

	MESSAGE ("starting initial population of book view");

	db = bf->priv->file_db;
	query = e_data_book_view_get_card_query (book_view);

	if ( ! strcmp (query, "(contains \"x-evolution-any-field\" \"\")")) {
		e_data_book_view_notify_status_message (book_view, _("Loading..."));
		allcontacts = TRUE;
	} else {
		e_data_book_view_notify_status_message (book_view, _("Searching..."));
		allcontacts = FALSE;
	}

	MESSAGE ("signalling parent thread");
	e_flag_set (closure->running);

	DEBUG ("Query is %s", query);
	if (e_book_backend_file_index_is_usable (bf->priv->index, query)) {
		DEBUG ("Using index for %s", query);

		ids = e_book_backend_file_index_query (bf->priv->index, query);
	} else if (allcontacts) {
		if (priv->sort_order) {
			DEBUG ("sending contacts in order sort by %s", priv->sort_order);
			ids = e_book_backend_file_index_get_ordered_ids (bf->priv->index, priv->sort_order);
		}
		else
			DEBUG ("priv->sort_order is not set");
	}

	if (ids)
	{
		int i;
		for (i = 0; i < ids->len; i ++) {
			char *id = g_ptr_array_index (ids, i);

			if (!e_flag_is_set (closure->running))
				break;

			dbt_fill_with_string (&id_dbt, id);
			memset (&vcard_dbt, 0, sizeof (vcard_dbt));
			vcard_dbt.flags = DB_DBT_MALLOC;

			db_error = db->get (db, NULL, &id_dbt, &vcard_dbt, 0);

			if (db_error == 0) {
				e_data_book_view_notify_update_prefiltered_vcard (book_view, id, vcard_dbt.data);
			}
			else {
				WARNING ("db->get failed with %s", db_strerror (db_error));
			}
		}

		/* free the elements of pointer array */
		g_ptr_array_foreach (ids, (GFunc) g_free, NULL);
		g_ptr_array_free (ids, TRUE);
	} else {
		/* iterate over the db and do the query there */
		DBC    *dbc;

		memset (&id_dbt, 0, sizeof (id_dbt));
		memset (&vcard_dbt, 0, sizeof (vcard_dbt));
		vcard_dbt.flags = DB_DBT_MALLOC;

		db_error = db->cursor (db, NULL, &dbc, 0);
		if (db_error == 0) {

			db_error = dbc->c_get(dbc, &id_dbt, &vcard_dbt, DB_FIRST);
			while (db_error == 0) {

				if (!e_flag_is_set (closure->running))
					break;

				/* don't include the version in the list of cards */
				if (strcmp (id_dbt.data, E_BOOK_BACKEND_FILE_VERSION_NAME)) {
					if (allcontacts)
						e_data_book_view_notify_update_prefiltered_vcard (book_view, id_dbt.data, vcard_dbt.data);
					else
						e_data_book_view_notify_update_vcard (book_view, vcard_dbt.data);
				} else {
					g_free (vcard_dbt.data);
				}

				db_error = dbc->c_get(dbc, &id_dbt, &vcard_dbt, DB_NEXT);
			}

			dbc->c_close (dbc);
			if (db_error && db_error != DB_NOTFOUND)
				WARNING ("e_book_backend_file_search: error building list: %s",
						db_strerror (db_error));
		}
	}

	if (e_flag_is_set (closure->running))
	{
		e_data_book_view_notify_complete (book_view, GNOME_Evolution_Addressbook_Success);
	}

	/* unref the */
	g_object_unref (book_view);

	MESSAGE ("finished population of book view");

	return NULL;
}

static void
e_book_backend_file_start_book_view (EBookBackend  *backend,
				     EDataBookView *book_view)
{
	FileBackendSearchClosure *closure = init_closure (book_view, E_BOOK_BACKEND_FILE (backend));

	MESSAGE ("starting book view thread");
	closure->thread = g_thread_create (book_view_thread, book_view, FALSE, NULL);
	g_object_ref (backend);

	e_flag_wait (closure->running);

	/* at this point we know the book view thread is actually running */
	MESSAGE ("returning from start_book_view");
}

static void
e_book_backend_file_stop_book_view (EBookBackend  *backend,
				    EDataBookView *book_view)
{
	FileBackendSearchClosure *closure = get_closure (book_view);

	g_object_unref (backend);
	MESSAGE ("stopping query");
	if (!closure) {
		/* TODO: find out why this happens! */
		WARNING ("NULL closure when stopping query");
		return;
	}
	e_flag_clear (closure->running);
}

static void
e_book_backend_file_set_book_view_sort_order (EBookBackend  *backend,
					      EDataBookView *book_view,
					      const gchar   *query_term)
{
	EBookBackendFile *bf = (EBookBackendFile *)backend;
	EBookBackendFilePrivate *priv = bf->priv;

	DEBUG ("setting sort order in backend to %s", query_term);
	g_free (priv->sort_order);
	priv->sort_order = g_strdup (query_term);
}

typedef struct {
	DB *db;

	GList *add_cards;
	GList *add_ids;
	GList *mod_cards;
	GList *mod_ids;
	GList *del_ids;
	GList *del_cards;
} EBookBackendFileChangeContext;

static void
e_book_backend_file_changes_foreach_key (const char *key, gpointer user_data)
{
	EBookBackendFileChangeContext *ctx = user_data;
	DB *db = ctx->db;
	DBT id_dbt, vcard_dbt;
	int db_error = 0;

	dbt_fill_with_string (&id_dbt, key);
	memset (&vcard_dbt, 0, sizeof (vcard_dbt));
	vcard_dbt.flags = DB_DBT_MALLOC;

	db_error = db->get (db, NULL, &id_dbt, &vcard_dbt, 0);

	if (db_error != 0) {
		EContact *contact;
		char *id = id_dbt.data;
		char *vcard_string;

		contact = e_contact_new ();
		e_contact_set (contact, E_CONTACT_UID, id);

		vcard_string = e_vcard_to_string (E_VCARD (contact), EVC_FORMAT_VCARD_30);

		ctx->del_ids = g_list_append (ctx->del_ids, g_strdup (id));
		ctx->del_cards = g_list_append (ctx->del_cards, vcard_string);

		g_object_unref (contact);

		g_free (vcard_dbt.data);
	}
}

static EBookBackendSyncStatus
e_book_backend_file_get_changes (EBookBackendSync *backend,
				 EDataBook        *book,
				 guint32           opid,
				 const char       *change_id,
				 GList           **changes_out)
{
	EBookBackendFile *bf = E_BOOK_BACKEND_FILE (backend);
	int db_error = 0;
	DBT id_dbt, vcard_dbt;
	char *filename;
	EDbHash *ehash;
	int     fd;
	GList *i, *v;
	DB *db = bf->priv->file_db;
	DBC *dbc;
	GList *changes = NULL;
	EBookBackendFileChangeContext ctx;
	EBookBackendSyncStatus result;

	memset (&id_dbt, 0, sizeof (id_dbt));
	memset (&vcard_dbt, 0, sizeof (vcard_dbt));

	memset (&ctx, 0, sizeof (ctx));

	ctx.db = db;

	/* Find the changed ids */
	filename = g_strdup_printf ("%s/%s" CHANGES_DB_SUFFIX, bf->priv->dirname, change_id);

	ehash = e_dbhash_new (filename);

	/* We lock the file so that the backup script doesn't save it
	 * while we are modifying it.
	 * We could use the file descriptor returned by db->fd() but
	 * there is no guarantee that the returned file descriptor
	 * corresponds to this file */
	fd = open (filename, O_RDONLY);
	if (fd >= 0)
		flock (fd, LOCK_EX);

	db_error = db->cursor (db, NULL, &dbc, 0);

	if (db_error != 0) {
		WARNING ("db->cursor failed with %s", db_strerror (db_error));
	} else {
		db_error = dbc->c_get(dbc, &id_dbt, &vcard_dbt, DB_FIRST);

		while (db_error == 0) {

			/* don't include the version in the list of cards */
			if (id_dbt.size != strlen(E_BOOK_BACKEND_FILE_VERSION_NAME) + 1
			    || strcmp (id_dbt.data, E_BOOK_BACKEND_FILE_VERSION_NAME)) {
				EContact *contact;
				char *id = id_dbt.data;
				char *vcard_string;

				/* Remove fields the user can't change
				 * and can change without the rest of the
				 * card changing */
				contact = create_contact (id_dbt.data, vcard_dbt.data);

#ifdef notyet
				g_object_set (card, "last_use", NULL, "use_score", 0.0, NULL);
#endif
				vcard_string = e_vcard_to_string (E_VCARD (contact), EVC_FORMAT_VCARD_30);
				g_object_unref (contact);

				/* check what type of change has occurred, if any */
				switch (e_dbhash_compare (ehash, id, vcard_string)) {
				case E_DBHASH_STATUS_SAME:
					g_free(vcard_string);
					break;
				case E_DBHASH_STATUS_NOT_FOUND:
					ctx.add_cards = g_list_append (ctx.add_cards, vcard_string);
					ctx.add_ids = g_list_append (ctx.add_ids, g_strdup(id));
					break;
				case E_DBHASH_STATUS_DIFFERENT:
					ctx.mod_cards = g_list_append (ctx.mod_cards, vcard_string);
					ctx.mod_ids = g_list_append (ctx.mod_ids, g_strdup(id));
					break;
				}
			}

			db_error = dbc->c_get(dbc, &id_dbt, &vcard_dbt, DB_NEXT);
		}
		dbc->c_close (dbc);
	}

	e_dbhash_foreach_key (ehash, (EDbHashFunc)e_book_backend_file_changes_foreach_key, &ctx);

	/* Send the changes */
	if (db_error != DB_NOTFOUND) {
		WARNING ("e_book_backend_file_changes: error building list\n");
		*changes_out = NULL;
		result = db_error_to_status (db_error);
	}
	else {
		/* Update the hash and build our changes list */
		for (i = ctx.add_ids, v = ctx.add_cards; i != NULL; i = i->next, v = v->next) {
			char *id = i->data;
			char *vcard = v->data;

			e_dbhash_add (ehash, id, vcard);
			changes = g_list_prepend (changes,
					e_book_backend_change_add_new (vcard));

			g_free (i->data);
			g_free (v->data);
		}
		for (i = ctx.mod_ids, v = ctx.mod_cards; i != NULL; i = i->next, v = v->next) {
			char *id = i->data;
			char *vcard = v->data;

			e_dbhash_add (ehash, id, vcard);
			changes = g_list_prepend (changes,
					e_book_backend_change_modify_new (vcard));

			g_free (i->data);
			g_free (v->data);
		}
		for (i = ctx.del_ids, v = ctx.del_cards; i != NULL; i = i->next, v = v->next) {
			char *id = i->data;
			char *vcard = v->data;

			e_dbhash_remove (ehash, id);

			changes = g_list_prepend (changes,
					e_book_backend_change_delete_new (vcard));
			g_free (i->data);
			g_free (v->data);
		}

		e_dbhash_write (ehash);

		result = GNOME_Evolution_Addressbook_Success;
		*changes_out = changes;
	}

	if (fd >= 0) {
		flock (fd, LOCK_UN);
		close (fd);
	}

	e_dbhash_destroy (ehash);
	g_free (filename);

	return result;
}

static char *
e_book_backend_file_extract_path_from_uri (const char *uri)
{
	g_assert (g_ascii_strncasecmp (uri, "file://", 7) == 0);

	return g_filename_from_uri (uri, NULL, NULL);
}

static EBookBackendSyncStatus
e_book_backend_file_authenticate_user (EBookBackendSync *backend,
				       EDataBook        *book,
				       guint32           opid,
				       const char       *user,
				       const char       *passwd,
				       const char       *auth_method)
{
	return GNOME_Evolution_Addressbook_Success;
}

static EBookBackendSyncStatus
e_book_backend_file_get_required_fields (EBookBackendSync *backend,
					 EDataBook        *book,
					 guint32           opid,
					 GList           **fields_out)
{
	GList *fields = NULL;

	fields = g_list_append (fields, g_strdup (e_contact_field_name (E_CONTACT_FILE_AS)));
	*fields_out = fields;
	return GNOME_Evolution_Addressbook_Success;
}


static EBookBackendSyncStatus
e_book_backend_file_get_supported_fields (EBookBackendSync *backend,
					  EDataBook        *book,
					  guint32           opid,
					  GList           **fields_out)
{
	GList *fields = NULL;
	int i;

	/* XXX we need a way to say "we support everything", since the
	 * file backend does */
	for (i = 1; i < E_CONTACT_FIELD_LAST; i ++)
		fields = g_list_append (fields, g_strdup (e_contact_field_name (i)));

	*fields_out = fields;
	return GNOME_Evolution_Addressbook_Success;
}

/*
** versions:
**
** 0.0 just a list of cards
**
** 0.1 same as 0.0, but with the version tag
**
** 0.2 not a real format upgrade, just a hack to fix broken ids caused
**     by a bug in early betas, but we only need to convert them if
**     the previous version is 0.1, since the bug existed after 0.1
**     came about.
*/
static gboolean
e_book_backend_file_upgrade_db (EBookBackendFile *bf, char *old_version)
{
	DB *db = bf->priv->file_db;
	int db_error;
	DBT version_name_dbt, version_dbt;

	if (strcmp (old_version, "0.0")
	    && strcmp (old_version, "0.1")) {
		WARNING ("unsupported version '%s' found in PAS backend file", old_version);
		return FALSE;
	}

	/* XXX: this should be a transaction, but in our case this should
	 *      never happen since we do always a fresh install */
	if (!strcmp (old_version, "0.1")) {
		/* we just loop through all the cards in the db,
		 * giving them valid ids if they don't have them */
		DBT id_dbt, vcard_dbt;
		DBC *dbc;
		int card_failed = 0;

		db_error = db->cursor (db, NULL, &dbc, 0);
		if (db_error != 0) {
			WARNING ("db->cursor failed with %s", db_strerror (db_error));
			return FALSE;
		}

		memset (&id_dbt, 0, sizeof (id_dbt));
		memset (&vcard_dbt, 0, sizeof (vcard_dbt));

		db_error = dbc->c_get(dbc, &id_dbt, &vcard_dbt, DB_FIRST);

		while (db_error == 0) {
			if (id_dbt.size != strlen(E_BOOK_BACKEND_FILE_VERSION_NAME) + 1
			    || strcmp (id_dbt.data, E_BOOK_BACKEND_FILE_VERSION_NAME)) {
				EContact *contact;

				contact = create_contact (id_dbt.data, vcard_dbt.data);

				/* the cards we're looking for are
				   created with a normal id dbt, but
				   with the id field in the vcard set
				   to something that doesn't match.
				   so, we need to modify the card to
				   have the same id as the the dbt. */
				if (strcmp (id_dbt.data, e_contact_get_const (contact, E_CONTACT_UID))) {
					char *vcard;

					e_contact_set (contact, E_CONTACT_UID, id_dbt.data);

					vcard = e_vcard_to_string (E_VCARD (contact), EVC_FORMAT_VCARD_30);
					dbt_fill_with_string (&vcard_dbt, vcard);

					db_error = db->put (db, NULL,
							&id_dbt, &vcard_dbt, 0);

					g_free (vcard);

					if (db_error != 0)
						card_failed++;
				}

				g_object_unref (contact);
			}

			db_error = dbc->c_get(dbc, &id_dbt, &vcard_dbt, DB_NEXT);
		}

		if (card_failed) {
			WARNING ("failed to update %d cards", card_failed);
			return FALSE;
		}
	}

	dbt_fill_with_string (&version_name_dbt, E_BOOK_BACKEND_FILE_VERSION_NAME);
	dbt_fill_with_string (&version_dbt, E_BOOK_BACKEND_FILE_VERSION);

	db_error = db->put (db, NULL, &version_name_dbt, &version_dbt, 0);
	if (db_error == 0)
		return TRUE;
	else
		return FALSE;
}

static gboolean
e_book_backend_file_maybe_upgrade_db (EBookBackendFile *bf)
{
	DB *db = bf->priv->file_db;
	DBT version_name_dbt, version_dbt;
	int db_error;
	char *version;
	gboolean ret_val = TRUE;

	dbt_fill_with_string (&version_name_dbt, E_BOOK_BACKEND_FILE_VERSION_NAME);
	memset (&version_dbt, 0, sizeof (version_dbt));
	version_dbt.flags = DB_DBT_MALLOC;

	db_error = db->get (db, NULL, &version_name_dbt, &version_dbt, 0);
	if (db_error == 0) {
		/* success */
		version = version_dbt.data;
	}
	else {
		/* key was not in file */
		version = g_strdup ("0.0");
	}

	if (strcmp (version, E_BOOK_BACKEND_FILE_VERSION))
		ret_val = e_book_backend_file_upgrade_db (bf, version);

	g_free (version);

	return ret_val;
}

#ifdef CREATE_DEFAULT_VCARD
# include <libedata-book/ximian-vcard.h>
#endif

static void
#if DB_VERSION_MAJOR == 4 && DB_VERSION_MINOR >= 3
file_errcall (const DB_ENV *env, const char *buf1, const char *buf2)
#else
file_errcall (const char *buf1, char *buf2)
#endif
{
	WARNING ("libdb error: %s", buf2);
}

static void
file_paniccall (DB_ENV *env, int errval)
{
	const char *db_home;
	char *filename = NULL;
	int fd = -1;
	int db_error;

	/* create the runrec file */
	db_error = env->get_home (env, &db_home);
	if (db_error != 0 || db_home == NULL) {
		WARNING ("cannot get db_home: %s", db_strerror (db_error));
		goto out;
	}
	filename = g_build_filename (db_home, RUNREC_FILE, NULL);

	fd = g_open (filename, O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR);
	if (G_UNLIKELY (fd == -1)) {
		WARNING ("cannot create %s: %s", filename, strerror (errno));
		goto out;
	}

out:
	if (-1 != fd)
		close (fd);

	g_free (filename);

	/* abort here, try to recover on the next start */
	g_error ("Oops, db panic: %s", db_strerror (errval));
}

#define PREINSTALL_DIR "/usr/share/evolution-data-server"
#define PREINST_CONF_DIR "evolution-data-server"
#define PREINST_CONF_FILE "preinst-stamps"

static GHashTable *
read_preinst_conf_file (const char *path)
{
	GHashTable *ht;
	GError *error = NULL;
	char *contents;
	char **lines;

	ht = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

	if (!g_file_get_contents (path, &contents, NULL, &error)) {
		if (!g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
			WARNING ("cannot read preinst config file: %s", error->message);
			g_error_free (error);
			g_hash_table_destroy (ht);
			return NULL;
		}

		/* if preinst config file doesn't exist we return with an empty HT,
		 * so if there are pre-installed contacts the we can add them. */
		DEBUG ("preinst config file does not exist");
		g_error_free (error);
		return ht;
	}

	/* split content lines */
	lines = g_strsplit (contents, "\n", -1);

	char **l;
	for (l = lines; l && *l && **l != '\0'; l++) {
		char **tokens;
		char *filename = NULL, *mtime_str = NULL;
		long int *mtime;

		tokens = g_strsplit (*l, ";", -1);

		if (tokens && tokens[0]) {
			filename = tokens[0];
			mtime_str = tokens[1];
		}
		if (!filename || !mtime_str) {
			WARNING ("content line is wrong: '%s'", *l);
			/* try the next line */
			g_strfreev (tokens);
			continue;
		}

		mtime = g_new0 (long int, 1);
		*mtime = strtol ((const char *)mtime_str, (char **)NULL, 10);

		if ((errno == ERANGE && (*mtime == LONG_MAX || *mtime == LONG_MIN))
				|| (errno != 0 && *mtime == 0)) {
			WARNING ("unable to parse mtime value: %s", mtime_str);
			g_free (mtime);
			g_strfreev (tokens);
			continue;
		}

		g_hash_table_insert (ht, g_strdup (filename), mtime);
		DEBUG ("adding filename=%s mtime=%ld to preinst-conf", filename, *mtime);
		g_strfreev (tokens);
	}

	g_strfreev (lines);
	g_free (contents);

	return ht;
}

static void
add_content_line_cb (const char *filename, const long int *mtime, GString *contents)
{
	g_return_if_fail (filename);
	g_return_if_fail (mtime);
	g_return_if_fail (contents);

	g_string_append_printf (contents, "%s;%ld\n", filename, *mtime);
}

static void
write_preinst_conf_file (const char *path, GHashTable *ht)
{
	GString *contents;
	GError *error = NULL;
	gchar *conf_dir;

	g_return_if_fail (path);
	g_return_if_fail (ht);

	contents = g_string_new (NULL);

	g_hash_table_foreach (ht, (GHFunc)add_content_line_cb, contents);

	conf_dir = g_build_filename (g_get_user_config_dir (), PREINST_CONF_DIR, NULL);
	if (!g_file_test (conf_dir, G_FILE_TEST_IS_DIR)) {
		if (g_mkdir_with_parents (conf_dir, S_IRUSR | S_IWUSR | S_IXUSR) != 0) {
			WARNING ("cannot create dir '%s': %s", conf_dir, strerror (errno));
			goto out;
		}
	}

	if (!g_file_set_contents (path, contents->str, -1, &error)) {
		WARNING ("cannot write preinst conf file: %s", error->message);
		g_error_free (error);
	}

out:
	g_free (conf_dir);

	g_string_free (contents, TRUE);
}

/* returns TRUE if pre-install file needs to be installed, FALSE otherwise */
static gboolean
check_preinst_conf (GHashTable *ht, const char *filename)
{
	long int *old_mtime;
	long int *new_mtime;
	struct stat file_stat;

	g_return_val_if_fail (ht, FALSE);
	g_return_val_if_fail (filename, FALSE);

	old_mtime = g_hash_table_lookup (ht, filename);

	if (g_stat (filename, &file_stat) != 0) {
		WARNING ("cannot get stat of file '%s'", filename);
		return FALSE;
	}

	if (old_mtime && *old_mtime == file_stat.st_mtime) {
		DEBUG ("file is already installed");
		return FALSE;
	}

	new_mtime = g_new (long int, 1);
	*new_mtime = file_stat.st_mtime;

	g_hash_table_insert (ht, g_strdup (filename), new_mtime);
	DEBUG ("adding filename=%s, mtime=%ld to preinst-conf", filename, *new_mtime);

	return TRUE;
}

static void
install_pre_installed_vcards (EBookBackend *backend)
{
	GDir *directory;
	GHashTable *ht;
	const gchar *name;
	char *conf_dir;
	gboolean sync_needed = FALSE;
	gboolean preinst_dirty = FALSE;
	EBookBackendFile *bf = E_BOOK_BACKEND_FILE (backend);
	GPtrArray *ops;

	/* Get files from pre-install directory */
	directory = g_dir_open (PREINSTALL_DIR, 0, NULL);
	if (directory == NULL) {
		DEBUG ("pre-install directory does not exist");
		return;
	}

	/* read pre-installed config file */
	conf_dir = g_build_filename (g_get_user_config_dir (), PREINST_CONF_DIR, PREINST_CONF_FILE, NULL);
	ht = read_preinst_conf_file (conf_dir);
	if (!ht) {
		/* we cannot read pre-installed config directory (error differs from ENOENT)
		 * so we cannot write it later, hence returning immediately */
		g_free (conf_dir);
		return;
	}

	ops = g_ptr_array_new ();

	/* Read directorys filenames */
	while ((name = g_dir_read_name (directory)) != NULL) {
		gchar *path_plus_name;
		gchar *vcard = NULL;
		GList *vcards;

		if (!g_str_has_suffix (name, ".vcf")) {
			continue;
		}

		path_plus_name = g_build_filename (PREINSTALL_DIR, name, NULL);
		if (!check_preinst_conf (ht, path_plus_name)) {
			g_free (path_plus_name);
			continue;
		}
		/* we don't write pre-installed conf file here,
		 * only after we are ready with all pre-installed files */
		preinst_dirty = TRUE;

		if (!g_file_get_contents (path_plus_name, &vcard, NULL, NULL)) {
			g_free (path_plus_name);
			continue;
		}
		g_free (path_plus_name);

		vcards = e_vcard_util_split_cards (vcard, NULL);
		while (vcards != NULL) {
			EContact *contact;

			if (G_LIKELY (insert_contact (bf, vcards->data, &contact, ops) == 0)) {
				sync_needed = TRUE;
			}
			else {
				WARNING ("cannot insert preinstalled contact");
			}

			g_object_unref (contact);
			g_free (vcards->data);
			vcards = g_list_delete_link (vcards, vcards);
		}
		g_free (vcard);
	}
	g_dir_close (directory);

	if (preinst_dirty) {
		write_preinst_conf_file (conf_dir, ht);
	}

	g_hash_table_destroy (ht);
	g_free (conf_dir);

	/* sync the main and index dbs only if needed */
	if (sync_needed) {
		/* store the running id */
		if (store_unique_id (bf, ops) != 0) {
			WARNING ("cannot store unique id");
			goto out;
		}

		/* commit the transaction */
		if (txn_execute_ops (bf->priv->env, NULL, ops) != 0) {
			WARNING ("cannot commit transaction");
			goto out;
		}

		/* sync databases */
		sync_dbs (bf);
	}

out:
	txn_ops_free (ops);
}

static int
e_book_backend_file_setup_running_ids (EBookBackendFile *bf)
{
	DB *sdb;
	int db_error;
	char *running_id_db = NULL;

	DEBUG ("%s", G_STRFUNC);
	g_return_val_if_fail (bf->priv->file_db, EINVAL);

	/* NOTE: running_id isn't stored in index.db anymore */

	db_error = db_create (&sdb, bf->priv->env, 0);
	if (db_error != 0) {
		WARNING ("running index db_create failed: %s", db_strerror (db_error));
		goto error;
	}

	running_id_db = g_build_filename (bf->priv->dirname, RUNNING_ID_DB_NAME, NULL);
	db_error = sdb->open (sdb, NULL, running_id_db, NULL, DB_HASH,
			      DB_THREAD | DB_AUTO_COMMIT, 0666);

	if (db_error != 0) {
		DEBUG ("%s doesn't exist, try to create it", running_id_db);

		/* close and create the DB object again to free up the allocated memory */
		sdb->close (sdb, 0);
		db_error = db_create (&sdb, bf->priv->env, 0);
		if (db_error != 0) {
			WARNING ("running index db_create failed: %s", db_strerror (db_error));
			goto error;
		}

		db_error = sdb->open (sdb, NULL, running_id_db, NULL, DB_HASH,
				      DB_CREATE | DB_THREAD | DB_AUTO_COMMIT, 0666);
		if (db_error != 0) {
			WARNING ("running index open failed: %s", db_strerror (db_error));
			sdb->close (sdb, 0);
			goto error;
		}
	}

	bf->priv->id_db = sdb;

	g_free (running_id_db);

	return 0;

error:
	g_free (running_id_db);

	return db_error;
}

/* set up our db_env, it return 0 on success, db_error otherwise */
static int
e_book_backend_file_setup_db_env (EBookBackendFile *bf, gboolean only_if_exists)
{
	DB_ENV *env = NULL;
	int db_error;
	char *db_home;

	g_return_val_if_fail (E_IS_BOOK_BACKEND_FILE (bf), EINVAL);
	/* opening an already opened env could lead into a serious problem */
	g_return_val_if_fail (bf->priv->env == NULL, EINVAL);
	/* we have to have a dirname */
	g_return_val_if_fail (bf->priv->dirname, EINVAL);

	db_home = bf->priv->dirname;

	DEBUG ("setting up db_env in '%s'", db_home);

	/* create db_home directory if it does not exist */
	if (!g_file_test (db_home, G_FILE_TEST_IS_DIR)) {
		if (only_if_exists) {
			WARNING ("dir does not exist and only_if_exist is requested");
			db_error = EACCES;
			goto error;
		}
		if (g_mkdir_with_parents (db_home, S_IRUSR | S_IWUSR | S_IXUSR) != 0) {
			WARNING ("cannot create dir '%s': %s", db_home, strerror (errno));
			db_error = errno;
			goto error;
		}
	}

	/* create the environment */
	db_error = db_env_create (&env, 0);
	if (db_error != 0) {
		WARNING ("db_env_create failed with %s", db_strerror (db_error));
		goto error;
	}

	/* set up our own error handler */
	/* NOTE: this function does not have any return value */
	env->set_errcall (env, file_errcall);

	/* set up our panic callback */
	db_error = env->set_paniccall (env, file_paniccall);
	if (db_error != 0) {
		WARNING ("set_paniccall failed with: %s", db_strerror (db_error));
		goto error;
	}

	/* set the allocation routines to the non-aborting GLib functions */
	db_error = env->set_alloc (env, (void *(*)(size_t))g_try_malloc,
			(void *(*)(void *, size_t))g_try_realloc,
			g_free);
	if (db_error != 0) {
		WARNING ("set_alloc failed with %s", db_strerror (db_error));
		goto error;
	}

	/* do deadlock detection internally. */
	db_error = env->set_lk_detect(env, DB_LOCK_DEFAULT);
	if (db_error != 0) {
		WARNING ("set_lk_detect failed with %s", db_strerror (db_error));
		goto error;
	}

	/* open the environment for use */
	db_error = env->open (env, db_home, DB_CREATE | DB_INIT_MPOOL | DB_PRIVATE
			| DB_THREAD | DB_INIT_LOCK
			| DB_RECOVER | DB_INIT_TXN, 0);
	if (db_error != 0) {
		WARNING ("db_env_open failed with %s", db_strerror (db_error));
		goto error;
	}

	bf->priv->env = env;

	return db_error;

error:
	if (env)
		env->close(env, 0);

	return db_error;
}

static gboolean
e_book_backend_file_maybe_recover (const char *dirname)
{
	char *recfile;
	const char *filename;
	GDir *dir;

	recfile = g_build_filename (dirname, RUNREC_FILE, NULL);
	if (!g_file_test (recfile, G_FILE_TEST_IS_REGULAR)) {
		DEBUG ("recovery is not needed");
		g_free (recfile);
		return TRUE;
	}
	g_free (recfile);

	MESSAGE ("recovery is needed, removing corrupted files");

	/* TODO: make a backup from the old contents,
	 * maybe it is possible to recover some data from it */

	/* remove the old damaged directory */
	dir = g_dir_open (dirname, 0, NULL);
	if (!dir) {
		WARNING ("cannot open direcotory %s", dirname);
		return FALSE;
	}

	while ((filename = g_dir_read_name (dir))) {
		char *rm_file;

		rm_file = g_build_filename (dirname, filename, NULL);

		if (g_remove (rm_file) != 0) {
			WARNING ("cannot remove file '%s': %s", filename, strerror (errno));
			g_free (rm_file);
			g_dir_close (dir);
			return FALSE;
		}

		g_free (rm_file);
	}
	g_dir_close (dir);

	if (g_remove (dirname) != 0) {
		WARNING ("cannot remove directory: %s", strerror (errno));
		return FALSE;
	}

	/* recovery procedure was successful */
	return TRUE;
}

/* Check if there is any lock file left behind, if yes try to remove them.
 * NOTE: lock files only can be left on filesystem when process exits
 *       while the db->open() runs. Removing of these file is safe because
 *       only one backend object per source exists, so it is not possible
 *       that another backend object is using the same environment. */
static gboolean
e_book_backend_file_remove_db_locks (const char *dirname)
{
	const char *filename;
	GDir *dir;
	GError *error = NULL;

	dir = g_dir_open (dirname, 0, &error);
	if (error) {
		if (g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
			/* there is no such directory, so there can't be any lock file */
			DEBUG ("%s", error->message);
			g_error_free (error);
			return TRUE;
		}
		else {
			WARNING ("%s", error->message);
			g_error_free (error);
			return FALSE;
		}
	}

	while ((filename = g_dir_read_name (dir))) {
		if (g_str_has_prefix (filename, "__db.")) {
			char *rm_file;

			rm_file = g_build_filename (dirname, filename, NULL);
			WARNING ("lockfile found: %s", rm_file);

			if (g_remove (rm_file) != 0) {
				g_critical ("cannot remove lockfile '%s': %s",
						filename, strerror (errno));
				g_free (rm_file);
				g_dir_close (dir);
				return FALSE;
			}

			g_free (rm_file);
		}
	}

	g_dir_close (dir);

	return TRUE;
}

static GNOME_Evolution_Addressbook_CallStatus
e_book_backend_file_load_source (EBookBackend *backend,
				 ESource      *source,
				 gboolean      only_if_exists)
{
	EBookBackendFile *bf = E_BOOK_BACKEND_FILE (backend);
	char *dirname, *filename;
	gboolean writable = FALSE;
	int db_error = 0;
	DB *db;
	gchar *uri;

	uri = e_source_get_uri (source);

	MESSAGE ("Opening %s", uri);

	dirname = e_book_backend_file_extract_path_from_uri (uri);
	filename = g_build_filename (dirname, "addressbook.db", NULL);
	g_free (uri);

	bf->priv->dirname = dirname;
	bf->priv->filename = filename;

	/* recover from a corrupted DB */
	/* NOTE: currently it only removes the directory,
	 *       later this should run the real recovery when transactions
	 *       are introduced */
	if (!e_book_backend_file_maybe_recover (dirname)) {
		WARNING ("cannot recover dir '%s'", dirname);
		goto error;
	}

	/* remove libdb's lock files if there is any */
	if (!e_book_backend_file_remove_db_locks (dirname)) {
		goto error;
	}

	/* continue an aborted db_upgrade (this won't happen in our case) */
	db_error = e_db3_utils_maybe_recover (filename);
	if (db_error != 0) {
		WARNING ("db recovery failed: %s", db_strerror (db_error));
		goto error;
	}

	/* setup db_env, create the directory if needed */
	db_error = e_book_backend_file_setup_db_env (bf, only_if_exists);
	if (db_error != 0) {
		WARNING ("cannot setup db_env to %s", dirname);
		goto error;
	}

	/* setup our DBs */
	/* TODO: create a function for this */
	db_error = db_create (&db, bf->priv->env, 0);
	if (db_error != 0) {
		WARNING ("db_create failed: %s", db_strerror (db_error));
		goto error;
	}

	db_error = (*db->open) (db, NULL, filename, NULL, DB_HASH,
			DB_THREAD | DB_AUTO_COMMIT, 0666);

	if (db_error == DB_OLD_VERSION) {
		db_error = e_db3_utils_upgrade_format (filename);

		if (db_error != 0) {
			WARNING ("db format upgrade failed: %s", db_strerror (db_error));
			goto error;
		}

		db_error = (*db->open) (db, NULL, filename, NULL, DB_HASH,
				DB_THREAD | DB_AUTO_COMMIT, 0666);
	}

	if (db_error == 0) {
		writable = TRUE;
	} else {
		db->close (db, 0);

		db_error = db_create (&db, bf->priv->env, 0);
		if (db_error != 0) {
			WARNING ("db_create failed: %s", db_strerror (db_error));
			goto error;
		}
		db_error = (*db->open) (db, NULL, filename, NULL, DB_HASH,
				DB_RDONLY | DB_THREAD | DB_AUTO_COMMIT, 0666);

		if (db_error != 0 && !only_if_exists) {
			/* the database didn't exist, so we create the db */

			db->close (db, 0);

			db_error = db_create (&db, bf->priv->env, 0);
			if (db_error != 0) {
				WARNING ("db_create failed: %s", db_strerror (db_error));
				goto error;
			}

			db_error = (*db->open) (db, NULL, filename, NULL, DB_HASH,
					DB_CREATE | DB_THREAD | DB_AUTO_COMMIT, 0666);
			if (db_error != 0) {
				db->close (db, 0);
				WARNING ("db->open (... DB_CREATE ...) failed: %s",
						db_strerror (db_error));
			}
			else {
				/* our freshly created db needs a version */
				DBT version_name_dbt, version_dbt;

				dbt_fill_with_string (&version_name_dbt,
						E_BOOK_BACKEND_FILE_VERSION_NAME);
				dbt_fill_with_string (&version_dbt,
						E_BOOK_BACKEND_FILE_VERSION);

				db_error = db->put (db, NULL, &version_name_dbt,
						&version_dbt, DB_AUTO_COMMIT);
				if (db_error != 0) {
					WARNING ("cannot insert version string: %s",
							db_strerror (db_error));
				}
#ifdef CREATE_DEFAULT_VCARD
				EContact *contact = NULL;

				db_error = insert_contact_sync (bf, XIMIAN_VCARD, &contact);
				if (db_error != 0) {
					WARNING ("Cannot create default contact: %s", db_strerror (db_error));
				}
				if (contact)
					g_object_unref (contact);
#endif

				writable = TRUE;
			}
		}
	}

	if (db_error != 0) {
		bf->priv->file_db = NULL;
		goto error;
	}

	bf->priv->file_db = db;

	if (!e_book_backend_file_maybe_upgrade_db (bf)) {
		WARNING ("e_book_backend_file_maybe_upgrade_db failed");
		goto error;
	}

	bf->priv->index = e_book_backend_file_index_new ();

	if (TRUE == e_book_backend_file_index_setup_indicies (bf->priv->index, db,
				bf->priv->dirname)) {
		/* index file is usable */
		db_error = e_book_backend_file_setup_running_ids (bf);
		if (db_error != 0) {
			WARNING ("cannot setup the running_id");
			goto error;
		}

		install_pre_installed_vcards (backend);
	}
	else {
		/* index file is not usable, we must operate in read-only mode
		 * since running_id is stored in index.db as well */
		WARNING ("setup indices failed, index is not usable. "
				"try to live without them");
		writable = FALSE;

		/* unref the index object */
		g_object_unref (bf->priv->index);
		bf->priv->index = NULL;
	}

	e_book_backend_set_is_loaded (backend, TRUE);
	e_book_backend_set_is_writable (backend, writable);

	MESSAGE ("book %p has been loaded successfully", bf);

	return GNOME_Evolution_Addressbook_Success;

error:
	/* we have to do cleanup here, since if this operation fails and a
	 * dummy client tries to open the book again then we would leak memory */

	g_free (bf->priv->dirname);
	bf->priv->dirname = NULL;

	g_free (bf->priv->filename);
	bf->priv->filename = NULL;

	if (bf->priv->index) {
		g_object_unref (bf->priv->index);
		bf->priv->index = NULL;
	}

	if (bf->priv->file_db) {
		bf->priv->file_db->close (bf->priv->file_db, 0);
		bf->priv->file_db = NULL;
	}

	if (bf->priv->id_db) {
		bf->priv->id_db->close (bf->priv->id_db, 0);
		bf->priv->id_db = NULL;
	}

	if (bf->priv->env) {
		bf->priv->env->close (bf->priv->env, 0);
		bf->priv->env = NULL;
	}

	if (db_error == 0)
		return GNOME_Evolution_Addressbook_OtherError;

	return db_error_to_status (db_error);
}

static gboolean
select_changes (const char *name)
{
	char *p;

	if (strlen (name) < strlen (CHANGES_DB_SUFFIX))
		return FALSE;

	p = strstr (name, CHANGES_DB_SUFFIX);
	if (!p)
		return FALSE;

	if (strlen (p) != strlen (CHANGES_DB_SUFFIX))
		return FALSE;

	return TRUE;
}

static EBookBackendSyncStatus
e_book_backend_file_remove (EBookBackendSync *backend,
			    EDataBook        *book,
			    guint32           opid)
{
	EBookBackendFile *bf = E_BOOK_BACKEND_FILE (backend);
	GDir *dir;
	char *running_id_db;
	int db_error;

	/* close our main db */
	if (bf->priv->file_db) {
		db_error = bf->priv->file_db->close (bf->priv->file_db, 0);

		if (db_error != 0)
			WARNING ("db->close failed: %s", db_strerror (db_error));

		bf->priv->file_db = NULL;
	}

	/* remove main db */
	if (-1 == g_unlink (bf->priv->filename)) {
		if (errno == EACCES || errno == EPERM)
			return GNOME_Evolution_Addressbook_PermissionDenied;
		else
			return GNOME_Evolution_Addressbook_OtherError;
	}

	/* delete index dbs */
	e_book_backend_file_index_remove_dbs (bf->priv->index);

	/* close running_id db */
	db_error = bf->priv->id_db->close (bf->priv->id_db, 0);
	if (db_error != 0)
		WARNING ("db->close failed: %s", db_strerror (db_error));
	bf->priv->id_db = NULL;

	/* delete running_id.db */
	running_id_db = g_build_filename (bf->priv->dirname, RUNNING_ID_DB_NAME, NULL);
	if (-1 == g_unlink (running_id_db))
		WARNING ("failed to remove %s: %s", running_id_db, strerror (errno));
	g_free (running_id_db);

	/* remove changes dbs */
	dir = g_dir_open (bf->priv->dirname, 0, NULL);
	if (dir) {
		const char *name;

		while ((name = g_dir_read_name (dir))) {
			if (select_changes (name)) {
				char *full_path = g_build_filename (bf->priv->dirname,
						name, NULL);
				if (-1 == g_unlink (full_path)) {
					WARNING ("failed to remove change db `%s': %s",
							full_path, strerror (errno));
				}
				g_free (full_path);
			}
		}

		g_dir_close (dir);
	}

	if (-1 == g_rmdir (bf->priv->dirname))
		WARNING ("failed to remove directory `%s`: %s",
				bf->priv->dirname, strerror (errno));

	/* we may not have actually succeeded in removing the
	   backend's files/dirs, but there's nothing we can do about
	   it here..  the only time we should return failure is if we
	   failed to remove the actual data.  a failure should mean
	   that the addressbook is still valid */
	return GNOME_Evolution_Addressbook_Success;
}

static char *
e_book_backend_file_get_static_capabilities (EBookBackend *backend)
{
	return g_strdup("local,do-initial-query,bulk-removes,contact-lists,freezable");
}

static GNOME_Evolution_Addressbook_CallStatus
e_book_backend_file_cancel_operation (EBookBackend *backend, EDataBook *book)
{
	return GNOME_Evolution_Addressbook_CouldNotCancel;
}

static void
e_book_backend_file_set_mode (EBookBackend                         *backend,
			      GNOME_Evolution_Addressbook_BookMode  mode)
{
	if (e_book_backend_is_loaded (backend)) {
		e_book_backend_notify_writable (backend, TRUE);
		e_book_backend_notify_connection_status (backend, TRUE);
	}
}

static void
e_book_backend_file_sync (EBookBackend *backend)
{
	EBookBackendFile *bf = E_BOOK_BACKEND_FILE (backend);

	g_return_if_fail (bf != NULL);
	g_return_if_fail (bf->priv->file_db != NULL);

	if (e_book_backend_is_writable (E_BOOK_BACKEND (backend)) == FALSE) {
		WARNING ("book is read only");
		return;
	}

	sync_dbs (bf);
}

static gboolean
e_book_backend_file_construct (EBookBackendFile *backend)
{
	g_assert (backend != NULL);
	g_assert (E_IS_BOOK_BACKEND_FILE (backend));

	if (! e_book_backend_construct (E_BOOK_BACKEND (backend)))
		return FALSE;

	return TRUE;
}

/**
 * e_book_backend_file_new:
 */
EBookBackend *
e_book_backend_file_new (void)
{
	EBookBackendFile *backend;

	backend = g_object_new (E_TYPE_BOOK_BACKEND_FILE, NULL);

	if (! e_book_backend_file_construct (backend)) {
		g_object_unref (backend);

		return NULL;
	}

	return E_BOOK_BACKEND (backend);
}

static void
e_book_backend_file_dispose (GObject *object)
{
	EBookBackendFile *bf;

	bf = E_BOOK_BACKEND_FILE (object);

	MESSAGE ("disposing book at %p", bf);

	if (bf->priv->id_db) {
		bf->priv->id_db->close (bf->priv->id_db, 0);
		bf->priv->id_db = NULL;
	}

	if (bf->priv->file_db) {
		bf->priv->file_db->close (bf->priv->file_db, 0);
		bf->priv->file_db = NULL;
	}

	if (bf->priv->index) {
		g_object_unref (bf->priv->index);
		bf->priv->index = NULL;
	}

	if (bf->priv->env) {
		bf->priv->env->close (bf->priv->env, 0);
		bf->priv->env = NULL;
	}

	G_OBJECT_CLASS (e_book_backend_file_parent_class)->dispose (object);
}

static void
e_book_backend_file_finalize (GObject *object)
{
	EBookBackendFile *bf;

	bf = E_BOOK_BACKEND_FILE (object);

	g_free (bf->priv->filename);
	g_free (bf->priv->dirname);
	g_free (bf->priv->sort_order);

	g_free (bf->priv);

	G_OBJECT_CLASS (e_book_backend_file_parent_class)->finalize (object);
}

#ifdef G_OS_WIN32
/* Avoid compiler warning by providing a function with exactly the
 * prototype that db_env_set_func_open() wants for the open method.
 */

static int
my_open (const char *name, int oflag, ...)
{
	int mode = 0;

	if (oflag & O_CREAT) {
		va_list arg;
		va_start (arg, oflag);
		mode = va_arg (arg, int);
		va_end (arg);
	}

	return g_open (name, oflag, mode);
}

int
my_rename (const char *oldname, const char *newname)
{
	return g_rename (oldname, newname);
}

int
my_exists (const char *name, int *isdirp)
{
	if (!g_file_test (name, G_FILE_TEST_EXISTS))
		return ENOENT;
	if (isdirp != NULL)
		*isdirp = g_file_test (name, G_FILE_TEST_IS_DIR);
	return 0;
}

int
my_unlink (const char *name)
{
	return g_unlink (name);
}

#endif

static void
e_book_backend_file_class_init (EBookBackendFileClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	EBookBackendSyncClass *sync_class;
	EBookBackendClass *backend_class;

	sync_class = E_BOOK_BACKEND_SYNC_CLASS (klass);
	backend_class = E_BOOK_BACKEND_CLASS (klass);

	/* Set the virtual methods. */
	backend_class->load_source             = e_book_backend_file_load_source;
	backend_class->get_static_capabilities = e_book_backend_file_get_static_capabilities;
	backend_class->start_book_view         = e_book_backend_file_start_book_view;
	backend_class->stop_book_view          = e_book_backend_file_stop_book_view;
	backend_class->cancel_operation        = e_book_backend_file_cancel_operation;
	backend_class->set_mode                = e_book_backend_file_set_mode;
	backend_class->sync                    = e_book_backend_file_sync;
	backend_class->set_view_sort_order     = e_book_backend_file_set_book_view_sort_order;
	sync_class->remove_sync                = e_book_backend_file_remove;
	sync_class->create_contact_sync        = e_book_backend_file_create_contact;
        sync_class->create_contacts_sync       = e_book_backend_file_create_contacts;
	sync_class->remove_contacts_sync       = e_book_backend_file_remove_contacts;
	sync_class->modify_contact_sync        = e_book_backend_file_modify_contact;
	sync_class->modify_contacts_sync       = e_book_backend_file_modify_contacts;
	sync_class->get_contact_sync           = e_book_backend_file_get_contact;
	sync_class->get_contact_list_sync      = e_book_backend_file_get_contact_list;
	sync_class->get_changes_sync           = e_book_backend_file_get_changes;
	sync_class->authenticate_user_sync     = e_book_backend_file_authenticate_user;
	sync_class->get_supported_fields_sync  = e_book_backend_file_get_supported_fields;
	sync_class->get_required_fields_sync   = e_book_backend_file_get_required_fields;

	object_class->dispose = e_book_backend_file_dispose;
	object_class->finalize = e_book_backend_file_finalize;

#ifdef G_OS_WIN32
	/* Use the gstdio wrappers to open, check, rename and unlink files
	 * from libdb. */
	db_env_set_func_open (my_open);
	db_env_set_func_close (close);
	db_env_set_func_exists (my_exists);
	db_env_set_func_rename (my_rename);
	db_env_set_func_unlink (my_unlink);
#endif
}

static void
e_book_backend_file_init (EBookBackendFile *backend)
{
	EBookBackendFilePrivate *priv;

	priv = g_new0 (EBookBackendFilePrivate, 1);

	backend->priv = priv;
}
