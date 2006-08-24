#include <glib-object.h>
#include <dbus/dbus-glib.h>
#include "e-book.h"
#include "e-book-view.h"
#include "e-book-view-private.h"
#include "e-data-book-view-bindings.h"
#include "e-book-marshal.h"

G_DEFINE_TYPE(EBookView, e_book_view, G_TYPE_OBJECT);
#define E_BOOK_VIEW_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), E_TYPE_BOOK_VIEW, EBookViewPrivate))

typedef struct _EBookViewPrivate {
  EBook *book;
  DBusGProxy *view_proxy;
  EBookViewListener *listener;
} EBookViewPrivate;

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
  EBookViewPrivate *priv = E_BOOK_VIEW_GET_PRIVATE (view);

  if (priv->book) {
    g_object_unref (priv->book);
    priv->book = NULL;
  }
  
  if (priv->listener) {
    e_book_view_listener_stop (priv->listener);
    g_object_unref (priv->listener);
    priv->listener = NULL;
  }
  
  if (priv->view_proxy) {
    g_object_unref (priv->view_proxy);
    priv->view_proxy = NULL;
  }
}

static void
e_book_view_class_init (EBookViewClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
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

  object_class->dispose = e_book_view_dispose;
}

static void
e_book_view_init (EBookView *view)
{
}

static void
on_response (EBookViewListener *listener, EBookViewListenerResponse *response, EBookView *book_view)
{
  switch (response->op) {
  case StatusMessageEvent:
    g_signal_emit (book_view, signals [STATUS_MESSAGE], 0, response->message);
    break;
  case ContactsModifiedEvent:
    g_signal_emit (book_view, signals [CONTACTS_CHANGED], 0, response->contacts);
    break;
  case ContactsRemovedEvent:
    g_signal_emit (book_view, signals [CONTACTS_REMOVED], 0, response->ids);
    break;
  case ContactsAddedEvent:
    g_signal_emit (book_view, signals [CONTACTS_ADDED], 0, response->contacts);
    break;
  case SequenceCompleteEvent:
    g_signal_emit (book_view, signals [SEQUENCE_COMPLETE], 0, response->status);
    break;
  default:
    g_warning("Unhandled response type %d\n", response->op);
  }
}

EBookView*
e_book_view_new (EBook *book, DBusGProxy *view_proxy, EBookViewListener *listener)
{
  EBookView *view;
  EBookViewPrivate *priv;
  view = g_object_new (E_TYPE_BOOK_VIEW, NULL);
  priv = E_BOOK_VIEW_GET_PRIVATE (view);
  priv->book = g_object_ref (book);
  /* Take ownership of the view_proxy and listener objects */
  priv->view_proxy = view_proxy;
  g_object_add_weak_pointer (G_OBJECT (view_proxy), (gpointer)&priv->view_proxy);
  priv->listener = listener;

  g_signal_connect (priv->listener, "response", G_CALLBACK (on_response), view);
  return view;
}

EBook *
e_book_view_get_book (EBookView *book_view)
{
  EBookViewPrivate *priv;
  g_return_val_if_fail (E_IS_BOOK_VIEW (book_view), NULL);
  priv = E_BOOK_VIEW_GET_PRIVATE (book_view);
  return priv->book;
}

void
e_book_view_start (EBookView *book_view)
{
  EBookViewPrivate *priv;
  GError *error = NULL;

  g_return_if_fail (E_IS_BOOK_VIEW (book_view));

  priv = E_BOOK_VIEW_GET_PRIVATE (book_view);

  e_book_view_listener_start (priv->listener);

  if (priv->view_proxy) {
    org_gnome_evolution_dataserver_addressbook_BookView_start (priv->view_proxy, &error);
    if (error) {
      g_printerr("Cannot start book view: %s\n", error->message);
      g_error_free (error);
    }
  }
}

void
e_book_view_stop (EBookView *book_view)
{
  EBookViewPrivate *priv;
  GError *error = NULL;

  g_return_if_fail (E_IS_BOOK_VIEW (book_view));

  priv = E_BOOK_VIEW_GET_PRIVATE (book_view);

  e_book_view_listener_stop (priv->listener);

  if (priv->view_proxy) {
    org_gnome_evolution_dataserver_addressbook_BookView_stop (priv->view_proxy, &error);
    if (error) {
      g_printerr("Cannot stop book view: %s\n", error->message);
      g_error_free (error);
    }
  }
}
