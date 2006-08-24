#ifndef __E_BOOK_VIEW_LISTENER_H__
#define __E_BOOK_VIEW_LISTENER_H__

#include "e-book-types.h"

#define E_TYPE_BOOK_VIEW_LISTENER           (e_book_view_listener_get_type ())
#define E_BOOK_VIEW_LISTENER(o)             (G_TYPE_CHECK_INSTANCE_CAST ((o), E_TYPE_BOOK_VIEW_LISTENER, EBookViewListener))
#define E_BOOK_VIEW_LISTENER_CLASS(k)       (G_TYPE_CHECK_CLASS_CAST((k), E_TYPE_BOOK_VIEW_LISTENER, EBookViewListenerClass))
#define E_IS_BOOK_VIEW_LISTENER(o)          (G_TYPE_CHECK_INSTANCE_TYPE ((o), E_TYPE_BOOK_VIEW_LISTENER))
#define E_IS_BOOK_VIEW_LISTENER_CLASS(k)    (G_TYPE_CHECK_CLASS_TYPE ((k), E_TYPE_BOOK_VIEW_LISTENER))
#define E_BOOK_VIEW_LISTENER_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS ((obj), E_TYPE_BOOK_VIEW_LISTENER, EBookViewListenerClass))

G_BEGIN_DECLS

typedef struct _EBookViewListener EBookViewListener;
typedef struct _EBookViewListenerClass EBookViewListenerClass;
typedef struct _EBookViewListenerResponse EBookViewListenerResponse;
typedef struct _EBookViewListenerPrivate EBookViewListenerPrivate;

struct _EBookViewListener {
  GObject parent;
  EBookViewListenerPrivate *priv;
};

struct _EBookViewListenerClass {
  GObjectClass parent;
  void (*response) (EBookViewListener *listener, EBookViewListenerResponse *response);
};

GType e_book_view_listener_get_type (void);
EBookViewListener *e_book_view_listener_new (void);
void e_book_view_listener_start (EBookViewListener *listener);
void e_book_view_listener_stop (EBookViewListener *listener);

/* TODO: remove this and merge the view into ebook? */
typedef enum {
  ContactsAddedEvent,
  ContactsRemovedEvent,
  ContactsModifiedEvent,
  SequenceCompleteEvent,
  StatusMessageEvent,
} EBookViewListenerOperation;

struct _EBookViewListenerResponse {
  EBookViewListenerOperation op;
  
  /* For SequenceComplete */
  EBookViewStatus status;
  
  /* For ContactsRemovedEvent */
  GList *ids;
  
  /* For Contact[sAdded|Modified]Event */
  GList *contacts; /* Of type EContact. */
  
  /* For StatusMessageEvent */
  char *message;
};
G_END_DECLS

#endif /* ! __E_BOOK_VIEW_LISTENER_H__ */
