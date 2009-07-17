/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * e-book-sexp.h
 * Copyright 2000, 2001, Ximian, Inc.
 *
 * Authors:
 *   Chris Lahey <clahey@ximian.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License, version 2, as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

#ifndef __E_BOOK_SEXP_H__
#define __E_BOOK_SEXP_H__

#include <glib.h>
#include <glib-object.h>
#include <libebook/e-book-types.h>
#include <libebook/e-contact.h>

G_BEGIN_DECLS

#define E_TYPE_BOOK_SEXP         (e_book_sexp_get_type ())
#define E_BOOK_SEXP(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), E_TYPE_BOOK_SEXP, EBookSExp))
#define E_BOOK_SEXP_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), E_BOOK_TYPE, EBookSExpClass))
#define E_IS_BOOK_SEXP(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), E_TYPE_BOOK_SEXP))
#define E_IS_BOOK_SEXP_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), E_TYPE_BOOK_SEXP))
#define E_BOOK_SEXP_GET_CLASS(k) (G_TYPE_INSTANCE_GET_CLASS ((obj), E_TYPE_BOOK_SEXP, EBookSExpClass))

typedef struct _EBookSExp        EBookSExp;
typedef struct _EBookSExpClass   EBookSExpClass;
typedef struct _EBookSExpPrivate EBookSExpPrivate;

struct _EBookSExp {
	GObject parent_object;
	EBookSExpPrivate *priv;
};

struct _EBookSExpClass {
	GObjectClass parent_class;
};

EBookSExp *e_book_sexp_new      (const char *text);
GType      e_book_sexp_get_type (void);

gboolean   e_book_sexp_parse (EBookSExp *sexp, const char *text);
gboolean   e_book_sexp_match_vcard (EBookSExp *sexp, const char *vcard);
gboolean   e_book_sexp_match_contact (EBookSExp *sexp, EContact *contact);

G_END_DECLS

#endif /* __E_BOOK_SEXP_H__ */
