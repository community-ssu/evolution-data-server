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

#ifndef __E_BOOK_VIEW_H__
#define __E_BOOK_VIEW_H__

#include <glib.h>
#include <glib-object.h>
#include "e-book-types.h"

#define E_TYPE_BOOK_VIEW           (e_book_view_get_type ())
#define E_BOOK_VIEW(o)             (G_TYPE_CHECK_INSTANCE_CAST ((o), E_TYPE_BOOK_VIEW, EBookView))
#define E_BOOK_VIEW_CLASS(k)       (G_TYPE_CHECK_CLASS_CAST((k), E_TYPE_BOOK_VIEW, EBookViewClass))
#define E_IS_BOOK_VIEW(o)          (G_TYPE_CHECK_INSTANCE_TYPE ((o), E_TYPE_BOOK_VIEW))
#define E_IS_BOOK_VIEW_CLASS(k)    (G_TYPE_CHECK_CLASS_TYPE ((k), E_TYPE_BOOK_VIEW))
#define E_BOOK_VIEW_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS ((obj), E_TYPE_BOOK_VIEW, EBookViewClass))

G_BEGIN_DECLS

typedef struct _EBookView        EBookView;
typedef struct _EBookViewClass   EBookViewClass;
typedef struct _EBookViewPrivate EBookViewPrivate;

struct _EBook;  /* Forward reference */

struct _EBookView {
	GObject     parent;
	/*< private >*/
	EBookViewPrivate *priv;
};

struct _EBookViewClass {
	GObjectClass parent;

	/*
	 * Signals.
	 */
	void (* contacts_changed)  (EBookView *book_view, const GList *contacts);
	void (* contacts_removed)  (EBookView *book_view, const GList *ids);
	void (* contacts_added)    (EBookView *book_view, const GList *contacts);
	void (* sequence_complete) (EBookView *book_view, EBookViewStatus status);
	void (* status_message)    (EBookView *book_view, const char *message);

	/* Padding for future expansion */
	void (*_ebook_reserved0) (void);
	void (*_ebook_reserved1) (void);
	void (*_ebook_reserved2) (void);
	void (*_ebook_reserved3) (void);
	void (*_ebook_reserved4) (void);
};

GType              e_book_view_get_type               (void);

void               e_book_view_start                  (EBookView *book_view);
void               e_book_view_stop                   (EBookView *book_view);
void               e_book_view_thaw                   (EBookView *book_view);

struct _EBook     *e_book_view_get_book               (EBookView *book_view);
struct EBookQuery *e_book_view_get_query              (EBookView *book_view);

void               e_book_view_set_freezable          (EBookView *book_view, gboolean freezable);
gboolean           e_book_view_is_freezable           (EBookView *book_view);

void               e_book_view_set_parse_vcards       (EBookView *book_view, gboolean emit_vcards);
gboolean           e_book_view_get_parse_vcards       (EBookView *book_view);

void               e_book_view_set_sort_order         (EBookView *book_view, const gchar *query_term);

G_END_DECLS

#endif /* ! __E_BOOK_VIEW_H__ */
