/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Author:
 *   Nat Friedman (nat@ximian.com)
 *
 * Copyright 2000, Ximian, Inc.
 */

#include <config.h>
#include "e-data-book-view.h"
#include "e-data-book.h"
#include "e-book-backend.h"
#include "libedataserver/e-flag.h"

#define E_BOOK_BACKEND_CHECK_METHOD(backend, method, ...) G_STMT_START{ \
	if (G_UNLIKELY (!E_BOOK_BACKEND_GET_CLASS ((backend))->method)) { \
		g_warning ("%s: %s: unsupported method: %s", G_STRFUNC, G_OBJECT_TYPE_NAME ((backend)), #method); \
		e_data_book_respond_ ## method (book, opid, MethodNotSupported, ## __VA_ARGS__); \
		return; \
	} \
}G_STMT_END

#define E_BOOK_BACKEND_CHECK_FLAG(backend, flag, method, error, ...) G_STMT_START{ \
	if (G_UNLIKELY (!(backend)->priv->flag)) { \
		g_warning ("%s: %s: asserting failed: %s is not set", \
				G_STRFUNC, G_OBJECT_TYPE_NAME ((backend)), #flag); \
		e_data_book_respond_ ## method (book, opid, error, ## __VA_ARGS__); \
		return; \
	} \
}G_STMT_END

/* convenience macro for checking priv->loaded flag */
#define E_BOOK_BACKEND_CHECK_LOADED(method, ...) \
	E_BOOK_BACKEND_CHECK_FLAG(backend, loaded, method, OtherError, NULL);

/* convenience macro for checking priv->writable flag */
#define E_BOOK_BACKEND_CHECK_WRITABLE(method, ...) \
	E_BOOK_BACKEND_CHECK_FLAG(backend, writable, method, PermissionDenied, NULL);

/* define aliases for symbols with inconsistent name */
#define e_data_book_respond_create_contact e_data_book_respond_create
#define e_data_book_respond_modify_contact e_data_book_respond_modify

struct _EBookBackendPrivate {
	GMutex *open_mutex;

	GMutex *clients_mutex;
	GList *clients;

	ESource *source;
	gboolean loaded, writable, removed;

	GMutex *views_mutex;
	EList *views;

        EFlag *opened_flag;
};

/* Signal IDs */
enum {
	LAST_CLIENT_GONE,
	LAST_SIGNAL
};

static guint e_book_backend_signals[LAST_SIGNAL];

static GObjectClass *parent_class;

/**
 * e_book_backend_construct:
 * @backend: an #EBookBackend
 *
 * Does nothing.
 *
 * Return value: %TRUE.
 **/
gboolean
e_book_backend_construct (EBookBackend *backend)
{
	return TRUE;
}

/**
 * e_book_backend_load_source:
 * @backend: an #EBookBackend
 * @source: an #ESource to load
 * @only_if_exists: %TRUE to prevent the creation of a new book
 *
 * Loads @source into @backend.
 *
 * Return value: A #GNOME_Evolution_Addressbook_CallStatus indicating the outcome.
 **/
EDataBookStatus
e_book_backend_load_source (EBookBackend           *backend,
			    ESource                *source,
			    gboolean                only_if_exists)
{
	EDataBookStatus status;

	g_return_val_if_fail (E_IS_BOOK_BACKEND (backend), OtherError);
	g_return_val_if_fail (source, OtherError);
	g_return_val_if_fail (backend->priv->loaded == FALSE, OtherError);
	g_return_val_if_fail (E_BOOK_BACKEND_GET_CLASS (backend)->load_source, OtherError);

	status = (* E_BOOK_BACKEND_GET_CLASS (backend)->load_source) (backend, source, only_if_exists);

	if (status == Success || status == InvalidServerVersion) {
		g_object_ref (source);
		backend->priv->source = source;
	}

	return status;
}

/**
 * e_book_backend_get_source:
 * @backend: An addressbook backend.
 * 
 * Queries the source that an addressbook backend is serving.
 * 
 * Return value: ESource for the backend.
 **/
ESource *
e_book_backend_get_source (EBookBackend *backend)
{
	g_return_val_if_fail (E_IS_BOOK_BACKEND (backend), NULL);

	return backend->priv->source;
}

/**
 * e_book_backend_open:
 * @backend: an #EBookBackend
 * @book: an #EDataBook
 * @opid: the ID to use for this operation
 * @only_if_exists: %TRUE to prevent the creation of a new book
 *
 * Executes an 'open' request specified by @opid on @book
 * using @backend.
 **/
void
e_book_backend_open (EBookBackend *backend,
		     EDataBook    *book,
		     guint32       opid,
		     gboolean      only_if_exists)
{
	g_return_if_fail (E_IS_BOOK_BACKEND (backend));
	g_return_if_fail (E_IS_DATA_BOOK (book));

        e_flag_clear (backend->priv->opened_flag);

	g_mutex_lock (backend->priv->open_mutex);

	if (backend->priv->loaded) {
		e_data_book_respond_open (book, opid, Success, backend->priv->writable);
	} else {
		EDataBookStatus status =
			e_book_backend_load_source (backend, e_data_book_get_source (book), only_if_exists);

		e_data_book_respond_open (book, opid, status, backend->priv->writable);
	}

	g_mutex_unlock (backend->priv->open_mutex);

        e_flag_set (backend->priv->opened_flag);
}

