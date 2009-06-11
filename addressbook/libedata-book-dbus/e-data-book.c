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
#include "opid.h"

#define GET_PRIV(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), E_TYPE_DATA_BOOK, EDataBookPrivate))

extern DBusGConnection *connection;

/* DBus glue */
static void impl_AddressBook_Book_open(EDataBook *book, gboolean only_if_exists, DBusGMethodInvocation *context);
static void impl_AddressBook_Book_remove(EDataBook *book, DBusGMethodInvocation *context);
static void impl_AddressBook_Book_getContact(EDataBook *book, const char *IN_uid, DBusGMethodInvocation *context);
static void impl_AddressBook_Book_getContactList(EDataBook *book, const char *query, DBusGMethodInvocation *context);
static void impl_AddressBook_Book_authenticateUser(EDataBook *book, const char *IN_user, const char *IN_passwd, const char *IN_auth_method, DBusGMethodInvocation *context);
static void impl_AddressBook_Book_addContact(EDataBook *book, const char *IN_vcard, DBusGMethodInvocation *context);
static void impl_AddressBook_Book_addContacts(EDataBook *book, const char **IN_vcards, DBusGMethodInvocation *context);
static void impl_AddressBook_Book_modifyContact(EDataBook *book, const char *IN_vcard, DBusGMethodInvocation *context);
static void impl_AddressBook_Book_modifyContacts(EDataBook *book, const char **IN_vcards, DBusGMethodInvocation *context);
static void impl_AddressBook_Book_removeContacts(EDataBook *book, const char **IN_uids, DBusGMethodInvocation *context);
static void impl_AddressBook_Book_removeAllContacts(EDataBook *book, DBusGMethodInvocation *context);
static gboolean impl_AddressBook_Book_getStaticCapabilities(EDataBook *book, char **OUT_capabilities, GError **error);
static void impl_AddressBook_Book_getSupportedFields(EDataBook *book, DBusGMethodInvocation *context);
static void impl_AddressBook_Book_getRequiredFields(EDataBook *book, DBusGMethodInvocation *context);
static void impl_AddressBook_Book_getSupportedAuthMethods(EDataBook *book, DBusGMethodInvocation *context);
static void impl_AddressBook_Book_getBookView (EDataBook *book, const char *search, const guint max_results, DBusGMethodInvocation *context);
static void impl_AddressBook_Book_getChanges(EDataBook *book, const char *IN_change_id, DBusGMethodInvocation *context);
static gboolean impl_AddressBook_Book_cancelOperation(EDataBook *book, GError **error);
static void impl_AddressBook_Book_close(EDataBook *book, DBusGMethodInvocation *context);
#include "e-data-book-glue.h"

static void return_status_and_list (guint32 opid, EDataBookStatus status, GList *list, gboolean free_data);

#define g_idle_add(_func, _user_data) g_idle_add_full(G_PRIORITY_HIGH_IDLE, _func, _user_data, NULL)

/* Infrastructure for returning an error in an idle handler via DBus */
typedef struct
{
    DBusGMethodInvocation *context;
    GError *error;
} DBusErrorReturnData;

static DBusErrorReturnData*
dbus_error_return_data_new (DBusGMethodInvocation *context,
                            GError *error)
{
    DBusErrorReturnData* d = g_new0 (DBusErrorReturnData, 1);
    d->context = context;
    d->error = error;
    return d;
}

static void
dbus_error_return_data_free (DBusErrorReturnData *data)
{
    g_error_free (data->error);
    g_free (data);
}

static gboolean
idle_dbus_return_error (gpointer user_data)
{
    DBusErrorReturnData *data = (DBusErrorReturnData*) user_data;
    dbus_g_method_return_error (data->context, data->error);
    dbus_error_return_data_free (data);

    return FALSE;
}

/* Infrastructure for returning a void in an idle handler via DBus */
static gboolean
idle_dbus_method_return (gpointer user_data)
{
    DBusGMethodInvocation *context = (DBusGMethodInvocation*) user_data;
    dbus_g_method_return (context);

    return FALSE;
}

