/* -*- Mode: C; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 * Copyright (C) 2006 OpenedHand Ltd
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
 * Author: Ross Burton <ross@openedhand.com>
 */

#include <config.h>
#include <string.h>
#include <dbus/dbus.h>
#include <dbus/dbus-glib-lowlevel.h>
#include <libebook/e-contact.h>
#include "e-data-book-view.h"

static gboolean impl_BookView_start (EDataBookView *view, GError **error);
static gboolean impl_BookView_stop (EDataBookView *view, GError **error);
static gboolean impl_BookView_dispose (EDataBookView *view, GError **eror);
static gboolean impl_BookView_set_freezable (EDataBookView *view, gboolean freezable, GError **error);
static gboolean impl_BookView_thaw (EDataBookView *view, GError **error);
static gboolean impl_BookView_set_sort_order (EDataBookView *view, gchar *query_term, GError **error);

#include "e-data-book-view-glue.h"

G_DEFINE_TYPE (EDataBookView, e_data_book_view, G_TYPE_OBJECT);
#define E_DATA_BOOK_VIEW_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), E_TYPE_DATA_BOOK_VIEW, EDataBookViewPrivate))

#define THRESHOLD 32

struct _EDataBookViewPrivate {
  EDataBook *book;
  EBookBackend *backend;
  DBusConnection *conn;

  char* card_query;
  EBookBackendSExp *card_sexp;
  int max_results;

  gboolean running;
  GMutex *pending_mutex;

  GPtrArray *adds;
  GPtrArray *changes;
  GPtrArray *removes;

  GHashTable *ids;
  guint idle_id;

   guint thaw_idle_id;
   GMutex *thaw_lock;
   GCond *thaw_cond;
   gboolean frozen;
   gboolean freezable;
};