/**
 * e_book_backend_remove:
 * @backend: an #EBookBackend
 * @book: an #EDataBook
 * @opid: the ID to use for this operation
 *
 * Executes a 'remove' request to remove all of @backend's data,
 * specified by @opid on @book.
 **/
void
e_book_backend_remove (EBookBackend *backend,
		       EDataBook    *book,
		       guint32       opid)
{
	g_return_if_fail (E_IS_BOOK_BACKEND (backend));
	g_return_if_fail (E_IS_DATA_BOOK (book));

	E_BOOK_BACKEND_CHECK_METHOD (backend, remove);

	(* E_BOOK_BACKEND_GET_CLASS (backend)->remove) (backend, book, opid);
}

/**
 * e_book_backend_create_contact:
 * @backend: an #EBookBackend
 * @book: an #EDataBook
 * @opid: the ID to use for this operation
 * @vcard: the VCard to add
 *
 * Executes a 'create contact' request specified by @opid on @book
 * using @backend.
 **/
void
e_book_backend_create_contact (EBookBackend *backend,
			       EDataBook    *book,
			       guint32       opid,
			       const char   *vcard)
{
	g_return_if_fail (E_IS_BOOK_BACKEND (backend));
	g_return_if_fail (E_IS_DATA_BOOK (book));
	g_return_if_fail (vcard);

	E_BOOK_BACKEND_CHECK_METHOD (backend, create_contact, NULL);

	/* wait till book is opened */
	e_flag_wait (backend->priv->opened_flag);

	/* don't bother the backend if it is not loaded or it is not writable */
	E_BOOK_BACKEND_CHECK_LOADED (create_contact, NULL);
	E_BOOK_BACKEND_CHECK_WRITABLE (create_contact, NULL);

	(* E_BOOK_BACKEND_GET_CLASS (backend)->create_contact) (backend, book, opid, vcard);
}

/**
 * e_book_backend_create_contacts:
 * @backend: an #EBookBackend
 * @book: an #EDataBook
 * @opid: the ID to use for this operation
 * @vcards: the VCards to update
 *
 * Executes a 'create contacts' request specified by @opid on @book
 * using @backend.
 **/
void
e_book_backend_create_contacts (EBookBackend *backend,
				EDataBook    *book,
				guint32       opid,
				const char  **vcards)
{
	g_return_if_fail (E_IS_BOOK_BACKEND (backend));
	g_return_if_fail (E_IS_DATA_BOOK (book));
	g_return_if_fail (vcards);

	E_BOOK_BACKEND_CHECK_METHOD (backend, create_contacts, NULL);

        e_flag_wait (backend->priv->opened_flag);

	E_BOOK_BACKEND_CHECK_LOADED (create_contacts, NULL);
	E_BOOK_BACKEND_CHECK_WRITABLE (create_contacts, NULL);

	(* E_BOOK_BACKEND_GET_CLASS (backend)->create_contacts) (backend, book, opid, vcards);
}

/**
 * e_book_backend_remove_contacts:
 * @backend: an #EBookBackend
 * @book: an #EDataBook
 * @opid: the ID to use for this operation
 * @id_list: list of string IDs to remove
 *
 * Executes a 'remove contacts' request specified by @opid on @book
 * using @backend.
 **/
void
e_book_backend_remove_contacts (EBookBackend *backend,
				EDataBook    *book,
				guint32       opid,
				GList        *id_list)
{
	g_return_if_fail (E_IS_BOOK_BACKEND (backend));
	g_return_if_fail (E_IS_DATA_BOOK (book));
	/* if no ids are requested, just respond that we 'successfully' deleted
	 * zero contacts */
	if (!id_list) {
		g_warning ("%s: attempted to delete 0 contacts", G_STRFUNC);
		e_data_book_respond_remove_contacts (book, opid, Success, id_list);
		return;
	}

	E_BOOK_BACKEND_CHECK_METHOD (backend, remove_contacts, NULL);

        e_flag_wait (backend->priv->opened_flag);

	E_BOOK_BACKEND_CHECK_LOADED (remove_contacts, NULL);
	E_BOOK_BACKEND_CHECK_WRITABLE (remove_contacts, NULL);

	(* E_BOOK_BACKEND_GET_CLASS (backend)->remove_contacts) (backend, book, opid, id_list);
}

/**
 * e_book_backend_remove_all_contacts:
 * @backend: an #EBookBackend
 * @book: an #EDataBook
 * @opid: the ID to use for this operation
 *
 * Executes a 'remove all contacts' request specified by @opid on @book
 * using @backend.
 **/