/* Infrastructure for returning a string in an idle handler via DBus */
typedef struct {
    DBusGMethodInvocation *context;
    char *str;
} DBusMethodReturnStrData;

static DBusMethodReturnStrData*
dbus_method_return_str_data_new (DBusGMethodInvocation *context,
                                 char *str)
{
    DBusMethodReturnStrData* d = g_new0 (DBusMethodReturnStrData, 1);
    d->context = context;
    d->str = str;

    return d;
}

static void
dbus_method_return_str_data_free (DBusMethodReturnStrData *data)
{
    g_free (data->str);
    g_free (data);
}

static gboolean
idle_dbus_method_return_str (gpointer user_data)
{
    DBusMethodReturnStrData *data = (DBusMethodReturnStrData*) user_data;
    dbus_g_method_return (data->context, data->str);
    dbus_method_return_str_data_free (data);

    return FALSE;
}

/* Infrastructure for returning a char** in an idle handler via DBus */
typedef struct {
    DBusGMethodInvocation *context;
    char **array;
} DBusMethodReturnStrarrayData;

static DBusMethodReturnStrarrayData*
dbus_method_return_strarray_data_new (DBusGMethodInvocation *context,
                                      char **array)
{
    DBusMethodReturnStrarrayData* d =
        g_new0 (DBusMethodReturnStrarrayData, 1);
    d->context = context;
    d->array = array;

    return d;
}

static void
dbus_method_return_strarray_data_free (DBusMethodReturnStrarrayData *data)
{
    g_strfreev (data->array);
    g_free (data);
}

static gboolean
idle_dbus_method_return_strarray (gpointer user_data)
{
    DBusMethodReturnStrarrayData *data = (DBusMethodReturnStrarrayData*) user_data;
    dbus_g_method_return (data->context, data->array);
    dbus_method_return_strarray_data_free (data);

    return FALSE;
}

/* Infrastructure for returning a GPtrArray in an idle handler via DBus */
typedef struct {
    DBusGMethodInvocation *context;
    GPtrArray *array;
} DBusMethodReturnPtrarrayData;

static DBusMethodReturnPtrarrayData*
dbus_method_return_ptrarray_data_new (DBusGMethodInvocation *context,
                                      GPtrArray *array)
{
    DBusMethodReturnPtrarrayData* d =
        g_new0 (DBusMethodReturnPtrarrayData, 1);
    d->context = context;
    d->array = array;

    return d;
}

static void
dbus_method_return_ptrarray_data_free (DBusMethodReturnPtrarrayData *data)
{
    g_ptr_array_foreach (data->array, (GFunc)g_value_array_free, NULL);
    g_ptr_array_free (data->array, TRUE);

    g_free (data);
}

static gboolean
idle_dbus_method_return_ptrarray (gpointer user_data)
{
    DBusMethodReturnPtrarrayData *data = (DBusMethodReturnPtrarrayData*) user_data;
    dbus_g_method_return (data->context, data->array);
    dbus_method_return_ptrarray_data_free (data);

    return FALSE;
}

typedef struct {
    DBusGMethodInvocation *context;
} DBusReturnData;

