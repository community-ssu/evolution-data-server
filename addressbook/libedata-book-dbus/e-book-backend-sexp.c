/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * pas-backend-card-sexp.c
 * Copyright 1999, 2000, 2001, Ximian, Inc.
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

#include <string.h>
#include "libedataserver/e-sexp.h"
#include "libedataserver/e-data-server-util.h"
#include "e-book-backend-sexp.h"

/**
 * e_book_backend_sexp_match_contact:
 * @sexp: an #EBookBackendSExp
 * @contact: an #EContact
 *
 * Checks if @contact matches @sexp.
 *
 * Return value: %TRUE if the contact matches, %FALSE otherwise.
 **/
gboolean
e_book_backend_sexp_match_contact (EBookBackendSExp *sexp, EContact *contact)
{
	return e_book_sexp_match_contact ((EBookSExp *) sexp, contact);
}

/**
 * e_book_backend_sexp_match_vcard:
 * @sexp: an #EBookBackendSExp
 * @vcard: a VCard string
 *
 * Checks if @vcard matches @sexp.
 *
 * Return value: %TRUE if the VCard matches, %FALSE otherwise.
 **/
gboolean
e_book_backend_sexp_match_vcard (EBookBackendSExp *sexp, const char *vcard)
{
	return e_book_sexp_match_vcard ((EBookSExp *) sexp, vcard);
}



/**
 * e_book_backend_sexp_new:
 * @text: an s-expression to parse
 *
 * Creates a new #EBookBackendSExp from @text.
 *
 * Return value: A new #EBookBackendSExp.
 **/
EBookBackendSExp *
e_book_backend_sexp_new (const char *text)
{
	EBookBackendSExp *sexp = g_object_new (E_TYPE_BACKEND_SEXP, NULL);

	if (!e_book_sexp_parse ((EBookSExp *) sexp, text)) {
		g_object_unref (sexp);
		sexp = NULL;
	}

	return sexp;
}

/**
 * e_book_backend_sexp_get_type:
 */
GType
e_book_backend_sexp_get_type (void)
{
	static GType type = 0;

	if (! type) {
		GTypeInfo info = {
			sizeof (EBookBackendSExpClass),
			NULL, /* base_class_init */
			NULL, /* base_class_finalize */
			NULL, /* class_init */
			NULL, /* class_finalize */
			NULL, /* class_data */
			sizeof (EBookBackendSExp),
			0,    /* n_preallocs */
			NULL  /* instance_init */
		};

		type = g_type_register_static (E_TYPE_BOOK_SEXP, "EBookBackendSExp", &info, 0);
	}

	return type;
}