void
e_book_backend_remove_all_contacts (EBookBackend *backend,
				    EDataBook    *book,
				    guint32       opid)
{
	g_return_if_fail (E_IS_BOOK_BACKEND (backend));
	g_return_if_fail (E_IS_DATA_BOOK (book));

	E_BOOK_BACKEND_CHECK_METHOD (backend, remove_all_contacts, NULL);

        e_flag_wait (backend->priv->opened_flag);

	E_BOOK_BACKEND_CHECK_LOADED (remove_all_contacts, NULL);
	E_BOOK_BACKEND_CHECK_WRITABLE (remove_all_contacts, NULL);

	(* E_BOOK_BACKEND_GET_CLASS (backend)->remove_all_contacts) (backend, book, opid);
}

/**
 * e_book_backend_modify_contact:
 * @backend: an #EBookBackend
 * @book: an #EDataBook
 * @opid: the ID to use for this operation
 * @vcard: the VCard to update
 *
 * Executes a 'modify contact' request specified by @opid on @book
 * using @backend.
 **/
void
e_book_backend_modify_contact (EBookBackend *backend,
			       EDataBook    *book,
			       guint32       opid,
			       const char   *vcard)
{
	g_return_if_fail (E_IS_BOOK_BACKEND (backend));
	g_return_if_fail (E_IS_DATA_BOOK (book));
	g_return_if_fail (vcard);

	E_BOOK_BACKEND_CHECK_METHOD (backend, modify_contact, NULL);

	e_flag_wait (backend->priv->opened_flag);

	E_BOOK_BACKEND_CHECK_LOADED (modify_contact, NULL);
	E_BOOK_BACKEND_CHECK_WRITABLE (modify_contact, NULL);

	(* E_BOOK_BACKEND_GET_CLASS (backend)->modify_contact) (backend, book, opid, vcard);
}

/**
 * e_book_backend_modify_contacts:
 * @backend: an #EBookBackend
 * @book: an #EDataBook
 * @opid: the ID to use for this operation
 * @vcards: the VCards to update
 *
 * Executes a 'modify contacts' request specified by @opid on @book
 * using @backend.
 **/
void
e_book_backend_modify_contacts (EBookBackend *backend,
				EDataBook    *book,
				guint32       opid,
				const char  **vcards)
{
	g_return_if_fail (E_IS_BOOK_BACKEND (backend));
	g_return_if_fail (E_IS_DATA_BOOK (book));
	g_return_if_fail (vcards);

	E_BOOK_BACKEND_CHECK_METHOD (backend, modify_contacts, NULL);

        e_flag_wait (backend->priv->opened_flag);

	E_BOOK_BACKEND_CHECK_LOADED (modify_contacts, NULL);
	E_BOOK_BACKEND_CHECK_WRITABLE (modify_contacts, NULL);

	(* E_BOOK_BACKEND_GET_CLASS (backend)->modify_contacts) (backend, book, opid, vcards);
}

/**
 * e_book_backend_get_contact:
 * @backend: an #EBookBackend
 * @book: an #EDataBook
 * @opid: the ID to use for this operation
 * @id: the ID of the contact to get
 *
 * Executes a 'get contact' request specified by @opid on @book
 * using @backend.
 **/
void
e_book_backend_get_contact (EBookBackend *backend,
			    EDataBook    *book,
			    guint32       opid,
			    const char   *id)
{
	g_return_if_fail (E_IS_BOOK_BACKEND (backend));
	g_return_if_fail (E_IS_DATA_BOOK (book));
	g_return_if_fail (id);

	E_BOOK_BACKEND_CHECK_METHOD (backend, get_contact, NULL);

        e_flag_wait (backend->priv->opened_flag);

	E_BOOK_BACKEND_CHECK_LOADED (get_contact, NULL);

	(* E_BOOK_BACKEND_GET_CLASS (backend)->get_contact) (backend, book, opid, id);
}

/**
 * e_book_backend_get_contact_list:
 * @backend: an #EBookBackend
 * @book: an #EDataBook
 * @opid: the ID to use for this operation
 * @query: the s-expression to match
 *
 * Executes a 'get contact list' request specified by @opid on @book
 * using @backend.
 **/
void
e_book_backend_get_contact_list (EBookBackend *backend,
				 EDataBook    *book,
				 guint32       opid,
				 const char   *query)
{
	g_return_if_fail (E_IS_BOOK_BACKEND (backend));
	g_return_if_fail (E_IS_DATA_BOOK (book));
	g_return_if_fail (query);

	E_BOOK_BACKEND_CHECK_METHOD (backend, get_contact_list, NULL);

        e_flag_wait (backend->priv->opened_flag);

	E_BOOK_BACKEND_CHECK_LOADED (get_contact_list, NULL);

	(* E_BOOK_BACKEND_GET_CLASS (backend)->get_contact_list) (backend, book, opid, query);
}

/**
 * e_book_backend_start_book_view:
 * @backend: an #EBookBackend
 * @book_view: the #EDataBookView to start
 *
 * Starts running the query specified by @book_view, emitting
 * signals for matching contacts.
 **/
void
e_book_backend_start_book_view (EBookBackend  *backend,
				EDataBookView *book_view)
{
	g_return_if_fail (E_IS_BOOK_BACKEND (backend));
	g_return_if_fail (E_IS_DATA_BOOK_VIEW (book_view));
	g_return_if_fail (E_BOOK_BACKEND_GET_CLASS (backend)->start_book_view);

        e_flag_wait (backend->priv->opened_flag);

	g_return_if_fail (backend->priv->loaded);

	(* E_BOOK_BACKEND_GET_CLASS (backend)->start_book_view) (backend, book_view);
}

