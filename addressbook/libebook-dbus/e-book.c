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
#include <unistd.h>
#include <string.h>
#include <glib-object.h>
#include <glib/gi18n-lib.h>
#include <dbus/dbus-glib.h>
#include "e-book.h"
#include "e-error.h"
#include "e-contact.h"
#include "e-book-view-private.h"
#include "e-data-book-factory-bindings.h"
#include "e-data-book-bindings.h"
#include "libedata-book/e-data-book-types.h"
#include "e-book-marshal.h"

#define DBUS_API_SUBJECT_TO_CHANGE
#include <dbus/dbus-glib-lowlevel.h>

#define E_DATA_BOOK_FACTORY_SERVICE_NAME "org.gnome.evolution.dataserver.AddressBook"

static char** flatten_stringlist(GList *list);
static GList *array_to_stringlist (char **list);
static gboolean unwrap_gerror(GError *error, GError **client_error);
static EBookStatus get_status_from_error (GError *error);

G_DEFINE_TYPE(EBook, e_book, G_TYPE_OBJECT)
#define E_BOOK_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), E_TYPE_BOOK, EBookPrivate))

enum {
	WRITABLE_STATUS,
	CONNECTION_STATUS,
	AUTH_REQUIRED,
	BACKEND_DIED,
	LAST_SIGNAL
};

static guint e_book_signals [LAST_SIGNAL];

struct _EBookPrivate {
  ESource *source;
  char *uri;
  DBusGProxy *proxy;
  gboolean loaded;
  gboolean writable;
  gboolean connected;
  char *cap;
  gboolean cap_queried;
};

static DBusGConnection *connection = NULL;
static DBusGProxy *factory_proxy = NULL;

typedef struct {
  EBook *book;
  EBookQuery *query;
  void *callback; /* TODO union */
  gpointer closure;
} AsyncData;

/* helper functions for allocating and freeing AsyncData structs that
 * automatically ref and unref the book and query attributes */
static AsyncData *
async_data_new (EBook *book, EBookQuery *query, void* callback,
                gpointer closure)
{
    AsyncData* data = g_slice_new0 (AsyncData);
    if (book)
        data->book = g_object_ref (book);
    if (query)
        data->query = e_book_query_ref (query);
    data->callback = callback;
    data->closure = closure;
    return data;
}

static void
async_data_free (AsyncData *data)
{
    if (data->book)
        g_object_unref (data->book);
    if (data->query)
        e_book_query_unref (data->query);
    g_slice_free (AsyncData, data);
}

GQuark
e_book_error_quark (void)
{
  static GQuark q = 0;
  if (q == 0)
    q = g_quark_from_static_string ("e-book-error-quark");
  return q;
}

/*
 * Called when the addressbook server dies.
 */
static void
proxy_destroyed (gpointer data, GObject *object)
{
  EBook *book = data;

  g_assert (E_IS_BOOK (book));

  g_warning (G_STRLOC ": e-d-s proxy died");

  /* Ensure that everything relevant is NULL */
  factory_proxy = NULL;
  book->priv->proxy = NULL;

  g_signal_emit (G_OBJECT (book), e_book_signals [BACKEND_DIED], 0);
}

static void
e_book_dispose (GObject *object)
{
  EBook *book = E_BOOK (object);

  book->priv->loaded = FALSE;

  if (book->priv->proxy) {
    g_object_weak_unref (G_OBJECT (book->priv->proxy), proxy_destroyed, book);
    org_gnome_evolution_dataserver_addressbook_Book_close (book->priv->proxy, NULL);
  }
  if (book->priv->source) {
    g_object_unref (book->priv->source);
    book->priv->source = NULL;
  }
  if (book->priv->proxy) {
    g_object_unref (book->priv->proxy);
    book->priv->proxy = NULL;
  }

  if (G_OBJECT_CLASS (e_book_parent_class)->dispose)
    G_OBJECT_CLASS (e_book_parent_class)->dispose (object);
}

static void
e_book_finalize (GObject *object)
{
  EBook *book = E_BOOK (object);

  if (book->priv->uri)
    g_free (book->priv->uri);

  if (book->priv->cap)
    g_free (book->priv->cap);

  if (G_OBJECT_CLASS (e_book_parent_class)->finalize)
    G_OBJECT_CLASS (e_book_parent_class)->finalize (object);
}

static void
e_book_class_init (EBookClass *e_book_class)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (e_book_class);

  e_book_signals [WRITABLE_STATUS] =
		g_signal_new ("writable_status",
			      G_OBJECT_CLASS_TYPE (gobject_class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (EBookClass, writable_status),
			      NULL, NULL,
			      e_book_marshal_NONE__BOOL,
			      G_TYPE_NONE, 1,
			      G_TYPE_BOOLEAN);

  e_book_signals [CONNECTION_STATUS] =
    g_signal_new ("connection_status",
                  G_OBJECT_CLASS_TYPE (gobject_class),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (EBookClass, connection_status),
                  NULL, NULL,
                  e_book_marshal_NONE__BOOL,
                  G_TYPE_NONE, 1,
                  G_TYPE_BOOLEAN);

  e_book_signals [AUTH_REQUIRED] =
    g_signal_new ("auth_required",
                  G_OBJECT_CLASS_TYPE (gobject_class),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (EBookClass, auth_required),
                  NULL, NULL,
                  e_book_marshal_NONE__NONE,
                  G_TYPE_NONE, 0);

  e_book_signals [BACKEND_DIED] =
    g_signal_new ("backend_died",
                  G_OBJECT_CLASS_TYPE (gobject_class),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (EBookClass, backend_died),
                  NULL, NULL,
                  e_book_marshal_NONE__NONE,
                  G_TYPE_NONE, 0);

  gobject_class->dispose = e_book_dispose;
  gobject_class->finalize = e_book_finalize;

  g_type_class_add_private (e_book_class, sizeof (EBookPrivate));
}

static void
e_book_init (EBook *book)
{
  EBookPrivate *priv = E_BOOK_GET_PRIVATE (book);
  priv->source = NULL;
  priv->uri = NULL;
  priv->proxy = NULL;
  priv->loaded = FALSE;
  priv->writable = FALSE;
  priv->connected = FALSE;
  priv->cap = NULL;
  priv->cap_queried = FALSE;
  book->priv = priv;
}

/* one-time start up for libebook */
static gboolean
e_book_activate(GError **error)
{
  DBusError derror;

  if (G_LIKELY (factory_proxy)) {
    return TRUE;
  }

  if (!connection) {
    connection = dbus_g_bus_get (DBUS_BUS_SESSION, error);
    if (!connection)
      return FALSE;
  }

  dbus_error_init (&derror);
  if (!dbus_bus_start_service_by_name (dbus_g_connection_get_connection (connection),
                                       E_DATA_BOOK_FACTORY_SERVICE_NAME,
                                       0, NULL, &derror)) {
    dbus_set_g_error (error, &derror);
    dbus_error_free (&derror);
    return FALSE;
  }

  if (!factory_proxy) {
    factory_proxy = dbus_g_proxy_new_for_name_owner (connection,
                                                     E_DATA_BOOK_FACTORY_SERVICE_NAME,
                                                     "/org/gnome/evolution/dataserver/addressbook/BookFactory",
                                                     "org.gnome.evolution.dataserver.addressbook.BookFactory",
                                                     error);
    if (!factory_proxy) {
      return FALSE;
    }
    g_object_add_weak_pointer (G_OBJECT (factory_proxy), (gpointer)&factory_proxy);
  }

  return TRUE;
}

static void
writable_cb (DBusGProxy *proxy, gboolean writable, EBook *book)
{
  g_return_if_fail (E_IS_BOOK (book));

  book->priv->writable = writable;

  g_signal_emit (G_OBJECT (book), e_book_signals [WRITABLE_STATUS], 0, writable);
}

static void
connection_cb (DBusGProxy *proxy, gboolean connected, EBook *book)
{
  g_return_if_fail (E_IS_BOOK (book));

  book->priv->connected = connected;

  g_signal_emit (G_OBJECT (book), e_book_signals [CONNECTION_STATUS], 0, connected);
}

static void
auth_required_cb (DBusGProxy *proxy, EBook *book)
{
  g_return_if_fail (E_IS_BOOK (book));

  g_signal_emit (G_OBJECT (book), e_book_signals [AUTH_REQUIRED], 0);
}

/**
 * e_book_get_addressbooks:
 * @addressbook_sources: A pointer to a ESourceList* to set
 * @error: A pointer to a GError* to set on error
 *
 * Populate *addressbook_sources with the list of all sources which have been
 * added to Evolution.
 *
 * Return value: %TRUE if @addressbook_sources was set, otherwise %FALSE.
 */
gboolean
e_book_get_addressbooks (ESourceList **addressbook_sources, GError **error)
{
	GConfClient *gconf;

	e_return_error_if_fail (addressbook_sources, E_BOOK_ERROR_INVALID_ARG);

	gconf = gconf_client_get_default();
	*addressbook_sources = e_source_list_new_for_gconf (gconf, "/apps/evolution/addressbook/sources");
	g_object_unref (gconf);

	return TRUE;
}

/**
 * e_book_new:
 * @source: An #ESource pointer
 * @error: A #GError pointer
 *
 * Creates a new #EBook corresponding to the given source.  There are
 * only two operations that are valid on this book at this point:
 * e_book_open(), and e_book_remove().
 *
 * Return value: a new but unopened #EBook.
 */
EBook*
e_book_new (ESource *source, GError **error)
{
  GError *err = NULL;
  EBook *book;
  char *path;

  e_return_error_if_fail (E_IS_SOURCE (source), E_BOOK_ERROR_INVALID_ARG);

  if (!e_book_activate (&err)) {
    g_warning (G_STRLOC ": cannot activate book: %s\n", err->message);
    g_propagate_error (error, err);
    return NULL;
  }

  book = g_object_new (E_TYPE_BOOK, NULL);

  book->priv->source = g_object_ref (source);
  book->priv->uri = e_source_get_uri (source);

  if (!org_gnome_evolution_dataserver_addressbook_BookFactory_get_book (factory_proxy, book->priv->uri, &path, &err)) {
    g_warning (G_STRLOC ": cannot get book from factory: %s", err ? err->message : "[no error]");
    g_propagate_error (error, err);
    g_object_unref (book);
    return NULL;
  }

  book->priv->proxy = dbus_g_proxy_new_for_name_owner (connection,
                                                       E_DATA_BOOK_FACTORY_SERVICE_NAME, path,
                                                       "org.gnome.evolution.dataserver.addressbook.Book",
                                                       &err);
  if (!book->priv->proxy) {
    g_warning (G_STRLOC ": cannot get proxy for book %s: %s", path, err->message);
    g_propagate_error (error, err);
    g_free (path);
    g_object_unref (book);
    return NULL;
  }
  g_free (path);

  g_object_weak_ref (G_OBJECT (book->priv->proxy), proxy_destroyed, book);

  dbus_g_proxy_add_signal (book->priv->proxy, "writable", G_TYPE_BOOLEAN, G_TYPE_INVALID);
  dbus_g_proxy_connect_signal (book->priv->proxy, "writable", G_CALLBACK (writable_cb), book, NULL);
  dbus_g_proxy_add_signal (book->priv->proxy, "connection", G_TYPE_BOOLEAN, G_TYPE_INVALID);
  dbus_g_proxy_connect_signal (book->priv->proxy, "connection", G_CALLBACK (connection_cb), book, NULL);
  dbus_g_proxy_add_signal (book->priv->proxy, "auth_required", G_TYPE_INVALID);
  dbus_g_proxy_connect_signal (book->priv->proxy, "auth_required", G_CALLBACK (auth_required_cb), book, NULL);

  return book;
}

