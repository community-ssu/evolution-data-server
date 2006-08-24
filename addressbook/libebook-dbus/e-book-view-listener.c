#include <glib-object.h>
#include "e-contact.h"
#include "e-book-view-listener.h"
#include "e-book-marshal.h"

static gboolean impl_notifyStatusMessage(EBookViewListener *listener, const char* message, GError **error);
static gboolean impl_notifyContactsAdded(EBookViewListener *listener, const char **vcards, GError **error);
static gboolean impl_notifyContactsChanged(EBookViewListener *listener, const char **vcards, GError **error);
static gboolean impl_notifyContactsRemoved(EBookViewListener *listener, const char **ids, GError **error);
static gboolean impl_notifyComplete(EBookViewListener *listener, const int status, GError **error);
#include "e-data-book-view-listener-glue.h"

G_DEFINE_TYPE(EBookViewListener, e_book_view_listener, G_TYPE_OBJECT);

struct _EBookViewListenerPrivate {
  gboolean stopped;
  GQueue *queue;
  GStaticMutex idle_mutex;
  int idle_id;
};

#define E_BOOK_VIEW_LISTENER_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), E_TYPE_BOOK_VIEW_LISTENER, EBookViewListenerPrivate))

enum {
  RESPONSE,
  LAST_SIGNAL
};
static guint signals [LAST_SIGNAL];

static void
e_book_view_listener_dispose (GObject *object)
{
  EBookViewListener *listener = E_BOOK_VIEW_LISTENER (object);
  g_queue_free (listener->priv->queue);
  g_static_mutex_free (&listener->priv->idle_mutex);
  /* TODO: stop the idle */
}

static void
e_book_view_listener_class_init (EBookViewListenerClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (EBookViewListenerPrivate));
  
  dbus_g_object_type_install_info (G_TYPE_FROM_CLASS (klass), &dbus_glib_e_data_book_view_listener_object_info);

  signals[RESPONSE] = g_signal_new ("response",
                                    G_OBJECT_CLASS_TYPE (G_OBJECT_CLASS (klass)),
                                    G_SIGNAL_RUN_LAST,
                                    G_STRUCT_OFFSET (EBookViewListenerClass, response),
                                    NULL, NULL,
                                    e_book_marshal_NONE__POINTER,
                                    G_TYPE_NONE, 1, G_TYPE_POINTER);

  gobject_class->dispose = e_book_view_listener_dispose;
}

static void
e_book_view_listener_init (EBookViewListener *listener)
{
  listener->priv = E_BOOK_VIEW_LISTENER_GET_PRIVATE (listener);
  listener->priv->stopped = TRUE;
  listener->priv->queue = g_queue_new ();
  g_static_mutex_init (&listener->priv->idle_mutex);
  listener->priv->idle_id = -1;
}

EBookViewListener*
e_book_view_listener_new (void)
{
  EBookViewListener *listener;
  listener = g_object_new (E_TYPE_BOOK_VIEW_LISTENER, NULL);
  return listener;
}

static void
free_response (EBookViewListenerResponse *response)
{
	g_return_if_fail (response != NULL);
	g_list_foreach (response->ids, (GFunc)g_free, NULL);
	g_list_free (response->ids);
	g_list_foreach (response->contacts, (GFunc) g_object_unref, NULL);
	g_list_free (response->contacts);
	g_free (response->message);
	g_free (response);
}

/* TODO: don't think we need this as dbus isn't re-entrant/threaded */

static gboolean
main_thread_send_response (gpointer data)
{
  EBookViewListener *listener = E_BOOK_VIEW_LISTENER (data);
  EBookViewListenerResponse *response;
  
  g_object_ref (listener);
  g_static_mutex_lock (&listener->priv->idle_mutex);
  
  while ((response = g_queue_pop_head (listener->priv->queue)) != NULL) {
    g_signal_emit (listener, signals [RESPONSE], 0, response);
    free_response (response);
  }
  
  listener->priv->idle_id = -1;
  g_static_mutex_unlock (&listener->priv->idle_mutex);
  g_object_unref (listener);
  
  return FALSE;
}