/**
 * e_book_backend_stop_book_view:
 * @backend: an #EBookBackend
 * @book_view: the #EDataBookView to stop
 *
 * Stops running the query specified by @book_view, emitting
 * no more signals.
 **/
void
e_book_backend_stop_book_view (EBookBackend  *backend,
			       EDataBookView *book_view)
{
	g_return_if_fail (E_IS_BOOK_BACKEND (backend));
	g_return_if_fail (E_IS_DATA_BOOK_VIEW (book_view));
	g_return_if_fail (E_BOOK_BACKEND_GET_CLASS (backend)->stop_book_view);

        e_flag_wait (backend->priv->opened_flag);

	g_return_if_fail (backend->priv->loaded);

	(* E_BOOK_BACKEND_GET_CLASS (backend)->stop_book_view) (backend, book_view);
}

/**
 * e_book_backend_get_changes:
 * @backend: an #EBookBackend
 * @book: an #EDataBook
 * @opid: the ID to use for this operation
 * @change_id: the ID of the changeset
 *
 * Executes a 'get changes' request specified by @opid on @book
 * using @backend.
 **/
void
e_book_backend_get_changes (EBookBackend *backend,
			    EDataBook    *book,
			    guint32       opid,
			    const char   *change_id)
{
	g_return_if_fail (E_IS_BOOK_BACKEND (backend));
	g_return_if_fail (E_IS_DATA_BOOK (book));
	g_return_if_fail (change_id);

	E_BOOK_BACKEND_CHECK_METHOD (backend, get_changes, NULL);

        e_flag_wait (backend->priv->opened_flag);

	E_BOOK_BACKEND_CHECK_LOADED (get_changes, NULL);

	(* E_BOOK_BACKEND_GET_CLASS (backend)->get_changes) (backend, book, opid, change_id);
}

/**
 * e_book_backend_reset_changes:
 * @backend: an #EBookBackend
 * @book: an #EDataBook
 * @opid: the ID to use for this operation
 * @change_id: the ID of the changeset
 *
 * Executes a 'reset changes' request specified by @opid on @book
 * using @backend.
 */
void
e_book_backend_reset_changes (EBookBackend *backend,
			      EDataBook    *book,
			      guint32       opid,
			      const char   *change_id)
{
	g_return_if_fail (E_IS_BOOK_BACKEND (backend));
	g_return_if_fail (E_IS_DATA_BOOK (book));
	g_return_if_fail (change_id);

	E_BOOK_BACKEND_CHECK_METHOD (backend, reset_changes);

	e_flag_wait (backend->priv->opened_flag);

	E_BOOK_BACKEND_CHECK_FLAG(backend, loaded, reset_changes, OtherError);

	(* E_BOOK_BACKEND_GET_CLASS (backend)->reset_changes) (backend, book, opid, change_id);
}

/**
 * e_book_backend_authenticate_user:
 * @backend: an #EBookBackend
 * @book: an #EDataBook
 * @opid: the ID to use for this operation
 * @user: the name of the user account
 * @passwd: the user's password
 * @auth_method: the authentication method to use
 *
 * Executes an 'authenticate' request specified by @opid on @book
 * using @backend.
 **/
void
e_book_backend_authenticate_user (EBookBackend *backend,
				  EDataBook    *book,
				  guint32       opid,
				  const char   *user,
				  const char   *passwd,
				  const char   *auth_method)
{
	g_return_if_fail (E_IS_BOOK_BACKEND (backend));
	g_return_if_fail (E_IS_DATA_BOOK (book));
	g_return_if_fail (user && passwd && auth_method);

	E_BOOK_BACKEND_CHECK_METHOD (backend, authenticate_user);

        e_flag_wait (backend->priv->opened_flag);

	(* E_BOOK_BACKEND_GET_CLASS (backend)->authenticate_user) (backend, book, opid, user, passwd, auth_method);
}

/**
 * e_book_backend_get_required_fields:
 * @backend: an #EBookBackend
 * @book: an #EDataBook
 * @opid: the ID to use for this operation
 *
 * Executes a 'get required fields' request specified by @opid on @book
 * using @backend.
 **/
void
e_book_backend_get_required_fields (EBookBackend *backend,
				     EDataBook    *book,
				     guint32       opid)

{
	g_return_if_fail (E_IS_BOOK_BACKEND (backend));
	g_return_if_fail (E_IS_DATA_BOOK (book));

	E_BOOK_BACKEND_CHECK_METHOD (backend, get_required_fields, NULL);

	(* E_BOOK_BACKEND_GET_CLASS (backend)->get_required_fields) (backend, book, opid);
}

/**
 * e_book_backend_get_supported_fields:
 * @backend: an #EBookBackend
 * @book: an #EDataBook
 * @opid: the ID to use for this operation
 *
 * Executes a 'get supported fields' request specified by @opid on @book
 * using @backend.
 **/