enum {
  CONTACTS_ADDED,
  CONTACTS_CHANGED,
  CONTACTS_REMOVED,
  STATUS_MESSAGE,
  COMPLETE,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

static void e_data_book_view_dispose (GObject *object);
static void e_data_book_view_finalize (GObject *object);

static void e_data_book_view_thaw (EDataBookView *view);

static void
e_data_book_view_class_init (EDataBookViewClass *klass)
{ 
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = e_data_book_view_dispose;
  object_class->finalize = e_data_book_view_finalize;

  signals[CONTACTS_ADDED] =
    g_signal_new ("contacts-added",
                  G_OBJECT_CLASS_TYPE (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL,
                  g_cclosure_marshal_VOID__BOXED,
                  G_TYPE_NONE, 1, G_TYPE_STRV);

  signals[CONTACTS_CHANGED] =
    g_signal_new ("contacts-changed",
                  G_OBJECT_CLASS_TYPE (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL,
                  g_cclosure_marshal_VOID__BOXED,
                  G_TYPE_NONE, 1, G_TYPE_STRV);

  signals[CONTACTS_REMOVED] =
    g_signal_new ("contacts-removed",
                  G_OBJECT_CLASS_TYPE (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL,
                  g_cclosure_marshal_VOID__BOXED,
                  G_TYPE_NONE, 1, G_TYPE_STRV);

  signals[STATUS_MESSAGE] =
    g_signal_new ("status-message",
                  G_OBJECT_CLASS_TYPE (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL,
                  g_cclosure_marshal_VOID__STRING,
                  G_TYPE_NONE, 1, G_TYPE_STRING);

  signals[COMPLETE] =
    g_signal_new ("complete",
                  G_OBJECT_CLASS_TYPE (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL,
                  g_cclosure_marshal_VOID__UINT,
                  G_TYPE_NONE, 1, G_TYPE_UINT);

  g_type_class_add_private (klass, sizeof (EDataBookViewPrivate));

  dbus_g_object_type_install_info (G_TYPE_FROM_CLASS (klass), &dbus_glib_e_data_book_view_object_info);
}

static void
e_data_book_view_init (EDataBookView *book_view)
{
  EDataBookViewPrivate *priv = E_DATA_BOOK_VIEW_GET_PRIVATE (book_view);
  book_view->priv = priv;

  priv->running = FALSE;
  priv->pending_mutex = g_mutex_new ();

  /* THRESHOLD * 2: we store UID and vCard */
  priv->adds = g_ptr_array_sized_new (THRESHOLD * 2);
  priv->changes = g_ptr_array_sized_new (THRESHOLD * 2);
  priv->removes = g_ptr_array_sized_new (THRESHOLD);

  priv->ids = g_hash_table_new_full (g_str_hash, g_str_equal,
                                     g_free, NULL);

  priv->freezable = FALSE;
  priv->frozen = FALSE;
  priv->thaw_lock = g_mutex_new ();
  priv->thaw_cond = g_cond_new ();
}

static void
book_destroyed_cb (gpointer data, GObject *dead)
{
  EDataBookView *view = E_DATA_BOOK_VIEW (data);
  EDataBookViewPrivate *priv = view->priv;

  /* The book has just died, so unset the pointer so we don't try and remove a
     dead weak reference. */
  view->priv->book = NULL;

  /* If the view is running stop it here. */
  if (priv->running) {
    e_data_book_view_thaw (view);
    e_book_backend_stop_book_view (priv->backend, view);
    priv->running = FALSE;
  }
}

/* Oh dear god, think of the children. */
#define _DBUS_POINTER_SHIFT(p)   ((void*) (((char*)p) + sizeof (void*)))
#define DBUS_G_CONNECTION_FROM_CONNECTION(x)     ((DBusGConnection*) _DBUS_POINTER_SHIFT(x))

static void
new_connection_func (DBusServer *server, DBusConnection *conn, gpointer user_data)
{
  EDataBookView *view = user_data;

  dbus_connection_ref (conn);
  dbus_connection_setup_with_g_main (conn, NULL);

  dbus_g_connection_register_g_object
    (DBUS_G_CONNECTION_FROM_CONNECTION (conn), "/", G_OBJECT (view));

  /* This is a one-shot server */
  dbus_server_unref (server);
  dbus_server_disconnect (server);

  view->priv->conn = conn;
}

/**
 * e_data_book_view_new:
 * @book: The #EDataBook to search
 * @path: The socket path that this book view should open
 * @card_query: The query as a string
 * @card_sexp: The query as an #EBookBackendSExp
 * @max_results: The maximum number of results to return
 *
 * Create a new #EDataBookView for the given #EBook, filtering on #card_sexp,
 * and place it on DBus at the object path #path.
 */
EDataBookView *
e_data_book_view_new (EDataBook *book, const char *path, const char *card_query, EBookBackendSExp *card_sexp, int max_results)
{
  DBusError error;
  DBusServer *server;
  EDataBookView *view;
  EDataBookViewPrivate *priv;

  g_return_val_if_fail (E_IS_DATA_BOOK (book), NULL);
  g_return_val_if_fail (path, NULL);

  dbus_error_init (&error);
  server = dbus_server_listen (path, &error);
  if (!server) {
    g_warning ("Cannot create server: %s", error.message);
    dbus_error_free (&error);
    return NULL;
  }
  dbus_server_setup_with_g_main (server, NULL);
  
  view = g_object_new (E_TYPE_DATA_BOOK_VIEW, NULL);
  priv = view->priv;

  priv->book = book;
  /* Attach a weak reference to the book, so if it dies the book view is destroyed too */
  g_object_weak_ref (G_OBJECT (priv->book), book_destroyed_cb, view);
  priv->backend = g_object_ref (e_data_book_get_backend (book));
  priv->card_query = g_strdup (card_query);
  priv->card_sexp = card_sexp;
  priv->max_results = max_results;

  dbus_server_set_new_connection_function (server, new_connection_func, view, NULL);

  return view;
}

static void
e_data_book_view_dispose (GObject *object)
{
  EDataBookView *book_view = E_DATA_BOOK_VIEW (object);
  EDataBookViewPrivate *priv = book_view->priv;

  if (priv->book) {
    /* Remove the weak reference */
    g_object_weak_unref (G_OBJECT (priv->book), book_destroyed_cb, book_view);
    priv->book = NULL;
  }

  if (priv->backend) {
    e_book_backend_remove_book_view (priv->backend, book_view);
    g_object_unref (priv->backend);
    priv->backend = NULL;
  }
  
  if (priv->card_sexp) {
    g_object_unref (priv->card_sexp);
    priv->card_sexp = NULL;
  }

  if (priv->idle_id) {
    g_source_remove (priv->idle_id);
    priv->idle_id = 0;
  }

  G_OBJECT_CLASS (e_data_book_view_parent_class)->dispose (object);
}

static void reset_array (GPtrArray *array);

static void
e_data_book_view_finalize (GObject *object)
{
  EDataBookView *book_view = E_DATA_BOOK_VIEW (object);
  EDataBookViewPrivate *priv = book_view->priv;

  reset_array (priv->adds);
  reset_array (priv->changes);
  reset_array (priv->removes);
  g_ptr_array_free (priv->adds, FALSE);
  g_ptr_array_free (priv->changes, FALSE);
  g_ptr_array_free (priv->removes, FALSE);

  g_free (priv->card_query);

  g_mutex_free (priv->pending_mutex);

  g_mutex_free (priv->thaw_lock);
  g_cond_free (priv->thaw_cond);

  g_hash_table_destroy (priv->ids);

  if (priv->conn) {
    dbus_connection_close (priv->conn);
  }

  G_OBJECT_CLASS (e_data_book_view_parent_class)->finalize (object);
}

static gboolean
bookview_idle_start (gpointer data)
{
  EDataBookView *book_view = data;

  book_view->priv->running = TRUE;
  book_view->priv->idle_id = 0;

  e_book_backend_start_book_view (book_view->priv->backend, book_view);
  return FALSE;
}

static gboolean
impl_BookView_start (EDataBookView *book_view, GError **error)
{
  book_view->priv->idle_id = g_idle_add (bookview_idle_start, book_view);
  return TRUE;
}

static gboolean
bookview_idle_stop (gpointer data)
{
  EDataBookView *book_view = data;

  e_data_book_view_thaw (book_view);
  e_book_backend_stop_book_view (book_view->priv->backend, book_view);

  book_view->priv->running = FALSE;
  book_view->priv->idle_id = 0;

  return FALSE;
}

static gboolean
impl_BookView_stop (EDataBookView *book_view, GError **error)
{
  if (book_view->priv->idle_id)
    g_source_remove (book_view->priv->idle_id);
  
  book_view->priv->idle_id = g_idle_add (bookview_idle_stop, book_view);
  return TRUE;
}

static gboolean
impl_BookView_dispose (EDataBookView *book_view, GError **eror)
{
  g_object_unref (book_view);
  
  return TRUE;
}

static void
e_data_book_view_thaw (EDataBookView *view)
{
  EDataBookViewPrivate *priv = view->priv;

  g_mutex_lock (priv->thaw_lock);
  if (priv->frozen)
  {
    priv->frozen = FALSE;
    g_cond_signal (priv->thaw_cond);
  }
  g_mutex_unlock (priv->thaw_lock);
}

static gboolean
bookview_idle_thaw (gpointer data)
{
  EDataBookView *book_view = data;
  EDataBookViewPrivate *priv = book_view->priv;

  priv->thaw_idle_id = 0;
  e_data_book_view_thaw (book_view);

  return FALSE;
}

static gboolean
impl_BookView_thaw (EDataBookView *book_view, GError **error)
{
  if (book_view->priv->thaw_idle_id)
    g_source_remove (book_view->priv->thaw_idle_id);
 
  book_view->priv->thaw_idle_id = g_idle_add (bookview_idle_thaw, book_view);
  return TRUE;
}

static gboolean 
impl_BookView_set_freezable (EDataBookView *view, gboolean freezable, GError **error)
{
  EDataBookViewPrivate *priv = view->priv;

  priv->freezable = freezable;

  g_debug (G_STRLOC ": setting freezable");

  if (!freezable)
  {
    e_data_book_view_thaw (view);
  }

  return TRUE;
}

static gboolean
impl_BookView_set_sort_order (EDataBookView *view, gchar *query_term, GError **error)
{
  EDataBookViewPrivate *priv = view->priv;

  e_book_backend_set_book_view_sort_order (priv->backend, view, query_term);
  return TRUE;
}

void
e_data_book_view_set_thresholds (EDataBookView *book_view,
                                 int minimum_grouping_threshold,
                                 int maximum_grouping_threshold)
{
  g_return_if_fail (E_IS_DATA_BOOK_VIEW (book_view));
  
  g_debug ("e_data_book_view_set_thresholds does nothing in eds-dbus");
}

const char*
e_data_book_view_get_card_query (EDataBookView *book_view)
{
  g_return_val_if_fail (E_IS_DATA_BOOK_VIEW (book_view), NULL);
  return book_view->priv->card_query;
}

EBookBackendSExp*
e_data_book_view_get_card_sexp (EDataBookView *book_view)
{
  g_return_val_if_fail (E_IS_DATA_BOOK_VIEW (book_view), NULL);
  return book_view->priv->card_sexp;
}

int
e_data_book_view_get_max_results (EDataBookView *book_view)
{
  g_return_val_if_fail (E_IS_DATA_BOOK_VIEW (book_view), 0);
  return book_view->priv->max_results;
}

EBookBackend*
e_data_book_view_get_backend (EDataBookView *book_view)
{
  g_return_val_if_fail (E_IS_DATA_BOOK_VIEW (book_view), NULL);
  return book_view->priv->backend;
}

static void
reset_array (GPtrArray *array)
{
  int i;

  for (i = 0; i < array->len; ++i)
    g_free (array->pdata[i]);

  g_ptr_array_set_size (array, 0);
}

static void
send_pending_adds (EDataBookView *view)
{
  EDataBookViewPrivate *priv = view->priv;

  if (priv->adds->len == 0)
    return;

  g_ptr_array_add (priv->adds, NULL);
  g_signal_emit (view, signals[CONTACTS_ADDED], 0, priv->adds->pdata);
  reset_array (priv->adds);
}

static void
send_pending_changes (EDataBookView *view)
{
  EDataBookViewPrivate *priv = view->priv;

  if (priv->changes->len == 0)
    return;

  g_ptr_array_add (priv->changes, NULL);
  g_signal_emit (view, signals[CONTACTS_CHANGED], 0, priv->changes->pdata);
  reset_array (priv->changes);
}

static void
send_pending_removes (EDataBookView *view)
{
  EDataBookViewPrivate *priv = view->priv;

  if (priv->removes->len == 0)
    return;

  g_ptr_array_add (priv->removes, NULL);
  g_signal_emit (view, signals[CONTACTS_REMOVED], 0, priv->removes->pdata);
  reset_array (priv->removes);
}

static void
notify_change (EDataBookView *view, const char *id, char *vcard)
{
  EDataBookViewPrivate *priv = view->priv;
  send_pending_adds (view);
  send_pending_removes (view);

  g_ptr_array_add (priv->changes, vcard);
  g_ptr_array_add (priv->changes, g_strdup (id));
}

static void
notify_remove (EDataBookView *view, const char *id)
{
  EDataBookViewPrivate *priv = view->priv;

  send_pending_adds (view);
  send_pending_changes (view);

  g_ptr_array_add (priv->removes, g_strdup (id));
  g_hash_table_remove (priv->ids, id);
}

static void
notify_add (EDataBookView *view, const char *id, char *vcard)
{
  EDataBookViewPrivate *priv = view->priv;

  send_pending_changes (view);
  send_pending_removes (view);

  if (priv->adds->len == 2 * THRESHOLD) {
    if (priv->freezable)
    {
      g_mutex_lock (priv->thaw_lock);
      send_pending_adds (view);
      priv->frozen = TRUE;
      g_cond_wait (priv->thaw_cond, priv->thaw_lock);
      g_mutex_unlock (priv->thaw_lock);
    } else {
      send_pending_adds (view);
    }
  }

  g_ptr_array_add (priv->adds, vcard);
  g_ptr_array_add (priv->adds, g_strdup (id));

  g_hash_table_insert (priv->ids, g_strdup (id),
                       GUINT_TO_POINTER (1));
}

void
e_data_book_view_notify_update (EDataBookView *book_view, EContact *contact)
{
  EDataBookViewPrivate *priv = book_view->priv;
  gboolean currently_in_view, want_in_view;
  const char *id;
  char *vcard;

  if (!priv->running)
    return;

  g_mutex_lock (priv->pending_mutex);

  id = e_contact_get_const (contact, E_CONTACT_UID);

  currently_in_view =
    g_hash_table_lookup (priv->ids, id) != NULL;
  want_in_view =
    e_book_backend_sexp_match_contact (priv->card_sexp, contact);

  if (want_in_view) {
    vcard = e_vcard_to_string (E_VCARD (contact),
                               EVC_FORMAT_VCARD_30);

    if (currently_in_view)
      notify_change (book_view, id, vcard);
    else
      notify_add (book_view, id, vcard);
  } else {
    if (currently_in_view)
      notify_remove (book_view, id);
    /* else nothing; we're removing a card that wasn't there */
  }

  g_mutex_unlock (priv->pending_mutex);
}

void
e_data_book_view_notify_update_vcard (EDataBookView *book_view, char *vcard)
{
  EDataBookViewPrivate *priv = book_view->priv;
  gboolean currently_in_view, want_in_view;
  const char *id;
  EContact *contact;

  if (!priv->running)
    return;

  g_mutex_lock (priv->pending_mutex);

  contact = e_contact_new_from_vcard (vcard);
  id = e_contact_get_const (contact, E_CONTACT_UID);
  currently_in_view =
    g_hash_table_lookup (priv->ids, id) != NULL;
  want_in_view =
    e_book_backend_sexp_match_contact (priv->card_sexp, contact);

  if (want_in_view) {
    if (currently_in_view)
      notify_change (book_view, id, vcard);
    else
      notify_add (book_view, id, vcard);
  } else {
    if (currently_in_view)
      notify_remove (book_view, id);
    else
      /* else nothing; we're removing a card that wasn't there */
      g_free (vcard);
  }
  /* Do this last so that id is still valid when notify_ is called */
  g_object_unref (contact);

  g_mutex_unlock (priv->pending_mutex);
}

void
e_data_book_view_notify_update_prefiltered_vcard (EDataBookView *book_view, const char *id, char *vcard)
{
  EDataBookViewPrivate *priv = book_view->priv;
  gboolean currently_in_view;

  if (!priv->running)
    return;

  g_mutex_lock (priv->pending_mutex);

  currently_in_view =
    g_hash_table_lookup (priv->ids, id) != NULL;

  if (currently_in_view)
    notify_change (book_view, id, vcard);
  else
    notify_add (book_view, id, vcard);

  g_mutex_unlock (priv->pending_mutex);
}

void
e_data_book_view_notify_remove (EDataBookView *book_view, const char *id)
{
  EDataBookViewPrivate *priv = E_DATA_BOOK_VIEW_GET_PRIVATE (book_view);

  if (!priv->running)
    return;

  g_mutex_lock (priv->pending_mutex);

  if (g_hash_table_lookup (priv->ids, id))
    notify_remove (book_view, id);
  
  g_mutex_unlock (priv->pending_mutex);
}

void
e_data_book_view_notify_complete (EDataBookView *book_view, EDataBookStatus status)
{
  EDataBookViewPrivate *priv = book_view->priv;

  if (!priv->running)
    return;

  g_mutex_lock (priv->pending_mutex);

  send_pending_adds (book_view);
  send_pending_changes (book_view);
  send_pending_removes (book_view);

  g_mutex_unlock (priv->pending_mutex);
  
  /* We're done now, so tell the backend to stop?  TODO: this is a bit different to
     how the CORBA backend works... */

  g_signal_emit (book_view, signals[COMPLETE], 0, status);
}

void
e_data_book_view_notify_status_message (EDataBookView *book_view, const char *message)
{
  EDataBookViewPrivate *priv = E_DATA_BOOK_VIEW_GET_PRIVATE (book_view);

  if (!priv->running)
    return;

  g_signal_emit (book_view, signals[STATUS_MESSAGE], 0, message);
}

