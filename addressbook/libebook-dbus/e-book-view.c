/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
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

#include <glib-object.h>
#include <dbus/dbus-glib.h>
#include "e-book.h"
#include "e-book-view.h"
#include "e-book-view-private.h"
#include "e-data-book-view-bindings.h"
#include "e-book-marshal.h"

G_DEFINE_TYPE(EBookView, e_book_view, G_TYPE_OBJECT);

#define E_BOOK_VIEW_GET_PRIVATE(o) \
  (G_TYPE_INSTANCE_GET_PRIVATE ((o), E_TYPE_BOOK_VIEW, EBookViewPrivate))

struct _EBookViewPrivate {
  EBook *book;
  DBusGProxy *view_proxy;

  gboolean running : 1;
  gboolean freezable : 1;
};

enum {
  CONTACTS_CHANGED,
  CONTACTS_REMOVED,
  CONTACTS_ADDED,
  SEQUENCE_COMPLETE,
  STATUS_MESSAGE,
  LAST_SIGNAL
};
static guint signals [LAST_SIGNAL];

static void
e_book_view_dispose (GObject *object)
{
  EBookView *view = E_BOOK_VIEW (object);

  if (view->priv->view_proxy) {
    org_gnome_evolution_dataserver_addressbook_BookView_dispose (view->priv->view_proxy, NULL);
    g_object_unref (view->priv->view_proxy);
    view->priv->view_proxy = NULL;
  }

  if (view->priv->book) {
    g_object_unref (view->priv->book);
    view->priv->book = NULL;
  }
}