void
e_book_backend_get_supported_fields (EBookBackend *backend,
				     EDataBook    *book,
				     guint32       opid)

{
	g_return_if_fail (E_IS_BOOK_BACKEND (backend));
	g_return_if_fail (E_IS_DATA_BOOK (book));

	E_BOOK_BACKEND_CHECK_METHOD (backend, get_supported_fields, NULL);

	(* E_BOOK_BACKEND_GET_CLASS (backend)->get_supported_fields) (backend, book, opid);
}

/**
 * e_book_backend_get_supported_auth_methods:
 * @backend: an #EBookBackend
 * @book: an #EDataBook
 * @opid: the ID to use for this operation
 *
 * Executes a 'get supported auth methods' request specified by @opid on @book
 * using @backend.
 **/
void
e_book_backend_get_supported_auth_methods (EBookBackend *backend,
					   EDataBook    *book,
					   guint32       opid)
{
	g_return_if_fail (E_IS_BOOK_BACKEND (backend));
	g_return_if_fail (E_IS_DATA_BOOK (book));

	E_BOOK_BACKEND_CHECK_METHOD (backend, get_supported_auth_methods, NULL);

	(* E_BOOK_BACKEND_GET_CLASS (backend)->get_supported_auth_methods) (backend, book, opid);
}

/**
 * e_book_backend_cancel_operation:
 * @backend: an #EBookBackend
 * @book: an #EDataBook whose operation should be cancelled
 *
 * Cancel @book's running operation on @backend.
 *
 * Return value: A GNOME_Evolution_Addressbook_CallStatus indicating the outcome.
 **/
EDataBookStatus
e_book_backend_cancel_operation (EBookBackend *backend,
				 EDataBook    *book)
{
	g_return_val_if_fail (E_IS_BOOK_BACKEND (backend), OtherError);
	g_return_val_if_fail (E_IS_DATA_BOOK (book), OtherError);
	g_return_val_if_fail (E_BOOK_BACKEND_GET_CLASS (backend)->cancel_operation, OtherError);

	return (* E_BOOK_BACKEND_GET_CLASS (backend)->cancel_operation) (backend, book);
}

static void
book_destroy_cb (gpointer data, GObject *where_book_was)
{
	EBookBackend *backend = E_BOOK_BACKEND (data);

	e_book_backend_remove_client (backend, (EDataBook *)where_book_was);
}

static void
last_client_gone (EBookBackend *backend)
{
	g_signal_emit (backend, e_book_backend_signals[LAST_CLIENT_GONE], 0);
}

/**
 * e_book_backend_get_book_views:
 * @backend: an #EBookBackend
 *
 * Gets the list of #EDataBookView views running on this backend.
 *
 * Return value: An #EList of #EDataBookView objects,
 * caller should unref the result with #g_object_unref().
 **/
EList*
e_book_backend_get_book_views (EBookBackend *backend)
{
	EList *copy;

	g_return_val_if_fail (E_IS_BOOK_BACKEND (backend), NULL);

	g_mutex_lock (backend->priv->views_mutex);

	copy = e_list_duplicate (backend->priv->views);

	g_mutex_unlock (backend->priv->views_mutex);

	return copy;
}

/**
 * e_book_backend_add_book_view:
 * @backend: an #EBookBackend
 * @view: an #EDataBookView
 *
 * Adds @view to @backend for querying.
 **/
void
e_book_backend_add_book_view (EBookBackend *backend,
			      EDataBookView *view)
{
	g_return_if_fail (E_IS_BOOK_BACKEND (backend));

	g_mutex_lock (backend->priv->views_mutex);

	e_list_append (backend->priv->views, view);

	g_mutex_unlock (backend->priv->views_mutex);
}

/**
 * e_book_backend_remove_book_view:
 * @backend: an #EBookBackend
 * @view: an #EDataBookView
 *
 * Removes @view from @backend.
 **/
void
e_book_backend_remove_book_view (EBookBackend *backend,
				 EDataBookView *view)
{
	g_return_if_fail (E_IS_BOOK_BACKEND (backend));

	g_mutex_lock (backend->priv->views_mutex);

	e_list_remove (backend->priv->views, view);

	g_mutex_unlock (backend->priv->views_mutex);
}

/**
 * e_book_backend_add_client:
 * @backend: An addressbook backend.
 * @book: the corba object representing the client connection.
 *
 * Adds a client to an addressbook backend.
 *
 * Return value: TRUE on success, FALSE on failure to add the client.
 */
gboolean
e_book_backend_add_client (EBookBackend      *backend,
			   EDataBook         *book)
{
	g_return_val_if_fail (E_IS_BOOK_BACKEND (backend), FALSE);
	g_return_val_if_fail (E_IS_DATA_BOOK (book), FALSE);
	g_object_weak_ref (G_OBJECT (book), book_destroy_cb, backend);
	g_mutex_lock (backend->priv->clients_mutex);
	backend->priv->clients = g_list_prepend (backend->priv->clients, book);
	g_mutex_unlock (backend->priv->clients_mutex);

	return TRUE;
}