/**
 * e_book_new_from_uri:
 * @uri: the URI to load
 * @error: A #GError pointer
 *
 * Creates a new #EBook corresponding to the given uri.  See the
 * documentation for e_book_new for further information.
 *
 * Return value: a new but unopened #EBook.
 */
EBook*
e_book_new_from_uri (const char *uri, GError **error)
{
  ESource *source;
  EBook *book;

  e_return_error_if_fail (uri, E_BOOK_ERROR_INVALID_ARG);

  source = e_source_new_with_absolute_uri ("", uri);

  book = e_book_new (source, error);

  g_object_unref (source);

  return book;
}

/**
 * e_book_new_system_addressbook:
 * @uri: the URI to load
 * @error: A #GError pointer
 *
 * Creates a new #EBook corresponding to the user's system
 * addressbook.  See the documentation for e_book_new for further
 * information.
 *
 * Return value: a new but unopened #EBook.
 */
EBook*
e_book_new_system_addressbook (GError **error)
{
	ESourceList *sources;
	GSList *g;
	ESource *system_source = NULL;
	EBook *book = NULL;

	if (!e_book_get_addressbooks (&sources, error)) {
		return FALSE;
	}

	for (g = e_source_list_peek_groups (sources); g; g = g->next) {
		ESourceGroup *group = E_SOURCE_GROUP (g->data);
		GSList *s;
		for (s = e_source_group_peek_sources (group); s; s = s->next) {
			ESource *source = E_SOURCE (s->data);

			if (e_source_get_property (source, "system")) {
				system_source = source;
				break;
			}
		}

		if (system_source)
			break;
	}

	if (system_source) {
		book = e_book_new (system_source, error);
	}
	else {
		char *filename;
		char *uri;

		filename = g_build_filename (g_get_home_dir(),
					     ".osso-abook",
					     "db",
					     NULL);
		uri = g_strdup_printf ("file://%s", filename);

		g_free (filename);

		book = e_book_new_from_uri (uri, error);

		g_free (uri);
	}

	g_object_unref (sources);

	return book;
}

/**
 * e_book_new_default_addressbook:
 * @uri: the URI to load
 * @error: A #GError pointer
 *
 * Creates a new #EBook corresponding to the user's default
 * addressbook.  See the documentation for e_book_new for further
 * information.
 *
 * Return value: a new but unopened #EBook.
 */
EBook*
e_book_new_default_addressbook   (GError **error)
{
	ESourceList *sources;
	GSList *g;
	ESource *default_source = NULL;
	EBook *book;

	if (!e_book_get_addressbooks (&sources, error)) {
		return FALSE;
	}

	for (g = e_source_list_peek_groups (sources); g; g = g->next) {
		ESourceGroup *group = E_SOURCE_GROUP (g->data);
		GSList *s;
		for (s = e_source_group_peek_sources (group); s; s = s->next) {
			ESource *source = E_SOURCE (s->data);

			if (e_source_get_property (source, "default")) {
				default_source = source;
				break;
			}
		}

		if (default_source)
			break;
	}

	if (default_source)
		book = e_book_new (default_source, error);
	else
		book = e_book_new_system_addressbook (error);

	g_object_unref (sources);

	return book;
}

/**
 * e_book_get_source:
 * @book: an #EBook
 *
 * Get the #ESource that this book has loaded.
 *
 * Return value: The source.
 */
ESource*
e_book_get_source (EBook *book)
{
  g_return_val_if_fail (E_IS_BOOK (book), NULL);

  return book->priv->source;
}

/**
 * e_book_open:
 * @book: an #EBook
 * @only_if_exists: if %TRUE, fail if this book doesn't already exist, otherwise create it first
 * @error: a #GError to set on failure
 *
 * Opens the addressbook, making it ready for queries and other operations.
 *
 * Return value: %TRUE if the book was successfully opened, %FALSE otherwise.
 */
gboolean
e_book_open (EBook *book, gboolean only_if_exists, GError **error)
{
  GError *err = NULL;
  EBookStatus status;

  e_return_error_if_fail (E_IS_BOOK (book), E_BOOK_ERROR_INVALID_ARG);
  e_return_error_if_fail (book->priv->proxy, E_BOOK_ERROR_REPOSITORY_OFFLINE);

  if (!org_gnome_evolution_dataserver_addressbook_Book_open (book->priv->proxy, only_if_exists, &err)) {
    g_propagate_error (error, err);
    return FALSE;
  }

  status = get_status_from_error (err);

  if (status == E_BOOK_ERROR_OK) {
    book->priv->loaded = TRUE;
    return TRUE;
  } else {
    g_propagate_error (error, err);
    return FALSE;
  }
}

static void
open_reply(DBusGProxy *proxy, GError *error, gpointer user_data)
{
  AsyncData *data = user_data;
  EBookCallback cb = data->callback;
  EDataBookStatus status;

  status = get_status_from_error (error);
  if (error)
    g_error_free (error);

  data->book->priv->loaded = (status == E_BOOK_ERROR_OK);

  if (cb)
    cb (data->book, status, data->closure);
  async_data_free (data);
}

/**
 * e_book_async_open:
 * @book: an #EBook
 * @only_if_exists: if %TRUE, fail if this book doesn't already exist, otherwise create it first
 * @open_response: a function to call when the operation finishes
 * @closure: data to pass to callback function
 *
 * Opens the addressbook, making it ready for queries and other operations.
 * This function does not block.
 *
 * Return value: %FALSE if successful, %TRUE otherwise.
 **/
guint
e_book_async_open (EBook *book, gboolean only_if_exists, EBookCallback cb, gpointer closure)
{
  AsyncData *data;

  e_return_async_error_if_fail (E_IS_BOOK (book), E_BOOK_ERROR_INVALID_ARG);
  e_return_async_error_if_fail (book->priv->proxy, E_BOOK_ERROR_REPOSITORY_OFFLINE);

  data = async_data_new (book, NULL, cb, closure);

  if (!org_gnome_evolution_dataserver_addressbook_Book_open_async (book->priv->proxy, only_if_exists, open_reply, data))
    cb (book, E_BOOK_ERROR_CORBA_EXCEPTION, closure);

  return 0;
}

/**
 * e_book_remove:
 * @book: an #EBook
 * @error: a #GError to set on failure
 *
 * Removes the backing data for this #EBook. For example, with the file backend this
 * deletes the database file. You cannot get it back!
 *
 * Return value: %TRUE on success, %FALSE on failure.
 */
gboolean
e_book_remove (EBook *book, GError **error)
{
  GError *err = NULL;

  e_return_error_if_fail (E_IS_BOOK (book), E_BOOK_ERROR_INVALID_ARG);
  e_return_error_if_fail (book->priv->proxy, E_BOOK_ERROR_REPOSITORY_OFFLINE);

  org_gnome_evolution_dataserver_addressbook_Book_remove (book->priv->proxy, &err);
  return unwrap_gerror (err, error);
}

static void
remove_reply(DBusGProxy *proxy, GError *error, gpointer user_data)
{
  AsyncData *data = user_data;
  EBookCallback cb = data->callback;
  if (cb)
    cb (data->book, get_status_from_error (error), data->closure);
  if (error)
    g_error_free (error);
  async_data_free (data);
}

/**
 * e_book_async_remove:
 * @book: an #EBook
 * @cb: a function to call when the operation finishes
 * @closure: data to pass to callback function
 *
 * Remove the backing data for this #EBook. For example, with the file backend this
 * deletes the database file. You cannot get it back!
 *
 * Return value: %FALSE if successful, %TRUE otherwise.
 **/
guint
e_book_async_remove (EBook *book, EBookCallback cb, gpointer closure)
{
  AsyncData *data;

  e_return_async_error_if_fail (E_IS_BOOK (book), E_BOOK_ERROR_INVALID_ARG);
  e_return_async_error_if_fail (book->priv->proxy, E_BOOK_ERROR_REPOSITORY_OFFLINE);

  data = async_data_new (book, NULL, cb, closure);

  if (!org_gnome_evolution_dataserver_addressbook_Book_remove_async (book->priv->proxy, remove_reply, data))
    cb (book, E_BOOK_ERROR_CORBA_EXCEPTION, closure);

  return 0;
}

/**
 * e_book_get_required_fields:
 * @book: an #EBook
 * @fields: a #GList of fields to set on success
 * @error: a #GError to set on failure
 *
 * Gets a list of fields that are required to be filled in for
 * all contacts in this @book. The list will contain pointers
 * to allocated strings, and both the #GList and the strings
 * must be freed by the caller.
 *
 * Return value: %TRUE if successful, %FALSE otherwise.
 **/
gboolean
e_book_get_required_fields (EBook *book, GList **fields, GError **error)
{
  GError *err = NULL;
  char **list = NULL;

  e_return_error_if_fail (E_IS_BOOK (book), E_BOOK_ERROR_INVALID_ARG);
  e_return_error_if_fail (book->priv->proxy, E_BOOK_ERROR_REPOSITORY_OFFLINE);

  org_gnome_evolution_dataserver_addressbook_Book_get_required_fields (book->priv->proxy, &list, &err);
  if (list) {
    *fields = array_to_stringlist (list);
    return TRUE;
  } else {
    return unwrap_gerror (err, error);
  }
}

static void
get_required_fields_reply(DBusGProxy *proxy, char **fields, GError *error, gpointer user_data)
{
  AsyncData *data = user_data;
  EBookEListCallback cb = data->callback;
  char **i;
  EList *efields = e_list_new (NULL,
                               (EListFreeFunc) g_free,
                               NULL);

  if (error)
    fields = NULL;

  i = fields;
  while (*i != NULL) {
    e_list_append (efields, (*i++));
  }

  if (cb)
    cb (data->book, get_status_from_error (error), efields, data->closure);

  if (error)
    g_error_free (error);

  g_object_unref (efields);
  g_free (fields);

  async_data_free (data);
}

/**
 * e_book_async_get_required_fields:
 * @book: an #EBook
 * @cb: function to call when the operation finishes
 * @closure: data to pass to callback function
 *
 * Gets a list of fields that are required to be filled in for
 * all contacts in this @book. This function does not block.
 *
 * Return value: %TRUE if the operation was started, %FALSE otherwise.
 **/
guint
e_book_async_get_required_fields (EBook *book, EBookEListCallback cb, gpointer closure)
{
  AsyncData *data;

  e_return_async_error_val_if_fail (E_IS_BOOK (book), E_BOOK_ERROR_INVALID_ARG);
  e_return_async_error_val_if_fail (book->priv->proxy, E_BOOK_ERROR_REPOSITORY_OFFLINE);

  data = async_data_new (book, NULL, cb, closure);

  if (!org_gnome_evolution_dataserver_addressbook_Book_get_required_fields_async (book->priv->proxy, get_required_fields_reply, data))
    cb (book, E_BOOK_ERROR_CORBA_EXCEPTION, NULL, closure);

  return 0;
}

