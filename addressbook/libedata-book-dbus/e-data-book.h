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

#ifndef __E_DATA_BOOK_H__
#define __E_DATA_BOOK_H__

#include <glib-object.h>
#include <dbus/dbus-glib.h>
#include <libedataserver/e-source.h>
#include "e-book-backend.h"
#include "e-data-book-types.h"

typedef void (* EDataBookClosedCallback) (EDataBook *book, const char *client);

struct _EDataBook
{
  GObject       parent;
  EBookBackend *backend;
  ESource      *source;
};

struct _EDataBookClass
{
  GObjectClass parent;
};

#define E_TYPE_DATA_BOOK              (e_data_book_get_type ())
#define E_DATA_BOOK(object)           (G_TYPE_CHECK_INSTANCE_CAST ((object), E_TYPE_DATA_BOOK, EDataBook))
#define E_DATA_BOOK_CLASS(klass)      (G_TYPE_CHECK_CLASS_CAST ((klass), E_TYPE_DATA_BOOK, EDataBookClass))
#define E_IS_DATA_BOOK(object)        (G_TYPE_CHECK_INSTANCE_TYPE ((object), E_TYPE_DATA_BOOK))
#define E_IS_DATA_BOOK_CLASS(klass)   (G_TYPE_CHECK_CLASS_TYPE ((klass), E_TYPE_DATA_BOOK))
#define E_DATA_BOOK_GET_CLASS(obj)    (G_TYPE_INSTANCE_GET_CLASS ((obj), E_TYPE_DATA_BOOK, EDataBookClass))

GQuark e_data_book_error_quark (void);
#define E_DATA_BOOK_ERROR e_data_book_error_quark ()

GType e_data_book_get_type(void);

EDataBook * e_data_book_new (EBookBackend *backend, ESource *source, EDataBookClosedCallback closed_cb);

ESource* e_data_book_get_source (EDataBook *book);
EBookBackend* e_data_book_get_backend (EDataBook *book);

/* Callbacks from the backends */
void e_data_book_respond_open (EDataBook *book, guint opid, EDataBookStatus status);
void e_data_book_respond_get_contact (EDataBook *book, guint32 opid, EDataBookStatus status, char *vcard);
void e_data_book_respond_get_contact_list (EDataBook *book, guint32 opid, EDataBookStatus status, GList *cards);
void e_data_book_respond_remove_contacts (EDataBook *book, guint32 opid, EDataBookStatus  status, GList *ids);
void e_data_book_respond_remove (EDataBook *book, guint32 opid, EDataBookStatus status);
void e_data_book_respond_modify (EDataBook *book, guint32 opid, EDataBookStatus status, EContact *contact);
void e_data_book_respond_modify_contacts (EDataBook *book, guint32 opid, EDataBookStatus status, GList *contacts);
void e_data_book_respond_create (EDataBook *book, guint32 opid, EDataBookStatus status, EContact *contact);
void e_data_book_respond_create_contacts (EDataBook *book, guint32 opid, EDataBookStatus status, GList *contacts);
void e_data_book_respond_get_changes (EDataBook *book, guint32 opid, EDataBookStatus status, GList *changes);
void e_data_book_respond_authenticate_user (EDataBook *book, guint32 opid, EDataBookStatus status);
void e_data_book_respond_get_supported_fields (EDataBook *book, guint32 opid, EDataBookStatus status, GList *fields);
void e_data_book_respond_get_required_fields (EDataBook *book, guint32 opid, EDataBookStatus status, GList *fields);
void e_data_book_respond_get_supported_auth_methods (EDataBook *book, guint32 opid, EDataBookStatus status, GList *fields);

void e_data_book_report_writable (EDataBook *book, gboolean writable);
void e_data_book_report_connection_status (EDataBook *book, gboolean is_online);
void e_data_book_report_auth_required (EDataBook *book);

#endif /* __E_DATA_BOOK_H__ */