/**
 * e_book_backend_remove_client:
 * @backend: an #EBookBackend
 * @book: an #EDataBook to remove
 *
 * Removes @book from the list of @backend's clients.
 **/
void
e_book_backend_remove_client (EBookBackend *backend,
			      EDataBook    *book)
{
	g_return_if_fail (E_IS_BOOK_BACKEND (backend));
	g_return_if_fail (E_IS_DATA_BOOK (book));

	/* up our backend's refcount here so that last_client_gone
	   doesn't end up unreffing us (while we're holding the
	   lock) */
	g_object_ref (backend);

	/* Disconnect */
	g_mutex_lock (backend->priv->clients_mutex);
	backend->priv->clients = g_list_remove (backend->priv->clients, book);

	/* When all clients go away, notify the parent factory about it so that
	 * it may decide whether to kill the backend or not.
	 */
	if (!backend->priv->clients)
		last_client_gone (backend);
	
	g_mutex_unlock (backend->priv->clients_mutex);

	g_object_unref (backend);
}

/**
 * e_book_backend_has_out_of_proc_clients:
 * @backend: an #EBookBackend
 *
 * Checks if @backend has clients running in other system processes.
 *
 * Return value: %TRUE if there are clients in other processes, %FALSE otherwise.
 **/
gboolean
e_book_backend_has_out_of_proc_clients (EBookBackend *backend)
{
	return TRUE;
}

/**
 * e_book_backend_get_static_capabilities:
 * @backend: an #EBookBackend
 *
 * Gets the capabilities offered by this @backend.
 *
 * Return value: A string listing the capabilities.
 **/
char *
e_book_backend_get_static_capabilities (EBookBackend *backend)
{
	g_return_val_if_fail (E_IS_BOOK_BACKEND (backend), NULL);
	g_return_val_if_fail (E_BOOK_BACKEND_GET_CLASS (backend)->get_static_capabilities, NULL);

	return E_BOOK_BACKEND_GET_CLASS (backend)->get_static_capabilities (backend);
}

/**
 * e_book_backend_is_loaded:
 * @backend: an #EBookBackend
 *
 * Checks if @backend's storage has been opened and the backend
 * itself is ready for accessing.
 *
 * Return value: %TRUE if loaded, %FALSE otherwise.
 **/
gboolean
e_book_backend_is_loaded (EBookBackend *backend)
{
	g_return_val_if_fail (E_IS_BOOK_BACKEND (backend), FALSE);

	return backend->priv->loaded;
}

/**
 * e_book_backend_set_is_loaded:
 * @backend: an #EBookBackend
 * @is_loaded: A flag indicating whether the backend is loaded
 *
 * Sets the flag indicating whether @backend is loaded to @is_loaded.
 * Meant to be used by backend implementations.
 **/
void
e_book_backend_set_is_loaded (EBookBackend *backend, gboolean is_loaded)
{
	g_return_if_fail (E_IS_BOOK_BACKEND (backend));

	backend->priv->loaded = is_loaded;
}

/**
 * e_book_backend_is_writable:
 * @backend: an #EBookBackend
 *
 * Checks if we can write to @backend.
 *
 * Return value: %TRUE if writeable, %FALSE if not.
 **/
gboolean
e_book_backend_is_writable (EBookBackend *backend)
{
	g_return_val_if_fail (E_IS_BOOK_BACKEND (backend), FALSE);
	
	return backend->priv->writable;
}

/**
 * e_book_backend_set_is_writable:
 * @backend: an #EBookBackend
 * @is_writable: A flag indicating whether the backend is writeable
 *
 * Sets the flag indicating whether @backend is writeable to @is_writeable.
 * Meant to be used by backend implementations.
 **/
void
e_book_backend_set_is_writable (EBookBackend *backend, gboolean is_writable)
{
	g_return_if_fail (E_IS_BOOK_BACKEND (backend));
	
	backend->priv->writable = is_writable;
}

/**
 * e_book_backend_is_removed:
 * @backend: an #EBookBackend
 *
 * Checks if @backend has been removed from its physical storage.
 *
 * Return value: %TRUE if @backend has been removed, %FALSE otherwise.
 **/
gboolean
e_book_backend_is_removed (EBookBackend *backend)
{
	g_return_val_if_fail (E_IS_BOOK_BACKEND (backend), FALSE);
	
	return backend->priv->removed;
}

/**
 * e_book_backend_set_is_removed:
 * @backend: an #EBookBackend
 * @is_removed: A flag indicating whether the backend's storage was removed
 *
 * Sets the flag indicating whether @backend was removed to @is_removed.
 * Meant to be used by backend implementations.
 **/
void
e_book_backend_set_is_removed (EBookBackend *backend, gboolean is_removed)
{
	g_return_if_fail (E_IS_BOOK_BACKEND (backend));

	backend->priv->removed = is_removed;
}

/**
 * e_book_backend_set_mode:
 * @backend: an #EBookbackend
 * @mode: a mode indicating the online/offline status of the backend
 *
 * Sets @backend's online/offline mode to @mode. Mode can be 1 for offline
 * or 2 indicating that it is connected and online.
 **/