/**
 * e_book_get_supported_fields:
 * @book: an #EBook
 * @fields: a #GList of fields to set on success
 * @error: a #GError to set on failure
 *
 * Gets a list of fields that can be stored for contacts
 * in this @book. Other fields may be discarded. The list
 * will contain pointers to allocated strings, and both the
 * #GList and the strings must be freed by the caller.
 *
 * Return value: %TRUE if successful, %FALSE otherwise
 **/
gboolean
e_book_get_supported_fields (EBook *book, GList **fields, GError **error)
{
  GError *err = NULL;
  char **list = NULL;

  e_return_error_if_fail (E_IS_BOOK (book), E_BOOK_ERROR_INVALID_ARG);
  e_return_error_if_fail (book->priv->proxy, E_BOOK_ERROR_REPOSITORY_OFFLINE);

  org_gnome_evolution_dataserver_addressbook_Book_get_supported_fields (book->priv->proxy, &list, &err);
  if (list) {
    *fields = array_to_stringlist (list);
    return TRUE;
  } else {
    return unwrap_gerror (err, error);
  }
}

/* FIXME: this function is identical to get_required_fields_reply */
static void
get_supported_fields_reply(DBusGProxy *proxy, char **fields, GError *error, gpointer user_data)
{
  AsyncData *data = user_data;
  EBookEListCallback cb = data->callback;
  char **i;
  EList *efields = e_list_new (NULL,  (EListFreeFunc) g_free, NULL);

  if (error)
    fields = NULL;

  i = fields;
  while (*i != NULL) {
    e_list_append (efields, (*i++));
  }

  if (cb)
    cb (data->book, get_status_from_error (error), efields, data->closure);

  if (error)
    g_error_free (error);

  g_object_unref (efields);
  g_free (fields);

  async_data_free (data);
}

/**
 * e_book_async_get_supported_fields:
 * @book: an #EBook
 * @cb: function to call when the operation finishes
 * @closure: data to pass to callback function
 *
 * Gets a list of fields that can be stored for contacts
 * in this @book. Other fields may be discarded. This
 * function does not block.
 *
 * Return value: %TRUE if successful, %FALSE otherwise.
 **/
guint
e_book_async_get_supported_fields (EBook *book, EBookEListCallback cb, gpointer closure)
{
  AsyncData *data;

  e_return_async_error_val_if_fail (E_IS_BOOK (book), E_BOOK_ERROR_INVALID_ARG);
  e_return_async_error_val_if_fail (book->priv->proxy, E_BOOK_ERROR_REPOSITORY_OFFLINE);

  data = async_data_new (book, NULL, cb, closure);

  if (!org_gnome_evolution_dataserver_addressbook_Book_get_supported_fields_async (book->priv->proxy, get_supported_fields_reply, data))
    cb (book, E_BOOK_ERROR_CORBA_EXCEPTION, NULL, closure);

  return 0;
}

/**
 * e_book_get_supported_auth_methods:
 * @book: an #EBook
 * @auth_methods: a #GList of auth methods to set on success
 * @error: a #GError to set on failure
 *
 * Queries @book for the list of authentication methods it supports.
 * The list will contain pointers to allocated strings, and both the
 * #GList and the strings must be freed by the caller.
 *
 * Return value: %TRUE if successful, %FALSE otherwise
 **/
gboolean
e_book_get_supported_auth_methods (EBook *book, GList **auth_methods, GError **error)
{
  GError *err = NULL;
  char **list = NULL;

  e_return_error_if_fail (E_IS_BOOK (book), E_BOOK_ERROR_INVALID_ARG);
  e_return_error_if_fail (book->priv->proxy, E_BOOK_ERROR_REPOSITORY_OFFLINE);

  org_gnome_evolution_dataserver_addressbook_Book_get_supported_auth_methods (book->priv->proxy, &list, &err);
  if (list) {
    *auth_methods = array_to_stringlist (list);
    return TRUE;
  } else {
    return unwrap_gerror (err, error);
  }
}

/* FIXME: this function is also identical to get_required_fields_reply */
static void
get_supported_auth_methods_reply(DBusGProxy *proxy, char **methods, GError *error, gpointer user_data)
{
  AsyncData *data = user_data;
  EBookEListCallback cb = data->callback;
  char **i;
  EList *emethods = e_list_new (NULL,
                                (EListFreeFunc) g_free,
                                NULL);

  if (error)
    methods = NULL;

  i = methods;
  while (*i != NULL) {
    e_list_append (emethods, (*i++));
  }

  if (cb)
    cb (data->book, get_status_from_error (error), emethods, data->closure);

  if (error)
    g_error_free (error);

  g_object_unref (emethods);
  g_free (methods);

  async_data_free (data);
}

/**
 * e_book_async_get_supported_auth_methods:
 * @book: an #EBook
 * @cb: function to call when the operation finishes
 * @closure: data to pass to callback function
 *
 * Queries @book for the list of authentication methods it supports.
 * This function does not block.
 *
 * Return value: %TRUE if successful, %FALSE otherwise.
 **/guint
e_book_async_get_supported_auth_methods (EBook *book, EBookEListCallback cb, gpointer closure)
{
  AsyncData *data;

  e_return_async_error_val_if_fail (E_IS_BOOK (book), E_BOOK_ERROR_INVALID_ARG);
  e_return_async_error_val_if_fail (book->priv->proxy, E_BOOK_ERROR_REPOSITORY_OFFLINE);

  data = async_data_new (book, NULL, cb, closure);

  if (!org_gnome_evolution_dataserver_addressbook_Book_get_supported_auth_methods_async (book->priv->proxy, get_supported_auth_methods_reply, data))
    cb (book, E_BOOK_ERROR_CORBA_EXCEPTION, NULL, closure);

  return 0;
}

/**
 * e_book_authenticate_user:
 * @book: an #EBook
 * @user: a string
 * @passwd: a string
 * @auth_method: a string
 * @error: a #GError to set on failure
 *
 * Authenticates @user with @passwd, using the auth method
 * @auth_method.  @auth_method must be one of the authentication
 * methods returned using e_book_get_supported_auth_methods.
 *
 * Return value: %TRUE if successful, %FALSE otherwise
 **/
gboolean
e_book_authenticate_user (EBook *book, const char *user, const char *passwd, const char *auth_method, GError **error)
{
  GError *err = NULL;

  e_return_error_if_fail (E_IS_BOOK (book), E_BOOK_ERROR_INVALID_ARG);
  e_return_error_if_fail (book->priv->proxy, E_BOOK_ERROR_REPOSITORY_OFFLINE);

  org_gnome_evolution_dataserver_addressbook_Book_authenticate_user (book->priv->proxy, user, passwd, auth_method, &err);
  return unwrap_gerror (err, error);
}

static void
authenticate_user_reply(DBusGProxy *proxy, GError *error, gpointer user_data)
{
  AsyncData *data = user_data;
  EBookCallback cb = data->callback;

  if (cb)
    cb (data->book, get_status_from_error (error), data->closure);

  if (error)
    g_error_free (error);

  async_data_free (data);
}

/**
 * e_book_async_authenticate_user:
 * @book: an #EBook
 * @user: user name
 * @passwd: password
 * @auth_method: string indicating authentication method
 * @cb: function to call when the operation finishes
 * @closure: data to pass to callback function
 *
 * Authenticate @user with @passwd, using the auth method
 * @auth_method. @auth_method must be one of the authentication
 * methods returned using e_book_get_supported_auth_methods.
 * This function does not block.
 *
 * Return value: %FALSE if successful, %TRUE otherwise.
 **/
guint
e_book_async_authenticate_user (EBook *book, const char *user, const char *passwd, const char *auth_method, EBookCallback cb, gpointer closure)
{
  AsyncData *data;

  e_return_async_error_if_fail (E_IS_BOOK (book), E_BOOK_ERROR_INVALID_ARG);
  e_return_async_error_if_fail (book->priv->proxy, E_BOOK_ERROR_REPOSITORY_OFFLINE);
  e_return_async_error_if_fail (user, E_BOOK_ERROR_INVALID_ARG);
  e_return_async_error_if_fail (passwd, E_BOOK_ERROR_INVALID_ARG);
  e_return_async_error_if_fail (auth_method, E_BOOK_ERROR_INVALID_ARG);

  data = async_data_new (book, NULL, cb, closure);

  if (!org_gnome_evolution_dataserver_addressbook_Book_authenticate_user_async (book->priv->proxy, user, passwd, auth_method, authenticate_user_reply, data))
    cb (book, E_BOOK_ERROR_CORBA_EXCEPTION, closure);

  return 0;
}

/**
 * e_book_get_contact:
 * @book: an #EBook
 * @id: a unique string ID specifying the contact
 * @contact: an #EContact
 * @error: a #GError to set on failure
 *
 * Fills in @contact with the contents of the vcard in @book
 * corresponding to @id.
 *
 * Return value: %TRUE if successful, %FALSE otherwise
 **/
gboolean
e_book_get_contact (EBook *book, const char  *id, EContact **contact, GError **error)
{
  GError *err = NULL;
  char *vcard = NULL;

  e_return_error_if_fail (E_IS_BOOK (book), E_BOOK_ERROR_INVALID_ARG);
  e_return_error_if_fail (book->priv->proxy, E_BOOK_ERROR_REPOSITORY_OFFLINE);
  e_return_error_if_fail (book->priv->loaded, E_BOOK_ERROR_SOURCE_NOT_LOADED);
  e_return_error_if_fail (id && g_utf8_validate (id, -1, NULL), E_BOOK_ERROR_INVALID_ARG);

  org_gnome_evolution_dataserver_addressbook_Book_get_contact (book->priv->proxy, id, &vcard, &err);
  if (vcard) {
    *contact = e_contact_new_from_vcard (vcard);
    g_free (vcard);
  }
  return unwrap_gerror (err, error);
}

static void
get_contact_reply(DBusGProxy *proxy, char *vcard, GError *error, gpointer user_data)
{
  AsyncData *data = user_data;
  EBookContactCallback cb = data->callback;
  EBookStatus status = get_status_from_error (error);

  /* Protect against garbage return values on error */
  if (error) {
    vcard = NULL;
    g_warning (G_STRLOC ": cannot get contact: %s", error->message);
  }

  if (cb) {
    if (error == NULL) {
      cb (data->book, status, e_contact_new_from_vcard (vcard), data->closure);
    } else {
      cb (data->book, status, NULL, data->closure);
    }
  } else {
    g_warning (G_STRLOC ": No callback provided for get_contact()");
  }

  if (error)
    g_error_free (error);
  g_free (vcard);

  async_data_free (data);
}

/**
 * e_book_async_get_contact:
 * @book: an #EBook
 * @id: a unique string ID specifying the contact
 * @cb: function to call when operation finishes
 * @closure: data to pass to callback function
 *
 * Retrieves a contact specified by @id from @book.
 *
 * Return value: %FALSE if successful, %TRUE otherwise
 **/
