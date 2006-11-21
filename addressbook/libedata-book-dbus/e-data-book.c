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

#include <unistd.h>
#include <stdlib.h>
#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-lowlevel.h>
#include <glib/gi18n.h>
#include <glib-object.h>
#include "e-data-book-enumtypes.h"
#include "e-data-book-factory.h"
#include "e-data-book.h"
#include "e-data-book-view.h"
#include "e-book-backend-sexp.h"

extern DBusGConnection *connection;

/* DBus glue */
static gboolean impl_AddressBook_Book_open(EDataBook *book, gboolean only_if_exists, DBusGMethodInvocation *context);
static gboolean impl_AddressBook_Book_remove(EDataBook *book, DBusGMethodInvocation *context);
static gboolean impl_AddressBook_Book_getContact(EDataBook *book, const char *IN_uid, DBusGMethodInvocation *context);
static gboolean impl_AddressBook_Book_getContactList(EDataBook *book, const char *query, DBusGMethodInvocation *context);
static gboolean impl_AddressBook_Book_authenticateUser(EDataBook *book, const char *IN_user, const char *IN_passwd, const char *IN_auth_method, DBusGMethodInvocation *context);
static gboolean impl_AddressBook_Book_addContact(EDataBook *book, const char *IN_vcard, DBusGMethodInvocation *context);
static gboolean impl_AddressBook_Book_modifyContact(EDataBook *book, const char *IN_vcard, DBusGMethodInvocation *context);
static gboolean impl_AddressBook_Book_removeContacts(EDataBook *book, const char **IN_uids, DBusGMethodInvocation *context);
static gboolean impl_AddressBook_Book_getStaticCapabilities(EDataBook *book, char **OUT_capabilities, GError **error);
static gboolean impl_AddressBook_Book_getSupportedFields(EDataBook *book, DBusGMethodInvocation *context);
static gboolean impl_AddressBook_Book_getRequiredFields(EDataBook *book, DBusGMethodInvocation *context);
static gboolean impl_AddressBook_Book_getSupportedAuthMethods(EDataBook *book, DBusGMethodInvocation *context);
static gboolean impl_AddressBook_Book_getBookView (EDataBook *book, const char *search, const char **fields, const guint max_results, DBusGMethodInvocation *context);
static gboolean impl_AddressBook_Book_getChanges(EDataBook *book, const char *IN_change_id, DBusGMethodInvocation *context);
static gboolean impl_AddressBook_Book_cancelOperation(EDataBook *book, GError **error);
static gboolean impl_AddressBook_Book_close(EDataBook *book, DBusGMethodInvocation *context);
#include "e-data-book-glue.h"

static void return_status_and_list (guint32 opid, EDataBookStatus status, GList *list, gboolean free_data);