void 
e_book_backend_set_mode (EBookBackend *backend,
			 EDataBookMode  mode)
{
	g_return_if_fail (E_IS_BOOK_BACKEND (backend));
	g_return_if_fail (E_BOOK_BACKEND_GET_CLASS (backend)->set_mode);

	(* E_BOOK_BACKEND_GET_CLASS (backend)->set_mode) (backend,  mode);

}

void 
e_book_backend_sync (EBookBackend *backend)
{
	g_return_if_fail (E_IS_BOOK_BACKEND (backend));
	
	if (E_BOOK_BACKEND_GET_CLASS (backend)->sync)
		(* E_BOOK_BACKEND_GET_CLASS (backend)->sync) (backend);
}

/**
 * e_book_backend_change_add_new:
 * @vcard: a VCard string
 *
 * Creates a new change item indicating @vcard was added.
 * Meant to be used by backend implementations.
 *
 * Return value: A new #GNOME_Evolution_Addressbook_BookChangeItem.
 **/
EDataBookChange *
e_book_backend_change_add_new     (const char *vcard)
{
        EDataBookChange *new_change = g_new (EDataBookChange, 1);

	new_change->change_type = E_BOOK_BACKEND_CHANGE_ADDED;
	new_change->vcard = g_strdup (vcard);

	return new_change;
}

/**
 * e_book_backend_change_modify_new:
 * @vcard: a VCard string
 *
 * Creates a new change item indicating @vcard was modified.
 * Meant to be used by backend implementations.
 *
 * Return value: A new #GNOME_Evolution_Addressbook_BookChangeItem.
 **/
EDataBookChange *
e_book_backend_change_modify_new  (const char *vcard)
{
        EDataBookChange *new_change = g_new (EDataBookChange, 1);

	new_change->change_type = E_BOOK_BACKEND_CHANGE_MODIFIED;
	new_change->vcard = g_strdup (vcard);

	return new_change;
}

/**
 * e_book_backend_change_delete_new:
 * @vcard: a VCard string
 *
 * Creates a new change item indicating @vcard was deleted.
 * Meant to be used by backend implementations.
 *
 * Return value: A new #GNOME_Evolution_Addressbook_BookChangeItem.
 **/
EDataBookChange *
e_book_backend_change_delete_new  (const char *vcard)
{
        EDataBookChange *new_change = g_new (EDataBookChange, 1);

	new_change->change_type = E_BOOK_BACKEND_CHANGE_DELETED;
	new_change->vcard = g_strdup (vcard);

	return new_change;
}



static void
e_book_backend_foreach_view (EBookBackend *backend,
			     void (*callback) (EDataBookView *, gpointer),
			     gpointer user_data)
{
	EDataBookView *view;
	EIterator *iter;

	g_mutex_lock (backend->priv->views_mutex);

	iter = e_list_get_iterator (backend->priv->views);

	while (e_iterator_is_valid (iter)) {
		view = (EDataBookView*)e_iterator_get (iter);

		g_object_ref (view);
		callback (view, user_data);
		g_object_unref (view);

		e_iterator_next (iter);
	}

	e_list_remove_iterator (backend->priv->views, iter);
	g_object_unref (iter);

	g_mutex_unlock (backend->priv->views_mutex);
}


static void
view_notify_update (EDataBookView *view, gpointer contact)
{
	e_data_book_view_notify_update (view, contact);
}

/**
 * e_book_backend_notify_update:
 * @backend: an #EBookBackend
 * @contact: a new or modified contact
 *
 * Notifies all of @backend's book views about the new or modified
 * contacts @contact.
 *
 * e_data_book_respond_create() and e_data_book_respond_modify() call this
 * function for you. You only need to call this from your backend if
 * contacts are created or modified by another (non-PAS-using) client.
 **/
void
e_book_backend_notify_update (EBookBackend *backend, EContact *contact)
{
	e_book_backend_foreach_view (backend, view_notify_update, contact);
}


static void
view_notify_remove (EDataBookView *view, gpointer id)
{
	e_data_book_view_notify_remove (view, id);
}

/**
 * e_book_backend_notify_remove:
 * @backend: an #EBookBackend
 * @id: a contact id
 *
 * Notifies all of @backend's book views that the contact with UID
 * @id has been removed.
 *
 * e_data_book_respond_remove_contacts() calls this function for you. You
 * only need to call this from your backend if contacts are removed by
 * another (non-PAS-using) client.
 **/
void
e_book_backend_notify_remove (EBookBackend *backend, const char *id)
{
	e_book_backend_foreach_view (backend, view_notify_remove, (gpointer)id);
}


static void
view_notify_complete (EDataBookView *view, gpointer unused)
{
	e_data_book_view_notify_complete (view, Success);
}

/**
 * e_book_backend_notify_complete:
 * @backend: an #EBookbackend
 *
 * Notifies all of @backend's book views that the current set of
 * notifications is complete; use this after a series of
 * e_book_backend_notify_update() and e_book_backend_notify_remove() calls.
 **/
void
e_book_backend_notify_complete (EBookBackend *backend)
{
	e_book_backend_foreach_view (backend, view_notify_complete, NULL);
}