guint
e_book_async_get_contact (EBook *book, const char *id, EBookContactCallback cb, gpointer closure)
{
  AsyncData *data;

  e_return_async_error_val_if_fail (E_IS_BOOK (book), E_BOOK_ERROR_INVALID_ARG);
  e_return_async_error_val_if_fail (book->priv->proxy, E_BOOK_ERROR_REPOSITORY_OFFLINE);
  e_return_async_error_val_if_fail (book->priv->loaded, E_BOOK_ERROR_SOURCE_NOT_LOADED);
  e_return_async_error_val_if_fail (id && g_utf8_validate (id, -1, NULL), E_BOOK_ERROR_INVALID_ARG);

  data = async_data_new (book, NULL, cb, closure);

  if (!org_gnome_evolution_dataserver_addressbook_Book_get_contact_async (book->priv->proxy, id, get_contact_reply, data))
    cb (book, E_BOOK_ERROR_CORBA_EXCEPTION, NULL, closure);

  return 0;
}

/**
 * e_book_get_contacts:
 * @book: an #EBook
 * @query: an #EBookQuery
 * @contacts: a #GList pointer, will be set to the list of contacts
 * @error: a #GError to set on failure
 *
 * Query @book with @query, setting @contacts to the list of contacts which
 * matched. On failed, @error will be set and %FALSE returned.
 *
 * Return value: %TRUE on success, %FALSE otherwise
 **/
gboolean
e_book_get_contacts (EBook *book, EBookQuery *query, GList **contacts, GError **error)
{
  GError *err = NULL;
  char **list = NULL;
  char *sexp;

  e_return_error_if_fail (E_IS_BOOK (book), E_BOOK_ERROR_INVALID_ARG);
  e_return_error_if_fail (book->priv->proxy, E_BOOK_ERROR_REPOSITORY_OFFLINE);
  e_return_error_if_fail (query, E_BOOK_ERROR_INVALID_ARG);
  e_return_error_if_fail (book->priv->loaded, E_BOOK_ERROR_SOURCE_NOT_LOADED);

  sexp = e_book_query_to_string (query);
  org_gnome_evolution_dataserver_addressbook_Book_get_contact_list (book->priv->proxy, sexp, &list, &err);
  g_free (sexp);
  if (!err) {
    GList *l = NULL;
    char **i = list;
    while (*i != NULL) {
      l = g_list_prepend (l, e_contact_new_from_vcard (*i++));
    }
    *contacts = g_list_reverse (l);
    g_strfreev (list);
    return TRUE;
  } else {
    return unwrap_gerror (err, error);
  }
}

static void
get_contacts_reply(DBusGProxy *proxy, char **vcards, GError *error, gpointer user_data)
{
  AsyncData *data = user_data;
  GList *list = NULL;
  EBookListCallback cb = data->callback;

  if (error) {
    vcards = NULL;
  }

  if (vcards) {
    char **i = vcards;
    while (*i != NULL) {
      list = g_list_prepend (list, e_contact_new_from_vcard (*i++));
    }
  }
  list = g_list_reverse (list);

  if (cb)
    cb (data->book, get_status_from_error (error), list, data->closure);
  g_strfreev (vcards);

  if (error)
    g_error_free (error);

  async_data_free (data);
}

/**
 * e_book_async_get_contacts:
 * @book: an #EBook
 * @query: an #EBookQuery
 * @cb: a function to call when the operation finishes
 * @closure: data to pass to callback function
 *
 * Query @book with @query.
 *
 * Return value: %FALSE on success, %TRUE otherwise
 **/
guint
e_book_async_get_contacts (EBook *book, EBookQuery *query, EBookListCallback cb, gpointer closure)
{
  AsyncData *data;
  char *sexp;

  e_return_async_error_val_if_fail (E_IS_BOOK (book), E_BOOK_ERROR_INVALID_ARG);
  e_return_async_error_val_if_fail (book->priv->proxy, E_BOOK_ERROR_REPOSITORY_OFFLINE);
  e_return_async_error_val_if_fail (query, E_BOOK_ERROR_INVALID_ARG);
  e_return_async_error_val_if_fail (book->priv->loaded, E_BOOK_ERROR_SOURCE_NOT_LOADED);

  sexp = e_book_query_to_string (query);

  data = async_data_new (book, NULL, cb, closure);

  if (!org_gnome_evolution_dataserver_addressbook_Book_get_contact_list_async (book->priv->proxy, sexp, get_contacts_reply, data))
    cb (book, E_BOOK_ERROR_CORBA_EXCEPTION, NULL, closure);

  g_free (sexp);
  return 0;
}

static EBookChange *
create_change (GValueArray *vals)
{
  EBookChange *change;
  EBookChangeType change_type;
  EContact *contact;
  GValue *val;

  g_return_val_if_fail (vals, NULL);

  val = g_value_array_get_nth (vals, 0);
  g_return_val_if_fail (val, NULL);

  change_type = g_value_get_uint (val);

  val = g_value_array_get_nth (vals, 1);
  g_return_val_if_fail (val, NULL);

  contact = e_contact_new_from_vcard (g_value_get_string (val));
  g_return_val_if_fail (contact, NULL);

  change = g_slice_new (EBookChange);
  change->change_type = change_type;
  change->contact = contact;

  return change;
}

static GList *
parse_changes_array (GPtrArray *array)
{
  GList *l = NULL;
  int i;

  if (array == NULL)
    return NULL;

  for (i = 0; i < array->len; i++) {
    EBookChange *change;
    GValueArray *vals;

    vals = g_ptr_array_index (array, i);
    if (!vals)
      continue;

    change = create_change (vals);
    if (change)
      l = g_list_prepend (l, change);

    g_value_array_free (vals);
  }

  g_ptr_array_free (array, TRUE);

  return g_list_reverse (l);
}

/**
 * e_book_get_changes:
 * @book: an #EBook
 * @changeid:  the change ID
 * @changes: return location for a #GList of #EBookChange items
 * @error: a #GError to set on failure.
 *
 * Get the set of changes since the previous call to #e_book_get_changes for a
 * given change ID.
 *
 * Return value: TRUE on success, FALSE otherwise
 */
gboolean
e_book_get_changes (EBook *book, char *changeid, GList **changes, GError **error)
{
  GError *err = NULL;
  GPtrArray *array = NULL;

  e_return_error_if_fail (E_IS_BOOK (book), E_BOOK_ERROR_INVALID_ARG);
  e_return_error_if_fail (book->priv->proxy, E_BOOK_ERROR_REPOSITORY_OFFLINE);
  e_return_error_if_fail (book->priv->loaded, E_BOOK_ERROR_SOURCE_NOT_LOADED);

  org_gnome_evolution_dataserver_addressbook_Book_get_changes (book->priv->proxy, changeid, &array, &err);
  if (!err) {
    *changes = parse_changes_array (array);
    return TRUE;
  } else {
    return unwrap_gerror (err, error);
  }
}

static void
get_changes_reply (DBusGProxy *proxy, GPtrArray *changes, GError *error, gpointer user_data)
{
  AsyncData *data = user_data;
  EBookListCallback cb = data->callback;
  GList *list = NULL;

  if (error)
    changes = NULL;

  if (changes)
    list = parse_changes_array (changes);

  if (cb)
    cb (data->book, get_status_from_error (error), list, data->closure);

  if (error)
    g_error_free (error);

  async_data_free (data);
}

/**
 * e_book_async_get_changes:
 * @book: an #EBook
 * @changeid:  the change ID
 * @cb: function to call when operation finishes
 * @closure: data to pass to callback function
 *
 * Get the set of changes since the previous call to #e_book_async_get_changes
 * for a given change ID.
 *
 * Return value: TRUE on success, FALSE otherwise
 */
guint
e_book_async_get_changes (EBook *book, char *changeid, EBookListCallback cb, gpointer closure)
{
  AsyncData *data;

  e_return_async_error_val_if_fail (E_IS_BOOK (book), E_BOOK_ERROR_INVALID_ARG);
  e_return_async_error_val_if_fail (book->priv->proxy, E_BOOK_ERROR_REPOSITORY_OFFLINE);
  e_return_async_error_val_if_fail (book->priv->loaded, E_BOOK_ERROR_SOURCE_NOT_LOADED);

  data = async_data_new (book, NULL, cb, closure);

  if (!org_gnome_evolution_dataserver_addressbook_Book_get_changes_async (book->priv->proxy, changeid, get_changes_reply, data))
    cb (book, E_BOOK_ERROR_CORBA_EXCEPTION, NULL, closure);

  return 0;
}

/**
 * e_book_free_change_list:
 * @change_list: a #GList of #EBookChange items
 *
 * Free the contents of #change_list, and the list itself.
 */
void
e_book_free_change_list (GList *change_list)
{
	GList *l;
	for (l = change_list; l; l = l->next) {
		EBookChange *change = l->data;

		g_object_unref (change->contact);
		g_slice_free (EBookChange, change);
	}

	g_list_free (change_list);
}

/**
 * e_book_add_contact:
 * @book: an #EBook
 * @contact: an #EContact
 * @error: a #GError to set on failure
 *
 * Adds @contact to @book.
 *
 * Return value: %TRUE if successful, %FALSE otherwise.
 **/
gboolean
e_book_add_contact (EBook *book, EContact *contact, GError **error)
{
  GError *err = NULL;
  char *vcard, *uid = NULL;

  e_return_error_if_fail (E_IS_BOOK (book), E_BOOK_ERROR_INVALID_ARG);
  e_return_error_if_fail (book->priv->proxy, E_BOOK_ERROR_REPOSITORY_OFFLINE);
  e_return_error_if_fail (E_IS_CONTACT (contact), E_BOOK_ERROR_INVALID_ARG);
  e_return_error_if_fail (book->priv->loaded, E_BOOK_ERROR_SOURCE_NOT_LOADED);

  vcard = e_vcard_to_string (E_VCARD (contact), EVC_FORMAT_VCARD_30);
  e_return_error_if_fail (vcard, E_BOOK_ERROR_OTHER_ERROR);

  org_gnome_evolution_dataserver_addressbook_Book_add_contact (book->priv->proxy, vcard, &uid, &err);
  g_free (vcard);
  if (uid) {
    e_contact_set (contact, E_CONTACT_UID, uid);
    g_free (uid);
  }
  return unwrap_gerror (err, error);
}

static void
add_contact_reply (DBusGProxy *proxy, char *uid, GError *error, gpointer user_data)
{
  AsyncData *data = user_data;
  EBookIdCallback cb = data->callback;

  /* If there is an error returned the GLib bindings currently return garbage
     for the OUT values. This is bad. */
  if (error)
    uid = NULL;

  if (cb)
    cb (data->book, get_status_from_error (error), uid, data->closure);

  if (uid)
    g_free (uid);

  if (error)
    g_error_free (error);

  async_data_free (data);
}

/**
 * e_book_async_add_contact:
 * @book: an #EBook
 * @contact: an #EContact
 * @cb: function to call when the operation finishes
 * @closure: data to pass to callback function
 *
 * Adds @contact to @book without blocking.
 *
 * Return value: %TRUE if the operation was started, %FALSE otherwise.
 **/
guint
e_book_async_add_contact (EBook *book, EContact *contact, EBookIdCallback cb, gpointer closure)
{
  char *vcard;
  AsyncData *data;

  e_return_async_error_val_if_fail (E_IS_BOOK (book), E_BOOK_ERROR_INVALID_ARG);
  e_return_async_error_val_if_fail (book->priv->proxy, E_BOOK_ERROR_REPOSITORY_OFFLINE);
  e_return_async_error_val_if_fail (E_IS_CONTACT (contact), E_BOOK_ERROR_INVALID_ARG);
  e_return_async_error_val_if_fail (book->priv->loaded, E_BOOK_ERROR_SOURCE_NOT_LOADED);

  vcard = e_vcard_to_string (E_VCARD (contact), EVC_FORMAT_VCARD_30);
  e_return_async_error_val_if_fail (vcard, E_BOOK_ERROR_OTHER_ERROR);

  data = async_data_new (book, NULL, cb, closure);

  if (!org_gnome_evolution_dataserver_addressbook_Book_add_contact_async (book->priv->proxy, vcard, add_contact_reply, data))
    cb (book, E_BOOK_ERROR_CORBA_EXCEPTION, NULL, closure);

  g_free (vcard);
  return 0;
}