static void
e_book_view_listener_queue_response (EBookViewListener *listener, EBookViewListenerResponse *response)
{
  if (response == NULL)
    return;
  
  if (listener->priv->stopped) {
    free_response (response);
    return;
  }
  
  g_static_mutex_lock (&listener->priv->idle_mutex);
  g_queue_push_tail (listener->priv->queue, response);
  if (listener->priv->idle_id == -1)
    listener->priv->idle_id = g_idle_add (main_thread_send_response, listener);
  g_static_mutex_unlock (&listener->priv->idle_mutex);
}

static gboolean
impl_notifyStatusMessage(EBookViewListener *listener, const char* message, GError **error)
{
  EBookViewListenerResponse *r;
  g_assert (E_IS_BOOK_VIEW_LISTENER (listener));
  r = g_new0(EBookViewListenerResponse, 1);
  r->op = StatusMessageEvent;
  r->message = g_strdup (message);
  e_book_view_listener_queue_response (listener, r);
  return TRUE;
}

static gboolean
impl_notifyContactsAdded(EBookViewListener *listener, const char **vcards, GError **error)
{
  const char **p;
  EBookViewListenerResponse *r;
  g_assert (E_IS_BOOK_VIEW_LISTENER (listener));
  r = g_new0 (EBookViewListenerResponse, 1);
  r->op = ContactsAddedEvent;
  for (p = vcards; *p; p++) {
    r->contacts = g_list_prepend (r->contacts, e_contact_new_from_vcard (*p));
  }

  e_book_view_listener_queue_response (listener, r);
  return TRUE;
}

static gboolean
impl_notifyContactsChanged(EBookViewListener *listener, const char **vcards, GError **error)
{
  const char **p;
  EBookViewListenerResponse *r;
  g_assert (E_IS_BOOK_VIEW_LISTENER (listener));
  r = g_new0 (EBookViewListenerResponse, 1);
  r->op = ContactsModifiedEvent;
  for (p = vcards; *p; p++) {
    r->contacts = g_list_prepend (r->contacts, e_contact_new_from_vcard (*p));
  }

  e_book_view_listener_queue_response (listener, r);
  return TRUE;
}

static gboolean
impl_notifyContactsRemoved(EBookViewListener *listener, const char **ids, GError **error)
{
  const char **p;
  EBookViewListenerResponse *r;
  g_assert (E_IS_BOOK_VIEW_LISTENER (listener));
  r = g_new0 (EBookViewListenerResponse, 1);
  r->op = ContactsRemovedEvent;
  for (p = ids; *p; p++) {
    r->ids = g_list_prepend (r->ids, g_strdup (*p));
  }

  e_book_view_listener_queue_response (listener, r);
  return TRUE;
}

static gboolean
impl_notifyComplete(EBookViewListener *listener, const int status, GError **error)
{
  EBookViewListenerResponse *r;
  g_assert (E_IS_BOOK_VIEW_LISTENER (listener));
  r = g_new0(EBookViewListenerResponse, 1);
  r->op = SequenceCompleteEvent;
  r->status = status;
  e_book_view_listener_queue_response (listener, r);
  return TRUE;
}

/**
 * e_book_view_listener_start:
 * @listener: an #EBookViewListener
 *
 * Makes @listener start generating events.
 **/
void
e_book_view_listener_start (EBookViewListener *listener)
{
  g_return_if_fail (E_IS_BOOK_VIEW_LISTENER (listener));
  listener->priv->stopped = FALSE;
}

/**
 * e_book_view_listener_stop:
 * @listener: an #EBookViewListener
 *
 * Makes @listener stop generating events.
 **/
void
e_book_view_listener_stop (EBookViewListener *listener)
{
  g_return_if_fail (E_IS_BOOK_VIEW_LISTENER (listener));
  listener->priv->stopped = TRUE;
  
  if (listener->priv->idle_id != -1) {
    g_source_remove (listener->priv->idle_id);
    listener->priv->idle_id = -1;
  }
}