enum {
  WRITABLE,
  CONNECTION,
  AUTH_REQUIRED,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

static GThreadPool *op_pool;

typedef enum {
  OP_OPEN,
  OP_AUTHENTICATE,
  OP_ADD_CONTACT,
  OP_ADD_CONTACTS,
  OP_GET_CONTACT,
  OP_GET_CONTACTS,
  OP_MODIFY_CONTACT,
  OP_MODIFY_CONTACTS,
  OP_REMOVE_CONTACTS,
  OP_REMOVE_ALL_CONTACTS,
  OP_GET_CHANGES,
} OperationID;

typedef struct {
  OperationID op;
  guint32 id; /* operation id */
  EDataBook *book; /* book */
  union {
    /* OP_OPEN */
    gboolean only_if_exists;
    /* OP_AUTHENTICATE */
    struct {
      char *username;
      char *password;
      char *method;
    } auth;
    /* OP_ADD_CONTACT */
    /* OP_MODIFY_CONTACT */
    char *vcard;
    /* OP_GET_CONTACT */
    char *uid;
    /* OP_GET_CONTACTS */
    char *query;
    /* OP_MODIFY_CONTACT */
    char **vcards;
    /* OP_REMOVE_CONTACTS */
    GList *ids;
    /* OP_GET_CHANGES */
    char *change_id;
  };
} OperationData;

typedef struct {
  EDataBookClosedCallback closed_cb;
} EDataBookPrivate;

static void
operation_thread (gpointer data, gpointer user_data)
{
  OperationData *op = data;
  EBookBackend *backend;

  backend = e_data_book_get_backend (op->book);

  switch (op->op) {
  case OP_OPEN:
    e_book_backend_open (backend, op->book, op->id, op->only_if_exists);
    break;
  case OP_AUTHENTICATE:
    e_book_backend_authenticate_user (backend, op->book, op->id,
                                      op->auth.username,
                                      op->auth.password,
                                      op->auth.method);
    g_free (op->auth.username);
    g_free (op->auth.password);
    g_free (op->auth.method);
    break;
  case OP_ADD_CONTACT:
    e_book_backend_create_contact (backend, op->book, op->id, op->vcard);
    g_free (op->vcard);
    break;
  case OP_ADD_CONTACTS:
    e_book_backend_create_contacts (backend, op->book, op->id, (const char**)op->vcards);
    g_strfreev (op->vcards);
    break;
  case OP_GET_CONTACT:
    e_book_backend_get_contact (backend, op->book, op->id, op->uid);
    g_free (op->uid);
    break;
  case OP_GET_CONTACTS:
    e_book_backend_get_contact_list (backend, op->book, op->id, op->query);
    g_free (op->query);
    break;
  case OP_MODIFY_CONTACT:
    e_book_backend_modify_contact (backend, op->book, op->id, op->vcard);
    g_free (op->vcard);
    break;
  case OP_MODIFY_CONTACTS:
    /* C is weird at times, need to cast to const char** */
    e_book_backend_modify_contacts (backend, op->book, op->id, (const char**)op->vcards);
    g_strfreev (op->vcards);
    break;
  case OP_REMOVE_CONTACTS:
    e_book_backend_remove_contacts (backend, op->book, op->id, op->ids);
    g_list_foreach (op->ids, (GFunc)g_free, NULL);
    g_list_free (op->ids);
    break;
  case OP_REMOVE_ALL_CONTACTS:
    e_book_backend_remove_all_contacts (backend, op->book, op->id);
    break;
  case OP_GET_CHANGES:
    e_book_backend_get_changes (backend, op->book, op->id, op->change_id);
    g_free (op->change_id);
    break;
  }

  g_object_unref (op->book);
  g_slice_free (OperationData, op);
}

static OperationData *
op_new (OperationID op, EDataBook *book, DBusGMethodInvocation *context)
{
  OperationData *data;

  data = g_slice_new0 (OperationData);
  data->op = op;
  data->book = g_object_ref (book);
  data->id = opid_store (context);

  return data;
}


/* Create the EDataBook error quark */
GQuark
e_data_book_error_quark (void)
{
  static GQuark quark = 0;
  if (G_UNLIKELY (quark == 0))
    quark = g_quark_from_static_string ("e-data-book-error");
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

  g_type_class_add_private (e_data_book_class, sizeof (EDataBookPrivate));

  dbus_g_object_type_install_info (G_TYPE_FROM_CLASS (e_data_book_class), &dbus_glib_e_data_book_object_info);

  dbus_g_error_domain_register (E_DATA_BOOK_ERROR, NULL, E_TYPE_DATA_BOOK_STATUS);

  op_pool = g_thread_pool_new (operation_thread, NULL, 10, FALSE, NULL);
  /* Kill threads which don't do anything for 10 seconds */
  g_thread_pool_set_max_idle_time (10 * 1000);
}

/* Instance init */
static void
e_data_book_init (EDataBook *ebook)
{
}

EDataBook *
e_data_book_new (EBookBackend *backend, ESource *source, EDataBookClosedCallback closed_cb)
{
  EDataBookPrivate *priv;
  EDataBook *book;

  book = g_object_new (E_TYPE_DATA_BOOK, NULL);
  priv = GET_PRIV (book);

  book->backend = g_object_ref (backend);
  book->source = g_object_ref (source);
  priv->closed_cb = closed_cb;

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

static void
impl_AddressBook_Book_open(EDataBook *book, gboolean only_if_exists, DBusGMethodInvocation *context)
{
  OperationData *op;

  op = op_new (OP_OPEN, book, context);
  op->only_if_exists = only_if_exists;
  g_thread_pool_push (op_pool, op, NULL);
}

void
e_data_book_respond_open (EDataBook *book, guint opid, EDataBookStatus status)
{
  DBusGMethodInvocation *context = opid_fetch (opid);

  if (status != Success) {
    DBusErrorReturnData *data =
        dbus_error_return_data_new (context,
                                    g_error_new (E_DATA_BOOK_ERROR, status,
                                                 _("Cannot open book")));
    g_idle_add (idle_dbus_return_error, data);
  } else {
    g_idle_add (idle_dbus_method_return, context);
  }
}

static void
impl_AddressBook_Book_remove(EDataBook *book, DBusGMethodInvocation *context)
{
  e_book_backend_remove (book->backend, book, opid_store (context));
}

void
e_data_book_respond_remove (EDataBook *book, guint opid, EDataBookStatus status)
{
  DBusGMethodInvocation *context = opid_fetch (opid);

  if (status != Success) {
    GError *error = g_error_new (E_DATA_BOOK_ERROR, status, _("Cannot remove book"));
    DBusErrorReturnData *data = dbus_error_return_data_new (context, error);
    g_idle_add (idle_dbus_return_error, data);
  } else {
    g_idle_add (idle_dbus_method_return, context);
  }
}

static void
impl_AddressBook_Book_getContact (EDataBook *book, const char *IN_uid, DBusGMethodInvocation *context)
{
  OperationData *op;

  if (IN_uid == NULL) {
    GError *error = g_error_new (E_DATA_BOOK_ERROR, ContactNotFound, _("Cannot get contact"));
    DBusErrorReturnData *data = dbus_error_return_data_new (context, error);
    g_idle_add (idle_dbus_return_error, data);
    return;
  }

  op = op_new (OP_GET_CONTACT, book, context);
  op->uid = g_strdup (IN_uid);
  g_thread_pool_push (op_pool, op, NULL);
}

void
e_data_book_respond_get_contact (EDataBook *book, guint32 opid, EDataBookStatus status, char *vcard)
{
  DBusGMethodInvocation *context = opid_fetch (opid);

  if (status != Success) {
    GError *error = g_error_new (E_DATA_BOOK_ERROR, status, _("Cannot get contact"));
    DBusErrorReturnData *data = dbus_error_return_data_new (context, error);
    g_idle_add (idle_dbus_return_error, data);
  } else {
    g_idle_add (idle_dbus_method_return_str,
                dbus_method_return_str_data_new (context, g_strdup (vcard)));
  }
}

static void
impl_AddressBook_Book_getContactList (EDataBook *book, const char *query, DBusGMethodInvocation *context)
{
  OperationData *op;

  if (query == NULL || query[0] == '\0') {
    GError *error = g_error_new (E_DATA_BOOK_ERROR, InvalidQuery, _("Empty query"));
    DBusErrorReturnData *data = dbus_error_return_data_new (context, error);
    g_idle_add (idle_dbus_return_error, data);
    return;
  }

  op = op_new (OP_GET_CONTACTS, book, context);
  op->query = g_strdup (query);
  g_thread_pool_push (op_pool, op, NULL);
}

void
e_data_book_respond_get_contact_list (EDataBook *book, guint32 opid, EDataBookStatus status, GList *cards)
{
  return_status_and_list (opid, status, cards, TRUE);
}

static void
impl_AddressBook_Book_authenticateUser(EDataBook *book, const char *IN_user, const char *IN_passwd, const char *IN_auth_method, DBusGMethodInvocation *context)
{
  OperationData *op;

  op = op_new (OP_AUTHENTICATE, book, context);
  op->auth.username = g_strdup (IN_user);
  op->auth.password = g_strdup (IN_passwd);
  op->auth.method = g_strdup (IN_auth_method);
  g_thread_pool_push (op_pool, op, NULL);
}

void
e_data_book_respond_authenticate_user (EDataBook *book, guint32 opid, EDataBookStatus status)
{
  DBusGMethodInvocation *context = opid_fetch (opid);

  if (status != Success) {
    GError *error = g_error_new (E_DATA_BOOK_ERROR, status, _("Cannot authenticate user"));
    DBusErrorReturnData *data = dbus_error_return_data_new (context, error);
    g_idle_add (idle_dbus_return_error, data);
  } else {
    g_idle_add (idle_dbus_method_return, context);
  }
}

static void
impl_AddressBook_Book_addContact (EDataBook *book, const char *IN_vcard, DBusGMethodInvocation *context)
{
  OperationData *op;

  if (IN_vcard == NULL || IN_vcard[0] == '\0') {
    GError *error = g_error_new (E_DATA_BOOK_ERROR, InvalidQuery, _("Cannot add contact"));
    DBusErrorReturnData *data = dbus_error_return_data_new (context, error);
    g_idle_add (idle_dbus_return_error, data);
    return;
  }

  op = op_new (OP_ADD_CONTACT, book, context);
  op->vcard = g_strdup (IN_vcard);
  g_thread_pool_push (op_pool, op, NULL);
}

void
e_data_book_respond_create (EDataBook *book, guint32 opid, EDataBookStatus status, EContact *contact)
{
  DBusGMethodInvocation *context = opid_fetch (opid);

  if (status != Success) {
    GError *error = g_error_new (E_DATA_BOOK_ERROR, status, _("Cannot add contact"));
    DBusErrorReturnData *data = dbus_error_return_data_new (context, error);
    g_idle_add (idle_dbus_return_error, data);
  } else {
    e_book_backend_notify_update (e_data_book_get_backend (book), contact);
    e_book_backend_notify_complete (e_data_book_get_backend (book));

    g_idle_add (idle_dbus_method_return_str,
                dbus_method_return_str_data_new (context,
                                                 e_contact_get (contact,
                                                                E_CONTACT_UID)));
  }
}

static void
impl_AddressBook_Book_addContacts(EDataBook *book, const char **IN_vcards, DBusGMethodInvocation *context)
{
  OperationData *op;

  if (IN_vcards == NULL || IN_vcards[0] == NULL) {
    GError *error = g_error_new (E_DATA_BOOK_ERROR, InvalidQuery, _("Cannot add contacts"));
    DBusErrorReturnData *data = dbus_error_return_data_new (context, error);
    g_idle_add (idle_dbus_return_error, data);
    return;
  }

  op = op_new (OP_ADD_CONTACTS, book, context);
  op->vcards = g_strdupv ((char**)IN_vcards);
  g_thread_pool_push (op_pool, op, NULL);
}

void
e_data_book_respond_create_contacts (EDataBook *book, guint32 opid, EDataBookStatus status, GList *contacts)
{
  DBusGMethodInvocation *context = opid_fetch (opid);
  GList *contact_list = contacts;

  if (status != Success) {
    GError *error = g_error_new (E_DATA_BOOK_ERROR, status, _("Cannot add contacts"));
    DBusErrorReturnData *data = dbus_error_return_data_new (context, error);
    g_idle_add (idle_dbus_return_error, data);
  } else {
    char **uids;
    int i = 0;

    uids = g_new0 (char *, g_list_length (contacts)+1);
    for (; contacts; contacts = contacts->next) {
      if (!E_IS_CONTACT (contacts->data)) {
        g_warning ("%s: not a contact", G_STRFUNC);
        continue;
      }
      /* we are freeing uids in idle_dbus_method_return_strarray */
      uids[i++] = (char *) g_strdup (e_contact_get_const (contacts->data, E_CONTACT_UID));
    }

    /* send the status to sender first */
    g_idle_add (idle_dbus_method_return_strarray,
                dbus_method_return_strarray_data_new (context, uids));

    /* then notify all views */
    for (contacts = contact_list; contacts; contacts = contacts->next) {
      if (!E_IS_CONTACT (contacts->data)) {
        g_warning ("%s: not a contact", G_STRFUNC);
        continue;
      }
      e_book_backend_notify_update (e_data_book_get_backend (book), contacts->data);
    }
    e_book_backend_notify_complete (e_data_book_get_backend (book));
  }
}

static void
impl_AddressBook_Book_modifyContact (EDataBook *book, const char *IN_vcard, DBusGMethodInvocation *context)
{
  OperationData *op;

  if (IN_vcard == NULL) {
    GError *error = g_error_new (E_DATA_BOOK_ERROR, InvalidQuery, _("Cannot modify contact"));
    DBusErrorReturnData *data = dbus_error_return_data_new (context, error);
    g_idle_add (idle_dbus_return_error, data);
    return;
  }

  op = op_new (OP_MODIFY_CONTACT, book, context);
  op->vcard = g_strdup (IN_vcard);
  g_thread_pool_push (op_pool, op, NULL);
}

void
e_data_book_respond_modify (EDataBook *book, guint32 opid, EDataBookStatus status, EContact *contact)
{
  DBusGMethodInvocation *context = opid_fetch (opid);

  if (status != Success) {
    GError *error = g_error_new (E_DATA_BOOK_ERROR, status, _("Cannot modify contact"));
    DBusErrorReturnData *data = dbus_error_return_data_new (context, error);
    g_idle_add (idle_dbus_return_error, data);
  } else {
    e_book_backend_notify_update (e_data_book_get_backend (book), contact);
    e_book_backend_notify_complete (e_data_book_get_backend (book));

    g_idle_add (idle_dbus_method_return, context);
  }
}

static void
impl_AddressBook_Book_modifyContacts(EDataBook *book, const char **IN_vcards, DBusGMethodInvocation *context)
{
  OperationData *op;

  if (IN_vcards == NULL || IN_vcards[0] == NULL) {
    GError *error = g_error_new (E_DATA_BOOK_ERROR, InvalidQuery, _("Cannot modify contacts"));
    DBusErrorReturnData *data = dbus_error_return_data_new (context, error);
    g_idle_add (idle_dbus_return_error, data);
    return;
  }

  op = op_new (OP_MODIFY_CONTACTS, book, context);
  op->vcards = g_strdupv ((char**)IN_vcards);
  g_thread_pool_push (op_pool, op, NULL);
}

void
e_data_book_respond_modify_contacts (EDataBook *book, guint32 opid, EDataBookStatus status, GList *contacts)
{
  DBusGMethodInvocation *context = opid_fetch (opid);

  if (status != Success) {
    GError *error = g_error_new (E_DATA_BOOK_ERROR, status, _("Cannot modify contacts"));
    DBusErrorReturnData *data = dbus_error_return_data_new (context, error);
    g_idle_add (idle_dbus_return_error, data);
  } else {

    /* notify sender */
    g_idle_add (idle_dbus_method_return, context);

    /* notify book-views */
    for (; contacts; contacts = contacts->next) {
      e_book_backend_notify_update (e_data_book_get_backend (book), contacts->data);
    }
    e_book_backend_notify_complete (e_data_book_get_backend (book));
  }
}

static void
impl_AddressBook_Book_removeContacts(EDataBook *book, const char **IN_uids, DBusGMethodInvocation *context)
{
  OperationData *op;

  /* Allow an empty array to be removed */
  if (IN_uids == NULL) {
    g_idle_add (idle_dbus_method_return, context);
    return;
  }

  op = op_new (OP_REMOVE_CONTACTS, book, context);

  for (; *IN_uids; IN_uids++) {
    op->ids = g_list_prepend (op->ids, g_strdup (*IN_uids));
  }

  g_thread_pool_push (op_pool, op, NULL);
}

static void
impl_AddressBook_Book_removeAllContacts(EDataBook *book, DBusGMethodInvocation *context)
{
  OperationData *op;

  op = op_new (OP_REMOVE_ALL_CONTACTS, book, context);

  g_thread_pool_push (op_pool, op, NULL);
}

void
e_data_book_respond_remove_contacts (EDataBook *book, guint32 opid, EDataBookStatus status, GList *ids)
{
  DBusGMethodInvocation *context = opid_fetch (opid);

  if (status != Success) {
    GError *error = g_error_new (E_DATA_BOOK_ERROR, status, _("Cannot remove contacts"));
    DBusErrorReturnData *data = dbus_error_return_data_new (context, error);
    g_idle_add (idle_dbus_return_error, data);
  } else {
    GList *i;

    /* return to sender first */
    g_idle_add (idle_dbus_method_return, context);

    /* notify views after */
    for (i = ids; i; i = i->next)
      e_book_backend_notify_remove (e_data_book_get_backend (book), i->data);
    e_book_backend_notify_complete (e_data_book_get_backend (book));
  }
}

void
e_data_book_respond_remove_all_contacts (EDataBook *book, guint32 opid, EDataBookStatus status, GList *ids)
{
  DBusGMethodInvocation *context = opid_fetch (opid);

  if (status != Success) {
    GError *error = g_error_new (E_DATA_BOOK_ERROR, status, _("Cannot remove all contacts"));
    DBusErrorReturnData *data = dbus_error_return_data_new (context, error);
    g_idle_add (idle_dbus_return_error, data);
  } else {
    GList *i;

    /* return to sender first */
    g_idle_add (idle_dbus_method_return, context);

    /* notify views after */
    for (i = ids; i; i = i->next)
      e_book_backend_notify_remove (e_data_book_get_backend (book), i->data);
    e_book_backend_notify_complete (e_data_book_get_backend (book));
  }
}

static gboolean
impl_AddressBook_Book_getStaticCapabilities(EDataBook *book, char **OUT_capabilities, GError **error)
{
  *OUT_capabilities = e_book_backend_get_static_capabilities (e_data_book_get_backend (book));
  return TRUE;
}

static void
impl_AddressBook_Book_getSupportedFields(EDataBook *book, DBusGMethodInvocation *context)
{
  e_book_backend_get_supported_fields (e_data_book_get_backend (book), book, opid_store (context));
}

void
e_data_book_respond_get_supported_fields (EDataBook *book, guint32 opid, EDataBookStatus status, GList *fields)
{
  return_status_and_list (opid, status, fields, FALSE);
}

static void
impl_AddressBook_Book_getRequiredFields(EDataBook *book, DBusGMethodInvocation *context)
{
  e_book_backend_get_required_fields (e_data_book_get_backend (book), book, opid_store (context));
}

void
e_data_book_respond_get_required_fields (EDataBook *book, guint32 opid, EDataBookStatus status, GList *fields)
{
  return_status_and_list (opid, status, fields, FALSE);
}

static void
impl_AddressBook_Book_getSupportedAuthMethods(EDataBook *book, DBusGMethodInvocation *context)
{
  e_book_backend_get_supported_auth_methods (e_data_book_get_backend (book), book, opid_store (context));
}

void
e_data_book_respond_get_supported_auth_methods (EDataBook *book, guint32 opid, EDataBookStatus status, GList *auth_methods)
{
  return_status_and_list (opid, status, auth_methods, FALSE);
}

static char*
construct_bookview_path (void)
{
  static volatile guint counter = 1;

  return g_strdup_printf ("unix:abstract=/e-addressbook-view-%d-%d",
                          getpid (),
                          g_atomic_int_exchange_and_add ((int*)&counter, 1));
}

static void
impl_AddressBook_Book_getBookView (EDataBook *book, const char *search, const guint max_results, DBusGMethodInvocation *context)
{
  EBookBackend *backend = e_data_book_get_backend (book);
  EBookBackendSExp *card_sexp;
  EDataBookView *book_view;
  char *path;

  card_sexp = e_book_backend_sexp_new (search);
  if (!card_sexp) {
    GError *error = g_error_new (E_DATA_BOOK_ERROR, InvalidQuery, _("Invalid query"));
    DBusErrorReturnData *data = dbus_error_return_data_new (context, error);
    g_idle_add (idle_dbus_return_error, data);
    return;
  }

  path = construct_bookview_path ();
  book_view = e_data_book_view_new (book, path, search, card_sexp, max_results);

  e_book_backend_add_book_view (backend, book_view);

  g_idle_add (idle_dbus_method_return_str,
              dbus_method_return_str_data_new (context, path));
}

static void
impl_AddressBook_Book_getChanges(EDataBook *book, const char *IN_change_id, DBusGMethodInvocation *context)
{
  OperationData *op;

  op = op_new (OP_GET_CHANGES, book, context);
  op->change_id = g_strdup (IN_change_id);
  g_thread_pool_push (op_pool, op, NULL);
}

void
e_data_book_respond_get_changes (EDataBook *book, guint32 opid, EDataBookStatus status, GList *changes)
{
  DBusGMethodInvocation *context = opid_fetch (opid);

  if (status != Success) {
    GError *error = g_error_new (E_DATA_BOOK_ERROR, status, _("Cannot get changes"));
    DBusErrorReturnData *data = dbus_error_return_data_new (context, error);
    g_idle_add (idle_dbus_return_error, data);
  } else {
    /* The DBus interface to this is a(us), an array of structs of unsigned ints
       and strings.  In dbus-glib this is a GPtrArray of GValueArray of unsigned
       int and strings. */
    GPtrArray *array;

    array = g_ptr_array_new ();

    while (changes != NULL) {
      EDataBookChange *change = (EDataBookChange *) changes->data;
      GValueArray *vals;

      vals = g_value_array_new (2);

      g_value_array_append (vals, NULL);
      g_value_init (g_value_array_get_nth (vals, 0), G_TYPE_UINT);
      g_value_set_uint (g_value_array_get_nth (vals, 0), change->change_type);

      g_value_array_append (vals, NULL);
      g_value_init (g_value_array_get_nth (vals, 1), G_TYPE_STRING);
      g_value_take_string (g_value_array_get_nth (vals, 1), change->vcard);
      /* Now change->vcard is owned by the GValue */

      g_ptr_array_add (array, vals);

      g_free (change);
      changes = g_list_remove (changes, change);
    }

    g_idle_add (idle_dbus_method_return_ptrarray,
                dbus_method_return_ptrarray_data_new (context, array));
  }
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

static void
impl_AddressBook_Book_close(EDataBook *book, DBusGMethodInvocation *context)
{
  EDataBookPrivate *priv = GET_PRIV (book);
  char *sender;

  sender = dbus_g_method_get_sender (context);

  priv->closed_cb (book, sender);

  g_free (sender);

  g_object_unref (book);

  g_idle_add (idle_dbus_method_return, context);
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
  DBusGMethodInvocation *context = opid_fetch (opid);

  if (status == Success) {
    char **array;
    GList *l;
    int i = 0;

    array = g_new0 (char*, g_list_length (list) + 1);
    for (l = list; l != NULL; l = l->next) {
      /* since we're returning this in an idle hander, we need to avoid freeing
       * the array until the idle handler gets a chance to run, so dup all
       * elements if we don't already own them */
      array[i++] = free_data ? l->data : g_strdup (l->data);
    }

    g_idle_add (idle_dbus_method_return_strarray,
                dbus_method_return_strarray_data_new (context, array));
  } else {
    GError *error = g_error_new (E_DATA_BOOK_ERROR, status, _("Cannot complete operation"));
    DBusErrorReturnData *data = dbus_error_return_data_new (context, error);
    g_idle_add (idle_dbus_return_error, data);
  }
}