/* Contacts-list is grouped to chunks of 32 entries.
 * Just as server is sending 32 entries at a time, we ensure that Client also sends in the same way.
 * This should avoid the mutex issues at the server side (book_view notification) as well. 
 */
static gboolean
e_book_add_contacts_by_threshold_limit (EBook *book, GList *contacts_list_threshold, GError **error)
{
  GError *err = NULL;
  GList *it;
  char **vcards, **i, **uids = NULL;

  g_debug ("%s: %d", G_STRFUNC, g_list_length (contacts_list_threshold));

  vcards = g_new0 (char*, g_list_length (contacts_list_threshold)+1);
  for (i = vcards, it = contacts_list_threshold; it; it = it->next, i++) {
    *i = e_vcard_to_string (E_VCARD (it->data), EVC_FORMAT_VCARD_30);
    if (!*i) {
      g_strfreev (vcards);
      e_return_error_if_fail (*i, E_BOOK_ERROR_OTHER_ERROR);
    }
  }

  org_gnome_evolution_dataserver_addressbook_Book_add_contacts (book->priv->proxy, (const char **)vcards, &uids, &err);
  if (!err) {
    for (i = uids, it = contacts_list_threshold; *i && it; i++, it = it->next) {
      e_contact_set (it->data, E_CONTACT_UID, *i);
    }
  }
  if (vcards)
    g_strfreev (vcards);
  if (uids)
    g_strfreev (uids);

  return unwrap_gerror (err, error);
}

/**
 * e_book_add_contacts:
 * @book: an #EBook
 * @contacts: an #GList of #EContacts
 * @error: a #GError to set on failure
 *
 * Adds @contacts to @book. This is always more efficient than calling
 * e_book_add_contact if you have more than one @contacts to add, as some
 * backends can implement it as a batch request.
 *
 * Return value: %TRUE if successful, %FALSE otherwise
 **/
gboolean
e_book_add_contacts (EBook *book, GList *contacts, GError **error)
{
  int i;
  GList *list_threshold = NULL;
  GList *node = contacts;

  e_return_error_if_fail (E_IS_BOOK (book), E_BOOK_ERROR_INVALID_ARG);
  e_return_error_if_fail (book->priv->proxy, E_BOOK_ERROR_REPOSITORY_OFFLINE);
  e_return_error_if_fail (contacts != NULL, E_BOOK_ERROR_INVALID_ARG);
  e_return_error_if_fail (book->priv->loaded, E_BOOK_ERROR_SOURCE_NOT_LOADED);

  while (node) {
    /* assemble the list_threshold */
    for (i = 0, list_threshold = NULL;
         node && i < THRESHOLD;
         node = g_list_next (node), i++) {
      list_threshold = g_list_prepend (list_threshold, node->data);
    }
    list_threshold = g_list_reverse (list_threshold);

    /* add them to book */
    if (FALSE == e_book_add_contacts_by_threshold_limit (book, list_threshold, error)) {
      g_list_free (list_threshold);
      return FALSE;
    }
    g_list_free (list_threshold);
  }

  return TRUE;
}

static void
add_contacts_reply (DBusGProxy *proxy, char **uids, GError *error, gpointer user_data)
{
  AsyncData *data = user_data;
  EBookCallback cb = data->callback;

  if (error)
    uids = NULL;

  /* TODO: what to do with uids? */
  if (cb)
    cb (data->book, get_status_from_error (error), data->closure);

  if (error)
    g_error_free (error);

  g_strfreev (uids);
  async_data_free (data);
}

/**
 * e_book_async_add_contacts:
 * @book: an #EBook
 * @contacts: a #GList of #EContacts
 * @cb: function to call when the operation finishes
 * @closure: data to pass to callback function
 *
 * Adds @contacts to @book without blocking. This is always more
 * efficient than calling e_book_add_contact if you have more than
 * one @contacts to add, as some backends can implement it as a
 * batch request.
 *
 * Return value: %TRUE if the operation was started, %FALSE otherwise.
 **/
guint
e_book_async_add_contacts (EBook *book, GList *contacts, EBookCallback cb, gpointer closure)
{
  char **vcards, **i;
  AsyncData *data;

  e_return_async_error_if_fail (E_IS_BOOK (book), E_BOOK_ERROR_INVALID_ARG);
  e_return_async_error_if_fail (book->priv->proxy, E_BOOK_ERROR_REPOSITORY_OFFLINE);
  e_return_async_error_if_fail (contacts != NULL, E_BOOK_ERROR_INVALID_ARG);
  e_return_async_error_if_fail (book->priv->loaded, E_BOOK_ERROR_SOURCE_NOT_LOADED);

  vcards = g_new0 (char*, g_list_length (contacts)+1);
  for (i = vcards; contacts; contacts = contacts->next, i++) {
    *i = e_vcard_to_string (E_VCARD (contacts->data), EVC_FORMAT_VCARD_30);
    if (!*i) {
      g_strfreev (vcards);
      e_return_async_error_if_fail (*i, E_BOOK_ERROR_OTHER_ERROR);
    }
  }

  data = async_data_new (book, NULL, cb, closure);

  if (!org_gnome_evolution_dataserver_addressbook_Book_add_contacts_async (book->priv->proxy, (const char**)vcards, add_contacts_reply, data))
    cb (book, E_BOOK_ERROR_CORBA_EXCEPTION, closure);

  g_strfreev (vcards);
  return 0;
}

/**
 * e_book_commit_contact:
 * @book: an #EBook
 * @contact: an #EContact
 * @error: a #GError to set on failure
 *
 * Applies the changes made to @contact to the stored version in
 * @book.
 *
 * Return value: %TRUE if successful, %FALSE otherwise
 **/
gboolean
e_book_commit_contact (EBook *book, EContact *contact, GError **error)
{
  GError *err = NULL;
  char *vcard;

  e_return_error_if_fail (E_IS_BOOK (book), E_BOOK_ERROR_INVALID_ARG);
  e_return_error_if_fail (book->priv->proxy, E_BOOK_ERROR_REPOSITORY_OFFLINE);
  e_return_error_if_fail (E_IS_CONTACT (contact), E_BOOK_ERROR_INVALID_ARG);
  e_return_error_if_fail (book->priv->loaded, E_BOOK_ERROR_SOURCE_NOT_LOADED);

  vcard = e_vcard_to_string (E_VCARD (contact), EVC_FORMAT_VCARD_30);
  e_return_error_if_fail (vcard, E_BOOK_ERROR_OTHER_ERROR);

  org_gnome_evolution_dataserver_addressbook_Book_modify_contact (book->priv->proxy, vcard, &err);
  g_free (vcard);
  return unwrap_gerror (err, error);
}

static void
modify_contact_reply (DBusGProxy *proxy, GError *error, gpointer user_data)
{
  AsyncData *data = user_data;
  EBookCallback cb = data->callback;

  if (cb)
    cb (data->book, get_status_from_error (error), data->closure);

  if (error)
    g_error_free (error);

  async_data_free (data);
}

/**
 * e_book_async_commit_contact:
 * @book: an #EBook
 * @contact: an #EContact
 * @cb: function to call when the operation finishes
 * @closure: data to pass to callback function
 *
 * Applies the changes made to @contact to the stored version in
 * @book without blocking.
 *
 * Return value: %TRUE if the operation was started, %FALSE otherwise.
 **/
guint
e_book_async_commit_contact (EBook *book, EContact *contact, EBookCallback cb, gpointer closure)
{
  char *vcard;
  AsyncData *data;

  e_return_async_error_if_fail (E_IS_BOOK (book), E_BOOK_ERROR_INVALID_ARG);
  e_return_async_error_if_fail (book->priv->proxy, E_BOOK_ERROR_REPOSITORY_OFFLINE);
  e_return_async_error_if_fail (E_IS_CONTACT (contact), E_BOOK_ERROR_INVALID_ARG);
  e_return_async_error_if_fail (book->priv->loaded, E_BOOK_ERROR_SOURCE_NOT_LOADED);

  vcard = e_vcard_to_string (E_VCARD (contact), EVC_FORMAT_VCARD_30);
  e_return_async_error_if_fail (vcard, E_BOOK_ERROR_OTHER_ERROR);

  data = async_data_new (book, NULL, cb, closure);

  if (!org_gnome_evolution_dataserver_addressbook_Book_modify_contact_async (book->priv->proxy, vcard, modify_contact_reply, data))
    cb (book, E_BOOK_ERROR_CORBA_EXCEPTION, closure);

  g_free (vcard);
  return 0;
}

gboolean
e_book_commit_contacts (EBook *book, GList *contacts, GError **error)
{
  GError *err = NULL;
  char **vcards, **i;

  e_return_error_if_fail (E_IS_BOOK (book), E_BOOK_ERROR_INVALID_ARG);
  e_return_error_if_fail (book->priv->proxy, E_BOOK_ERROR_REPOSITORY_OFFLINE);
  e_return_error_if_fail (contacts != NULL, E_BOOK_ERROR_INVALID_ARG);
  e_return_error_if_fail (book->priv->loaded, E_BOOK_ERROR_SOURCE_NOT_LOADED);

  vcards = g_new0 (char*, g_list_length (contacts)+1);
  for (i = vcards; contacts; contacts = contacts->next, i++) {
    *i = e_vcard_to_string (E_VCARD (contacts->data), EVC_FORMAT_VCARD_30);
    if (!*i) {
      g_strfreev (vcards);
      e_return_error_if_fail (*i, E_BOOK_ERROR_OTHER_ERROR);
    }
  }

  org_gnome_evolution_dataserver_addressbook_Book_modify_contacts (book->priv->proxy, (const char**)vcards, &err);

  g_strfreev (vcards);

  return unwrap_gerror (err, error);
}

static void
modify_contacts_reply (DBusGProxy *proxy, GError *error, gpointer user_data)
{
  AsyncData *data = user_data;
  EBookCallback cb = data->callback;

  if (cb)
    cb (data->book, get_status_from_error (error), data->closure);

  if (error)
    g_error_free (error);

  async_data_free (data);
}

guint
e_book_async_commit_contacts (EBook *book, GList *contacts, EBookCallback cb, gpointer closure)
{
  char **vcards, **i;
  AsyncData *data;

  e_return_async_error_if_fail (E_IS_BOOK (book), E_BOOK_ERROR_INVALID_ARG);
  e_return_async_error_if_fail (book->priv->proxy, E_BOOK_ERROR_REPOSITORY_OFFLINE);
  e_return_async_error_if_fail (contacts != NULL, E_BOOK_ERROR_INVALID_ARG);
  e_return_async_error_if_fail (book->priv->loaded, E_BOOK_ERROR_SOURCE_NOT_LOADED);

  vcards = g_new0 (char*, g_list_length (contacts)+1);
  for (i = vcards; contacts; contacts = contacts->next, i++) {
    *i = e_vcard_to_string (E_VCARD (contacts->data), EVC_FORMAT_VCARD_30);
    if (!*i) {
      g_strfreev (vcards);
      e_return_async_error_if_fail (*i, E_BOOK_ERROR_OTHER_ERROR);
    }
  }

  data = async_data_new (book, NULL, cb, closure);

  if (!org_gnome_evolution_dataserver_addressbook_Book_modify_contacts_async (book->priv->proxy, (const char**)vcards, modify_contacts_reply, data))
    cb (book, E_BOOK_ERROR_CORBA_EXCEPTION, closure);

  g_strfreev (vcards);
  return 0;
}