enum
{
  WRITABLE,
  CONNECTION,
  AUTH_REQUIRED,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

/* Create the EDataBook error quark */
GQuark
e_data_book_error_quark (void)
{
  static GQuark quark = 0;
  if (!quark)
    quark = g_quark_from_static_string ("e_data_book_error");
  return quark;
}


/* Generate the GObject boilerplate */
G_DEFINE_TYPE(EDataBook, e_data_book, G_TYPE_OBJECT)

static void
e_data_book_dispose (GObject *object)
{
  EDataBook *book = E_DATA_BOOK (object);
  
  if (book->backend) {
    g_object_unref (book->backend);
    book->backend = NULL;
  }

  if (book->source) {
    g_object_unref (book->source);
    book->source = NULL;
  }
  
  if (G_OBJECT_CLASS (e_data_book_parent_class)->dispose)
    G_OBJECT_CLASS (e_data_book_parent_class)->dispose (object);	
}

static void
e_data_book_class_init (EDataBookClass *e_data_book_class)
{ 
  GObjectClass *object_class = G_OBJECT_CLASS (e_data_book_class);

  object_class->dispose = e_data_book_dispose;

  signals[WRITABLE] =
    g_signal_new ("writable",
                  G_OBJECT_CLASS_TYPE (e_data_book_class),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  g_cclosure_marshal_VOID__BOOLEAN,
                  G_TYPE_NONE, 1, G_TYPE_BOOLEAN);

  signals[CONNECTION] =
    g_signal_new ("connection",
                  G_OBJECT_CLASS_TYPE (e_data_book_class),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  g_cclosure_marshal_VOID__BOOLEAN,
                  G_TYPE_NONE, 1, G_TYPE_BOOLEAN);

  signals[AUTH_REQUIRED] =
    g_signal_new ("auth-required",
                  G_OBJECT_CLASS_TYPE (e_data_book_class),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  g_cclosure_marshal_VOID__VOID,
                  G_TYPE_NONE, 0);

  dbus_g_object_type_install_info (G_TYPE_FROM_CLASS (e_data_book_class), &dbus_glib_e_data_book_object_info);
  
  dbus_g_error_domain_register (E_DATA_BOOK_ERROR, NULL, E_TYPE_DATA_BOOK_STATUS);
}

/* Instance init */
static void
e_data_book_init (EDataBook *ebook)
{
}

EDataBook *
e_data_book_new (EBookBackend *backend, ESource *source, EDataBookClosedCallback closed_cb)
{
  /* TODO: still need closed_cb? */
  EDataBook *book;
  book = g_object_new (E_TYPE_DATA_BOOK, NULL);
  book->backend = g_object_ref (backend);
  book->source = g_object_ref (source);
  book->closed_cb = closed_cb;
  return book;
}

ESource*
e_data_book_get_source (EDataBook *book)
{
  g_return_val_if_fail (book != NULL, NULL);

  return book->source;
}

EBookBackend*
e_data_book_get_backend (EDataBook *book)
{
  g_return_val_if_fail (book != NULL, NULL);

  return book->backend;
}

static gboolean
impl_AddressBook_Book_open(EDataBook *book, gboolean only_if_exists, DBusGMethodInvocation *context)
{
  e_book_backend_open (book->backend, book, GPOINTER_TO_INT (context), only_if_exists);
  return TRUE;
}

void
e_data_book_respond_open (EDataBook *book, guint opid, EDataBookStatus status)
{
  if (status != Success) {
    dbus_g_method_return_error (GINT_TO_POINTER (opid), g_error_new (E_DATA_BOOK_ERROR, status, _("Cannot open book")));
  } else {
    dbus_g_method_return (GINT_TO_POINTER (opid));
  }
}

static gboolean
impl_AddressBook_Book_remove(EDataBook *book, DBusGMethodInvocation *context)
{
  e_book_backend_remove (book->backend, book, GPOINTER_TO_INT (context));
  return TRUE;
}

void
e_data_book_respond_remove (EDataBook *book, guint opid, EDataBookStatus status)
{
  if (status != Success) {
    dbus_g_method_return_error (GINT_TO_POINTER (opid), g_error_new (E_DATA_BOOK_ERROR, status, _("Cannot remove book")));
  } else {
    dbus_g_method_return (GINT_TO_POINTER (opid));
  }
}

static gboolean
impl_AddressBook_Book_getContact (EDataBook *book, const char *IN_uid, DBusGMethodInvocation *context)
{
  if (IN_uid == NULL) {
    dbus_g_method_return_error (context, g_error_new (E_DATA_BOOK_ERROR, ContactNotFound, _("Cannot get contact")));
    return FALSE;
  }
  
  e_book_backend_get_contact (book->backend, book, GPOINTER_TO_INT (context), IN_uid);
  return TRUE;
}

void
e_data_book_respond_get_contact (EDataBook *book, guint32 opid, EDataBookStatus status, char *vcard)
{
  if (status != Success) {
    dbus_g_method_return_error (GINT_TO_POINTER (opid), g_error_new (E_DATA_BOOK_ERROR, status, _("Cannot get contact")));
  } else {
    dbus_g_method_return (GINT_TO_POINTER (opid), vcard);
  }
}

static gboolean
impl_AddressBook_Book_getContactList(EDataBook *book, const char *query, DBusGMethodInvocation *context)
{
  if (query == NULL) {
    dbus_g_method_return_error (context, g_error_new (E_DATA_BOOK_ERROR, InvalidQuery, _("Empty query")));
    return FALSE;
  }

  e_book_backend_get_contact_list (book->backend, book, GPOINTER_TO_INT (context), query);
  return TRUE;
}

void
e_data_book_respond_get_contact_list (EDataBook *book, guint32 opid, EDataBookStatus status, GList *cards)
{
  return_status_and_list (opid, status, cards, TRUE);
}

static gboolean
impl_AddressBook_Book_authenticateUser(EDataBook *book, const char *IN_user, const char *IN_passwd, const char *IN_auth_method, DBusGMethodInvocation *context)
{
  e_book_backend_authenticate_user (e_data_book_get_backend (book), book,
                                    GPOINTER_TO_INT (context), IN_user, IN_passwd, IN_auth_method);

  return TRUE;
}

void
e_data_book_respond_authenticate_user (EDataBook *book, guint32 opid, EDataBookStatus status)
{
  if (status != Success) {
    dbus_g_method_return_error (GINT_TO_POINTER (opid), g_error_new (E_DATA_BOOK_ERROR, status, _("Cannot authenticate user")));
  } else {
    dbus_g_method_return (GINT_TO_POINTER (opid));
  }
}

static gboolean
impl_AddressBook_Book_addContact(EDataBook *book, const char *IN_vcard, DBusGMethodInvocation *context)
{
  if (IN_vcard == NULL) {
    dbus_g_method_return_error (context, g_error_new (E_DATA_BOOK_ERROR, InvalidQuery, _("Cannot add contact")));
    return FALSE;
  }

  e_book_backend_create_contact (e_data_book_get_backend (book), book,
                                 GPOINTER_TO_INT (context), IN_vcard);

  return TRUE;
}

void
e_data_book_respond_create (EDataBook *book, guint32 opid, EDataBookStatus status, EContact *contact)
{
  if (status != Success) {
    dbus_g_method_return_error (GINT_TO_POINTER (opid), g_error_new (E_DATA_BOOK_ERROR, status, _("Cannot add contact")));
  } else {
    e_book_backend_notify_update (e_data_book_get_backend (book), contact);
    e_book_backend_notify_complete (e_data_book_get_backend (book));

    dbus_g_method_return (GINT_TO_POINTER (opid), e_contact_get_const (contact, E_CONTACT_UID));
  }
}

static gboolean
impl_AddressBook_Book_modifyContact(EDataBook *book, const char *IN_vcard, DBusGMethodInvocation *context)
{
  if (IN_vcard == NULL) {
    dbus_g_method_return_error (context, g_error_new (E_DATA_BOOK_ERROR, InvalidQuery, _("Cannot modify contact")));
    return FALSE;
  }

  e_book_backend_modify_contact (e_data_book_get_backend (book), book,
                                 GPOINTER_TO_INT (context), IN_vcard);

  return TRUE;
}

void
e_data_book_respond_modify (EDataBook *book, guint32 opid, EDataBookStatus status, EContact *contact)
{
  if (status != Success) {
    dbus_g_method_return_error (GINT_TO_POINTER (opid), g_error_new (E_DATA_BOOK_ERROR, status, _("Cannot modify contact")));
  } else {
    e_book_backend_notify_update (e_data_book_get_backend (book), contact);
    e_book_backend_notify_complete (e_data_book_get_backend (book));

    dbus_g_method_return (GINT_TO_POINTER (opid));
  }
}

static gboolean
impl_AddressBook_Book_removeContacts(EDataBook *book, const char **IN_uids, DBusGMethodInvocation *context)
{
  GList *id_list = NULL;
  int i = 0;

  /* Allow an empty array to be removed */
  if (IN_uids == NULL) {
    dbus_g_method_return (context);
    return TRUE;
  }

  while (IN_uids[i] != NULL) {
    id_list = g_list_prepend (id_list, (gpointer) IN_uids[i]);
    i++;
  }
  
  e_book_backend_remove_contacts (e_data_book_get_backend (book), book, 
                                  GPOINTER_TO_INT (context), id_list);
  g_list_free (id_list);

  return TRUE;
}

void
e_data_book_respond_remove_contacts (EDataBook *book, guint32 opid, EDataBookStatus status, GList *ids)
{
  if (status != Success) {
    dbus_g_method_return_error (GINT_TO_POINTER (opid), g_error_new (E_DATA_BOOK_ERROR, status, _("Cannot remove contacts")));
  } else {
    GList *i;
    
    for (i = ids; i; i = i->next)
      e_book_backend_notify_remove (e_data_book_get_backend (book), i->data);
    e_book_backend_notify_complete (e_data_book_get_backend (book));

    dbus_g_method_return (GINT_TO_POINTER (opid));
  }
}

static gboolean
impl_AddressBook_Book_getStaticCapabilities(EDataBook *book, char **OUT_capabilities, GError **error)
{
  *OUT_capabilities = e_book_backend_get_static_capabilities (e_data_book_get_backend (book));
  return TRUE;
}

static gboolean
impl_AddressBook_Book_getSupportedFields(EDataBook *book, DBusGMethodInvocation *context)
{
  e_book_backend_get_supported_fields (e_data_book_get_backend (book), book, GPOINTER_TO_INT (context));
  return TRUE;
}

void
e_data_book_respond_get_supported_fields (EDataBook *book, guint32 opid, EDataBookStatus status, GList *fields)
{
  return_status_and_list (opid, status, fields, FALSE);
}

static gboolean
impl_AddressBook_Book_getRequiredFields(EDataBook *book, DBusGMethodInvocation *context)
{
  e_book_backend_get_required_fields (e_data_book_get_backend (book), book, GPOINTER_TO_INT (context));
  return TRUE;
}

void
e_data_book_respond_get_required_fields (EDataBook *book, guint32 opid, EDataBookStatus status, GList *fields)
{
  return_status_and_list (opid, status, fields, FALSE);
}

static gboolean
impl_AddressBook_Book_getSupportedAuthMethods(EDataBook *book, DBusGMethodInvocation *context)
{
  e_book_backend_get_supported_auth_methods (e_data_book_get_backend (book), book, GPOINTER_TO_INT (context));
  return TRUE;
}

void
e_data_book_respond_get_supported_auth_methods (EDataBook *book, guint32 opid, EDataBookStatus status, GList *auth_methods)
{
  return_status_and_list (opid, status, auth_methods, FALSE);
}

static char*
construct_bookview_path (void)
{
  static guint counter = 1; /* TODO: add mutex for possible future threading */
  return g_strdup_printf ("/org/gnome/evolution/dataserver/addressbook/BookView/%d/%d", getpid(), counter++);
}

static gboolean
impl_AddressBook_Book_getBookView (EDataBook *book, const char *search, const char **fields, const guint max_results, DBusGMethodInvocation *context)
{
  EBookBackend *backend = e_data_book_get_backend (book);
  EBookBackendSExp *card_sexp;
  EDataBookView *book_view;
  char *path;

  card_sexp = e_book_backend_sexp_new (search);
  if (!card_sexp) {
    dbus_g_method_return_error (context, g_error_new (E_DATA_BOOK_ERROR, InvalidQuery, _("Invalid query")));
    return FALSE;
  }

  path = construct_bookview_path ();
  book_view = e_data_book_view_new (book, path, search, card_sexp, max_results);

  e_book_backend_add_book_view (backend, book_view);

  dbus_g_method_return (context, path);
  g_free (path);
  return TRUE;
}

static gboolean
impl_AddressBook_Book_getChanges(EDataBook *book, const char *IN_change_id, DBusGMethodInvocation *context)
{
  e_book_backend_get_changes (e_data_book_get_backend (book), book, GPOINTER_TO_INT (context), IN_change_id);
  return TRUE;
}

void
e_data_book_respond_get_changes (EDataBook *book, guint32 opid, EDataBookStatus status, GList *changes)
{
  char **list;
  int i = 0;
  /* TODO: can probably assume that on !Success list is always NULL */
  /* DBus wants a char**, so convert */
  list = g_new0 (char*, g_list_length (changes) + 1);
  while (changes != NULL) {
    EDataBookChange *change = (EDataBookChange *) changes->data;
    list[i++] = g_strdup_printf ("%d\n%s", change->change_type, change->vcard);
    g_free (change->vcard);
    g_free (change);
    changes = g_list_remove (changes, change);
  }
  if (status != Success) {
    dbus_g_method_return_error (GINT_TO_POINTER (opid), g_error_new (E_DATA_BOOK_ERROR, status, _("Cannot get changes")));
  } else {
    dbus_g_method_return (GINT_TO_POINTER (opid), list);
  }
  g_strfreev (list);
}

static gboolean
impl_AddressBook_Book_cancelOperation(EDataBook *book, GError **error)
{
  if (!e_book_backend_cancel_operation (e_data_book_get_backend (book), book)) {
    g_set_error (error, E_DATA_BOOK_ERROR, CouldNotCancel, "Failed to cancel operation");
    return FALSE;
  }

  return TRUE;
}

static gboolean
impl_AddressBook_Book_close(EDataBook *book, DBusGMethodInvocation *context)
{
  char *sender;

  sender = dbus_g_method_get_sender (context);

  book->closed_cb (book, sender);

  g_free (sender);

  g_object_unref (book);

  dbus_g_method_return (context);

  return TRUE;
}

void
e_data_book_report_writable (EDataBook *book, gboolean writable)
{
  g_return_if_fail (book != NULL);

  g_signal_emit (book, signals[WRITABLE], 0, writable);
}

void
e_data_book_report_connection_status (EDataBook *book, gboolean connected)
{
  g_return_if_fail (book != NULL);

  g_signal_emit (book, signals[CONNECTION], 0, connected);
}

void
e_data_book_report_auth_required (EDataBook *book)
{
  g_return_if_fail (book != NULL);

  g_signal_emit (book, signals[AUTH_REQUIRED], 0);
}

static void
return_status_and_list (guint32 opid, EDataBookStatus status, GList *list, gboolean free_data)
{
  if (status == Success) {
    char **array;
    GList *l;
    int i = 0;
    
    array = g_new0 (char*, g_list_length (list) + 1);
    for (l = list; l != NULL; l = l->next) {
      array[i++] = l->data;
    }
    
    dbus_g_method_return (GINT_TO_POINTER (opid), array);
    
    if (free_data) {
      g_strfreev (array);
    } else {
      g_free (array);
    }
  } else {
    dbus_g_method_return_error (GINT_TO_POINTER (opid), g_error_new (E_DATA_BOOK_ERROR, status, _("Cannot complete operation")));
  }
}
