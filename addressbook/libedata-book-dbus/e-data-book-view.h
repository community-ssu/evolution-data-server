#ifndef __E_DATA_BOOK_VIEW_H__
#define __E_DATA_BOOK_VIEW_H__

#include <glib.h>
#include <glib-object.h>
#include <dbus/dbus-glib.h>
#include <libebook/e-contact.h>
#include "e-data-book-types.h"
#include "e-book-backend.h"
#include "e-book-backend-sexp.h"

G_BEGIN_DECLS

#define E_TYPE_DATA_BOOK_VIEW        (e_data_book_view_get_type ())
#define E_DATA_BOOK_VIEW(o)          (G_TYPE_CHECK_INSTANCE_CAST ((o), E_TYPE_DATA_BOOK_VIEW, EDataBookView))
#define E_DATA_BOOK_VIEW_CLASS(k)    (G_TYPE_CHECK_CLASS_CAST((k), E_TYPE_DATA_BOOK_VIEW, EDataBookViewClass))
#define E_IS_DATA_BOOK_VIEW(o)       (G_TYPE_CHECK_INSTANCE_TYPE ((o), E_TYPE_DATA_BOOK_VIEW))
#define E_IS_DATA_BOOK_VIEW_CLASS(k) (G_TYPE_CHECK_CLASS_TYPE ((k), E_TYPE_DATA_BOOK_VIEW))
#define E_DATA_BOOK_VIEW_GET_CLASS(k) (G_TYPE_INSTANCE_GET_CLASS ((obj), E_TYPE_DATA_BOOK_VIEW, EDataBookView))

typedef struct _EDataBookViewPrivate EDataBookViewPrivate;

struct _EDataBookView {
  GObject parent;
  EDataBookViewPrivate *priv;
};

struct _EDataBookViewClass {
  GObjectClass parent;
};

EDataBookView *e_data_book_view_new (EBookBackend *backend, DBusGProxy *listener, const char *card_query, EBookBackendSExp *card_sexp, int max_results);

void              e_data_book_view_set_thresholds    (EDataBookView *book_view,
                                                      int minimum_grouping_threshold,
                                                      int maximum_grouping_threshold);

const char*       e_data_book_view_get_card_query    (EDataBookView                *book_view);
EBookBackendSExp* e_data_book_view_get_card_sexp     (EDataBookView                *book_view);
int               e_data_book_view_get_max_results   (EDataBookView                *book_view);
EBookBackend*     e_data_book_view_get_backend       (EDataBookView                *book_view);
#if 0
GNOME_Evolution_Addressbook_BookViewListener e_data_book_view_get_listener (EDataBookView  *book_view);
GMutex*           e_data_book_view_get_mutex         (EDataBookView                *book_view);
#endif
void         e_data_book_view_notify_update          (EDataBookView                *book_view,
                                                      EContact                     *contact);
void         e_data_book_view_notify_update_vcard    (EDataBookView                *book_view,
                                                      char                         *vcard);
void         e_data_book_view_notify_update_prefiltered_vcard (EDataBookView *book_view, const char *id, char *vcard);
void         e_data_book_view_notify_remove          (EDataBookView                *book_view,
                                                      const char                   *id);
void         e_data_book_view_notify_complete        (EDataBookView                *book_view, EDataBookStatus status);
void         e_data_book_view_notify_status_message  (EDataBookView                *book_view,
                                                      const char                   *message);

GType        e_data_book_view_get_type               (void);

G_END_DECLS

#endif /* ! __E_DATA_BOOK_VIEW_H__ */