/**
 * e_book_remove_contact:
 * @book: an #EBook
 * @id: a string
 * @error: a #GError to set on failure
 *
 * Removes the contact with id @id from @book.
 *
 * Return value: %TRUE if successful, %FALSE otherwise
 **/
gboolean
e_book_remove_contact (EBook *book, const char *id, GError **error)
{
  GError *err = NULL;
  const char *l[2];

  e_return_error_if_fail (E_IS_BOOK (book), E_BOOK_ERROR_INVALID_ARG);
  e_return_error_if_fail (book->priv->proxy, E_BOOK_ERROR_REPOSITORY_OFFLINE);
  e_return_error_if_fail (book->priv->loaded, E_BOOK_ERROR_SOURCE_NOT_LOADED);
  e_return_error_if_fail (id && g_utf8_validate (id, -1, NULL), E_BOOK_ERROR_INVALID_ARG);

  l[0] = id;
  l[1] = NULL;

  org_gnome_evolution_dataserver_addressbook_Book_remove_contacts (book->priv->proxy, l, &err);
  return unwrap_gerror (err, error);
}

static void
remove_contact_reply (DBusGProxy *proxy, GError *error, gpointer user_data)
{
  AsyncData *data = user_data;
  EBookCallback cb = data->callback;

  if (cb)
    cb (data->book, get_status_from_error (error), data->closure);

  if (error)
    g_error_free (error);

  async_data_free (data);
}

/**
 * e_book_async_remove_contact:
 * @book: an #EBook
 * @contact: an #EContact
 * @cb: a function to call when the operation finishes
 * @closure: data to pass to callback function
 *
 * Removes @contact from @book.
 *
 * Return value: %TRUE if successful, %FALSE otherwise
 **/guint
e_book_async_remove_contact (EBook *book, EContact *contact, EBookCallback cb, gpointer closure)
{
  AsyncData *data;
  const char *l[2];

  e_return_async_error_if_fail (E_IS_BOOK (book), E_BOOK_ERROR_INVALID_ARG);
  e_return_async_error_if_fail (book->priv->proxy, E_BOOK_ERROR_REPOSITORY_OFFLINE);
  e_return_async_error_if_fail (E_IS_CONTACT (contact), E_BOOK_ERROR_INVALID_ARG);
  e_return_async_error_if_fail (book->priv->loaded, E_BOOK_ERROR_SOURCE_NOT_LOADED);

  l[0] = e_contact_get_const (contact, E_CONTACT_UID);
  l[1] = NULL;

  e_return_async_error_if_fail (g_utf8_validate (l[0], -1, NULL), E_BOOK_ERROR_INVALID_ARG);

  data = async_data_new (book, NULL, cb, closure);

  if (!org_gnome_evolution_dataserver_addressbook_Book_remove_contacts_async (book->priv->proxy, l, remove_contact_reply, data))
    cb (book, E_BOOK_ERROR_CORBA_EXCEPTION, closure);

  return 0;
}

static void
remove_contact_by_id_reply (DBusGProxy *proxy, GError *error, gpointer user_data)
{
  AsyncData *data = user_data;
  EBookCallback cb = data->callback;

  if (cb)
    cb (data->book, get_status_from_error (error), data->closure);

  if (error)
    g_error_free (error);

  async_data_free (data);
}

/**
 * e_book_async_remove_contact_by_id:
 * @book: an #EBook
 * @id: a unique ID string specifying the contact
 * @cb: a function to call when the operation finishes
 * @closure: data to pass to callback function
 *
 * Removes the contact with id @id from @book.
 *
 * Return value: %TRUE if successful, %FALSE otherwise
 **/
guint
e_book_async_remove_contact_by_id (EBook *book, const char *id, EBookCallback cb, gpointer closure)
{
  AsyncData *data;
  const char *l[2];

  e_return_async_error_if_fail (E_IS_BOOK (book), E_BOOK_ERROR_INVALID_ARG);
  e_return_async_error_if_fail (book->priv->proxy, E_BOOK_ERROR_REPOSITORY_OFFLINE);
  e_return_async_error_if_fail (book->priv->loaded, E_BOOK_ERROR_SOURCE_NOT_LOADED);
  e_return_async_error_if_fail (id && g_utf8_validate (id, -1, NULL), E_BOOK_ERROR_INVALID_ARG);

  l[0] = id;
  l[1] = NULL;

  data = async_data_new (book, NULL, cb, closure);

  if (!org_gnome_evolution_dataserver_addressbook_Book_remove_contacts_async (book->priv->proxy, l, remove_contact_by_id_reply, data))
    cb (book, E_BOOK_ERROR_CORBA_EXCEPTION, closure);

  return 0;
}

/**
 * e_book_remove_contacts:
 * @book: an #EBook
 * @ids: an #GList of const char *id's
 * @error: a #GError to set on failure
 *
 * Removes the contacts with ids from the list @ids from @book.  This is
 * always more efficient than calling e_book_remove_contact_by_id if you
 * have more than one id to remove, as some backends can implement it
 * as a batch request.
 *
 * Return value: %TRUE if successful, %FALSE otherwise
 **/
gboolean
e_book_remove_contacts (EBook *book, GList *ids, GError **error)
{
  GError *err = NULL;
  char **l;
  GList *id;

  e_return_error_if_fail (E_IS_BOOK (book), E_BOOK_ERROR_INVALID_ARG);
  e_return_error_if_fail (book->priv->proxy, E_BOOK_ERROR_REPOSITORY_OFFLINE);
  e_return_error_if_fail (ids, E_BOOK_ERROR_INVALID_ARG);
  e_return_error_if_fail (book->priv->loaded, E_BOOK_ERROR_SOURCE_NOT_LOADED);

  for (id = ids; id; id = id->next) {
    e_return_error_if_fail (g_utf8_validate (id->data, -1, NULL), E_BOOK_ERROR_INVALID_ARG);
  }

  l = flatten_stringlist (ids);

  org_gnome_evolution_dataserver_addressbook_Book_remove_contacts (book->priv->proxy, (const char **) l, &err);
  g_free (l);
  return unwrap_gerror (err, error);
}

static void
remove_contacts_reply (DBusGProxy *proxy, GError *error, gpointer user_data)
{
  AsyncData *data = user_data;
  EBookCallback cb = data->callback;

  if (cb)
    cb (data->book, get_status_from_error (error), data->closure);

  if (error)
    g_error_free (error);

  async_data_free (data);
}

/**
 * e_book_async_remove_contacts:
 * @book: an #EBook
 * @ids: a #GList of const char *id's
 * @cb: a function to call when the operation finishes
 * @closure: data to pass to callback function
 *
 * Removes the contacts with ids from the list @ids from @book.  This is
 * always more efficient than calling e_book_remove_contact() if you
 * have more than one id to remove, as some backends can implement it
 * as a batch request.
 *
 * Return value: %TRUE if successful, %FALSE otherwise
 **/
guint
e_book_async_remove_contacts (EBook *book, GList *id_list, EBookCallback cb, gpointer closure)
{
  AsyncData *data;
  char **l;
  GList *id;

  e_return_async_error_if_fail (E_IS_BOOK (book), E_BOOK_ERROR_INVALID_ARG);
  e_return_async_error_if_fail (book->priv->proxy, E_BOOK_ERROR_REPOSITORY_OFFLINE);
  e_return_async_error_if_fail (book->priv->loaded, E_BOOK_ERROR_SOURCE_NOT_LOADED);

  if (id_list == NULL) {
    if (cb)
      cb (book, E_BOOK_ERROR_OK, closure);
    return 0;
  }

  for (id = id_list; id; id = id->next) {
    e_return_async_error_if_fail (g_utf8_validate (id->data, -1, NULL), E_BOOK_ERROR_INVALID_ARG);
  }

  l = flatten_stringlist (id_list);

  data = async_data_new (book, NULL, cb, closure);

  if (!org_gnome_evolution_dataserver_addressbook_Book_remove_contacts_async (book->priv->proxy, (const char **) l, remove_contacts_reply, data))
    cb (book, E_BOOK_ERROR_CORBA_EXCEPTION, closure);

  g_free (l);
  return 0;
}

static gboolean
e_book_remove_all_contacts_fallback (EBook *book, GError **error)
{
  gboolean success = FALSE;
  GList *contacts = NULL;
  EBookQuery *query;

  query = e_book_query_any_field_contains ("");

  if (e_book_get_contacts (book, query, &contacts, error)) {
    GList *uids = NULL;
    GList *l;

    for (l = contacts; l; l = l->next)
      uids = g_list_prepend (uids, e_contact_get_const (l->data, E_CONTACT_UID));

    success = e_book_remove_contacts (book, uids, error);

    g_list_free (uids);
  }

  g_list_foreach (contacts, (GFunc) g_object_unref, NULL);
  g_list_free (contacts);

  e_book_query_unref (query);

  return success;
}

/**
 * e_book_remove_all_contacts:
 * @book: an #EBook
 * @error: a #GError to set on failure
 *
 * Removes all contacts from @book. This is always more efficient than calling
 * e_book_remove_contacts(), as some backends can implement it as a batch
 * request.
 *
 * Return value: %TRUE if successful, %FALSE otherwise
 **/
gboolean
e_book_remove_all_contacts (EBook *book, GError **error)
{
  GError *err = NULL;

  e_return_error_if_fail (E_IS_BOOK (book), E_BOOK_ERROR_INVALID_ARG);
  e_return_error_if_fail (book->priv->proxy, E_BOOK_ERROR_REPOSITORY_OFFLINE);
  e_return_error_if_fail (book->priv->loaded, E_BOOK_ERROR_SOURCE_NOT_LOADED);

  if (!e_book_check_static_capability (book, "remove-all-contacts"))
    return e_book_remove_all_contacts_fallback (book, error);

  org_gnome_evolution_dataserver_addressbook_Book_remove_all_contacts (book->priv->proxy, &err);

  return unwrap_gerror (err, error);
}

static void
remove_all_contacts_get_contacts_cb (EBook       *book,
                                     EBookStatus  status,
                                     GList       *contacts,
                                     gpointer     closure)
{
  AsyncData *data = closure;
  EBookCallback cb = data->callback;

  if (E_BOOK_ERROR_OK == status) {
    GList *uids = NULL;
    GList *l;

    for (l = contacts; l; l = l->next)
      uids = g_list_prepend (uids, e_contact_get_const (l->data, E_CONTACT_UID));

    if (e_book_async_remove_contacts (book, uids, cb, data->closure))
      status = E_BOOK_ERROR_OTHER_ERROR;

    g_list_free (uids);
  }

  g_list_foreach (contacts, (GFunc) g_object_unref, NULL);
  g_list_free (contacts);

  if (E_BOOK_ERROR_OK != status && cb)
    cb (data->book, status, data->closure);

  async_data_free (data);
}