static void
e_book_view_class_init (EBookViewClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = e_book_view_dispose;

  g_type_class_add_private (klass, sizeof (EBookViewPrivate));

  signals [CONTACTS_CHANGED] = g_signal_new ("contacts_changed",
                                             G_OBJECT_CLASS_TYPE (object_class),
                                             G_SIGNAL_RUN_LAST,
                                             G_STRUCT_OFFSET (EBookViewClass, contacts_changed),
                                             NULL, NULL,
                                             e_book_marshal_NONE__POINTER,
                                             G_TYPE_NONE, 1, G_TYPE_POINTER);
  signals [CONTACTS_REMOVED] = g_signal_new ("contacts_removed",
                                             G_OBJECT_CLASS_TYPE (object_class),
                                             G_SIGNAL_RUN_LAST,
                                             G_STRUCT_OFFSET (EBookViewClass, contacts_removed),
                                             NULL, NULL,
                                             e_book_marshal_NONE__POINTER,
                                             G_TYPE_NONE, 1, G_TYPE_POINTER);
  signals [CONTACTS_ADDED] = g_signal_new ("contacts_added",
                                           G_OBJECT_CLASS_TYPE (object_class),
                                           G_SIGNAL_RUN_LAST,
                                           G_STRUCT_OFFSET (EBookViewClass, contacts_added),
                                           NULL, NULL,
                                           e_book_marshal_NONE__POINTER,
                                           G_TYPE_NONE, 1, G_TYPE_POINTER);
  signals [SEQUENCE_COMPLETE] = g_signal_new ("sequence_complete",
                                              G_OBJECT_CLASS_TYPE (object_class),
                                              G_SIGNAL_RUN_LAST,
                                              G_STRUCT_OFFSET (EBookViewClass, sequence_complete),
                                              NULL, NULL,
                                              e_book_marshal_NONE__INT,
                                              G_TYPE_NONE, 1, G_TYPE_INT);
  signals [STATUS_MESSAGE] = g_signal_new ("status_message",
                                           G_OBJECT_CLASS_TYPE (object_class),
                                           G_SIGNAL_RUN_LAST,
                                           G_STRUCT_OFFSET (EBookViewClass, status_message),
                                           NULL, NULL,
                                           e_book_marshal_NONE__STRING,
                                           G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
e_book_view_init (EBookView *view)
{
  EBookViewPrivate *priv = E_BOOK_VIEW_GET_PRIVATE (view);

  priv->book = NULL;
  priv->view_proxy = NULL;
  priv->running = FALSE;

  view->priv = priv;
}

static void
status_message_cb (DBusGProxy *proxy, const char *message, EBookView *book_view)
{
  if (!book_view->priv->running)
    return;

  g_signal_emit (book_view, signals[STATUS_MESSAGE], 0, message);
}

static void
contacts_added_cb (DBusGProxy *proxy, const char **vcards, EBookView *book_view)
{
  const char **p;
  GList *contacts = NULL;

  if (!book_view->priv->running)
    return;

  for (p = vcards; *p; p++) {
    contacts = g_list_prepend (contacts, e_contact_new_from_vcard (*p));
  }
  contacts = g_list_reverse (contacts);

  g_signal_emit (book_view, signals[CONTACTS_ADDED], 0, contacts);

  g_list_foreach (contacts, (GFunc)g_object_unref, NULL);
  g_list_free (contacts);
}

static void
contacts_changed_cb (DBusGProxy *proxy, const char **vcards, EBookView *book_view)
{
  const char **p;
  GList *contacts = NULL;

  if (!book_view->priv->running)
    return;

  for (p = vcards; *p; p++) {
    contacts = g_list_prepend (contacts, e_contact_new_from_vcard (*p));
  }
  contacts = g_list_reverse (contacts);

  g_signal_emit (book_view, signals[CONTACTS_CHANGED], 0, contacts);

  g_list_foreach (contacts, (GFunc)g_object_unref, NULL);
  g_list_free (contacts);
}

static void
contacts_removed_cb (DBusGProxy *proxy, const char **ids, EBookView *book_view)
{
  const char **p;
  GList *list = NULL;

  if (!book_view->priv->running)
    return;

  for (p = ids; *p; p++) {
    list = g_list_prepend (list, (char*)*p);
  }
  list = g_list_reverse (list);

  g_signal_emit (book_view, signals[CONTACTS_REMOVED], 0, list);

  /* No need to free the values, our caller will */
  g_list_free (list);
}

static void
complete_cb (DBusGProxy *proxy, guint status, EBookView *book_view)
{
  if (!book_view->priv->running)
    return;

  g_signal_emit (book_view, signals[SEQUENCE_COMPLETE], 0, status);
}

/**
 * e_book_view_new:
 * @book: an #EBook
 * @view_proxy: The #DBusGProxy to get signals from
 *
 * Creates a new #EBookView based on #EBook and listening to @view_proxy.  This
 * is a private function, applications should call #e_book_get_book_view or
 * #e_book_async_get_book_view.
 *
 * Return value: A new #EBookView.
 **/
EBookView*
e_book_view_new (EBook *book, DBusGProxy *view_proxy)
{
  EBookView *view;
  EBookViewPrivate *priv;

  view = g_object_new (E_TYPE_BOOK_VIEW, NULL);
  priv = view->priv;

  priv->book = g_object_ref (book);

  /* Take ownership of the view_proxy object */
  priv->view_proxy = view_proxy;
  g_object_add_weak_pointer (G_OBJECT (view_proxy), (gpointer)&priv->view_proxy);

  dbus_g_proxy_add_signal (view_proxy, "StatusMessage", G_TYPE_STRING, G_TYPE_INVALID);
  dbus_g_proxy_connect_signal (view_proxy, "StatusMessage", G_CALLBACK (status_message_cb), view, NULL);
  dbus_g_proxy_add_signal (view_proxy, "ContactsAdded", G_TYPE_STRV, G_TYPE_INVALID);
  dbus_g_proxy_connect_signal (view_proxy, "ContactsAdded", G_CALLBACK (contacts_added_cb), view, NULL);
  dbus_g_proxy_add_signal (view_proxy, "ContactsChanged", G_TYPE_STRV, G_TYPE_INVALID);
  dbus_g_proxy_connect_signal (view_proxy, "ContactsChanged", G_CALLBACK (contacts_changed_cb), view, NULL);
  dbus_g_proxy_add_signal (view_proxy, "ContactsRemoved", G_TYPE_STRV, G_TYPE_INVALID);
  dbus_g_proxy_connect_signal (view_proxy, "ContactsRemoved", G_CALLBACK (contacts_removed_cb), view, NULL);
  dbus_g_proxy_add_signal (view_proxy, "Complete", G_TYPE_UINT, G_TYPE_INVALID);
  dbus_g_proxy_connect_signal (view_proxy, "Complete", G_CALLBACK (complete_cb), view, NULL);

  return view;
}

EBook *
e_book_view_get_book (EBookView *book_view)
{
  g_return_val_if_fail (E_IS_BOOK_VIEW (book_view), NULL);

  return book_view->priv->book;
}

/**
 * e_book_view_start:
 * @book_view: an #EBookView
 *
 * Tells @book_view to start processing events.
 */
void
e_book_view_start (EBookView *book_view)
{
  GError *error = NULL;

  g_return_if_fail (E_IS_BOOK_VIEW (book_view));

  book_view->priv->running = TRUE;

  if (book_view->priv->view_proxy) {
    org_gnome_evolution_dataserver_addressbook_BookView_start (book_view->priv->view_proxy, &error);
    if (error) {
      g_warning ("Cannot start book view: %s\n", error->message);

      /* Fake a sequence-complete so that the application knows this failed */
      /* TODO: use get_status_from_error */
      g_signal_emit (book_view, signals[SEQUENCE_COMPLETE], 0,
		     E_BOOK_ERROR_CORBA_EXCEPTION);

      g_error_free (error);
    }
  }
}

/**
 * e_book_view_stop:
 * @book_view: an #EBookView
 *
 * Tells @book_view to stop processing events.
 **/
void
e_book_view_stop (EBookView *book_view)
{
  GError *error = NULL;

  g_return_if_fail (E_IS_BOOK_VIEW (book_view));

  book_view->priv->running = FALSE;

  if (book_view->priv->view_proxy) {
    org_gnome_evolution_dataserver_addressbook_BookView_stop (book_view->priv->view_proxy, &error);
    if (error) {
      g_warning ("Cannot stop book view: %s\n", error->message);
      g_error_free (error);
    }
  }
}

void
e_book_view_set_freezable (EBookView *book_view, gboolean freezable)
{
  GError *error = NULL;

  g_return_if_fail (E_IS_BOOK_VIEW (book_view));

  if (freezable && !e_book_check_static_capability (e_book_view_get_book (book_view), "freezable")) {
    g_warning ("Underlying address book is not freezable");
    book_view->priv->freezable = FALSE;
    return;
  }

  if (book_view->priv->view_proxy) {
    org_gnome_evolution_dataserver_addressbook_BookView_set_freezable (book_view->priv->view_proxy, freezable, &error);

    if (error) {
      g_warning ("Cannot set freezable state: %s\n", error->message);
      g_error_free (error);
    } else {
      book_view->priv->freezable = freezable;
    }
  }
}

gboolean
e_book_view_is_freezable (EBookView *book_view)
{
  g_return_val_if_fail (E_IS_BOOK_VIEW (book_view), FALSE);
  return book_view->priv->freezable;
}

void
e_book_view_thaw (EBookView *book_view)
{
  GError *error = NULL;

  g_return_if_fail (E_IS_BOOK_VIEW (book_view));
  g_return_if_fail (e_book_view_is_freezable (book_view));

  if (book_view->priv->view_proxy) {
    org_gnome_evolution_dataserver_addressbook_BookView_thaw (book_view->priv->view_proxy, &error);
    if (error) {
      g_warning ("Unable to thaw book view: %s\n", error->message);
      g_error_free (error);
    }
  }
}


void
e_book_view_set_sort_order (EBookView *book_view, const gchar *query_term)
{
  GError *error = NULL;

  g_return_if_fail (E_IS_BOOK_VIEW (book_view));

  if (book_view->priv->view_proxy) {
    org_gnome_evolution_dataserver_addressbook_BookView_set_sort_order (book_view->priv->view_proxy, query_term, &error);
    if (error) {
      g_warning ("Unable to set query term on book view: %s\n", error->message);
      g_error_free (error);
    }
  }
}