/**
 * e_book_backend_notify_writable:
 * @backend: an #EBookBackend
 * @is_writable: flag indicating writable status
 *
 * Notifies all backends clients about the current writable state.
 **/
void 
e_book_backend_notify_writable (EBookBackend *backend, gboolean is_writable)
{
	EBookBackendPrivate *priv;
	GList *clients;
	
	priv = backend->priv;
	priv->writable = is_writable;
	g_mutex_lock (priv->clients_mutex);
	
	for (clients = priv->clients; clients != NULL; clients = g_list_next (clients))
		e_data_book_report_writable (E_DATA_BOOK (clients->data), is_writable);

	g_mutex_unlock (priv->clients_mutex);

}

/**
 * e_book_backend_notify_connection_status:
 * @backend: an #EBookBackend
 * @is_online: flag indicating whether @backend is connected and online
 *
 * Notifies clients of @backend's connection status indicated by @is_online.
 * Meant to be used by backend implementations.
 **/
void 
e_book_backend_notify_connection_status (EBookBackend *backend, gboolean is_online)
{
	EBookBackendPrivate *priv;
	GList *clients;
	
	priv = backend->priv;
	g_mutex_lock (priv->clients_mutex);
	
	for (clients = priv->clients; clients != NULL; clients = g_list_next (clients))
		e_data_book_report_connection_status (E_DATA_BOOK (clients->data), is_online);

	g_mutex_unlock (priv->clients_mutex);
}

/**
 * e_book_backend_notify_auth_required:
 * @backend: an #EBookBackend
 *
 * Notifies clients that @backend requires authentication in order to
 * connect. Means to be used by backend implementations.
 **/
void
e_book_backend_notify_auth_required (EBookBackend *backend)
{
	EBookBackendPrivate *priv;
	GList *clients;
	
	priv = backend->priv;
	g_mutex_lock (priv->clients_mutex);
	
	for (clients = priv->clients; clients != NULL; clients = g_list_next (clients))
		e_data_book_report_auth_required (E_DATA_BOOK (clients->data));
	g_mutex_unlock (priv->clients_mutex);
}

void 
e_book_backend_set_book_view_sort_order (EBookBackend  *backend,
					 EDataBookView *book_view, 
					 const gchar   *query_term)
{
	g_return_if_fail (E_IS_BOOK_BACKEND (backend));
	
	if (E_BOOK_BACKEND_GET_CLASS (backend)->set_view_sort_order)
		(* E_BOOK_BACKEND_GET_CLASS (backend)->set_view_sort_order) (backend, book_view, query_term);
}

static void
e_book_backend_init (EBookBackend *backend)
{
	EBookBackendPrivate *priv;

	priv          = g_new0 (EBookBackendPrivate, 1);
	priv->clients = NULL;
	priv->source  = NULL;
	priv->views   = e_list_new((EListCopyFunc) NULL, (EListFreeFunc) NULL, NULL);
	priv->open_mutex = g_mutex_new ();
	priv->clients_mutex = g_mutex_new ();
	priv->views_mutex = g_mutex_new ();
        priv->opened_flag = e_flag_new ();

	backend->priv = priv;
}

static void
e_book_backend_dispose (GObject *object)
{
	EBookBackend *backend;

	backend = E_BOOK_BACKEND (object);

	if (backend->priv) {
		g_list_free (backend->priv->clients);

		if (backend->priv->views) {
			g_object_unref (backend->priv->views);
			backend->priv->views = NULL;
		}

		if (backend->priv->source) {
			g_object_unref (backend->priv->source);
			backend->priv->source = NULL;
		}

		g_mutex_free (backend->priv->open_mutex);
		g_mutex_free (backend->priv->clients_mutex);
		g_mutex_free (backend->priv->views_mutex);

                e_flag_free (backend->priv->opened_flag);

		g_free (backend->priv);
		backend->priv = NULL;
	}

	G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
e_book_backend_class_init (EBookBackendClass *klass)
{
	GObjectClass *object_class;

	parent_class = g_type_class_peek_parent (klass);

	object_class = (GObjectClass *) klass;

	object_class->dispose = e_book_backend_dispose;

	e_book_backend_signals[LAST_CLIENT_GONE] =
		g_signal_new ("last_client_gone",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_FIRST,
			      G_STRUCT_OFFSET (EBookBackendClass, last_client_gone),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__VOID,
			      G_TYPE_NONE, 0);
}

/**
 * e_book_backend_get_type:
 */
GType
e_book_backend_get_type (void)
{
	static GType type = 0;

	if (! type) {
		GTypeInfo info = {
			sizeof (EBookBackendClass),
			NULL, /* base_class_init */
			NULL, /* base_class_finalize */
			(GClassInitFunc)  e_book_backend_class_init,
			NULL, /* class_finalize */
			NULL, /* class_data */
			sizeof (EBookBackend),
			0,    /* n_preallocs */
			(GInstanceInitFunc) e_book_backend_init
		};

		type = g_type_register_static (G_TYPE_OBJECT, "EBookBackend", &info, 0);
	}

	return type;
}