static guint
e_book_async_remove_all_contacts_fallback (EBook *book, EBookCallback cb, gpointer closure)
{
  EBookQuery *query;
  AsyncData *data;

  query = e_book_query_any_field_contains ("");
  data = async_data_new (book, query, cb, closure);
  e_book_query_unref (query);

  return e_book_async_get_contacts (book, query, remove_all_contacts_get_contacts_cb, data);
}

static void
remove_all_contacts_reply (DBusGProxy *proxy, GError *error, gpointer user_data)
{
  AsyncData *data = user_data;
  EBookCallback cb = data->callback;

  if (cb)
    cb (data->book, get_status_from_error (error), data->closure);

  if (error)
    g_error_free (error);

  async_data_free (data);
}

/**
 * e_book_async_remove_all_contacts:
 * @book: an #EBook
 * @cb: a function to call when the operation finishes
 * @closure: data to pass to callback function
 *
 * Removes all contacts from @book. This is always more efficient than calling
 * e_book_remove_contact(), as some backends can implement it as a batch request.
 *
 * Return value: %TRUE if successful, %FALSE otherwise
 **/
guint
e_book_async_remove_all_contacts (EBook *book, EBookCallback cb, gpointer closure)
{
  AsyncData *data;

  e_return_async_error_if_fail (E_IS_BOOK (book), E_BOOK_ERROR_INVALID_ARG);
  e_return_async_error_if_fail (book->priv->proxy, E_BOOK_ERROR_REPOSITORY_OFFLINE);
  e_return_async_error_if_fail (book->priv->loaded, E_BOOK_ERROR_SOURCE_NOT_LOADED);

  data = async_data_new (book, NULL, cb, closure);

  if (!e_book_check_static_capability (book, "remove-all-contacts"))
    return e_book_async_remove_all_contacts_fallback (book, cb, closure);

  if (!org_gnome_evolution_dataserver_addressbook_Book_remove_all_contacts_async (book->priv->proxy, remove_contacts_reply, data))
    cb (book, E_BOOK_ERROR_CORBA_EXCEPTION, closure);

  return 0;
}

/**
 * e_book_get_book_view:
 * @book: an #EBook
 * @query: an #EBookQuery
 * @requested_fields: a #GList containing the names of fields to return, or NULL for all
 * @max_results: the maximum number of contacts to show (or 0 for all)
 * @book_view: A #EBookView pointer, will be set to the view
 * @error: a #GError to set on failure
 *
 * Query @book with @query, creating a #EBookView in @book_view with the fields
 * specified by @requested_fields and limited at @max_results records. On an
 * error, @error is set and %FALSE returned.
 *
 * Return value: %TRUE if successful, %FALSE otherwise
 **/
gboolean
e_book_get_book_view (EBook *book, EBookQuery *query, GList *requested_fields, int max_results, EBookView **book_view, GError **error)
{
  GError *err = NULL;
  DBusGConnection *view_conn;
  DBusGProxy *view_proxy;
  char *sexp, *address;
  gboolean ret = TRUE;

  e_return_error_if_fail (E_IS_BOOK (book), E_BOOK_ERROR_INVALID_ARG);
  e_return_error_if_fail (book->priv->proxy, E_BOOK_ERROR_REPOSITORY_OFFLINE);
  e_return_error_if_fail (query, E_BOOK_ERROR_INVALID_ARG);
  e_return_error_if_fail (book->priv->loaded, E_BOOK_ERROR_SOURCE_NOT_LOADED);

  sexp = e_book_query_to_string (query);

  if (!org_gnome_evolution_dataserver_addressbook_Book_get_book_view (book->priv->proxy, sexp, max_results, &address, &err)) {
    *book_view = NULL;
    g_free (sexp);
    return unwrap_gerror (err, error);
  }

  view_conn = dbus_g_connection_open (address, &err);
  g_free (address);
  if (!view_conn) {
   return unwrap_gerror (err, error);
  }

  view_proxy = dbus_g_proxy_new_for_peer (view_conn, "/", "org.gnome.evolution.dataserver.addressbook.BookView");
  dbus_g_connection_unref (view_conn);

  if (view_proxy) {
    *book_view = e_book_view_new (book, query, view_proxy);
  } else {
    *book_view = NULL;
    g_set_error (error, E_BOOK_ERROR, E_BOOK_ERROR_CORBA_EXCEPTION,
                 "Cannot get connection to view");
    ret = FALSE;
  }

  g_free (sexp);

  return ret;
}

static void
get_book_view_reply (DBusGProxy *proxy, char *address, GError *error, gpointer user_data)
{
  AsyncData *data = user_data;
  GError *err = NULL;
  EBookView *view = NULL;
  EBookBookViewCallback cb = data->callback;
  DBusGConnection *view_conn;
  DBusGProxy *view_proxy;
  EBookStatus status;

  if (error)
    address = NULL;

  if (address) {
    view_conn = dbus_g_connection_open (address, &err);
    g_free (address);

    if (!view_conn)
    {
      g_warning (G_STRLOC ": cannot get connection: %s", err->message);
      g_error_free (err);
      status = E_BOOK_ERROR_CORBA_EXCEPTION;
    } else {

      view_proxy = dbus_g_proxy_new_for_peer (view_conn, "/", "org.gnome.evolution.dataserver.addressbook.BookView");
      dbus_g_connection_unref (view_conn);

      if (view_proxy) {
        view = e_book_view_new (data->book, data->query, view_proxy);
        status = E_BOOK_ERROR_OK;
      } else {
        g_warning (G_STRLOC ": cannot get connection to view: %s", err->message);
        g_error_free (err);
        status = E_BOOK_ERROR_CORBA_EXCEPTION;
      }
    }

  } else {
    status = get_status_from_error (error);
  }

  if (cb)
    cb (data->book, status, view, data->closure);

  if (error)
    g_error_free (error);

  async_data_free (data);
}

/**
 * e_book_async_get_book_view:
 * @book: an #EBook
 * @query: an #EBookQuery
 * @requested_fields: a #GList containing the names of fields to return, or NULL for all
 * @max_results: the maximum number of contacts to show (or 0 for all)
 * @cb: a function to call when the operation finishes
 * @closure: data to pass to callback function
 *
 * Query @book with @query, creating a #EBookView with the fields
 * specified by @requested_fields and limited at @max_results records.
 *
 * Return value: %FALSE if successful, %TRUE otherwise
 **/
guint
e_book_async_get_book_view (EBook *book, EBookQuery *query, GList *requested_fields, int max_results, EBookBookViewCallback cb, gpointer closure)
{
  AsyncData *data;
  char *sexp;

  e_return_async_error_val_if_fail (E_IS_BOOK (book), E_BOOK_ERROR_INVALID_ARG);
  e_return_async_error_val_if_fail (book->priv->proxy, E_BOOK_ERROR_REPOSITORY_OFFLINE);
  e_return_async_error_val_if_fail (query, E_BOOK_ERROR_INVALID_ARG);
  e_return_async_error_val_if_fail (book->priv->loaded, E_BOOK_ERROR_SOURCE_NOT_LOADED);

  data = async_data_new (book, query, cb, closure);

  sexp = e_book_query_to_string (query);

  if (!org_gnome_evolution_dataserver_addressbook_Book_get_book_view_async (book->priv->proxy, sexp, max_results, get_book_view_reply, data))
    cb (book, E_BOOK_ERROR_CORBA_EXCEPTION, NULL, closure);

  g_free (sexp);
  return 0;
}

/**
 * e_book_is_opened:
 * @book: and #EBook
 *
 * Check if this book has been opened.
 *
 * Return value: %TRUE if this book has been opened, otherwise %FALSE.
 */
gboolean
e_book_is_opened (EBook *book)
{
  g_return_val_if_fail (E_IS_BOOK (book), FALSE);

  return book->priv->loaded;
}

/**
 * e_book_is_writable:
 * @book: an #EBook
 *
 * Check if this book is writable.
 *
 * Return value: %TRUE if this book is writable, otherwise %FALSE.
 */
gboolean
e_book_is_writable (EBook *book)
{
  g_return_val_if_fail (E_IS_BOOK (book), FALSE);
  return book->priv->writable;
}

/**
 * e_book_is_online:
 * @book: an #EBook
 *
 * Check if this book is connected.
 *
 * Return value: %TRUE if this book is connected, otherwise %FALSE.
 **/
gboolean
e_book_is_online (EBook *book)
{
  g_return_val_if_fail (E_IS_BOOK (book), FALSE);

  return book->priv->connected;
}

/**
 * e_book_cancel:
 * @book: an #EBook
 * @error: a #GError to set on failure
 *
 * Used to cancel an already running operation on @book.  This
 * function makes a synchronous CORBA to the backend telling it to
 * cancel the operation.  If the operation wasn't cancellable (either
 * transiently or permanently) or had already comopleted on the server
 * side, this function will return E_BOOK_STATUS_COULD_NOT_CANCEL, and
 * the operation will continue uncancelled.  If the operation could be
 * cancelled, this function will return E_BOOK_ERROR_OK, and the
 * blocked e_book function corresponding to current operation will
 * return with a status of E_BOOK_STATUS_CANCELLED.
 *
 * Return value: %TRUE on success, %FALSE otherwise
 **/
gboolean
e_book_cancel (EBook *book, GError **error)
{
  e_return_error_if_fail (E_IS_BOOK (book), E_BOOK_ERROR_INVALID_ARG);
  e_return_error_if_fail (book->priv->proxy, E_BOOK_ERROR_REPOSITORY_OFFLINE);

  return org_gnome_evolution_dataserver_addressbook_Book_cancel_operation (book->priv->proxy, error);
}

/**
 * e_book_get_uri:
 * @book: an #EBook
 *
 * Get the URI that this book has loaded. This string should not be freed.
 *
 * Return value: The URI.
 */
const char *
e_book_get_uri (EBook *book)
{
  g_return_val_if_fail (E_IS_BOOK (book), NULL);

  return book->priv->uri;
}

/**
 * e_book_get_static_capabilities:
 * @book: an #EBook
 * @error: an #GError to set on failure
 *
 * Get the list of capabilities which the backend for this address book
 * supports. This string should not be freed.
 *
 * Return value: The capabilities list
 */
const char *
e_book_get_static_capabilities (EBook *book, GError **error)
{
  e_return_error_if_fail (E_IS_BOOK (book), E_BOOK_ERROR_INVALID_ARG);
  e_return_error_if_fail (book->priv->proxy, E_BOOK_ERROR_REPOSITORY_OFFLINE);

  if (!book->priv->cap_queried) {
    char *cap = NULL;

    if (!org_gnome_evolution_dataserver_addressbook_Book_get_static_capabilities (book->priv->proxy, &cap, error)) {
      return NULL;
    }

    book->priv->cap = cap;
    book->priv->cap_queried = TRUE;
  }

  return book->priv->cap;
}

#define SELF_UID_KEY "/apps/evolution/addressbook/self/self_uid"

/**
 * e_book_get_self:
 * @contact: an #EContact pointer to set
 * @book: an #EBook pointer to set
 * @error: a #GError to set on failure
 *
 * Get the #EContact referring to the user of the address book
 * and set it in @contact and @book.
 *
 * Return value: %TRUE if successful, otherwise %FALSE.
 **/
gboolean
e_book_get_self (EContact **contact, EBook **book, GError **error)
{
	GConfClient *gconf;
	gboolean status;
	char *uid;

	*book = e_book_new_system_addressbook (error);

	if (!*book) {
		return FALSE;
	}

	status = e_book_open (*book, FALSE, error);
	if (status == FALSE) {
		return FALSE;
	}

	gconf = gconf_client_get_default();
	uid = gconf_client_get_string (gconf, SELF_UID_KEY, NULL);
	g_object_unref (gconf);

	if (!uid) {
		g_object_unref (*book);
		*book = NULL;
		g_set_error (error, E_BOOK_ERROR, E_BOOK_ERROR_NO_SELF_CONTACT,
			     _("%s: there was no self contact uid stored in gconf"), "e_book_get_self");
		return FALSE;
	}

	if (!e_book_get_contact (*book, uid, contact, error)) {
		g_object_unref (*book);
		*book = NULL;
		return FALSE;
	}

	return TRUE;
}

/**
 * e_book_set_self:
 * @book: an #EBook
 * @contact: an #EContact
 * @error: a #GError to set on failure
 *
 * Specify that @contact residing in @book is the #EContact that
 * refers to the user of the address book.
 *
 * Return value: %TRUE if successful, %FALSE otherwise.
 **/
gboolean
e_book_set_self (EBook *book, EContact *contact, GError **error)
{
	GConfClient *gconf;

	e_return_error_if_fail (E_IS_BOOK (book), E_BOOK_ERROR_INVALID_ARG);
	e_return_error_if_fail (E_IS_CONTACT (contact), E_BOOK_ERROR_INVALID_ARG);

	gconf = gconf_client_get_default();
	gconf_client_set_string (gconf, SELF_UID_KEY, e_contact_get_const (contact, E_CONTACT_UID), NULL);
	g_object_unref (gconf);

	return TRUE;
}

/**
 * e_book_is_self:
 * @contact: an #EContact
 *
 * Check if @contact is the user of the address book.
 *
 * Return value: %TRUE if @contact is the user, %FALSE otherwise.
 **/
gboolean
e_book_is_self (EContact *contact)
{
	GConfClient *gconf;
	char *uid;
	gboolean rv;

	/* XXX this should probably be e_return_error_if_fail, but we
	   need a GError** arg for that */
	g_return_val_if_fail (contact && E_IS_CONTACT (contact), FALSE);

	gconf = gconf_client_get_default();
	uid = gconf_client_get_string (gconf, SELF_UID_KEY, NULL);
	g_object_unref (gconf);

	rv = (uid && !strcmp (uid, e_contact_get_const (contact, E_CONTACT_UID)));

	g_free (uid);

	return rv;
}

/**
 * e_book_set_default_addressbook:
 * @book: An #EBook pointer
 * @error: A #GError pointer
 *
 * sets the #ESource of the #EBook as the "default" addressbook.  This is the source
 * that will be loaded in the e_book_get_default_addressbook call.
 *
 * Return value: %TRUE if the setting was stored in libebook's ESourceList, otherwise %FALSE.
 */
gboolean
e_book_set_default_addressbook (EBook *book, GError **error)
{
  ESource *source;

  e_return_error_if_fail (E_IS_BOOK (book), E_BOOK_ERROR_INVALID_ARG);
  e_return_error_if_fail (book->priv->loaded == FALSE, E_BOOK_ERROR_SOURCE_ALREADY_LOADED);

  source = e_book_get_source (book);

  return e_book_set_default_source (source, error);
}


/**
 * e_book_set_default_source:
 * @source: An #ESource pointer
 * @error: A #GError pointer
 *
 * sets @source as the "default" addressbook.  This is the source that
 * will be loaded in the e_book_get_default_addressbook call.
 *
 * Return value: %TRUE if the setting was stored in libebook's ESourceList, otherwise %FALSE.
 */
gboolean
e_book_set_default_source (ESource *source, GError **error)
{
	ESourceList *sources;
	const char *uid;
	GSList *g;

	e_return_error_if_fail (source && E_IS_SOURCE (source), E_BOOK_ERROR_INVALID_ARG);

	uid = e_source_peek_uid (source);

	if (!e_book_get_addressbooks (&sources, error)) {
		return FALSE;
	}

	/* make sure the source is actually in the ESourceList.  if
	   it's not we don't bother adding it, just return an error */
	source = e_source_list_peek_source_by_uid (sources, uid);
	if (!source) {
		g_set_error (error, E_BOOK_ERROR, E_BOOK_ERROR_NO_SUCH_SOURCE,
			     _("%s: there was no source for uid `%s' stored in gconf."), "e_book_set_default_source", uid);
		g_object_unref (sources);
		return FALSE;
	}

	/* loop over all the sources clearing out any "default"
	   properties we find */
	for (g = e_source_list_peek_groups (sources); g; g = g->next) {
		GSList *s;
		for (s = e_source_group_peek_sources (E_SOURCE_GROUP (g->data));
		     s; s = s->next) {
			e_source_set_property (E_SOURCE (s->data), "default", NULL);
		}
	}

	/* set the "default" property on the source */
	e_source_set_property (source, "default", "true");

	if (!e_source_list_sync (sources, error)) {
		return FALSE;
	}

	return TRUE;
}

/**
 * e_book_check_static_capability:
 * @book: an #EBook
 * @cap: A capability string
 *
 * Check to see if the backend for this address book supports the capability
 * @cap.
 *
 * Return value: %TRUE if the backend supports @cap, %FALSE otherwise.
 */
gboolean
e_book_check_static_capability (EBook *book, const char *cap)
{
  const char *caps;

  g_return_val_if_fail (E_IS_BOOK (book), FALSE);

  caps = e_book_get_static_capabilities (book, NULL);

  /* XXX this is an inexact test but it works for our use */
  if (caps && strstr (caps, cap))
    return TRUE;

  return FALSE;
}

/**
 * If the specified GError is a remote error, then create a new error
 * representing the remote error.  If the error is anything else, then leave it
 * alone.
 */
static gboolean
unwrap_gerror (GError *error, GError **client_error)
{
  if (error == NULL)
    return TRUE;

  if (error->domain == DBUS_GERROR && error->code == DBUS_GERROR_REMOTE_EXCEPTION) {
    GError *new;
    gint code;
    if (client_error) {
      code = get_status_from_error (error);
      new = g_error_new_literal (E_BOOK_ERROR, code, error->message);
      *client_error = new;
    }
    g_error_free (error);
  } else {
    if (client_error)
      *client_error = error;
  }
  return FALSE;
}

/**
 * If the GError is a remote error, extract the EBookStatus embedded inside.
 * Otherwise return CORBA_EXCEPTION (I know this is DBus...).
 */
static EBookStatus
get_status_from_error (GError *error)
{
  static struct {
    const char  *name;
    EBookStatus  status;
    GQuark       quark;
  } error_mapping[] = {
    { "org.gnome.evolution.dataserver.addressbook.Book.repositoryoffline",
      E_BOOK_ERROR_REPOSITORY_OFFLINE, 0 },
    { "org.gnome.evolution.dataserver.addressbook.Book.permissiondenied",
      E_BOOK_ERROR_PERMISSION_DENIED, 0 },
    { "org.gnome.evolution.dataserver.addressbook.Book.contactnotfound",
      E_BOOK_ERROR_CONTACT_NOT_FOUND, 0 },
    { "org.gnome.evolution.dataserver.addressbook.Book.contactidalreadyexists",
      E_BOOK_ERROR_CONTACT_ID_ALREADY_EXISTS, 0 },
    { "org.gnome.evolution.dataserver.addressbook.Book.authenticationfailed",
      E_BOOK_ERROR_AUTHENTICATION_FAILED, 0 },
    { "org.gnome.evolution.dataserver.addressbook.Book.authenticationrequired",
      E_BOOK_ERROR_AUTHENTICATION_REQUIRED, 0 },
    { "org.gnome.evolution.dataserver.addressbook.Book.tlsnotavailable",
      E_BOOK_ERROR_TLS_NOT_AVAILABLE, 0 },
    { "org.gnome.evolution.dataserver.addressbook.Book.nosuchbook",
      E_BOOK_ERROR_NO_SUCH_BOOK, 0 },
    { "org.gnome.evolution.dataserver.addressbook.Book.offlineunavailable",
      E_BOOK_ERROR_OFFLINE_UNAVAILABLE, 0 },
    { "org.gnome.evolution.dataserver.addressbook.Book.couldnotcancel",
      E_BOOK_ERROR_COULD_NOT_CANCEL, 0 },
    { "org.gnome.evolution.dataserver.addressbook.Book.othererror",
      E_BOOK_ERROR_OTHER_ERROR, 0 },
    { "org.gnome.evolution.dataserver.addressbook.Book.invalidserverversion",
      E_BOOK_ERROR_INVALID_SERVER_VERSION, 0 },
    { "org.gnome.evolution.dataserver.addressbook.Book.nospace",
      E_BOOK_ERROR_NO_SPACE, 0 },
    { "org.gnome.evolution.dataserver.addressbook.Book.invalidfield",
      E_BOOK_ERROR_INVALID_FIELD, 0 },
    { "org.gnome.evolution.dataserver.addressbook.BookView.invalidquery",
      E_BOOK_VIEW_ERROR_INVALID_QUERY, 0 },
    { "org.gnome.evolution.dataserver.addressbook.BookView.queryrefused",
      E_BOOK_VIEW_ERROR_QUERY_REFUSED, 0 },
  };

  if G_LIKELY (error == NULL)
    return E_BOOK_ERROR_OK;

  if (error->domain == DBUS_GERROR && error->code == DBUS_GERROR_REMOTE_EXCEPTION) {
    const char *error_name;
    GQuark error_quark;
    int i;

    if (G_UNLIKELY (!error_mapping[0].quark)) {
      for (i = 0; i < G_N_ELEMENTS (error_mapping); ++i)
        error_mapping[i].quark = g_quark_from_static_string (error_mapping[i].name);
    }

    error_name = dbus_g_error_get_name (error);
    error_quark = g_quark_try_string (error_name);

    if (error_quark) {
      for (i = 0; i < G_N_ELEMENTS (error_mapping); ++i) {
        if (error_mapping[i].quark == error_quark)
          return error_mapping[i].status;
      }
    }

    g_warning (G_STRLOC ": unmatched error name %s", error_name);
    return E_BOOK_ERROR_OTHER_ERROR;
  } else {
    /* In this case the error was caused by DBus. Dump the message to the
       console as otherwise we have no idea what the problem is. */
    g_warning ("DBus error: %s", error->message);
    return E_BOOK_ERROR_CORBA_EXCEPTION;
  }
}

/**
 * Turn a GList of strings into an array of strings.
 */
static char **
flatten_stringlist (GList *list)
{
  char **array = g_new0 (char *, g_list_length (list) + 1);
  GList *l = list;
  int i = 0;
  while (l != NULL) {
    array[i++] = l->data;
    l = l->next;
  }
  return array;
}

/**
 * Turn an array of strings into a GList.
 */
static GList *
array_to_stringlist (char **list)
{
  GList *l = NULL;
  char **i = list;
  while (*i != NULL) {
    l = g_list_prepend (l, (*i++));
  }
  g_free (list);
  return g_list_reverse(l);
}
